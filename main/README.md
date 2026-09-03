# main 目录

本目录为 ESP-IDF 应用入口（`app_main`），仅负责整体初始化，随后全系统
进入 IDLE 待命，等 web 端经 WebSocket 下发指令。

## 当前内容

- `main.c`：应用入口，依次完成：
  1. **智能配网**：NVS 初始化 + `wifi_provision`（有凭据直连，否则 ESP-TOUCH 配网）；
  2. **音频播放器**：`audio_player_init`（ES8311 + esp_player，音量从 NVS 恢复）；
  3. **远程控制**：`player_ctrl`（播放控制任务）+ `ws_server`（`ws://<ip>:8080/ws`）；
  4. 之后 `app_main` 返回，系统 IDLE。
- `Kconfig.projbuild`：lx-server 连接参数菜单（IP / 端口 / Subsonic 账号）。
- `CMakeLists.txt`：依赖 `spi_flash`、`MiddleWires`。

## 变更记录

- 初始创建：空 `app_main` 骨架。
- 新增功能：NVS 初始化与 `wifi_provision` 配网调用。
- 新增功能：集成 `BSP/ES8311` 功放驱动与 1kHz 测试音示例。
- 重构：服务器参数 Kconfig 化（开源友好）。
- 重构：web 远程控制版——移除内置五歌手轮转自动播放，改为 IDLE 待命 +
  WebSocket 接收 web 指令驱动播放；音量支持 web 设置并存 NVS。
