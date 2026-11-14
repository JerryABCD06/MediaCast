# MPV 重绘架构审计

日期：2026-11-14

基线：`6c990a3`

审计提交：`c6f1222`

## 1. 审计范围

本审计只检查 Qt-free UI 进程内的 MPV 呈现链：

```text
Avalonia OpenGlControlBase
  -> MCast.AvaloniaHost.Native
  -> mpv_render_context
  -> PlaybackController / MpvCore
```

不改变 Protocol、Shell、wire、owner、claim、epoch 或播放命令语义。

## 2. 唯一所有权

| 对象 | 唯一所有者 | 审计结论 |
|---|---|---|
| MPV handle | `MpvCore` | 只有一个实例 |
| `mpv_render_context` | native render adapter | 每个 GL context generation 最多一个 |
| GL context | Avalonia | native 侧只保存受控 callbacks |
| FBO/OpenGL 调用线程 | Avalonia OpenGL callback | 不在 UI 普通消息线程直接执行 GL |
| media / transport state | `PlaybackController` / Protocol | renderer 不另建状态 |
| position / volume / picture value | MPV | renderer 只消费投影 |
| renderer readiness / generation | native render adapter | C# 只读 |

没有发现 C# 第二播放器、第二 renderer 或由 UI 推断的播放事实。

## 3. 帧唤醒链

实际路径：

```text
mpv_render_context update callback
  -> HostState::renderWake.mark()
  -> C# RenderUpdateRequested
  -> DispatcherPriority.Render
  -> OpenGlControlBase.RequestNextFrameRendering()
  -> OnOpenGlRender()
  -> mcast_avalonia_host_render_frame()
  -> renderWake.consume()
```

约束成立：

- native latch 最多保留一个待处理唤醒；
- C# 侧另有单飞标记，避免重复投递 Dispatcher 项；
- 实际 render 时消费 latch，下一帧可重新 arm；
- projection 刷新与视频重绘分离；
- 每帧不序列化 JSON、不刷新整套 UI 树。

审计中发现并修复的缺口：`lastFramebuffer/lastWidth/lastHeight` 虽在每帧更新，
但 renderer projection 只在首次 ready 时发布，导致 resize 后投影仍报告旧尺寸。
现已只在 render target 真正变化或 render 失败时发布一次 renderer 更新。

## 4. 行为矩阵

| 场景 | 当前行为 | 结论 |
|---|---|---|
| 初始化 | `OnOpenGlInit` 创建唯一 render context | 正确 |
| 首帧 | 成功 render 后 arm `FrameRendered` | 可用；present 边界证据有限 |
| 播放 | MPV update callback 驱动按需重绘 | 正确 |
| 暂停 | 无新视频帧时不持续空转 | 正确 |
| Seek | 播放核心产生新帧，经同一 callback 上屏 | 正确 |
| 窗口移动/缩放 | `WM_ENTERSIZEMOVE/WM_EXITSIZEMOVE` 暂停/恢复呈现 | 已实测 |
| 最大化/还原 | FBO 尺寸更新，媒体和 context 保持 | 已实测 |
| 页面往返 | surface 保留，只切换覆盖层/控件 | 已实测 |
| 全屏 | 使用 Avalonia 标准窗口状态 | 已实测 |
| context lost | C# 上报，native 清 callback 并标记 fail-closed | 后续阶段继续强化 |
| destroy | 先停止新调用，再释放 render context/player | 后续生命周期阶段复验 |

窗口移动期间只冻结视频呈现，不停止 MPV、不改变 position、不卸载媒体。

## 5. 线程与竞态

- `renderAttachImpl`、`renderDetachImpl`、`render_frame` 共用 render mutex；
  context handle 不会在 render 与 free 间并发访问。
- update callback 只通知“需要一帧”，不直接访问 Avalonia 控件。
- C# `RenderUpdateRequested` 由 Dispatcher 调度，不跨线程触碰 View。
- detach 先清 update callback，再释放 render context。
- context generation 只在实际 attach 时增加；resize、最大化、页面切换不增加。
- 移动和 resize 都由 Windows 的 `WM_ENTERSIZEMOVE` 模态边界冻结呈现；
  `WM_EXITSIZEMOVE` 处理完成后才恢复并请求最终几何的一帧。

## 6. 真实运行证据

测试使用 lavfi 60 fps 视频，经真实三进程 DLNA shadow E2E 投送。

最大化、还原后日志：

```text
Render target generation=1 framebuffer=1 pixels=849x936
Render target generation=1 framebuffer=1 pixels=1519x990
Render target generation=1 framebuffer=1 pixels=849x936
```

三次记录中：

```text
render context generation = 1
framebuffer id            = 1
```

这证明 resize 只改变 FBO 尺寸，没有重建 render context，也没有重新加载媒体。
最大化与还原后的画面均正常显示。

播放中执行 Cast -> Settings -> Cast：

- Cast 控件在 Settings 活动时从完整 UIA 树移除；
- 视频 surface 保持存活；
- 返回 Cast 后画面继续显示，无黑屏；
- 日志没有附加的 `Render context attached`，generation 仍为 1。

移动/resize 实测日志：

```text
Window move/resize entered; rendering suspended
Window move/resize exited; rendering resumed
Render target generation=1 framebuffer=1 pixels=1031x936
```

该路径只影响呈现唤醒；播放、position、媒体和 render context 都保持。

## 7. 已修复问题

### 7.1 renderer 投影尺寸陈旧

修复前：

- FBO 已随窗口变化；
- `RendererProjection.lastWidth/lastHeight` 保持初次 ready 值；
- 无法从投影或审计日志证明 resize 成功。

修复后：

- render target 变化时发布一次 renderer projection；
- 高频 resize 由 pending 标记合并；
- 每帧仍只调用 MPV，不增加 UI 树刷新；
- UI 日志只在状态或 render target 真变化时写出。

### 7.2 诊断日志每帧刷屏

修复前，同一 Host 诊断以约 60 次/秒重复写出，既掩盖关键事件又增加 UI 线程
字符串和文件 I/O。

修复后只在以下事实变化时记录：

- Control 连接/renderer lost/fatal reason；
- renderer ready；
- generation/framebuffer/pixel size。

### 7.3 拖边缩放没有进入呈现冻结边界

原实现监听 `WM_MOVING`，只覆盖拖动窗口，不覆盖 `WM_SIZING`。现已改为：

```text
WM_ENTERSIZEMOVE -> suspend presentation
WM_EXITSIZEMOVE  -> DefSubclassProc -> resume presentation -> one final frame
```

因此移动和 resize 共用同一个平台模态边界，且恢复发生在 Windows 处理完最终
几何之后。该变化不停止 MPV，不卸载媒体，也不修改播放语义。

## 8. 保留缺口

以下问题不在本次审计提交中混入语义修改：

1. `FrameRendered` 目前表示 `mpv_render_context_render` 成功，Avalonia 的最终
   compositor present 没有单独 callback。`UiFirstFrame` 仍由 Render 优先级调度
   延后上报，但需要继续保留为已知平台边界。
2. render 返回失败目前累计 `renderFailures`，但尚未按连续失败达到 context
   lost 的失败关闭阈值。此项归入后续 OpenGL context lost 阶段。
3. 真正销毁 GL context 后 `mpv_render_context_free` 的行为需要单独故障验证；
   当前不得增加自动重建或第二 renderer。

## 9. 闸门结论

MPV 重绘架构可以继续后续阶段：

- render context 所有权唯一；
- resize 不重建 context；
- 页面切换不销毁 context；
- 移动冻结不改变播放语义；
- renderer 投影能证明 FBO 变化；
- 高频投影与 render callback 已分离。

本阶段没有修改冻结的 Protocol/Shell/wire/owner 或播放命令语义。
