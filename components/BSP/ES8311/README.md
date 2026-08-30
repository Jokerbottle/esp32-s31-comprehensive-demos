# BSP / ES8311 音频功放模块

本模块为 ESP32-S31-Function-CoreBoard-1 板载 **ES8311 Codec + NS4150B 功放** 的
板级驱动（BSP），提供播放（DAC 输出）能力，不涉及 USB / 音频解码部分。

## 音频链路

```text
ESP32 I2S TX (GPIO56) -> ES8311 DSDIN (DAC 输入)
ES8311 差分输出 OUTP/OUTN -> NS4150B PA (GPIO57 高电平使能) -> 喇叭
```

> ⚠️ 关键引脚：**I2S DOUT=56 / DIN=54**。二者接反会导致音频数据发到错误引脚而完全无声，
> 即便 ES8311 所有寄存器状态均正常（来源：官方板定义 `board_peripherals.yaml`）。

## 引脚映射（官方板）

| 功能 | 引脚 |
|---|---|
| I2C SCL / SDA | GPIO50 / GPIO51（ES8311 地址 8bit 0x30，400kHz） |
| I2S MCLK / SCLK / LRCK | GPIO52 / GPIO53 / GPIO55 |
| I2S DOUT（播放） | GPIO56 |
| I2S DIN（录音，未使用） | GPIO54 |
| PA 使能 | GPIO57（NS4150B，高电平有效） |

## 对外接口（es8311_audio.h）

- `bsp_es8311_init()`：初始化 I2S / I2C / ES8311 Codec
- `bsp_es8311_open()`：打开音频通路、设置音量、使能 PA
- `bsp_es8311_write()`：写入 16bit 交织立体声 PCM 数据（播放）
- `bsp_es8311_set_volume()` / `bsp_es8311_get_volume()`：音量 0~100
- `bsp_es8311_set_pa()`：显式控制功放使能
- `bsp_es8311_close()`：关闭音频通路并禁用 PA
- `bsp_es8311_diag()`：串口诊断（PA 电平 / I2S 字节数 / ES8311 关键寄存器）

## 依赖

- 第三方组件 `espressif/esp_codec_dev`（v2.0.0-beta1），由 `components/BSP/idf_component.yml` 声明。

## 变更记录

- 初始创建：从参考工程 `02_usb_msc_audio_to_player` 的板级音频驱动移植 ES8311 功放部分，
  封装为独立 BSP 模块 `ES8311/`，保留关键修复（DAC 内部基准 `ref_enable=true`、DOUT/DIN 引脚正确）。
