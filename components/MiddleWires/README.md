# MiddleWires（中间件）

本目录用于存放中间件（MiddleWires），即连接底层 BSP 驱动与上层应用逻辑的
通用封装与抽象层，便于应用层以统一接口调用各外设功能。

## 目录结构（当前）

```text
MiddleWires/
├── CMakeLists.txt  # 组件构建配置
└── README.md       # 本说明文件
```

> 当前尚未开始编写具体中间件代码，故暂不创建子模块文件夹。

## 变更记录

- 初始创建：新增 `MiddleWires` 组件目录，包含 `CMakeLists.txt` 与 `README.md`。
