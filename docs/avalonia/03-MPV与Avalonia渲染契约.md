# MPV 与 Avalonia 渲染契约

基线：`6c990a3`

## 1. 结论

```text
Avalonia rendering backend:
  由 Avalonia 自行选择，生产首选其默认 ANGLE 路线

MPV rendering API:
  MPV_RENDER_API_TYPE_OPENGL，固定不变
```

Avalonia UI 框架决定如何合成自己的窗口；MPV 只接受 Avalonia 提供的 OpenGL
上下文、FBO 和 viewport。两者不是两套 UI 状态机，也不是两条媒体渲染管线。

不得强制 Avalonia 回到 WGL。目标机上曾经真实复现：

```text
WGL + 播放中 resize
  -> 画面频闪
  -> 再次改变尺寸有时暂时恢复
```

该现象属于呈现层失败。禁止用 `glFinish`、额外 flush、延迟、重复 swap 或定时
重绘来掩盖。ANGLE 路线通过真实 resize 和全屏验证后才可以作为生产路线。

## 2. 状态归属

| 对象/事实 | 唯一所有者 | Avalonia 看到什么 |
|---|---|---|
| mpv handle | `MpvCore` | 无，直接不可见 |
| 当前媒体、播放状态 | PlaybackController | 只读 projection |
| position/duration/volume/mute | MPV | 只读 projection |
| media/track info | MPV | 只读 projection |
| render context | native render adapter | attach/render/detach 的受控 ABI |
| GL context | Avalonia | `OnOpenGlInit` 提供 |
| framebuffer id | Avalonia | `OnOpenGlRender` 参数 |
| viewport/FBO 像素尺寸 | Avalonia surface | 由 Bounds × RenderScaling 计算 |
| context generation | native render adapter | 只读 `RendererProjection` |
| 页面 route | Avalonia | 不得影响 render context 所有权 |
| window state | Avalonia 窗口层 | 不得修改 MPV 播放语义 |

## 3. 渲染线程

所有 GL 访问只发生在 Avalonia 提供的 OpenGL 回调中：

```text
OnOpenGlInit
OnOpenGlRender
OnOpenGlDeinit
OnOpenGlLost
```

约束：

- 一个 GL context 只对应一个 mpv render context;
- render context 创建和每帧渲染只在当前 GL context 有效时执行;
- mpv update callback 只投递“需要下一帧”的唤醒，不直接访问可销毁的 View;
- UI 线程不同步等待 render;
- render 与 detach 不能并发释放;
- context 改变时只能有一个新 owner，不允许两个 renderer 短暂并存。

## 4. 每帧顺序

```text
Avalonia asks surface to render
  -> compute pixel viewport from bounds and scaling
  -> call mpv_render_context_update
  -> if a frame is needed:
       mpv_render_context_render(FBO, width, height, flipY)
  -> observe result
  -> first successful actual frame may mark renderer ready
```

投影刷新与视频帧唤醒必须分离：

- MPV render callback 只负责请求下一帧视频呈现;
- position/volume/state 等 projection 更新走有界定时合并;
- 不能让每帧 render callback 触发整套 JSON 投影和 UI 树刷新;
- 不能让 projection 刷新反过来无限驱动无意义的视频重绘。

## 5. 首帧与就绪

首帧需要区分三个事实：

```text
WindowOpened
FirstRenderedFrame
PlaybackUiReady
```

定义：

- Avalonia 窗口完成真实首帧后才发送 `UiFirstFrame`;
- `FirstRenderedFrame` 至少经过一次真实渲染并通过平台呈现边界;
- `PlaybackUiReady` 还必须同时满足：

```text
UiFirstFrame sent
MpvCore running
mpv render context attached
at least one successful render
PlaybackBridge connected
```

不得用控件构造完成、窗口 `Opened`、日志定时器或截图冒充渲染首帧。

## 6. resize

resize 只改变：

```text
surface bounds
render scaling
viewport width/height
FBO width/height
```

resize 不应改变：

```text
current media
MPV handle
render context generation
playback position
paused/playing state
```

每次实际尺寸变化记录：

```text
logical bounds
render scaling
framebuffer pixels
framebuffer id
render result
context generation
```

验收要求：

- 视频播放时拖边 resize 不频闪;
- 尺寸连续改变后画面比例正确;
- 最大化、还原、全屏往返不黑屏;
- 跨显示器时 scaling 更新;
- resize 不造成媒体重新 load。

## 7. 页面切换

Cast 页和 Settings 页切换时，播放表面必须保持存活：

```text
Root
├─ persistent video surface
├─ Cast overlay / controls
└─ Settings panel
```

推荐方式：

- video surface 始终留在视觉树;
- 非活动页面不接收输入、不进入可访问性树;
- Settings 覆盖在视频表面上或独立占位，但不销毁 surface;
- 切回 Cast 后直接恢复 UI 控件，不重建 mpv/render context。

如果某个 Avalonia 控件在 `IsVisible=false` 时仍会 detach OpenGL surface，必须
用真实日志验证；任何会触发 detach 的隐藏方式都不能用于页面切换。

## 8. 窗口移动

用户已确认拖动窗口时冻结视频画面是可接受方案，但“冻结呈现”不等于“暂停播放”：

```text
WM_ENTERSIZEMOVE
  -> suspend video presentation wakeups
  -> keep media/position/audio state

WM_EXITSIZEMOVE
  -> resume render wakeups
  -> render current MPV frame at final geometry
```

禁止在移动期间停止 MPV、卸载媒体或重建 context。

## 9. 全屏

优先使用 Avalonia 自己的窗口全屏能力，不移植 QML 的 Win32 兼容补丁。

验收顺序：

```text
普通 -> 全屏 -> 普通
普通 -> 最大化 -> 全屏 -> 普通/最大化
全屏 -> Esc
全屏 -> 最小化 -> 恢复
全屏 -> 跨显示器
全屏 -> 关闭窗口
全屏 -> 断开投送
连续 20 次往返
```

每次记录：

- Avalonia WindowState;
- 实际客户区尺寸;
- RenderScaling/DPI;
- surface 和 FBO 尺寸;
- position 是否连续;
- render context generation;
- 是否出现黑帧、透明帧、重复 swap。

只有 Avalonia 标准全屏真实失败时，才把平台层补丁作为独立核心/平台事项提出。

## 10. context lost

必须区分：

```text
surface temporarily detached
GL context changed
GL context permanently lost
render call failed
player lost
```

处理原则：

- 页面切换不得触发;
- resize/全屏不应触发;
- attach callback 消失时停止 render;
- context lost 后不得继续使用旧 handle;
- fail-closed 事实必须暴露到 `RendererProjection`;
- 若无法在同一所有权约束下安全恢复，退出 Ui 并由 Shell 启动新一代;
- 不通过 UI 自动重载媒体来隐藏 renderer 故障。

已知目标机风险：主动重建 mpv render context 可能让视频输出初始化失败并把媒体
拉回起点。因此“context 更换后无缝重建并保持 position”必须作为独立 spike；
在未证明前，生产策略是失败关闭，不是暗中重建。

## 11. 日志

必须能从一个日志序列证明：

```text
render context owner
context generation
first frame
resize
fullscreen transitions
page transitions
move freeze/resume
render failures
context detach/lost
PlaybackUiReady timing
```

最少字段：

```text
timestamp
pid
context generation
gl backend/vendor/renderer
framebuffer
logical/physical size
scaling
render result
playback state
position
```

日志不能把每一帧都写成无界文件；render 成功计数可以聚合，失败和代次变化必须
逐条记录。

## 12. 闸门

以下任一条件不满足，不进入生产 UI 页面扩展：

- Avalonia 默认/ANGLE 路线在目标机稳定;
- MPV 固定 OpenGL 且没有自动后端降级;
- 真实 resize 无永久黑屏或频闪;
- 20 次全屏往返正常;
- 页面往返不销毁 render context;
- 停止、暂停、恢复、自然结束、再次投送正常;
- 首帧门有可解释证据;
- context lost 有明确失败关闭路径;
- 没有新增 C# 播放状态机或第二个 renderer。

