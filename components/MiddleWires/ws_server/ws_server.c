/*
 * ws_server.c - 板载 WebSocket 服务器实现
 *
 * esp_http_server 开启 CONFIG_HTTPD_WS_SUPPORT 后，/ws 端点完成握手；
 * 每收到一帧回调 ws_handler：GET 方法为握手（记录客户端 fd 并发 hello），
 * TEXT 帧转交 player_ctrl 处理。多任务推送经互斥锁串行化。
 */
#include "ws_server.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "player_ctrl.h"

#define WS_SERVER_PORT  8080
#define WS_MAX_MSG_LEN  512

static const char *TAG = "ws_server";

static httpd_handle_t s_httpd = NULL;
static int s_client_fd = -1;               /* 当前接入的 web 客户端 socket fd */
static SemaphoreHandle_t s_send_mtx = NULL;

/* ---------------- 帧收发 ---------------- */

void ws_server_send_json(const char *json)
{
    if (s_httpd == NULL || s_client_fd < 0 || json == NULL) {
        return;
    }
    httpd_ws_frame_t pkt = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = strlen(json),
    };
    if (xSemaphoreTake(s_send_mtx, pdMS_TO_TICKS(500)) == pdTRUE) {
        esp_err_t err = httpd_ws_send_frame_async(s_httpd, s_client_fd, &pkt);
        xSemaphoreGive(s_send_mtx);
        if (err != ESP_OK) {
            /* 客户端已断开：丢弃 fd，等浏览器重连 */
            ESP_LOGW(TAG, "发送失败(%s)，丢弃客户端", esp_err_to_name(err));
            s_client_fd = -1;
        }
    }
}

/* WebSocket 处理：IDF master 在握手完成时不调用 handler，而是每个数据帧
 * 调用一次（会话中记录的 ws_handler）。因此这里在每帧处理时刷新客户端 fd，
 * 新客户端接入由 web 端先发 {"cmd":"hello"} 触发 s31 应答状态同步。 */
static esp_err_t ws_handler(httpd_req_t *req)
{
    /* 每帧刷新客户端 fd（异步发送依赖它） */
    s_client_fd = httpd_req_to_sockfd(req);

    httpd_ws_frame_t pkt = { 0 };
    pkt.type = HTTPD_WS_TYPE_TEXT;
    /* 先取帧长度 */
    if (httpd_ws_recv_frame(req, &pkt, 0) != ESP_OK) {
        return ESP_FAIL;
    }
    if (pkt.len == 0) {
        return ESP_OK;
    }
    if (pkt.len > WS_MAX_MSG_LEN) {
        ESP_LOGW(TAG, "消息过长(%u)，丢弃", (unsigned)pkt.len);
        return ESP_OK;
    }
    char *buf = calloc(1, pkt.len + 1);
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = (uint8_t *)buf;
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK) {
        player_ctrl_handle_message(buf);
    }
    free(buf);
    return ret;
}

/* ---------------- 启动 ---------------- */

esp_err_t ws_server_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }
    if (s_send_mtx == NULL) {
        s_send_mtx = xSemaphoreCreateMutex();
        if (s_send_mtx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = WS_SERVER_PORT;
    cfg.stack_size = 6144;
    cfg.max_uri_handlers = 4;
    cfg.lru_purge_enable = true;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd_start failed");

    static const httpd_uri_t ws_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = ws_handler,
        .is_websocket = true,
    };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &ws_uri),
                        TAG, "register /ws failed");

    ESP_LOGI(TAG, "WebSocket 服务器就绪: ws://<board-ip>:%d/ws", WS_SERVER_PORT);
    return ESP_OK;
}
