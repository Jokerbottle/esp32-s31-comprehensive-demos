/* ESP32-S31 综合例程 - 应用入口（web 远程控制版）
 *
 * 职责（仅初始化，之后全系统 IDLE 待命，等 web 端经 WebSocket 下发指令）：
 *  1) NVS + ESP-TOUCH 智能配网（NVS 有凭据直连，无凭据进入配网）；
 *  2) 初始化音频播放器（ES8311 + esp_player，音量从 NVS 恢复）；
 *  3) 启动播放控制任务（player_ctrl）与 WebSocket 服务器（ws_server）。
 *
 * 播放由 web 页面控制：搜索歌曲/歌单/音量/播放控制等经
 *   ws://<board-ip>:8080/ws 下发（协议见 player_ctrl.h）。
 * s31 从 lx-server 拉流播放（参数在 menuconfig "LX-Server Music Player
 * Configuration" 配置）。
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_provision.h"
#include "audio_player.h"
#include "player_ctrl.h"
#include "ws_server.h"

static const char *TAG = "app_main";

void app_main(void)
{
    /* ---------- 1. 智能配网（NVS 直连优先，否则 ESP-TOUCH 配网） ---------- */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    printf("=== ESP32-S31 web 远程音乐播放器 ===\r\n");

    ESP_ERROR_CHECK(wifi_provision_init());
    ESP_ERROR_CHECK(wifi_provision_start());

    /* 等待 WiFi 真正拿到 IP（web 与 lx-server 均在同一局域网） */
    ESP_LOGI(TAG, "等待 WiFi 连接...");
    if (wifi_provision_wait_connected(pdMS_TO_TICKS(30000)) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 连接超时");
        return;
    }
    ESP_LOGI(TAG, "WiFi 已连接");

    /* ---------- 2. 音频播放器（含 ES8311，音量从 NVS 恢复） ---------- */
    audio_player_t *player = NULL;
    ESP_ERROR_CHECK(audio_player_init(&player));
    ESP_ERROR_CHECK(player_ctrl_init(player));

    /* ---------- 3. 启动播放控制任务与 WebSocket 服务器 ---------- */
    ESP_ERROR_CHECK(player_ctrl_start());
    ESP_ERROR_CHECK(ws_server_start());

    /* ---------- IDLE：等待 web 端 WebSocket 指令 ---------- */
    ESP_LOGI(TAG, "初始化完成，IDLE 待命（等 web 端连接 ws://<ip>:8080/ws）");
}
