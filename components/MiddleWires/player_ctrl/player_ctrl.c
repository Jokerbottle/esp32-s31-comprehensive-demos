/*
 * player_ctrl.c - 播放控制器实现
 *
 * 任务模型（FreeRTOS）：
 *   - 控制任务 player_task：等通知（web 下发 play）→ 候选源链依次尝试 → 监控循环；
 *   - volume / ping / hello 等轻指令在 ws_server 上下文直接处理；
 *   - audio_player 事件回调仅更新状态并经 ws_server 上报，不做重活。
 *
 * 成功率策略（针对 lx-server 上游波动）：
 *   1. play 指令可携带 "alt" 备选编号数组（同一首歌的其他音源），失败自动切换；
 *   2. 每个候选的 302 预解析失败自动重试 1 次（间歇性失败重试即可成功）；
 *   3. 源健康度跟踪：某前缀（kw/mg/kg）10 分钟内失败 ≥3 次标记不健康，
 *      后续候选优先跳过该源（不再白等 20s 超时）；全部不健康时仍会兜底尝试；
 *   4. 详细的失败原因（哪一跳/超时/HTTP 状态码）上报 web 日志。
 */
#include "player_ctrl.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "ws_server.h"

/* lx-server 连接参数（menuconfig） */
#define LX_SERVER_IP    CONFIG_LX_SERVER_IP
#define LX_SERVER_PORT  CONFIG_LX_SERVER_PORT
#define LX_USER         CONFIG_LX_USER
#define LX_PASSWORD     CONFIG_LX_PASSWORD

#define CTRL_TASK_NAME       "player_ctrl"
#define CTRL_TASK_STACK_SIZE (12 * 1024)   /* 302 预解析含 TLS 握手，需较大栈 */

#define MAX_CANDIDATES  5     /* 主编号 + 最多 4 个备选源 */
#define RESOLVE_RETRY   2     /* 每个候选的解析尝试次数 */
#define CONFIRM_PLAY_MS 10000 /* 等待"真正进入 PLAYING"的最长时间 */

/* 状态机 */
typedef enum {
    PST_IDLE = 0,
    PST_PREPARING,
    PST_PLAYING,
    PST_PAUSED,
} player_state_t;

static const char *TAG = "player_ctrl";

static audio_player_t *s_player = NULL;
static TaskHandle_t    s_task = NULL;

static volatile player_state_t s_state = PST_IDLE;
static char  s_cur_id[64] = "";              /* 当前/最近一次播放的 song id */
static volatile bool s_pending_play = false; /* web 请求播放新歌 */
static char  s_pending_ids[MAX_CANDIDATES][64];
static int   s_pending_ncand = 0;
static volatile bool s_req_stop = false;
static volatile bool s_req_pause = false;
static volatile bool s_req_resume = false;

/* ---------------- 小工具 ---------------- */

static const char *state_name(player_state_t st)
{
    switch (st) {
        case PST_PREPARING: return "preparing";
        case PST_PLAYING:   return "playing";
        case PST_PAUSED:    return "paused";
        default:            return "idle";
    }
}

static void send_state(void)
{
    char buf[160];
    snprintf(buf, sizeof(buf),
             "{\"evt\":\"state\",\"state\":\"%s\",\"id\":\"%s\"}",
             state_name(s_state), s_cur_id);
    ws_server_send_json(buf);
}

static void send_error(const char *msg)
{
    char buf[288];
    snprintf(buf, sizeof(buf),
             "{\"evt\":\"error\",\"id\":\"%s\",\"msg\":\"%s\"}", s_cur_id, msg);
    ws_server_send_json(buf);
}

static void send_log(const char *text)
{
    char buf[288];
    snprintf(buf, sizeof(buf), "{\"evt\":\"log\",\"text\":\"%s\"}", text);
    ws_server_send_json(buf);
}

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

/* ---------------- 源健康度跟踪 ---------------- */

#define SRC_HEALTH_MAX 6
#define SRC_FAIL_THRESHOLD 3
#define SRC_HEALTH_WINDOW_US (10LL * 60 * 1000000)  /* 10 分钟窗口 */

typedef struct {
    char    pfx[4];
    int     fails;
    int64_t window_start_us;
} src_health_t;

static src_health_t s_health[SRC_HEALTH_MAX];

/* 取 song id 的源前缀（kw_/mg_/kg_...），无匹配返回 "xx" */
static const char *src_pfx(const char *id)
{
    static const char *known[] = { "kw", "mg", "kg", "wy", "tx" };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
        if (strncmp(id, known[i], 2) == 0 && id[2] == '_') {
            return known[i];
        }
    }
    return "xx";
}

static src_health_t *health_find(const char *pfx, bool create)
{
    for (int i = 0; i < SRC_HEALTH_MAX; i++) {
        if (s_health[i].pfx[0] != '\0' && strcmp(s_health[i].pfx, pfx) == 0) {
            return &s_health[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < SRC_HEALTH_MAX; i++) {
        if (s_health[i].pfx[0] == '\0') {
            strlcpy(s_health[i].pfx, pfx, sizeof(s_health[i].pfx));
            s_health[i].fails = 0;
            s_health[i].window_start_us = 0;
            return &s_health[i];
        }
    }
    return NULL;
}

/* 记录一次源失败：10 分钟窗口内累计 */
static void health_mark_fail(const char *id)
{
    src_health_t *h = health_find(src_pfx(id), true);
    if (h == NULL) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (h->window_start_us == 0 || now - h->window_start_us > SRC_HEALTH_WINDOW_US) {
        h->window_start_us = now;
        h->fails = 1;
    } else {
        h->fails++;
    }
}

/* 播放成功：清除该源的失败记录 */
static void health_mark_ok(const char *id)
{
    src_health_t *h = health_find(src_pfx(id), false);
    if (h) {
        h->fails = 0;
    }
}

/* 源是否不健康（窗口内失败次数达阈值） */
static bool health_is_bad(const char *id)
{
    src_health_t *h = health_find(src_pfx(id), false);
    if (h == NULL || h->fails < SRC_FAIL_THRESHOLD) {
        return false;
    }
    return esp_timer_get_time() - h->window_start_us <= SRC_HEALTH_WINDOW_US;
}

/* ---------------- 播放事件回调（esp_player 上下文） ---------------- */

static void player_event_cb(esp_player_event_type_t event, void *data, void *ctx)
{
    (void)data;
    (void)ctx;
    switch (event) {
        case ESP_PLAYER_EVENT_PLAYED:
            s_state = PST_PLAYING;
            send_state();
            break;
        case ESP_PLAYER_EVENT_PAUSED:
            s_state = PST_PAUSED;
            send_state();
            break;
        case ESP_PLAYER_EVENT_FINISHED:
            ESP_LOGI(TAG, "播放完成: %s", s_cur_id);
            s_state = PST_IDLE;
            {
                char buf[96];
                snprintf(buf, sizeof(buf), "{\"evt\":\"finished\",\"id\":\"%s\"}", s_cur_id);
                ws_server_send_json(buf);
            }
            send_state();
            break;
        case ESP_PLAYER_EVENT_ERROR:
            ESP_LOGW(TAG, "播放错误: %s", s_cur_id);
            s_state = PST_IDLE;
            send_error("播放错误");
            send_state();
            break;
        case ESP_PLAYER_EVENT_STOPPED:
            s_state = PST_IDLE;
            send_state();
            break;
        default:
            break;
    }
}

/* ---------------- 单候选播放 ---------------- */

/* 拉流准备（阻塞：302 预解析最长 20s）。成功返回 ESP_OK，失败原因写入 reason */
static esp_err_t do_prepare(const char *song_id, char *reason, int reason_sz)
{
    char url[512];
    snprintf(url, sizeof(url),
             "http://%s:%d/rest/stream.view?id=%s&u=%s&p=%s&v=1.16.1&c=esp32",
             LX_SERVER_IP, LX_SERVER_PORT, song_id, LX_USER, LX_PASSWORD);

    /* lx-server 对未缓存曲目返回 302，播放器内部 HTTP 不跟随重定向，
     * 这里先手动解析出最终 CDN 直连地址再播放 */
    char final_url[512];
    char detail[64] = "";
    esp_err_t err = audio_player_resolve_url(url, final_url, sizeof(final_url),
                                             detail, sizeof(detail));
    if (err != ESP_OK) {
        snprintf(reason, reason_sz, "解析失败(%s)", detail[0] ? detail : "未知");
        return err;
    }

    /* 只接受真正的 MP3 直链：酷我等源元数据标 MP3 但 CDN 实际给
     * flac/mflac/无后缀流，会导致解码停滞，此处按"无 MP3 音源"处理 */
    if (!url_is_mp3(final_url)) {
        ESP_LOGW(TAG, "CDN 直链非 MP3，拒绝播放: %.60s", final_url);
        snprintf(reason, reason_sz, "非 MP3 音源");
        return ESP_ERR_NOT_SUPPORTED;
    }
    ESP_LOGI(TAG, "开始拉流: %.80s", final_url);
    return audio_player_play_url(s_player, final_url);
}

/* 等待真正进入 PLAYING（play_url 成功不代表能播）。超时/出错返回 false */
static bool wait_playing(char *reason, int reason_sz)
{
    int64_t deadline = esp_timer_get_time() + CONFIRM_PLAY_MS * 1000LL;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(250));
        if (s_pending_play) {
            return false;   /* 新请求插入 */
        }
        if (s_state == PST_PLAYING) {
            return true;
        }
        if (s_state == PST_IDLE) {
            /* esp_player 报了 ERROR 且回调已发 error 事件 */
            snprintf(reason, reason_sz, "拉流失败");
            audio_player_stop(s_player);   /* 兜底清理残流 */
            return false;
        }
    }
    audio_player_stop(s_player);
    snprintf(reason, reason_sz, "启动超时");
    return false;
}

/* 播放监控循环：处理暂停/恢复/停止/切歌 + 停滞看门狗 + 进度上报 */
static void monitor_playback(void)
{
    uint64_t last_pos = 0;
    int stall_ticks = 0;       /* 250ms/tick，30s = 120 */
    int last_sent_sec = -1;
    int64_t last_sys_us = esp_timer_get_time();

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(250));

        /* 新歌请求：停当前，回到外层处理新序列 */
        if (s_pending_play) {
            audio_player_stop(s_player);
            break;
        }
        if (s_req_stop) {
            s_req_stop = false;
            audio_player_stop(s_player);
            s_state = PST_IDLE;
            send_state();
            break;
        }
        if (s_req_pause && s_state == PST_PLAYING) {
            s_req_pause = false;
            audio_player_pause(s_player);
            continue;
        }
        if (s_req_resume && s_state == PST_PAUSED) {
            s_req_resume = false;
            audio_player_resume(s_player);
            continue;
        }

        /* 状态离开播放态（FINISHED/ERROR 已由事件回调上报）即结束监控 */
        if (s_state != PST_PLAYING && s_state != PST_PAUSED &&
            s_state != PST_PREPARING) {
            break;
        }
        if (s_state != PST_PLAYING) {
            continue;   /* PAUSED：暂停期间不发进度 */
        }

        /* 停滞看门狗 + 进度上报 */
        uint64_t pos = 0, dur = 0;
        audio_player_get_position(s_player, &pos, &dur);
        if (pos > last_pos + 200) {
            last_pos = pos;
            stall_ticks = 0;
        } else if (++stall_ticks >= 120) {
            ESP_LOGW(TAG, "进度停滞 30 秒，强制跳过");
            audio_player_stop(s_player);
            send_error("进度停滞，播放失败");
            s_state = PST_IDLE;
            send_state();
            break;
        }
        int sec = (int)(pos / 1000);
        if (sec != last_sent_sec) {
            last_sent_sec = sec;
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "{\"evt\":\"progress\",\"id\":\"%s\",\"pos\":%llu,\"dur\":%llu}",
                     s_cur_id, (unsigned long long)pos, (unsigned long long)dur);
            ws_server_send_json(buf);
        }

        /* 系统负载（每 5 秒） */
        int64_t now = esp_timer_get_time();
        if (now - last_sys_us >= 5 * 1000000) {
            last_sys_us = now;
            char buf[160];
            int cpu = player_ctrl_get_cpu_load();
            int rssi = 0;
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
            }
            snprintf(buf, sizeof(buf),
                     "{\"evt\":\"sys\",\"cpu\":%d,\"heap\":%u,\"rssi\":%d}",
                     cpu, (unsigned)esp_get_free_heap_size(), rssi);
            ws_server_send_json(buf);
        }
    }
}

/* ---------------- 候选源链播放 ---------------- */

/* 依次尝试候选编号：解析自动重试 + 不健康源跳过（兜底仍尝试） */
static void play_sequence(void)
{
    bool started = false;
    char last_reason[96] = "未知失败";

    for (int i = 0; i < s_pending_ncand; i++) {
        if (s_pending_play && i > 0) {
            break;   /* 有新的播放请求插入，终止本序列（外层会处理） */
        }
        strlcpy(s_cur_id, s_pending_ids[i], sizeof(s_cur_id));

        /* 源健康度：不健康的源优先跳过（后面还有别的候选时） */
        if (health_is_bad(s_cur_id)) {
            bool has_healthy_after = false;
            for (int j = i + 1; j < s_pending_ncand; j++) {
                if (!health_is_bad(s_pending_ids[j])) {
                    has_healthy_after = true;
                    break;
                }
            }
            if (has_healthy_after) {
                char logbuf[160];
                snprintf(logbuf, sizeof(logbuf), "跳过 %s（源近期不稳定，自动换源）", s_cur_id);
                ESP_LOGW(TAG, "%s", logbuf);
                send_log(logbuf);
                continue;
            }
        }

        ESP_LOGI(TAG, "尝试候选 %d/%d: %s", i + 1, s_pending_ncand, s_cur_id);
        s_state = PST_PREPARING;
        send_state();

        /* 解析 + 启动，失败自动重试一次（上游波动具有间歇性） */
        char reason[96] = "";
        bool launched = false;
        for (int attempt = 0; attempt < RESOLVE_RETRY && !s_pending_play; attempt++) {
            if (attempt > 0) {
                ESP_LOGW(TAG, "解析失败，1 秒后重试(%d/%d): %s",
                         attempt + 1, RESOLVE_RETRY, s_cur_id);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (do_prepare(s_cur_id, reason, sizeof(reason)) == ESP_OK) {
                launched = true;
                break;
            }
            ESP_LOGW(TAG, "候选失败: %s (%s)", s_cur_id, reason);
        }
        if (!launched) {
            strlcpy(last_reason, reason, sizeof(last_reason));
            health_mark_fail(s_cur_id);
            continue;
        }

        /* 确认真正进入 PLAYING */
        if (wait_playing(reason, sizeof(reason))) {
            health_mark_ok(s_cur_id);
            started = true;
            monitor_playback();
            break;   /* 歌曲结束（播完/停止/出错已上报），序列完成 */
        }
        strlcpy(last_reason, reason, sizeof(last_reason));
        health_mark_fail(s_cur_id);
    }

    if (!started) {
        s_state = PST_IDLE;
        send_state();
        send_error(last_reason);
        ESP_LOGW(TAG, "全部候选源失败: %s", last_reason);
    }
}

/* 控制任务主体 */
static void player_task(void *arg)
{
    (void)arg;
    while (1) {
        /* IDLE 待命：等 web 下发 play（通知唤醒），期间每 5s 上报 sys */
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
            char buf[160];
            int cpu = player_ctrl_get_cpu_load();
            int rssi = 0;
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                rssi = ap.rssi;
            }
            snprintf(buf, sizeof(buf),
                     "{\"evt\":\"sys\",\"cpu\":%d,\"heap\":%u,\"rssi\":%d}",
                     cpu, (unsigned)esp_get_free_heap_size(), rssi);
            ws_server_send_json(buf);
            continue;
        }

        while (s_pending_play) {
            s_pending_play = false;
            play_sequence();
        }
    }
}

/* ---------------- CPU 占用率（空闲任务运行时计数反推） ---------------- */

int player_ctrl_get_cpu_load(void)
{
#if configGENERATE_RUN_TIME_STATS
    static uint32_t s_last_idle0 = 0, s_last_idle1 = 0;
    static int64_t  s_last_us = 0;
    static int      s_last_cpu = 0;

    int64_t now = esp_timer_get_time();
    uint32_t idle0 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(0);
#if portNUM_PROCESSORS > 1
    uint32_t idle1 = (uint32_t)ulTaskGetIdleRunTimeCounterForCore(1);
#else
    uint32_t idle1 = 0;
#endif
    if (s_last_us != 0) {
        int64_t dt_us = now - s_last_us;
        uint32_t idle_delta = (idle0 - s_last_idle0) + (idle1 - s_last_idle1);
        /* 总运行时间 ≈ dt * 核数（run time counter 单位为 us） */
        int64_t total = dt_us * portNUM_PROCESSORS;
        if (total > 0) {
            s_last_cpu = (int)(100 - (idle_delta * 100) / total);
            if (s_last_cpu < 0) {
                s_last_cpu = 0;
            }
            if (s_last_cpu > 100) {
                s_last_cpu = 100;
            }
        }
    }
    s_last_idle0 = idle0;
    s_last_idle1 = idle1;
    s_last_us = now;
    return s_last_cpu;
#else
    return -1;   /* 未启用 FreeRTOS 运行时统计 */
#endif
}

/* ---------------- web 指令处理（ws_server 上下文调用） ---------------- */

/* 从 JSON 文本中提取 "key":"value" 的字符串值 / "key":数值 */
static void json_get_str(const char *text, const char *key, char *out, int out_sz)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(text, pat);
    out[0] = '\0';
    if (!p) {
        return;
    }
    p = strchr(p + strlen(pat), '"');   /* 值的起始引号 */
    if (!p) {
        return;
    }
    p++;
    int n = 0;
    while (p[n] && p[n] != '"' && n < out_sz - 1) {
        out[n] = p[n];
        n++;
    }
    out[n] = '\0';
}

static int json_get_int(const char *text, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(text, pat);
    if (!p) {
        return 0;
    }
    p = strchr(p + strlen(pat), ':');
    return p ? atoi(p + 1) : 0;
}

void player_ctrl_handle_message(const char *text)
{
    char cmd[16] = {0};
    char id[64] = {0};
    json_get_str(text, "cmd", cmd, sizeof(cmd));

    if (strcmp(cmd, "play") == 0) {
        json_get_str(text, "id", id, sizeof(id));
        if (id[0] == '\0') {
            send_error("缺少歌曲编号");
            return;
        }
        /* 主编号 + alt 备选编号数组（同一首歌的其他音源） */
        memset(s_pending_ids, 0, sizeof(s_pending_ids));
        s_pending_ncand = 0;
        strlcpy(s_pending_ids[s_pending_ncand++], id, sizeof(s_pending_ids[0]));
        const char *alt = strstr(text, "\"alt\"");
        if (alt) {
            const char *arr = strchr(alt, '[');
            if (arr) {
                const char *p = arr + 1;
                while (s_pending_ncand < MAX_CANDIDATES && p) {
                    p = strchr(p, '"');
                    if (!p) {
                        break;
                    }
                    p++;
                    int n = 0;
                    char tmp[64] = {0};
                    while (p[n] && p[n] != '"' && n < (int)sizeof(tmp) - 1) {
                        tmp[n] = p[n];
                        n++;
                    }
                    if (n > 0 && strncmp(tmp, id, sizeof(tmp)) != 0) {
                        strlcpy(s_pending_ids[s_pending_ncand++], tmp,
                                sizeof(s_pending_ids[0]));
                    }
                    p += n + 1;
                }
            }
        }
        s_pending_play = true;
        xTaskNotifyGive(s_task);
    } else if (strcmp(cmd, "stop") == 0) {
        s_req_stop = true;
        s_pending_play = false;
        s_pending_ncand = 0;
    } else if (strcmp(cmd, "pause") == 0) {
        s_req_pause = true;
    } else if (strcmp(cmd, "resume") == 0) {
        s_req_resume = true;
    } else if (strcmp(cmd, "volume") == 0) {
        int value = json_get_int(text, "value");
        if (value < 0) {
            value = 0;
        }
        if (value > 100) {
            value = 100;
        }
        if (audio_player_set_volume(s_player, value) == ESP_OK) {
            ESP_LOGI(TAG, "音量设置为 %d%%（已存 NVS）", value);
            char buf[48];
            snprintf(buf, sizeof(buf), "{\"evt\":\"volume\",\"value\":%d}", value);
            ws_server_send_json(buf);
        }
    } else if (strcmp(cmd, "hello") == 0) {
        /* web 端接入后主动触发：应答 hello 帧同步音量/状态 */
        char buf[160];
        player_ctrl_build_hello(buf, sizeof(buf));
        ws_server_send_json(buf);
        ESP_LOGI(TAG, "web 客户端已接入");
    } else if (strcmp(cmd, "ping") == 0) {
        ws_server_send_json("{\"evt\":\"pong\"}");
    } else {
        ESP_LOGW(TAG, "未知指令: %s", text);
    }
}

void player_ctrl_on_client(void)
{
    char buf[160];
    player_ctrl_build_hello(buf, sizeof(buf));
    ws_server_send_json(buf);
    ESP_LOGI(TAG, "web 客户端已接入");
}

int player_ctrl_build_hello(char *buf, int size)
{
    int vol = 0;
    audio_player_get_volume(s_player, &vol);
    return snprintf(buf, size,
                    "{\"evt\":\"hello\",\"volume\":%d,\"state\":\"%s\",\"id\":\"%s\"}",
                    vol, state_name(s_state), s_cur_id);
}

/* ---------------- 对外接口 ---------------- */

esp_err_t player_ctrl_init(audio_player_t *player)
{
    if (player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_player = player;
    return audio_player_set_event_cb(s_player, player_event_cb, NULL);
}

esp_err_t player_ctrl_start(void)
{
    if (s_player == NULL || s_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(player_task, CTRL_TASK_NAME,
                    CTRL_TASK_STACK_SIZE / sizeof(StackType_t),
                    NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "创建控制任务失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "播放控制器就绪，IDLE 待命（服务器 %s:%d）",
             LX_SERVER_IP, LX_SERVER_PORT);
    return ESP_OK;
}
