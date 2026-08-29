/* ESP32-S31 智能配网（ESP-TOUCH）中间件
 *
 * 功能概述：
 *  本模块封装了 WiFi 配网逻辑，支持两种入网方式：
 *   1. 直连模式：若 NVS 中已保存有效的 WiFi 凭据，则直接读取并连接；
 *   2. 智能配网模式：若 NVS 中无凭据，则启动 ESP-TOUCH，由手机端下发 ssid/password。
 *
 * 配网成功后，会将 ssid / password 持久化到 NVS，供下次上电直连使用。
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "nvs_flash.h"
#include "wifi_provision.h"

/* ============================== 宏与常量定义 ============================== */

#define WIFI_PROV_NAMESPACE     "wifi_prov"    /* NVS 命名空间，用于隔离 WiFi 凭据数据 */
#define WIFI_PROV_KEY_SSID      "ssid"         /* NVS 中保存 WiFi 名的键 */
#define WIFI_PROV_KEY_PASS      "pass"         /* NVS 中保存 WiFi 密码的键 */
#define WIFI_PROV_SMARTCONFIG_TASK_STACK  4096 /* 智能配网任务栈大小 */

/* ============================== 静态变量 ============================== */

static const char *TAG = "wifi_provision";

/* 事件组：用于在主循环与事件回调之间同步 WiFi 连接 / 配网状态 */
static EventGroupHandle_t s_wifi_event_group;
static const int CONNECTED_BIT       = BIT0;  /* 已获取 IP，WiFi 连接成功 */
static const int ESPTOUCH_DONE_BIT  = BIT1;  /* ESP-TOUCH 配网流程结束 */

/* 运行时状态标记 */
static bool s_have_creds = false;        /* NVS 中是否已存在完整 WiFi 凭据 */
static bool s_from_smartconfig = false;  /* 本次连接是否来自智能配网 */

/* 智能配网获取到的 ssid / password 缓存，待连接成功后写入 NVS */
static uint8_t s_sc_ssid[33] = {0};
static uint8_t s_sc_password[65] = {0};

/* ============================== 内部函数声明 ============================== */

static void wifi_provision_event_handler(void *arg, esp_event_base_t event_base,
                                         int32_t event_id, void *event_data);
static void wifi_provision_smartconfig_task(void *arg);
static esp_err_t wifi_provision_read_nvs_creds(char *ssid, size_t ssid_len,
                                               char *pass, size_t pass_len);
static esp_err_t wifi_provision_save_nvs_creds(const char *ssid, const char *pass);

/* ============================== NVS 凭据读写 ============================== */

/**
 * @brief 从 NVS 读取已保存的 WiFi 凭据
 *
 * @param[out] ssid     用于存放 ssid 的缓冲区
 * @param[in]  ssid_len ssid 缓冲区长度（建议 >= 33）
 * @param[out] pass     用于存放 password 的缓冲区
 * @param[in]  pass_len password 缓冲区长度（建议 >= 65）
 * @return esp_err_t ESP_OK 表示成功读取到完整凭据
 */
static esp_err_t wifi_provision_read_nvs_creds(char *ssid, size_t ssid_len,
                                               char *pass, size_t pass_len)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_PROV_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS 中尚未保存 WiFi 凭据（命名空间 %s 不存在）",
                 WIFI_PROV_NAMESPACE);
        return err;
    }

    /* 先读取 ssid，再读取 pass，二者均存在才认为凭据完整 */
    size_t len = ssid_len;
    err = nvs_get_str(nvs_handle, WIFI_PROV_KEY_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = pass_len;
        err = nvs_get_str(nvs_handle, WIFI_PROV_KEY_PASS, pass, &len);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "NVS 中 WiFi 凭据不完整，将进入智能配网模式");
        return err;
    }

    ESP_LOGI(TAG, "已从 NVS 读取到 WiFi 凭据，准备直连：SSID=%s", ssid);
    return ESP_OK;
}

/**
 * @brief 将 WiFi 凭据写入 NVS 并持久化
 *
 * @param[in] ssid 待保存的 ssid
 * @param[in] pass 待保存的 password
 * @return esp_err_t ESP_OK 表示写入成功
 */
static esp_err_t wifi_provision_save_nvs_creds(const char *ssid, const char *pass)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(WIFI_PROV_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "打开 NVS 命名空间 %s 失败：%s",
                 WIFI_PROV_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs_handle, WIFI_PROV_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, WIFI_PROV_KEY_PASS, pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "写入 NVS 失败：%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "WiFi 凭据已保存至 NVS（命名空间 %s）", WIFI_PROV_NAMESPACE);
    return ESP_OK;
}

/* ============================== 事件回调 ============================== */

static void wifi_provision_event_handler(void *arg, esp_event_base_t event_base,
                                         int32_t event_id, void *event_data)
{
    /* WiFi 协议栈事件 */
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_have_creds) {
            /* 存在 NVS 凭据：直接发起连接 */
            ESP_LOGI(TAG, "使用 NVS 凭据直连 WiFi");
            esp_wifi_connect();
        } else {
            /* 无凭据：创建智能配网任务，等待手机端下发 */
            xTaskCreate(wifi_provision_smartconfig_task, "smartconfig_task",
                        WIFI_PROV_SMARTCONFIG_TASK_STACK, NULL, 3, NULL);
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* 断开后自动重连 */
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, CONNECTED_BIT);
        if (s_from_smartconfig) {
            /* 智能配网后成功获取 IP：提示成功、打印凭据并写入 NVS */
            ESP_LOGI(TAG, "===== 智能配网成功 =====");
            ESP_LOGI(TAG, "SSID:     %s", s_sc_ssid);
            ESP_LOGI(TAG, "PASSWORD: %s", s_sc_password);
            wifi_provision_save_nvs_creds((char *)s_sc_ssid, (char *)s_sc_password);
            s_from_smartconfig = false;
        } else {
            ESP_LOGI(TAG, "WiFi 已连接（直连模式）");
        }
    }
    /* ESP-TOUCH 配网事件 */
    else if (event_base == SC_EVENT && event_id == SC_EVENT_SCAN_DONE) {
        ESP_LOGI(TAG, "智能配网：信道扫描完成");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_FOUND_CHANNEL) {
        ESP_LOGI(TAG, "智能配网：已锁定目标信道");
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_GOT_SSID_PSWD) {
        ESP_LOGI(TAG, "智能配网：已收到手机下发的 SSID/PASSWORD");

        smartconfig_event_got_ssid_pswd_t *evt =
            (smartconfig_event_got_ssid_pswd_t *)event_data;
        wifi_config_t wifi_config;
        bzero(&wifi_config, sizeof(wifi_config_t));

        /* 拷贝手机下发的 ssid / password */
        memcpy(wifi_config.sta.ssid, evt->ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, evt->password, sizeof(wifi_config.sta.password));
        memcpy(s_sc_ssid, evt->ssid, sizeof(evt->ssid));
        memcpy(s_sc_password, evt->password, sizeof(evt->password));

        ESP_LOGI(TAG, "接收到 SSID:%s", s_sc_ssid);
        ESP_LOGI(TAG, "接收到 PASSWORD:%s", s_sc_password);

        /* 标记为来自智能配网，便于后续 IP 事件写入 NVS */
        s_from_smartconfig = true;

        /* 先断开再写入配置并发起连接 */
        ESP_ERROR_CHECK(esp_wifi_disconnect());
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        esp_wifi_connect();
    } else if (event_base == SC_EVENT && event_id == SC_EVENT_SEND_ACK_DONE) {
        /* 已向手机回 ACK，标记配网流程结束 */
        xEventGroupSetBits(s_wifi_event_group, ESPTOUCH_DONE_BIT);
    }
}

/* ============================== 智能配网任务 ============================== */

static void wifi_provision_smartconfig_task(void *arg)
{
    EventBits_t ux_bits;

    ESP_ERROR_CHECK(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH));
    smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_smartconfig_start(&cfg));

    ESP_LOGI(TAG, "已进入 ESP-TOUCH 智能配网模式，请在手机端发送 WiFi 信息...");

    /* 等待“已连接”与“配网结束”两个事件 */
    while (1) {
        ux_bits = xEventGroupWaitBits(s_wifi_event_group,
                                      CONNECTED_BIT | ESPTOUCH_DONE_BIT,
                                      true, false, portMAX_DELAY);
        if (ux_bits & CONNECTED_BIT) {
            ESP_LOGI(TAG, "WiFi 已连接到 AP");
        }
        if (ux_bits & ESPTOUCH_DONE_BIT) {
            ESP_LOGI(TAG, "智能配网流程结束");
            esp_smartconfig_stop();
            vTaskDelete(NULL);
        }
    }
}

/* ============================== 对外接口实现 ============================== */

esp_err_t wifi_provision_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "创建 WiFi 事件组失败");
        return ESP_FAIL;
    }

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_provision_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_provision_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_provision_event_handler, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    return ESP_OK;
}

esp_err_t wifi_provision_start(void)
{
    /* 读取 NVS 中已保存的 WiFi 凭据，决定是否进入智能配网 */
    char ssid[33] = {0};
    char pass[65] = {0};

    esp_err_t err = wifi_provision_read_nvs_creds(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err == ESP_OK) {
        /* 凭据完整：以 NVS 中的 ssid/pass 直连 */
        s_have_creds = true;
        wifi_config_t wifi_config;
        bzero(&wifi_config, sizeof(wifi_config_t));
        memcpy(wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
        memcpy(wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        /* STA_START 事件回调中会调用 esp_wifi_connect() */
    } else {
        /* 凭据缺失：交由 STA_START 事件回调启动智能配网任务 */
        s_have_creds = false;
    }

    /* 在事件回调依赖的 s_have_creds 确定后再启动 WiFi，
       以保证 STA_START 时能正确选择“直连”或“智能配网”分支 */
    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}
