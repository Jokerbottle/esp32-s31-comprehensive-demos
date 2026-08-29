# main 目录

本目录为 ESP-IDF 应用入口（`app_main`），负责整体初始化并调度各组件/中间件。

## 当前内容

- `main.c`：应用入口。上电后初始化 NVS，再调用 `MiddleWires` 中的
  `wifi_provision` 模块完成 WiFi 智能配网（NVS 直连优先，否则 ESP-TOUCH 配网）。
- `CMakeLists.txt`：依赖 `spi_flash` 与 `MiddleWires` 组件。

## 变更记录

- 初始创建：空 `app_main` 骨架。
- 新增功能：在 `app_main` 中集成 NVS 初始化与 `wifi_provision` 配网调用，
  实现 ESP-TOUCH 智能配网例程。
