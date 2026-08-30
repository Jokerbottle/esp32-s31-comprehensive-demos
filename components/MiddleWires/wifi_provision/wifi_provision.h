#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 WiFi 配网所需的运行环境
 *
 * 该函数会依次完成以下工作：
 *  1. 初始化 TCP/IP 协议栈（esp_netif）；
 *  2. 创建默认事件循环；
 *  3. 创建 STA 默认网络接口；
 *  4. 初始化 WiFi 并注册事件回调。
 *
 * 注意：调用本函数前，必须由 app_main 先完成 NVS 初始化（nvs_flash_init）。
 *
 * @return esp_err_t ESP_OK 表示初始化成功
 */
esp_err_t wifi_provision_init(void);

/**
 * @brief 启动配网流程
 *
 * 内部逻辑：
 *  - 优先读取 NVS 中命名空间 "wifi_prov" 下保存的 WiFi 凭据（ssid / pass）；
 *  - 若凭据完整，则直接使用该凭据连接 WiFi（直连模式）；
 *  - 若凭据缺失，则进入 ESP-TOUCH 智能配网模式，等待手机端发送 ssid / password。
 *
 * @return esp_err_t ESP_OK 表示启动成功
 */
esp_err_t wifi_provision_start(void);

/**
 * @brief 阻塞等待 WiFi 连接成功（获取到 IP）
 *
 * 在需要访问网络（如下载音频流）前调用，确保链路已就绪。
 *
 * @param[in] timeout_ticks 最大等待时间（FreeRTOS tick），可用 portMAX_DELAY 永久等待
 * @return esp_err_t ESP_OK 表示已连接；超时返回 ESP_ERR_TIMEOUT
 */
esp_err_t wifi_provision_wait_connected(TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
