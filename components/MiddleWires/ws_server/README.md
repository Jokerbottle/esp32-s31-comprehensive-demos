# ws_server（板载 WebSocket 服务器）

基于 esp_http_server 的 WebSocket 端点，供 web 浏览器直连控制开发板。

## 接入

- 地址：`ws://<开发板IP>:8080/ws`
- 协议：JSON 文本帧（指令/事件定义见 `components/MiddleWires/player_ctrl/player_ctrl.h` 头注释）
- 行为：
  - 浏览器握手成功 → 记录客户端 fd，主动推送 `hello`（当前音量/状态/曲目）；
  - 收到 TEXT 帧 → 转交 `player_ctrl_handle_message()`（>512 字节丢弃）；
  - 其他任务经 `ws_server_send_json()` 推送事件（互斥锁串行化，多任务安全）；
  - 发送失败视为客户端断开，丢弃 fd 等待重连。

## 依赖

- `CONFIG_HTTPD_WS_SUPPORT=y`（sdkconfig.defaults 已配置）
- 端口固定 8080（`WS_SERVER_PORT`）

## 变更记录

- 初始创建：web 远程控制方案传输层。
