# web（音乐播放器网页控制端）

通过浏览器远程控制 ESP32-S31 音乐播放器。纯 Python 标准库实现，无需安装任何依赖。

## 使用

1. 双击 `start_web.bat`（或手动运行 `python server.py`）；
2. 浏览器打开控制台打印的地址（如 `http://192.168.1.100:8888`）；
3. 首次打开会弹出 IP 设置弹窗，输入开发板 IP（串口日志可查，如 `192.168.1.50`），
   点击"确认连接"，右上角圆点变绿即连接成功；
4. 搜索歌曲 → 播放 / 加入歌单；歌单按顺序自动连播，支持顺序/单曲循环/随机。

> 前提：电脑、开发板、lx-server 在同一局域网；开发板固件为 web 远程控制版
> （WebSocket 端点 `ws://<board-ip>:8080/ws`）。

## 文件

| 文件 | 说明 |
|---|---|
| `server.py` | 本地服务器（静态页 + memory 读写 + lx-server API 代理） |
| `static/index.html` | 前端页面结构 |
| `static/app.css` | 深色主题样式 |
| `static/app.js` | 前端逻辑（WebSocket 直连开发板） |
| `memory.json` | 记忆文件（自动生成）：ESP IP、音量、歌单、播放模式、lx-server 账号 |

## 架构

```
浏览器 ──HTTP──> web 服务器(本目录 :8888) ──代理──> lx-server (:9527)
   │                    │
   │                memory.json（IP/音量/歌单持久化）
   └──WebSocket 直连──> ESP32-S31 (:8080/ws) ──HTTP 拉流──> lx-server
```

- 浏览器与开发板的 WebSocket **直连**，不经过 web 服务器（服务器只负责页面、
  记忆文件与 lx-server 跨域代理）；
- 歌单由 web 端持有，s31 一次只收一个编号，播完/失败后 web 再发下一首。

## s31 → web 事件

`hello`（接入同步）/ `state`（状态机）/ `finished`（播完）/ `error`（失败跳过）/
`progress`（每秒进度，驱动进度条与歌词）/ `volume`（回执）/ `sys`（每 5 秒：
CPU 占用率、空闲堆、WiFi RSSI）/ `pong`。
