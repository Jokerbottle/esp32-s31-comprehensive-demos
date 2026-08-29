# MiddleWires（中间件）

本目录用于存放中间件（MiddleWires），即连接底层 BSP 驱动与上层应用逻辑的
通用封装与抽象层，便于应用层以统一接口调用各外设功能。

## 目录结构（当前）

```text
MiddleWires/
├── CMakeLists.txt          # 组件构建配置
├── README.md               # 本说明文件
└── wifi_provision/         # WiFi 配网中间件（ESP-TOUCH 智能配网 + NVS 凭据管理）
    ├── wifi_provision.h    # 对外接口头文件
    └── wifi_provision.c    # 配网逻辑实现
```

## 模块说明

### wifi_provision
封装 ESP32-S31 的 WiFi 配网流程：
- **直连模式**：上电后读取 NVS 命名空间 `wifi_prov` 中已保存的 `ssid` / `pass`，
  若完整则直接使用该凭据连接 WiFi；
- **智能配网模式**：若 NVS 无凭据，则启动 ESP-TOUCH，等待手机端下发 `ssid` / `password`；
- 配网成功后通过串口打印 `ssid` 与 `password`，并将其写入 NVS 持久化，供下次上电直连。

## 变更记录

- 初始创建：新增 `MiddleWires` 组件目录，包含 `CMakeLists.txt` 与 `README.md`。
- 新增模块：加入 `wifi_provision/` 子模块，提供基于 ESP-TOUCH 的智能配网能力。
