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
│   ├── audio_player.h      # 播放/搜索/302 预解析对外接口
│   └── audio_player.c      # 播放管线与 lx-server Subsonic 接口实现
└── music_playlist/         # 音乐轮播控制器（五歌手轮转自动切歌状态机）
    ├── music_playlist.h    # 对外接口头文件
    └── music_playlist.c    # 选曲/切歌/看门狗逻辑实现
```

## 模块说明

### wifi_provision
封装 ESP32-S31 的 WiFi 配网流程：
- **直连模式**：上电后读取 NVS 命名空间 `wifi_prov` 中已保存的 `ssid` / `pass`，
  若完整则直接使用该凭据连接 WiFi；
- **智能配网模式**：若 NVS 无凭据，则启动 ESP-TOUCH，等待手机端下发 `ssid` / `password`；
- 配网成功后通过串口打印 `ssid` 与 `password`，并将其写入 NVS 持久化，供下次上电直连。

### audio_player
封装 esp_player 播放管线（HTTP → extractor → decoder → esp_audio_render → ES8311）：
- `play_url / play_file / stop / pause / resume / get_position` 等播放接口；
- `search_mp3()`：lx-server Subsonic `search3` 搜索指定歌手 MP3（mg_ 源优先）；
- `resolve_url()`：手动跟随 302 重定向，返回 CDN 直连地址（支持 HTTPS）。

### music_playlist
五歌手轮转自动切歌控制器（详见其 README）：MP3 直链白名单、FINISHED 轮转、
ERROR/停滞看门狗跳曲、事件竞态防护，后台任务全自动运行。

## 变更记录

- 初始创建：新增 `MiddleWires` 组件目录，包含 `CMakeLists.txt` 与 `README.md`。
- 新增模块：加入 `wifi_provision/` 子模块，提供基于 ESP-TOUCH 的智能配网能力。
- 新增模块：加入 `audio_player/` 子模块，封装 esp_player 播放管线与 lx-server 拉流。
- 新增模块：加入 `music_playlist/` 子模块，承接原 main.c 的播放控制逻辑
  （歌手轮转/自动切歌/停滞看门狗），服务器参数改为 Kconfig 配置。
