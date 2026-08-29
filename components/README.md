# components 目录

本目录用于存放项目自定义组件（components），按功能模块进行拆分与管理。

## 目录结构

```text
components/
├── BSP/            # 板级支持包：开发板各外设模块的模块化驱动代码
└── MiddleWires/   # 中间件：连接驱动层与应用层的中间件封装
```

## 变更记录

- 初始创建：新增 `components`、`components/BSP`、`components/MiddleWires` 三个文件夹，
  并为各级目录补充 README.md；`BSP` 与 `MiddleWires` 作为独立组件各含一份 CMakeLists.txt。
