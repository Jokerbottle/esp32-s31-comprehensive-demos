/* ESP32-S31 综合例程 - 应用入口
 *
 * 本例程演示基于 ESP-TOUCH 的智能配网：
 *   - 上电后先初始化 NVS；
 *   - 通过 MiddleWires 提供的 wifi_provision 模块完成配网；
 *   - 若 NVS 已保存 WiFi 凭据则直连，否则进入手机配网模式。
 */

#include <stdio.h>
#include "nvs_flash.h"
#include "wifi_provision.h"

void app_main(void)
{
    /* 1. 初始化 NVS（WiFi 凭据持久化依赖此功能） */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* NVS 分区异常时擦除后重试 */
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    printf("=== ESP32-S31 智能配网例程启动 ===\r\n");

    /* 2. 初始化 WiFi 配网运行环境 */
    ESP_ERROR_CHECK(wifi_provision_init());

    /* 3. 启动配网：优先使用 NVS 凭据直连，否则进入 ESP-TOUCH 配网 */
    ESP_ERROR_CHECK(wifi_provision_start());
}
