# esp32-s31-comprehensive-demos

基于 **ESP32-S31-Function-CoreBoard-1** 开发板的综合示例工程，使用 ESP-IDF（master / V6.2）开发。

---

## Release V1.0 功能说明

本分支（**Release-V1.0**）为首个发布版本，实现了基于 **ESP-TOUCH 的智能配网** 功能：

- **WiFi 智能配网（ESP-TOUCH）**
  - 开发板上电后自动初始化 NVS 与 WiFi 运行环境；
  - 优先读取 NVS 中已保存的 WiFi 凭据，若存在则**直接连接**目标路由器；
  - 若 NVS 中无凭据，则进入 **ESP-TOUCH 智能配网模式**，等待手机端（Espressif ESP-TOUCH App）下发 `SSID` 与 `PASSWORD`。
- **NVS 凭据持久化**
  - 智能配网成功后，通过串口打印 `SSID` 与 `PASSWORD`，并将其写入 NVS
    （命名空间 `wifi_prov`，键 `ssid` / `pass`）；
  - 下次上电将使用保存的凭据直连，无需重复配网。
- **代码结构（模块化）**
  - `components/MiddleWires/wifi_provision/`：配网中间件，封装 ESP-TOUCH 流程与 NVS 读写；
  - `main/main.c`：应用入口，仅负责初始化并调度配网模块；
  - `components/BSP/`：预留板级支持包目录（本版本尚未含具体驱动）。

---

## 快速使用

1. 使用 ESP-IDF Terminal 编译并烧录：
   ```powershell
   & 'C:\Espressif\tools\Microsoft.master.PowerShell_profile.ps1'
   idf.py build flash monitor
   ```
2. 首次上电后，打开手机 ESP-TOUCH App 发送当前 WiFi 的 `SSID` / `PASSWORD`；
3. 串口将输出配网成功信息并打印凭据，随后自动持久化；
4. 之后每次上电将自动直连，无需再次配网。

---

## 目录约定

- `main/`：应用入口
- `components/BSP/`：板级支持包（外设模块化驱动，待扩展）
- `components/MiddleWires/`：中间件（含 `wifi_provision` 智能配网模块）
- `opencode/`：AI 工作区（不纳入版本控制）
