# BSP（Board Support Package）

本目录为板级支持包，统一存放 ESP32-S31-Function-CoreBoard-1 开发板上各外设模块的模块化驱动代码。
每个外设模块在本目录下创建对应的子文件夹，并包含 `.c`、`.h` 及 `README.md`。

## 目录结构（当前）

```text
BSP/
├── CMakeLists.txt          # 组件构建配置（汇聚各模块源文件与依赖）
├── README.md               # 本说明文件
├── idf_component.yml       # 第三方组件依赖声明（esp_codec_dev）
└── ES8311/                # ES8311 Codec + NS4150B 功放驱动模块
    ├── es8311_audio.c      # 功放驱动实现（I2S + I2C + ES8311 + PA）
    ├── es8311_audio.h      # 对外接口头文件
    └── README.md           # 模块说明
```

## 变更记录

- 初始创建：新增 `BSP` 组件目录，包含 `CMakeLists.txt` 与 `README.md`。
- 新增模块：加入 `ES8311/` 子模块，提供板载 ES8311 Codec + NS4150B 功放的播放驱动，
  并引入第三方组件 `espressif/esp_codec_dev`（v2.0.0-beta1）作为依赖。
