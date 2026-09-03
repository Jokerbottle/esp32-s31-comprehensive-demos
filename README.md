# esp32-s31-comprehensive-demos

基于 **ESP32-S31-Function-CoreBoard-1**（乐鑫官方开发板，16MB Flash + 16MB PSRAM）的
综合示例项目，当前核心功能是一台 **网页远程控制的网络音乐播放器**。

开发框架：**ESP-IDF master (V6.2)** · 控制：浏览器 Web UI · 音源：自建
[lx-server](https://github.com/XCQ0607/lxserver)（洛雪音乐服务端，Subsonic 兼容 API）

## 功能总览

### 播放器
- **WiFi 智能配网**：ESP-TOUCH 配网 + NVS 凭据持久化，上电自动直连
- **网页点播**：浏览器搜索歌曲（仅 MP3 音源，提高播放成功率）→ 一键播放
- **歌单**：添加/删除/清空，顺序 / 单曲循环 / 随机三种模式，自动连播
- **音量调节**：网页滑条/数字输入，开发板侧 NVS 持久化，上电自动恢复
- **播放控制**：播放/暂停/停止/上一首/下一首，歌曲海报、歌名、歌手、时长、
  进度条与歌词同步显示
- **可靠性设计**：
  - 播放指令携带备选音源链（同一首歌的酷我/咪咕/酷狗版本），失败自动换源
  - 302 重定向预解析（获取 CDN 直连地址，支持 HTTPS）
  - 源健康度跟踪：不稳定的音源自动降权跳过
  - 停滞看门狗：进度 30 秒无推进强制跳过，歌单永不卡死
- **状态回传**：播放状态 / 每秒进度 / 每秒 CPU 占用率、空闲堆、WiFi RSSI

### 页面预览
深色主题三栏布局：播放器卡片（左）· 搜索（中）· 歌单 + 设备日志（右），
IP 连接弹窗 + 连接状态指示灯。

## 架构

```
浏览器 ──HTTP──> web 服务器(web/, :8888) ──代理──> lx-server (:9527)
   │                    │
   │                memory.json（IP/音量/歌单/模式持久化）
   └──WebSocket 直连──> ESP32-S31 (:8080/ws) ──HTTP 拉流──> lx-server / 音乐 CDN
```

- 浏览器与开发板 **WebSocket 直连**，web 服务器只负责页面托管、记忆文件与
  lx-server 跨域代理（纯 Python 标准库，零依赖）
- 播放链路：HTTP → 302 预解析 → CDN 直链(MP3) → esp_player 解码 →
  重采样(44100/16bit/双声道) → ES8311 Codec → NS4150B 功放

## 工程结构

```
├── main/                        # 应用入口（仅初始化，之后 IDLE 待命）
│   ├── main.c
│   └── Kconfig.projbuild        # lx-server 连接参数（menuconfig 配置）
├── components/
│   ├── BSP/
│   │   └── ES8311/              # 板级驱动：I2S + I2C + ES8311 Codec + PA
│   └── MiddleWires/
│       ├── wifi_provision/      # ESP-TOUCH 配网 + NVS 凭据管理
│       ├── audio_player/        # esp_player 封装：播放/音量(NVS)/302 预解析
│       ├── player_ctrl/         # 播放控制器：指令状态机/候选源链/看门狗/事件上报
│       └── ws_server/           # 板载 WebSocket 服务器 (:8080/ws)
├── web/                         # 网页控制端（server.py + static/ + start_web.bat）
├── partitions.csv               # 分区表（factory 5MB）
├── sdkconfig.defaults           # 项目级配置（PSRAM/解码器/WS 等）
└── dependencies.lock            # 组件版本锁（保证可复现构建）
```

各目录内均有 README.md 详述模块职责与变更记录。

## 快速开始

### 硬件
- ESP32-S31-Function-CoreBoard-1（板载 ES8311 + NS4150B，喇叭接 PA 输出）

### 1. 准备音源服务器
- PC 上运行 lx-server，创建一个 Subsonic 用户，记下 IP/端口/账号

### 2. 编译烧录固件
```bash
idf.py menuconfig   # → "LX-Server Music Player Configuration"
                    #   填入 lx-server 的 IP / 端口 / 用户名 / 密码
idf.py -p COM9 build flash monitor
```

### 3. 配网
首次上电进入 ESP-TOUCH 配网模式，用乐鑫 ESP-Touch App（或 EspTouch）下发
家庭 WiFi 的 SSID/密码；配网成功后自动持久化，之后上电直连。

### 4. 启动网页控制端
```
双击 web\start_web.bat
```
浏览器打开控制台打印的地址（如 `http://192.168.1.100:8888`），在弹窗中输入
开发板 IP（串口日志可查），确认连接后即可搜索点播。

> 开发板、电脑、lx-server 需在同一局域网。音源可用性取决于 lx-server 能访问
> 的音乐上游。

## WebSocket 协议（浏览器 ↔ 开发板）

| 方向 | 消息 | 说明 |
|---|---|---|
| → | `{"cmd":"play","id":"kw_x","alt":["mg_y","kg_z"]}` | 播放（alt=备选源，失败自动切换） |
| → | `{"cmd":"pause"}` / `{"cmd":"resume"}` / `{"cmd":"stop"}` | 播放控制 |
| → | `{"cmd":"volume","value":40}` | 音量（板端存 NVS） |
| → | `{"cmd":"hello"}` / `{"cmd":"ping"}` | 接入同步 / 心跳 |
| ← | `{"evt":"hello","state":"idle","volume":40,...}` | 状态同步 |
| ← | `{"evt":"state","state":"preparing|playing|paused|idle","id":...}` | 状态机 |
| ← | `{"evt":"progress","pos":ms,"dur":ms}` | 每秒进度 |
| ← | `{"evt":"finished"}` / `{"evt":"error","msg":...}` | 播完（发下一首）/ 全部候选失败 |
| ← | `{"evt":"sys","cpu":1,"heap":..,"rssi":..}` | 每 5 秒系统负载 |
| ← | `{"evt":"log","text":...}` | 过程信息（如自动换源） |

完整定义见 `components/MiddleWires/player_ctrl/player_ctrl.h`。

## 分支说明

| 分支 | 内容 |
|---|---|
| `master` | 活跃开发 |
| `release-v1` | 早期里程碑（工程骨架 + ESP-TOUCH 配网） |
| `release-v2` | 网络音乐播放器（网页远程控制版） |

## 许可

仅供学习交流，音源内容版权归各自平台所有。
