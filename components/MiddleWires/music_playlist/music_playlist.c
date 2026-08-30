/*
 * music_playlist.c - 五歌手轮转自动切歌播放控制器实现
 *
 * 状态机（主循环 500ms 轮询，事件回调只置标志、不直接切歌，规避旧流延迟事件竞态）：
 *   FINISHED        → 轮转到下一位歌手，清零其失败计数
 *   ERROR / 看门狗   → 同歌手换下一首（skip 计数 +1，≥4 换歌手）
 *   停滞看门狗       → 进度 30 秒无推进强制 stop 跳歌（防解码卡死无人报告事件）
 *
 * 选曲策略：
 *   search3(songCount=100) 收集全部 MP3 匹配 → skip=0 时优先 mg_（咪咕真 MP3）其次 kw_
 *   → 302 预解析得 CDN 直链 → 校验后缀必须 .mp3 → 播放；否则按无音源处理。
 */
#include "music_playlist.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"

#include "sdkconfig.h"
#include "audio_player.h"

/* 服务器连接参数（menuconfig: LX-Server Music Player Configuration） */
#define LX_SERVER_IP    CONFIG_LX_SERVER_IP
#define LX_SERVER_PORT  CONFIG_LX_SERVER_PORT
#define LX_USER         CONFIG_LX_USER
#define LX_PASSWORD     CONFIG_LX_PASSWORD

static const char *TAG = "music_playlist";

/* ---------------- 内部状态 ---------------- */

/* 轮转歌手列表：自然播完则切下一位；无 MP3/连续失败自动跳过 */
static const char *s_artists[] = {
    "邓紫棋",
    "周杰伦",
    "薛之谦",
    "林俊杰",
    "陈奕迅",
};
#define ARTIST_COUNT (sizeof(s_artists) / sizeof(s_artists[0]))

/* 连续失败达到该次数后换下一位歌手 */
#define MAX_FAIL_PER_ARTIST 4

/* 全部歌手无可用曲目时的冷却重试间隔（秒） */
#define RETRY_DELAY_SEC 60
#define RETRY_DELAY_TICKS (RETRY_DELAY_SEC * 2)   /* 500ms/tick */

/* 控制任务栈：搜索/重定向预解析含 TLS 握手，需要较大栈 */
#define PLAYLIST_TASK_STACK_SIZE (16 * 1024)
#define PLAYLIST_TASK_NAME       "music_ctrl"

static audio_player_t *s_player = NULL;        /* 播放器实例（audio_player 中间件） */
static int s_artist_idx = 0;                   /* 当前歌手下标 */
static char *s_current_id = NULL;              /* 当前播放的 song id（需 free） */
static volatile bool s_need_next = false;      /* 事件标志：主循环查状态后安全切歌 */
static int s_fail_count[ARTIST_COUNT] = { 0 }; /* 每歌手连续失败次数 */
static uint64_t s_last_pos = 0;                /* 看门狗：上次进度 */
static int s_stall_ticks = 0;                  /* 看门狗：停滞计数（2s/tick） */
static int s_retry_ticks = 0;                  /* >0：全部歌手无曲目时的冷却重试倒计时 */

/* ---------------- 内部工具 ---------------- */

/* 判断 URL 路径部分（去掉 ?query）是否以 .mp3 结尾 */
static bool url_is_mp3(const char *url)
{
    char path[512];
    const char *q = strchr(url, '?');
    int len = q ? (int)(q - url) : (int)strlen(url);
    if (len <= 0) {
        return false;
    }
    if (len >= (int)sizeof(path)) {
        len = (int)sizeof(path) - 1;
    }
    memcpy(path, url, len);
    path[len] = '\0';
    const char *dot = strrchr(path, '.');
    return dot && strcasecmp(dot, ".mp3") == 0;
}

/* 构造 stream URL → 预解析 302 得到最终 CDN 直连地址 → 校验 MP3 后缀 → 播放 */
static esp_err_t play_song(const char *song_id)
{
    char url[512];
    snprintf(url, sizeof(url),
             "http://%s:%d/rest/stream.view?id=%s&u=%s&p=%s&v=1.16.1&c=esp32",
             LX_SERVER_IP, LX_SERVER_PORT, song_id, LX_USER, LX_PASSWORD);

    /* lx-server 对未缓存曲目返回 302，播放器内部 HTTP 不跟随重定向，
     * 这里先手动解析出最终 CDN 直连地址再播放 */
    char final_url[512];
    esp_err_t err = audio_player_resolve_url(url, final_url, sizeof(final_url));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "解析最终地址失败（%s），跳过", esp_err_to_name(err));
        return err;
    }

    /* 只接受真正的 MP3 直链：酷我等源元数据标 MP3 但 CDN 实际给
     * flac/mflac/无后缀流，会导致解码停滞，此处按"无 MP3 音源"处理 */
    if (!url_is_mp3(final_url)) {
        ESP_LOGW(TAG, "CDN 直链非 MP3（%.60s...），放弃该曲目", final_url);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "播放: %s", final_url);
    return audio_player_play_url(s_player, final_url);
}

/* 搜索当前歌手的第 s_fail_count 个 MP3 并播放（skip=0 时 mg_ 源优先）。
 * 当前歌手无可用曲目或播放启动失败时，自动轮转到下一位歌手。
 * 返回 ESP_OK 表示已启动播放；ESP_FAIL 表示所有歌手暂无可用 MP3（调用方应安排重试）。 */
static esp_err_t play_next_song(void)
{
    /* 释放上一首的 song id */
    if (s_current_id) {
        free(s_current_id);
        s_current_id = NULL;
    }

    /* 最多尝试 ARTIST_COUNT 次（遍历所有歌手一圈） */
    for (int attempt = 0; attempt < (int)ARTIST_COUNT; attempt++) {
        const char *artist = s_artists[s_artist_idx];
        int skip = s_fail_count[s_artist_idx];
        if (skip > 0) {
            ESP_LOGI(TAG, "搜索 '%s' 的 MP3（跳过前 %d 首失败曲目）...", artist, skip);
        } else {
            ESP_LOGI(TAG, "搜索 '%s' 的 MP3...", artist);
        }

        char *found_id = NULL;
        esp_err_t err = audio_player_search_mp3(
            LX_SERVER_IP, LX_SERVER_PORT, LX_USER, LX_PASSWORD,
            artist, skip, &found_id);

        if (err != ESP_OK || !found_id) {
            /* 该歌手已无更多 MP3（或本就没有），轮转到下一位并清零其失败计数 */
            ESP_LOGW(TAG, "'%s' 无更多可用 MP3，切换歌手", artist);
            s_fail_count[s_artist_idx] = 0;
            s_artist_idx = (s_artist_idx + 1) % (int)ARTIST_COUNT;
            continue;
        }

        /* 预解析 + 启动播放。启动失败也计入失败，重试该歌手的下一首 */
        s_current_id = found_id;
        ESP_LOGI(TAG, ">> %s - song id: %s", artist, s_current_id);
        if (play_song(s_current_id) == ESP_OK) {
            return ESP_OK;
        }
        free(s_current_id);
        s_current_id = NULL;
        /* 连续失败过多：换歌手但保留失败计数——下一轮从该歌手列表更深处
         * 继续探索（避免每轮重复踩前几首的坑）；计数在"无更多结果"时才清零 */
        if (++s_fail_count[s_artist_idx] >= MAX_FAIL_PER_ARTIST) {
            s_artist_idx = (s_artist_idx + 1) % (int)ARTIST_COUNT;
        }
    }

    /* 遍历一圈仍无可用曲目：不放弃，交由控制任务冷却后重试
     * （服务器上游慢/临时故障时通常稍后即可恢复） */
    ESP_LOGW(TAG, "所有歌手暂无可用 MP3，%d 秒后重试...", RETRY_DELAY_SEC);
    return ESP_FAIL;
}

/* 播放完毕/出错后的安全切歌：查询真实播放器状态，避免旧流的延迟事件打断新流。
 * 切歌失败（全部歌手暂无可用曲目）时安排冷却重试。 */
static void check_auto_next(void)
{
    if (!s_need_next) {
        return;
    }
    esp_player_state_t st = ESP_PLAYER_STATE_STOPPED;
    audio_player_get_state(s_player, &st);
    if (st == ESP_PLAYER_STATE_PREPARING || st == ESP_PLAYER_STATE_PLAYING ||
        st == ESP_PLAYER_STATE_PAUSED) {
        /* 新歌已在播放/准备中：旧流的延迟事件，忽略 */
        s_need_next = false;
        return;
    }
    s_need_next = false;
    /* 播放器已空闲（FINISHED / ERROR 恢复到 IDLE），安全切歌 */
    if (st == ESP_PLAYER_STATE_FINISHED) {
        /* 自然播放完毕：轮转到下一位歌手 */
        s_fail_count[s_artist_idx] = 0;
        s_artist_idx = (s_artist_idx + 1) % (int)ARTIST_COUNT;
        ESP_LOGI(TAG, "=== 歌曲播放完毕，切到下一位歌手 ===");
    } else {
        /* 播放出错：同歌手换下一首（失败计数 +1，过多则换歌手并保留计数继续深探） */
        ESP_LOGW(TAG, "播放出错，重试 '%s' 的下一首", s_artists[s_artist_idx]);
        if (++s_fail_count[s_artist_idx] >= MAX_FAIL_PER_ARTIST) {
            s_artist_idx = (s_artist_idx + 1) % (int)ARTIST_COUNT;
        }
    }
    if (play_next_song() != ESP_OK) {
        s_retry_ticks = RETRY_DELAY_TICKS;
    }
}

/* 停滞看门狗：进度长时间不动（如解码器卡死且无事件）时强制跳过，保证歌单永不卡死 */
static void check_stall(void)
{
    uint64_t pos = 0, dur = 0;
    audio_player_get_position(s_player, &pos, &dur);
    esp_player_state_t st = ESP_PLAYER_STATE_STOPPED;
    audio_player_get_state(s_player, &st);

    if (st != ESP_PLAYER_STATE_PREPARING && st != ESP_PLAYER_STATE_PLAYING) {
        s_stall_ticks = 0;
        s_last_pos = 0;
        return;
    }
    if (pos > s_last_pos + 200) {
        /* 进度正常推进 */
        s_last_pos = pos;
        s_stall_ticks = 0;
        return;
    }
    /* 进度停滞，每 2s 记一次，30 秒（15 tick）无进展则强制切歌 */
    if (++s_stall_ticks >= 15) {
        ESP_LOGW(TAG, "进度停滞 30 秒（pos=%llu ms），强制跳过该曲目",
                 (unsigned long long)pos);
        audio_player_stop(s_player);
        s_stall_ticks = 0;
        s_last_pos = 0;
        if (++s_fail_count[s_artist_idx] >= MAX_FAIL_PER_ARTIST) {
            s_fail_count[s_artist_idx] = 0;
            s_artist_idx = (s_artist_idx + 1) % (int)ARTIST_COUNT;
        }
        s_need_next = true;
    }
}

/* ---------------- 播放事件回调与控制任务 ---------------- */

static void player_event_cb(esp_player_event_type_t event, void *data, void *ctx)
{
    (void)data;
    (void)ctx;
    switch (event) {
        case ESP_PLAYER_EVENT_FINISHED:
        case ESP_PLAYER_EVENT_ERROR:
            /* 仅置标志，控制任务查询状态后再切歌（事件可能来自旧流，不可直接切） */
            s_need_next = true;
            break;
        default:
            break;
    }
}

/* 播放控制任务：启动首曲 + 500ms 轮询状态机（切歌/看门狗/心跳/冷却重试） */
static void playlist_task(void *arg)
{
    (void)arg;
    int tick = 0;
    if (play_next_song() != ESP_OK) {
        s_retry_ticks = RETRY_DELAY_TICKS;
    }
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(500));

        /* 冷却重试：上一轮全部歌手无可用曲目，倒计时结束后再试一轮 */
        if (s_retry_ticks > 0) {
            if (--s_retry_ticks == 0) {
                if (play_next_song() != ESP_OK) {
                    s_retry_ticks = RETRY_DELAY_TICKS;
                }
            }
            continue;
        }

        check_auto_next();
        if (++tick % 4 == 0) {       /* 约每 2 秒：停滞检测 */
            check_stall();
        }
        if (tick % 20 == 0) {        /* 约每 10 秒：进度心跳 */
            uint64_t pos = 0, dur = 0;
            audio_player_get_position(s_player, &pos, &dur);
            ESP_LOGI(TAG, "进度: %llu / %llu ms | 歌手: %s",
                     (unsigned long long)pos, (unsigned long long)dur,
                     s_artists[s_artist_idx]);
        }
    }
}

/* ---------------- 对外接口 ---------------- */

esp_err_t music_playlist_start(void)
{
    if (s_player != NULL) {
        ESP_LOGW(TAG, "music_playlist 已启动");
        return ESP_OK;
    }

    /* 初始化播放器（BSP ES8311 + esp_player 管线） */
    ESP_RETURN_ON_ERROR(audio_player_init(&s_player), TAG, "audio_player_init failed");
    ESP_RETURN_ON_ERROR(audio_player_set_event_cb(s_player, player_event_cb, NULL),
                        TAG, "set_event_cb failed");

    /* 创建播放控制任务 */
    if (xTaskCreate(playlist_task, PLAYLIST_TASK_NAME,
                    PLAYLIST_TASK_STACK_SIZE / sizeof(StackType_t),
                    NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "创建播放控制任务失败");
        audio_player_deinit(s_player);
        s_player = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "音乐轮播已启动（服务器 %s:%d）", LX_SERVER_IP, LX_SERVER_PORT);
    return ESP_OK;
}
