# BSP（Board Support Package）

本目录为板级支持包，统一存放 ESP32-S31-Function-CoreBoard-1 开发板上各外设模块的模块化驱动代码。

后续每个外设模块（如 LED、按键、显示屏等）将在本目录下创建对应的子文件夹，
并各自包含 `.c` 与 `.h` 源文件及必要的构建配置。

## 目录结构（当前）

```text
BSP/
├── CMakeLists.txt  # 组件构建配置
└── README.md       # 本说明文件
```

> 当前尚未开始编写具体驱动代码，故暂不创建模块子文件夹。

## 变更记录

- 初始创建：新增 `BSP` 组件目录，包含 `CMakeLists.txt` 与 `README.md`。
