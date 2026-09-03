# player_ctrl（播放控制器）

接收 web 端经 WebSocket 下发的指令，驱动 `audio_player` 播放的中间层。

## 职责

- 指令处理：`play / stop / pause / resume / volume / ping`（协议见 `player_ctrl.h` 头注释）；
- 状态机：`IDLE → PREPARING → PLAYING ⇄ PAUSED → IDLE`，无歌曲时 **IDLE 待命**；
- 拉流准备：构造 lx-server `stream.view` 地址 → 302 预解析得 CDN 直链 → MP3 后缀校验；
- 事件上报：状态变化 / 自然播完（`finished`，web 依歌单决定下一首）/ 失败（`error`，
  web 跳过并提示）/ 每秒进度（`progress`）/ 每 5 秒系统负载（`sys`：CPU 占用率、
  空闲堆、WiFi RSSI）；
- 停滞看门狗：进度 30 秒无推进强制停止并上报失败。

## 说明

- 播放序列（歌单）由 **web 端**持有：s31 一次只播一首，播完/失败上报后等待 web
  下发下一首编号（一首一发协议）；
- 音量由 `audio_player_set_volume()` 写入 NVS，上电 `volume_load()` 自动恢复；
- 拉流任务栈 12KB（302 预解析含 TLS 握手）。

## 变更记录

- 初始创建：从原 `music_playlist`（五歌手轮转，已删除）演化而来，改为纯
  web 指令驱动模式。
