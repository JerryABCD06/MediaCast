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
5. [05-核心提案-PlaybackPipe重连释放旧实例.md](05-核心提案-PlaybackPipe重连释放旧实例.md)
6. [06-核心提案-confirmation-only冷启动.md](06-核心提案-confirmation-only冷启动.md)
7. [07-Qt-free验收记录-20261114.md](07-Qt-free验收记录-20261114.md)
8. [08-MPV重绘架构审计.md](08-MPV重绘架构审计.md)
9. [09-生命周期与关闭.md](09-生命周期与关闭.md)
10. [10-Qt-free迁移最终验收.md](10-Qt-free迁移最终验收.md)

## 当前基线

```text
main: `9643110`（最终文档提交后以 `main` HEAD 为准）
旧 UI: Qt 6 / QML / FluentUI / libmpv
新 UI 目标: Avalonia / .NET
UI 后端目标: Qt-free native C++
MPV render API: MPV_RENDER_API_TYPE_OPENGL
Avalonia rendering backend: 由 Avalonia 自行选择，生产候选为 ANGLE
```

事故实现历史备份：

```text
preserve/avalonia-ui-migration-main-d52deac
preserve/avalonia-ui-migration-2026-11-14
```

当前 `main` 是从干净基线重新实现的主线，不是事故分支合并结果。
## 第一优先级

```text
1. 状态机正确、所有权唯一、命令顺序可证明
2. 边界清楚、失败路径可收口
3. 真实交互和可访问性正确
4. 视觉还原
5. 开发速度
```

UI 永远是状态机的消费者，不是状态机的设计者。

