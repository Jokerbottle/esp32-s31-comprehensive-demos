/*
 * ws_server.h - 板载 WebSocket 服务器（供 web 浏览器直连控制 s31）
 *
 * 基于 esp_http_server 的 WebSocket 支持，端点 ws://<board-ip>:8080/ws。
 * 收到文本帧转交 player_ctrl_handle_message()；其他任务经
 * ws_server_send_json() 向当前客户端推送事件（多任务并发安全）。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 HTTP+WebSocket 服务器（端口 8080，端点 /ws）
 */
esp_err_t ws_server_start(void);

/**
 * @brief 向当前接入的 web 客户端发送一条 JSON 文本帧（无客户端时静默丢弃）
 *
 * @param[in] json 以 '\0' 结尾的 JSON 字符串
 */
void ws_server_send_json(const char *json);

#ifdef __cplusplus
}
#endif
