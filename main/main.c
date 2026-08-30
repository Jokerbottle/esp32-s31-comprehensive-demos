/* ESP32-S31 综合例程 - 应用入口
 *
 * 职责（仅初始化，播放控制全部在 MiddleWires/music_playlist 后台任务中）：
 *  1) NVS + ESP-TOUCH 智能配网（NVS 有凭据直连，无凭据进入配网）；
 *  2) WiFi 就绪后启动音乐轮播（music_playlist_start），其余全自动：
 *     五位歌手（邓紫棋/周杰伦/薛之谦/林俊杰/陈奕迅）轮转搜索 MP3 并连播，
 *     播完自动切歌、出错/停滞自动跳曲。
 *
 * lx-server 连接参数（IP/端口/账号）在 menuconfig 的
 * "LX-Server Music Player Configuration" 中配置（main/Kconfig.projbuild）。
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "wifi_provision.h"
#include "music_playlist.h"

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

    printf("=== ESP32-S31 网络音乐播放器启动 ===\r\n");

    ESP_ERROR_CHECK(wifi_provision_init());
    ESP_ERROR_CHECK(wifi_provision_start());

    /* 等待 WiFi 真正拿到 IP（lx-server 与开发板需在同一路由器下） */
    ESP_LOGI(TAG, "等待 WiFi 连接...");
    if (wifi_provision_wait_connected(pdMS_TO_TICKS(30000)) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 连接超时");
        return;
    }
    ESP_LOGI(TAG, "WiFi 已连接");

    /* ---------- 2. 启动音乐轮播（后台任务接管播放控制） ---------- */
    ESP_ERROR_CHECK(music_playlist_start());
}
