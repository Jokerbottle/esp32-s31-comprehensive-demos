# esp32-s31-comprehensive-demos

Comprehensive example set running on **ESP32-S31-Function-CoreBoard-1** (Espressif official dev board, 16MB Flash + 16MB PSRAM), built on **ESP-IDF master (V6.2)**.

## Current Demo: Network Music Player (5-Artist Auto-Rotation)

A self-healing internet-radio-style player that streams from a self-hosted
[lx-server](https://github.com/XCQ0607/lxserver) (LX Music server, Subsonic-compatible API):

- **WiFi provisioning** — ESP-TOUCH smart config, credentials persisted in NVS
  (direct connect on subsequent boots).
- **Artist rotation auto-DJ** — cycles through 邓紫棋 / 周杰伦 / 薛之谦 / 林俊杰 / 陈奕迅,
  searching MP3 sources on the server and playing them back-to-back, never exits.
- **MP3-only whitelist** — `search3` results are filtered for `contentType=audio/mpeg`;
  after 302-pre-resolving the CDN direct link, the URL suffix must be `.mp3`
  (meta says MP3 but CDNs sometimes serve FLAC/mflac — those tracks are skipped).
- **Self-healing playback** — finished track rotates to the next artist; errors retry
  the same artist's next song (per-artist fail counter); a stall watchdog force-skips
  any track whose progress freezes for 30s.
- **Audio output** — onboard ES8311 codec + NS4150B PA (I2S, 44100 Hz / 16-bit / stereo).

## Project Structure

```
components/
├── BSP/
│   └── ES8311/                 # Board support: I2S + I2C + ES8311 codec + PA driver
└── MiddleWires/
    ├── wifi_provision/         # ESP-TOUCH provisioning + NVS credential storage
    ├── audio_player/           # esp_player wrapper: play_url / search_mp3 / 302-resolve
    └── music_playlist/         # Rotation auto-DJ state machine (select/switch/watchdog)
main/
├── main.c                      # Init entry only (NVS → WiFi → start playlist task)
└── Kconfig.projbuild           # lx-server connection settings (menuconfig)
```

## Getting Started

1. Install ESP-IDF master (V6.2) and connect the board.
2. Run your own lx-server, create a Subsonic user, note its IP/port.
3. Configure the server connection:

   ```bash
   idf.py menuconfig   # → "LX-Server Music Player Configuration"
   ```

   | Option            | Description                        |
   |-------------------|------------------------------------|
   | `LX_SERVER_IP`    | Host running lx-server             |
   | `LX_SERVER_PORT`  | lx-server TCP port (default 9527)  |
   | `LX_USER`         | Subsonic username                  |
   | `LX_PASSWORD`     | Subsonic password                  |

4. Build, flash, then provision WiFi once with any ESP-TOUCH app (Espressif
   "ESP-Touch" / "EspTouch: SmartConfig"). The board connects, starts searching
   MP3 tracks and plays them automatically.

> Playback requires the board and lx-server to be on the same LAN. Audio quality
> and availability depend on the music sources your lx-server can reach.

## Branches

| Branch        | Content                                                        |
|---------------|----------------------------------------------------------------|
| `master`      | Active development                                             |
| `release-v1`  | Early milestone (project skeleton + ESP-TOUCH provisioning)    |
| `release-v2`  | Network music player (this demo)                               |
