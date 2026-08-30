# music_playlist（音乐轮播控制器）

基于 `audio_player` 中间件的高层播放控制模块：从 lx-server 按歌手轮转搜索 MP3
音源并自动连播，播放完毕切歌手、出错/停滞自动跳曲，永不退出。

## 功能

- **歌手轮转**：邓紫棋 → 周杰伦 → 薛之谦 → 林俊杰 → 陈奕迅 循环；
- **MP3 白名单**：搜索筛选 `contentType=audio/mpeg`，302 预解析出 CDN 直链后
  校验后缀必须为 `.mp3`（酷我等源元数据标 MP3 但实际给 flac/mflac，一律放弃）；
- **选曲优先级**：`mg_`（咪咕直给 MP3_320）> `kw_` > 其他源；
- **自动切歌**：FINISHED 轮转下一位歌手；ERROR 同歌手换下一首
  （失败计数 ≥4 换歌手）；进度 30 秒停滞由看门狗强制跳过；
- **事件竞态防护**：事件回调只置标志，控制任务 500ms 轮询真实状态后才切歌。

## 接口

- `music_playlist_start()`：初始化播放器（含 ES8311）并创建 16KB 栈的
  `music_ctrl` 后台任务（搜索/302 预解析含 TLS 握手），之后全自动运行。

## 配置

服务器连接参数来自 menuconfig（`main/Kconfig.projbuild`）：
`LX_SERVER_IP` / `LX_SERVER_PORT` / `LX_USER` / `LX_PASSWORD`。

## 变更记录

- 初始创建：从 `main/main.c` 迁入全部播放控制逻辑（选曲/切歌/看门狗），
  main.c 精简为初始化入口；服务器参数 Kconfig 化。
