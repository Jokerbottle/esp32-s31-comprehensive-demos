/*
 * audio_player.h - 基于 esp_player 的音频播放封装（数据源支持本地文件或 HTTP 流）
 *
 * 该模块将解码后的 PCM 通过 BSP 的 ES8311 功放输出，供应用层以极简接口播放音频。
 */
#pragma once

#include "esp_err.h"
#include "esp_player_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_player_s audio_player_t;

/**
 * @brief 播放事件回调
 * @param event  事件类型（PLAYED / PAUSED / STOPPED / FINISHED / ERROR ...）
 * @param data   事件负载（无则 NULL）
 * @param ctx    用户上下文
 */
typedef void (*audio_player_event_cb_t)(esp_player_event_type_t event, void *data, void *ctx);

/**
 * @brief 初始化音频渲染管线并创建播放器实例（内部会初始化并打开 ES8311 功放）
 */
esp_err_t audio_player_init(audio_player_t **player);

/**
 * @brief 停止播放并释放所有播放器资源
 */
esp_err_t audio_player_deinit(audio_player_t *player);

/**
 * @brief 播放一个 HTTP 音频流地址（如 lx-server 的 Subsonic stream 接口），会先停止当前播放
 * @param player 播放器实例
 * @param url    音频流 URL（http://...）
 */
esp_err_t audio_player_play_url(audio_player_t *player, const char *url);

/**
 * @brief 播放本地文件（绝对 VFS 路径），会先停止当前播放
 */
esp_err_t audio_player_play_file(audio_player_t *player, const char *path);

/**
 * @brief 停止播放（无播放时调用也安全）
 */
esp_err_t audio_player_stop(audio_player_t *player);

/**
 * @brief 暂停 / 恢复播放
 */
esp_err_t audio_player_pause(audio_player_t *player);
esp_err_t audio_player_resume(audio_player_t *player);
esp_err_t audio_player_toggle_pause(audio_player_t *player);

/**
 * @brief 查询当前播放器状态
 */
esp_err_t audio_player_get_state(audio_player_t *player, esp_player_state_t *state);

/**
 * @brief 查询当前播放进度与总时长（单位：毫秒）
 */
esp_err_t audio_player_get_position(audio_player_t *player, uint64_t *pos_ms, uint64_t *dur_ms);

/**
 * @brief 注册播放器事件回调
 */
esp_err_t audio_player_set_event_cb(audio_player_t *player, audio_player_event_cb_t cb, void *ctx);

/**
 * @brief 设置输出音量并写入 NVS 持久化（下次上电自动恢复）
 *
 * @param[in] player 播放器实例
 * @param[in] vol    音量值 0~100
 */
esp_err_t audio_player_set_volume(audio_player_t *player, int vol);

/**
 * @brief 读取当前输出音量（0~100）
 */
esp_err_t audio_player_get_volume(audio_player_t *player, int *vol);

/**
 * @brief 预解析播放地址：手动跟随 302 重定向，返回最终可直连的 CDN URL
 *
 * lx-server 对未缓存曲目返回 302（重定向到音乐 CDN），而播放器内部 HTTP 不跟随
 * 重定向。本函数用 esp_http_client 逐跳解析（最多 5 跳），返回 200 的最终地址。
 * 支持 HTTPS CDN（启用证书 bundle）。
 *
 * @param[in]  url_in     原始 URL
 * @param[out] url_out    输出最终 URL（调用者提供缓冲区）
 * @param[in]  out_size   url_out 缓冲区大小
 * @param[out] detail     失败原因详情（如 "hop0 超时" / "hop1 HTTP403"），可为 NULL
 * @param[in]  detail_sz  detail 缓冲区大小
 */
esp_err_t audio_player_resolve_url(const char *url_in, char *url_out, int out_size,
                                   char *detail, int detail_size);

#ifdef __cplusplus
}
#endif
