# main 目录

本目录为 ESP-IDF 应用入口（`app_main`），仅负责整体初始化，随后将控制权
交给中间件的后台任务。

## 当前内容

- `main.c`：应用入口，依次完成：
  1. **智能配网**：初始化 NVS 后调用 `MiddleWires` 的 `wifi_provision` 模块
     （NVS 有凭据则直连，否则进入 ESP-TOUCH 配网）；
  2. **启动音乐轮播**：WiFi 就绪后调用 `music_playlist_start()`，
     播放控制（搜索/切歌/看门狗）由其后台任务接管，`app_main` 随即返回。
- `Kconfig.projbuild`：lx-server 连接参数菜单（IP / 端口 / Subsonic 账号），
  通过 `idf.py menuconfig` 的 "LX-Server Music Player Configuration" 修改，
  便于不同使用者适配自己的服务器而无需改代码。
- `CMakeLists.txt`：依赖 `spi_flash`、`MiddleWires`。

## 变更记录

- 初始创建：空 `app_main` 骨架。
- 新增功能：集成 NVS 初始化与 `wifi_provision` 配网调用（ESP-TOUCH 智能配网）。
- 新增功能：集成 `BSP/ES8311` 功放驱动，加入 1kHz 测试音播放示例。
- 重构：网络音乐播放控制逻辑整体迁出至 `MiddleWires/music_playlist`，
  main.c 精简为纯初始化入口；新增 `Kconfig.projbuild` 将服务器 IP/端口/账号
  参数化（开源友好，使用者按自身环境 menuconfig 配置）。
