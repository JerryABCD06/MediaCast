# Media Cast Receiver Avalonia 接手文档

本目录以干净基线 `6c990a3` 为事实来源，重新建立 Avalonia UI 迁移契约。

这里不是事故分支的设计说明，也不是对旧补丁链的背书。事故实现只用于核对
已验证的现象、测试方法和失败记录；任何源码都必须从当前 `main` 基线重新接入
并重新验证。

## 文档顺序

1. [01-行为矩阵.md](01-行为矩阵.md)
2. [02-状态所有权与IPC适配.md](02-状态所有权与IPC适配.md)
3. [03-MPV与Avalonia渲染契约.md](03-MPV与Avalonia渲染契约.md)
4. [04-迁移顺序与验收门禁.md](04-迁移顺序与验收门禁.md)

## 当前基线

```text
main: 6c990a3
旧 UI: Qt 6 / QML / FluentUI / libmpv
新 UI 目标: Avalonia / .NET
UI 后端目标: Qt-free native C++
MPV render API: MPV_RENDER_API_TYPE_OPENGL
Avalonia rendering backend: 由 Avalonia 自行选择，生产候选为 ANGLE
```

完整迁移实现保存在独立分支，只能作为历史参考，不得直接合并到干净基线：

```text
preserve/avalonia-ui-migration-main-d52deac
```

## 第一优先级

```text
1. 状态机正确、所有权唯一、命令顺序可证明
2. 边界清楚、失败路径可收口
3. 真实交互和可访问性正确
4. 视觉还原
5. 开发速度
```

UI 永远是状态机的消费者，不是状态机的设计者。

