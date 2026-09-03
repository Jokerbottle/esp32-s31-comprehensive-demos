/*
 * audio_player.c - 基于 esp_player 的音频播放封装
 *
 * 数据流：HTTP/文件 -> esp_player 提取器/解码器（gmf）-> esp_audio_render
 *        -> audio_render_writer() -> BSP ES8311 功放（I2S/DAC/PA）
 *
 * 仅依赖 BSP 的 ES8311 接口，不关心底层 I2C/I2S/Codec 细节。
 */
#include "audio_player.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

#include "es8311_audio.h"
#include "media_lib_adapter.h"
#include "esp_extractor_defaults.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_render.h"
#include "esp_gmf_pool.h"
#include "esp_gmf_ch_cvt.h"
#include "esp_gmf_bit_cvt.h"
#include "esp_gmf_rate_cvt.h"
#include "esp_gmf_alc.h"
#include "esp_player.h"
#include "esp_player_advance.h"

#define AUDIO_PLAYER_STREAM_NUM   1
/* 渲染管线固定输出格式。所有音源（任意采样率/通道/位深）都会被重采样/重帧到该格式，
 * 须与 BSP ES8311 模块的输出格式（44100 / 16bit / stereo）一致。 */
#define AUDIO_OUT_SAMPLE_RATE     44100
#define AUDIO_OUT_BITS_PER_SAMPLE 16
#define AUDIO_OUT_CHANNELS        2

static const char *TAG = "audio_player";

static void volume_load(void);

/* 不透明播放器实例，持有 ESP-GMF 各句柄 */
struct audio_player_s {
    esp_player_handle_t        player;   /* 高层 esp_player 状态机 */
    esp_audio_render_handle_t   render;   /* 音频渲染管线（extractor->decoder->renderer） */
    esp_gmf_pool_handle_t      pool;     /* GMF 元素池：通道/位深/采样率转换 + ALC */
    audio_player_event_cb_t    event_cb; /* 可选用户事件回调 */
    void                      *event_ctx;
};

/* 最终输出：渲染管线以固定格式的 PCM 调用本函数，我们直接写入板载 ES8311 Codec */
static int audio_render_writer(uint8_t *pcm_data, uint32_t pcm_size, void *ctx)
{
    (void)ctx;
    return bsp_es8311_write(pcm_data, (int)pcm_size);
}

/* 注册渲染管线所需的 GMF 元素池：通道/位深/采样率转换 + ALC（自动电平控制）。
 * 这些元素让管线可以把任意音源格式适配为上面的 AUDIO_OUT_* 固定输出格式。 */
static esp_err_t register_render_pool(esp_gmf_pool_handle_t *out_pool)
{
    *out_pool = NULL;
    if (esp_gmf_pool_init(out_pool) != ESP_GMF_ERR_OK) {
        ESP_LOGE(TAG, "gmf pool init failed");
        return ESP_FAIL;
    }
    esp_gmf_element_handle_t el = NULL;

    esp_ae_ch_cvt_cfg_t ch_cfg = DEFAULT_ESP_GMF_CH_CVT_CONFIG();
    if (esp_gmf_ch_cvt_init(&ch_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(*out_pool, el, NULL);
    }
    esp_ae_bit_cvt_cfg_t bit_cfg = DEFAULT_ESP_GMF_BIT_CVT_CONFIG();
    if (esp_gmf_bit_cvt_init(&bit_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(*out_pool, el, NULL);
    }
    esp_ae_rate_cvt_cfg_t rate_cfg = DEFAULT_ESP_GMF_RATE_CVT_CONFIG();
    if (esp_gmf_rate_cvt_init(&rate_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(*out_pool, el, NULL);
    }
    esp_ae_alc_cfg_t alc_cfg = DEFAULT_ESP_GMF_ALC_CONFIG();
    if (esp_gmf_alc_init(&alc_cfg, &el) == ESP_GMF_ERR_OK) {
        esp_gmf_pool_register_element(*out_pool, el, NULL);
    }
    return ESP_OK;
}

/* 将 esp_player 事件桥接到用户回调 */
static esp_player_err_t player_event_cb(esp_player_event_msg_t *msg, void *ctx)
{
    audio_player_t *ap = (audio_player_t *)ctx;
    if (ap == NULL) {
        return ESP_PLAYER_ERR_INVALID_ARG;
    }
    if (ap->event_cb) {
        ap->event_cb(msg->event_type, msg->data, ap->event_ctx);
    }
    return ESP_PLAYER_ERR_OK;
}

/* 注册 ESP-GMF 默认媒体库：VFS 适配器、提取器（wav/mp3/flac）、解码器（AAC/MP3/FLAC/PCM...） */
static void register_media_defaults(void)
{
    media_lib_add_default_adapter();
    esp_extractor_register_default();
    esp_audio_dec_register_default();
}

/* 与 register_media_defaults() 对应的反注册 */
static void unregister_media_defaults(void)
{
    esp_audio_dec_unregister_default();
    esp_extractor_unregister_default();
}

/* 创建播放器。同时初始化并打开板载 ES8311 音频硬件，使通路在播放前就绪 */
esp_err_t audio_player_init(audio_player_t **out_player)
{
    audio_player_t *ap = NULL;
    if (out_player == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_player = NULL;

    ESP_RETURN_ON_ERROR(bsp_es8311_init(), TAG, "bsp_es8311_init failed");

    ap = (audio_player_t *)calloc(1, sizeof(audio_player_t));
    if (ap == NULL) {
        ESP_LOGE(TAG, "no mem");
        return ESP_ERR_NO_MEM;
    }

    register_media_defaults();

    if (register_render_pool(&ap->pool) != ESP_OK) {
        ESP_LOGE(TAG, "register_render_pool failed");
        goto fail;
    }

    esp_audio_render_cfg_t render_cfg = {
        .max_stream_num = AUDIO_PLAYER_STREAM_NUM,
        .out_writer = audio_render_writer,
        .out_ctx = NULL,
        .out_sample_info = {
            .sample_rate = AUDIO_OUT_SAMPLE_RATE,
            .bits_per_sample = AUDIO_OUT_BITS_PER_SAMPLE,
            .channel = AUDIO_OUT_CHANNELS,
        },
        .pool = ap->pool,
        .process_period = 20,
        .process_buf_align = 0,
    };
    if (esp_audio_render_create(&render_cfg, &ap->render) != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_render_create failed");
        goto fail;
    }

    esp_audio_render_stream_handle_t stream = NULL;
    if (esp_audio_render_stream_get(ap->render, ESP_AUDIO_RENDER_FIRST_STREAM, &stream)
        != ESP_AUDIO_RENDER_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_render_stream_get failed");
        goto fail;
    }

    esp_player_config_t player_cfg = ESP_PLAYER_CONFIG_DEFAULT();
    player_cfg.audio_render_hd = stream;
    if (esp_player_init(&player_cfg, &ap->player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "esp_player_init failed");
        goto fail;
    }

    /* 放宽网络缓冲门限：FLAC 等需要 seek 的容器在 HTTP 非 seekable 源上
     * 缓冲时长估计可能为 0，导致启动缓冲门永远不打开、解码器卡在 ready 事件。
     * 这里把启动/重缓冲阈值降到很低，http 预读缓冲加大，避免死锁。 */
    esp_player_buffer_config_t buf_cfg = {
        .extractor_pool_size   = 0,
        .http_read_buf_size    = 65536,
        .prebuffer_resume_ms   = 50,
        .rebuffer_enter_ms     = 10,
        .rebuffer_resume_ms    = 50,
        .rebuffer_grace_ms     = 2000,
    };
    esp_player_set_buffer_config(ap->player, &buf_cfg);

    if (esp_player_set_event_cb(ap->player, player_event_cb, ap) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "esp_player_set_event_cb failed");
        goto fail;
    }

    if (bsp_es8311_open() != ESP_OK) {
        ESP_LOGE(TAG, "bsp_es8311_open failed");
        goto fail;
    }
    volume_load();

    *out_player = ap;
    ESP_LOGI(TAG, "audio player ready (%u Hz / %u bit / %u ch)",
             AUDIO_OUT_SAMPLE_RATE, AUDIO_OUT_BITS_PER_SAMPLE, AUDIO_OUT_CHANNELS);
    return ESP_OK;

fail:
    if (ap->player) {
        esp_player_deinit(ap->player);
    }
    if (ap->render) {
        esp_audio_render_destroy(ap->render);
    }
    if (ap->pool) {
        esp_gmf_pool_deinit(ap->pool);
    }
    unregister_media_defaults();
    bsp_es8311_close();
    free(ap);
    return ESP_FAIL;
}

/* 停止并释放 audio_player_init() 创建的一切资源 */
esp_err_t audio_player_deinit(audio_player_t *ap)
{
    if (ap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ap->player) {
        esp_player_set_event_cb(ap->player, NULL, NULL);
        esp_player_deinit(ap->player);
        ap->player = NULL;
    }
    if (ap->render) {
        esp_audio_render_destroy(ap->render);
        ap->render = NULL;
    }
    if (ap->pool) {
        esp_gmf_pool_deinit(ap->pool);
        ap->pool = NULL;
    }
    unregister_media_defaults();
    bsp_es8311_close();
    free(ap);
    return ESP_OK;
}

/* 设置数据源为 HTTP 音频流地址并播放（会先停止当前播放） */
esp_err_t audio_player_play_url(audio_player_t *ap, const char *url)
{
    if (ap == NULL || url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_state_t state;
    if (esp_player_get_state(ap->player, &state) == ESP_PLAYER_ERR_OK
        && (state == ESP_PLAYER_STATE_PLAYING || state == ESP_PLAYER_STATE_PAUSED
            || state == ESP_PLAYER_STATE_PREPARING)) {
        esp_player_stop(ap->player);
    }
    esp_player_data_src_t src = ESP_PLAYER_DATA_SRC(url, ESP_PLAYER_MASK_AUDIO);
    if (esp_player_set_data_src(ap->player, &src) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "set_data_src failed for %s", url);
        return ESP_FAIL;
    }
    if (esp_player_run(ap->player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "run failed for %s", url);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "play url %s", url);
    return ESP_OK;
}

/* 播放本地文件（绝对 VFS 路径），会先停止当前播放 */
esp_err_t audio_player_play_file(audio_player_t *ap, const char *path)
{
    if (ap == NULL || path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_state_t state;
    if (esp_player_get_state(ap->player, &state) == ESP_PLAYER_ERR_OK
        && (state == ESP_PLAYER_STATE_PLAYING || state == ESP_PLAYER_STATE_PAUSED
            || state == ESP_PLAYER_STATE_PREPARING)) {
        esp_player_stop(ap->player);
    }
    esp_player_data_src_t src = ESP_PLAYER_DATA_SRC(path, ESP_PLAYER_MASK_AUDIO);
    if (esp_player_set_data_src(ap->player, &src) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "set_data_src failed for %s", path);
        return ESP_FAIL;
    }
    if (esp_player_run(ap->player) != ESP_PLAYER_ERR_OK) {
        ESP_LOGE(TAG, "run failed for %s", path);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "play %s", path);
    return ESP_OK;
}

esp_err_t audio_player_stop(audio_player_t *ap)
{
    if (ap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_stop(ap->player);
    return ESP_OK;
}

esp_err_t audio_player_pause(audio_player_t *ap)
{
    if (ap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_pause(ap->player);
    return ESP_OK;
}

esp_err_t audio_player_resume(audio_player_t *ap)
{
    if (ap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_resume(ap->player);
    return ESP_OK;
}

/* 根据当前状态暂停或恢复 */
esp_err_t audio_player_toggle_pause(audio_player_t *ap)
{
    if (ap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_state_t state = ESP_PLAYER_STATE_IDLE;
    if (esp_player_get_state(ap->player, &state) != ESP_PLAYER_ERR_OK) {
        return ESP_FAIL;
    }
    if (state == ESP_PLAYER_STATE_PLAYING) {
        esp_player_pause(ap->player);
    } else if (state == ESP_PLAYER_STATE_PAUSED) {
        esp_player_resume(ap->player);
    }
    return ESP_OK;
}

esp_err_t audio_player_get_state(audio_player_t *ap, esp_player_state_t *state)
{
    if (ap == NULL || state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_get_state(ap->player, state);
    return ESP_OK;
}

/* 查询当前播放进度与总时长（毫秒） */
esp_err_t audio_player_get_position(audio_player_t *ap, uint64_t *pos_ms, uint64_t *dur_ms)
{
    if (ap == NULL || pos_ms == NULL || dur_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_player_get_play_time(ap->player, pos_ms);
    if (esp_player_get_duration(ap->player, dur_ms) != ESP_PLAYER_ERR_OK) {
        *dur_ms = 0;
    }
    return ESP_OK;
}

esp_err_t audio_player_set_event_cb(audio_player_t *ap, audio_player_event_cb_t cb, void *ctx)
{
    if (ap == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ap->event_cb = cb;
    ap->event_ctx = ctx;
    return ESP_OK;
}


/* ====================== 音量 NVS 持久化 ====================== */

#include "nvs.h"

#define VOL_NVS_NAMESPACE "audiocfg"
#define VOL_NVS_KEY       "vol"

/* 上电恢复上次保存的音量（audio_player_init 内部在 codec 打开后调用） */
static void volume_load(void)
{
    nvs_handle_t h;
    if (nvs_open(VOL_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t vol = 0;
    if (nvs_get_u8(h, VOL_NVS_KEY, &vol) == ESP_OK && vol <= 100) {
        bsp_es8311_set_volume((int)vol);
        ESP_LOGI(TAG, "恢复上电音量: %d%%", (int)vol);
    }
    nvs_close(h);
}

esp_err_t audio_player_set_volume(audio_player_t *ap, int vol)
{
    if (ap == NULL || vol < 0 || vol > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = bsp_es8311_set_volume(vol);
    if (err != ESP_OK) {
        return err;
    }
    /* 写入 NVS，下次上电自动恢复 */
    nvs_handle_t h;
    if (nvs_open(VOL_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, VOL_NVS_KEY, (uint8_t)vol);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

esp_err_t audio_player_get_volume(audio_player_t *ap, int *vol)
{
    if (ap == NULL || vol == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return bsp_es8311_get_volume(vol);
}

/* ====================== 302 重定向预解析 ====================== */

#include "esp_http_client.h"
#include "esp_crt_bundle.h"

/* 捕获 302 响应的 Location 头 */
typedef struct {
    char location[768];
} redirect_ctx_t;

static esp_err_t redirect_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        strcasecmp(evt->header_key, "Location") == 0 && evt->user_data) {
        redirect_ctx_t *ctx = (redirect_ctx_t *)evt->user_data;
        strlcpy(ctx->location, evt->header_value, sizeof(ctx->location));
    }
    return ESP_OK;
}

esp_err_t audio_player_resolve_url(const char *url_in, char *url_out, int out_size,
                                   char *detail, int detail_size)
{
    if (detail && detail_size > 0) {
        detail[0] = '\0';
    }
    if (!url_in || !url_out || out_size <= 0) {
        if (detail) snprintf(detail, detail_size, "参数错误");
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(url_out, url_in, out_size);

    char cur[768];
    strlcpy(cur, url_in, sizeof(cur));

    for (int hop = 0; hop < 5; hop++) {
        redirect_ctx_t ctx = { .location = "" };
        esp_http_client_config_t cfg = {
            .url = cur,
            .disable_auto_redirect = true,   /* 手动逐跳跟随，拿到最终直连地址 */
            .event_handler = redirect_event_cb,
            .user_data = &ctx,
            /* hop0 是 lx-server stream.view：其需向上游音源取播放地址，
             * 上游（如咪咕）慢时可能超过 10s，故放宽到 20s */
            .timeout_ms = 20000,
            .crt_bundle_attach = esp_crt_bundle_attach,  /* 支持 HTTPS CDN（咪咕等） */
            .buffer_size = 2048,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (!client) {
            ESP_LOGE(TAG, "resolve: client init failed (hop %d)", hop);
            if (detail) snprintf(detail, detail_size, "hop%d 初始化失败", hop);
            return ESP_FAIL;
        }
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "resolve: open failed (hop %d): %s", hop, esp_err_to_name(err));
            if (detail) {
                snprintf(detail, detail_size, "hop%d %s",
                         hop, err == ESP_ERR_TIMEOUT ? "超时" : esp_err_to_name(err));
            }
            esp_http_client_cleanup(client);
            return err;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        ESP_LOGI(TAG, "resolve hop %d: status=%d", hop, status);

        if (status == 200) {
            strlcpy(url_out, cur, out_size);  /* cur 即最终直连地址 */
            return ESP_OK;
        }
        if ((status == 301 || status == 302 || status == 307 || status == 308)
            && ctx.location[0] != '\0') {
            strlcpy(cur, ctx.location, sizeof(cur));
            continue;
        }
        ESP_LOGE(TAG, "resolve: unexpected status %d", status);
        if (detail) {
            if (status <= 0) {
                snprintf(detail, detail_size, "hop%d 上游无响应", hop);
            } else {
                snprintf(detail, detail_size, "hop%d HTTP%d", hop, status);
            }
        }
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "resolve: too many redirects");
    if (detail) snprintf(detail, detail_size, "重定向过多");
    return ESP_FAIL;
}
