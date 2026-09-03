# MiddleWires（中间件）

本目录用于存放中间件（MiddleWires），即连接底层 BSP 驱动与上层应用逻辑的
通用封装与抽象层，便于应用层以统一接口调用各外设功能。

## 目录结构（当前）

```text
MiddleWires/
├── CMakeLists.txt          # 组件构建配置
├── README.md               # 本说明文件
├── wifi_provision/         # WiFi 配网中间件（ESP-TOUCH 智能配网 + NVS 凭据管理）
│   ├── wifi_provision.h    # 对外接口头文件
│   └── wifi_provision.c    # 配网逻辑实现
├── audio_player/           # 音频播放中间件（esp_player 封装 → BSP ES8311 输出）
│   ├── audio_player.h      # 播放/音量/302 预解析对外接口
│   └── audio_player.c      # 播放管线与 lx-server 拉流实现
├── player_ctrl/            # 播放控制器（web 指令驱动，状态机 + 看门狗 + 事件上报）
│   ├── player_ctrl.h       # 对外接口与 WebSocket 协议定义
│   └── player_ctrl.c       # 指令处理/状态机/进度与系统负载上报实现
└── ws_server/              # 板载 WebSocket 服务器（浏览器直连控制）
    ├── ws_server.h         # 对外接口头文件
    └── ws_server.c         # esp_http_server WS 端点实现
```

## 模块说明

### wifi_provision
封装 ESP32-S31 的 WiFi 配网流程：NVS 有凭据直连，否则 ESP-TOUCH 配网；
凭据串口打印并持久化。

### audio_player
封装 esp_player 播放管线（HTTP → extractor → decoder → esp_audio_render → ES8311）：
- `play_url / stop / pause / resume / get_position` 等播放接口；
- `set_volume / get_volume`：音量设置 + NVS 持久化（上电自动恢复）；
- `resolve_url()`：手动跟随 302 重定向，返回 CDN 直连地址（支持 HTTPS）。

### player_ctrl
web 指令驱动的播放控制器（协议详见其头文件注释）：
- 指令：`play / stop / pause / resume / volume / hello / ping`；
- 状态机 `IDLE → PREPARING → PLAYING ⇄ PAUSED → IDLE`，无指令时 IDLE 待命；
- 事件上报：状态变化 / 播完（finished）/ 失败（error）/ 每秒进度（progress）/
  每 5 秒系统负载（sys：CPU 占用率、空闲堆、RSSI）；
- 停滞看门狗：进度 30 秒无推进强制停止并上报失败。

### ws_server
板载 WebSocket 服务器（`ws://<board-ip>:8080/ws`）：收帧转交 player_ctrl，
其他任务经 `ws_server_send_json()` 推送事件（多任务安全）。

## 变更记录

- 初始创建：新增 `MiddleWires` 组件目录，包含 `CMakeLists.txt` 与 `README.md`。
- 新增模块：加入 `wifi_provision/` 子模块，提供基于 ESP-TOUCH 的智能配网能力。
- 新增模块：加入 `audio_player/` 子模块，封装 esp_player 播放管线与 lx-server 拉流。
- 变更：`music_playlist`（五歌手轮转）删除，功能由 web 端歌单替代；新增
  `player_ctrl/` 与 `ws_server/`，实现 web 远程控制（播放/音量/进度/负载上报）；
  `audio_player` 移除 `search_mp3`（搜索改由 web 端代理 lx-server 完成），
  新增 NVS 音量持久化。
