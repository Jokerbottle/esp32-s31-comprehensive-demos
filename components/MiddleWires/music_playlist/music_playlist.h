/*
 * music_playlist.h - 五歌手轮转自动切歌播放控制器
 *
 * 职责：
 *  - 从 lx-server（Subsonic API）按歌手轮转搜索 MP3 音源并播放；
 *  - 播放完毕（FINISHED）自动切到下一位歌手，播放出错/进度停滞自动跳曲重试；
 *  - 仅接受 CDN 直链为 .mp3 的音源（非 MP3 一律放弃），保证歌单永不卡死。
 *
 * 服务器连接参数（IP/端口/账号）来自 menuconfig：
 *  "LX-Server Music Player Configuration"（main/Kconfig.projbuild）。
 *
 * 用法：
 *      WiFi 连接成功后调用一次 music_playlist_start() 即可，
 *      内部创建后台任务负责搜索/播放/切歌/看门狗，无需应用层干预。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化播放器（含 ES8311 功放）并启动后台轮播任务
 *
 * 内部完成：
 *  1. audio_player_init()（BSP ES8311 + esp_player 管线，音量由 BSP 宏决定）；
 *  2. 创建 "music_ctrl" 后台任务（16KB 栈，搜索/302 预解析含 TLS 握手），
 *     任务内完成首曲搜索播放、切歌状态机与停滞看门狗。
 *
 * @return
 *      - ESP_OK      成功启动，音乐将自动开始播放
 *      - 其他        初始化失败（音频硬件/内存等）
 */
esp_err_t music_playlist_start(void);

#ifdef __cplusplus
}
#endif
