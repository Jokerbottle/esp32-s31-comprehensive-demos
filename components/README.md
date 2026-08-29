# components 目录

本目录用于存放项目自定义组件（components），按功能模块进行拆分与管理。

## 目录结构

```text
components/
├── BSP/            # 板级支持包：开发板各外设模块的模块化驱动代码
└── MiddleWires/   # 中间件：连接驱动层与应用层的中间件封装
    └── wifi_provision/  # WiFi 智能配网（ESP-TOUCH + NVS 凭据管理）
```

## 变更记录

- 初始创建：新增 `components`、`components/BSP`、`components/MiddleWires` 三个文件夹，
  并为各级目录补充 README.md；`BSP` 与 `MiddleWires` 作为独立组件各含一份 CMakeLists.txt。
- 新增模块：在 `components/MiddleWires/` 下加入 `wifi_provision/` 子模块，
  实现 ESP-TOUCH 智能配网与 NVS WiFi 凭据读写功能。
