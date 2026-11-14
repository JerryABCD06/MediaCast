# Qt-free 迁移最终验收

日期：2026-11-14

验收代码祖先：`9643110`

环境：Windows x64，MSVC 19.51，CMake/Ninja，Avalonia 12.1.6

## 1. 交付结论

Qt-free 主链已经从干净基线 `6c990a3` 重建并跑通：

```text
MCast.Shell.exe
  -> MCast.Protocol.exe
  -> MCast.Ui.exe
       -> .NET / Avalonia
       -> MCast.AvaloniaHost.Native.dll
            -> UiSession
            -> PlaybackController
            -> MpvCore
            -> PlaybackBridge
            -> WindowsMediaControls
            -> Avalonia OpenGL surface
```

默认生产构建：

```text
MCAST_BUILD_LEGACY_QML_UI=OFF
```

Qt/QML/FluentUI 旧 UI 不作为默认构建依赖，也不为本次 Qt-free 路径制作兼容层。

## 2. 不可破坏的所有权

| 事实/资源 | 唯一所有者 |
|---|---|
| peer owner、claim、session epoch | Protocol |
| peerConnected、当前/下一媒体 | Protocol |
| 实际 playback state | PlaybackController |
| position/duration/volume/mute/picture | MPV |
| mpv handle | MpvCore |
| mpv render context/generation | native Avalonia host |
| GL context/FBO/viewport | Avalonia |
| route/overlay/focus/gesture preview | Avalonia |
| language/theme/settings | Shell |
| Ui 进程启动和监督 | Shell |

UI 只消费 projection，并通过语义 ABI 发送意图。没有 C# 第二播放器、第二
renderer、owner 推断或播放命令重排。

## 3. 分阶段结果

### 3.1 MPV 重绘

提交：

```text
c6f1222 fix(ui): publish render target transitions
6a22002 fix(ui): freeze rendering during move and resize
```

结论：

- MPV 固定 `MPV_RENDER_API_TYPE_OPENGL`；
- Avalonia 自选 ANGLE/native GL 后端；
- 每代 GL context 最多一个 mpv render context；
- update callback 经 native latch 和 C# 单飞后请求一帧；
- projection 刷新与视频帧唤醒分离；
- 移动/resize 进入 `WM_ENTERSIZEMOVE` 后暂停呈现；
- `WM_EXITSIZEMOVE` 处理完成后请求最终几何一帧；
- resize 不改变媒体、position、MPV handle 或 render generation。

实测 FBO：

```text
generation=1 framebuffer=1 pixels=849x936
generation=1 framebuffer=1 pixels=1519x990
generation=1 framebuffer=1 pixels=1031x936
```

resize、最大化和还原期间 generation 始终为 1。

### 3.2 生命周期和关闭

提交：

```text
e6a8280 fix(ui): seal playback before window teardown
```

真实播放中点关闭，选择 `Pause and close`：

```text
传输状态 -> PAUSED
PauseAndFlushForWindowClose
Pause and state flush completed
Render context detached
Avalonia UI exiting
producers stopped
event loop drained
render context released
MPV event thread stopped
Avalonia native host stopped
```

PlaybackBridge 在 pause 状态提交后封口，GL detach 导致的本地 video-output
错误不会覆盖 Protocol 已接收的 pause 状态。

### 3.3 ControlPipe fail-closed

CTest：

```text
mcast_control_pipe_fail_closed
```

故障：

```text
MCAST_UI_TEST_DISCONNECT_CONTROL_MS=3000
```

Ui 收到不可恢复断管后：

```text
Host state controlConnected=False ... fatal=ControlPipe disconnected
failing closed
Avalonia shutdown
```

Ui 限时退出，不降级成离线 UI，也不继续播放。

### 3.4 PlaybackPipe 重连

CTest：

```text
mcast_playback_pipe_reconnect
```

故障：

```text
MCAST_UI_TEST_DISCONNECT_PLAYBACK_MS=3000
```

结果：

```text
PlaybackPipe disconnected
Playback UI ready
PlaybackPipe reconnected
Ui playback reconnect completed
```

不变量：

- Ui PID 不变；
- MainWindow 首次帧只发生一次；
- MpvCore 和 render context 不重建；
- `PlaybackUiReady` 第二次发送；
- 播放位置继续增长。

同时修复了 Ui 已经退出时仍尝试 PlaybackPipe reconnect 的问题：

```text
Ui process exited; PlaybackPipe reconnect skipped
```

### 3.5 playerAvailable / rendererReady

提交：

```text
f5ce2aa fix(ui): gate playback intents on player readiness
```

UI 播放控件只在以下条件同时成立时启用：

```text
Playback.PlayerAvailable
Renderer.RendererReady
!Renderer.ContextLost
```

native UI host 也会拒绝普通播放意图；EndCast 不受播放器坏掉阻挡。

### 3.6 换片摘要

提交：

```text
e8a152a test(mpv): reject stale summary after failed load
```

覆盖：

```text
A 已加载
A -> B 快速换片
A 的迟到 duration/track-list
B 加载成功
B 加载失败
```

旧 epoch 摘要不能覆盖 B；新片加载开始时 summary 立即清空；失败后保持空摘要。

### 3.7 UTF-8 / JSON / URI

已验证：

- 合法 UTF-8 原样穿过 C++、native ABI、System.Text.Json 和 Avalonia；
- JSON `\uD83D\uDE00` surrogate pair 变成一个 UTF-8 scalar；
- 未配对 surrogate 拒绝；
- overlong/非法 UTF-8 拒绝；
- JSON 深度限制；
- percent-decoded 中文 URI 标题；
- scheme/host/default port/percent escaping 等价比较；
- `file:` 中文封面 URI 编码。

UI 不按 UTF-16 code unit 截断协议文本，也不重新解码 DIDL。

### 3.8 画面设置和封面

画面值：

```text
Avalonia gesture
  -> PlaybackController/MPV
  -> 250ms 合并持久化 patch
  -> Shell
  -> SettingsUpdateResult
```

拖动实时作用于 MPV；松开提交最终值；拒绝时回滚权威快照；新 SettingsSnapshot
会重新应用持久化画面值。

封面提交：

```text
3e162cb fix(media): validate and stream local artwork
```

- Protocol 封面只接受 HTTP(S)；
- 本地封面必须是普通文件、存在且不超过 16 MiB；
- 缺失文件、目录、超大文件拒绝；
- SMTC 本地封面通过 `StorageFile -> RandomAccessFileReference`，
  不把 `file:` URI 直接交给 `CreateFromUri`。

### 3.9 OpenGL context lost

提交：

```text
827705b fix(ui): fail closed on render context loss
```

CTest：

```text
mcast_opengl_context_lost_fail_closed
```

事实链：

```text
Test fault: losing OpenGL context
Render context lost
Host state rendererLost=True
failing closed
Avalonia native host stopped
```

attach 失败、render 失败和 `OnOpenGlLost` 都进入同一 fail-closed，不会静默留黑
或自动创建第二个 renderer。

### 3.10 confirmation-only

CTest：

```text
mcast_confirmation_only_smoke
```

轻量 Ui 启动后日志中不存在：

```text
Render context attached
MPV:
Windows media controls attached
PlaybackBridge
```

Shell 自动冷启动仍需要修改冻结的 Shell/ControlPipe ready 契约，本次没有越过
边界实施。

### 3.11 Wrapped-TS

提交：

```text
bad4659 fix(wrapped-ts): honor suffix and invalid ranges
```

代理保持：

- 固定 4 worker；
- pending 32 上限；
- 64 MiB 按字节 LRU；
- 有界 WinHTTP/socket 超时；
- stop token 和 listener/active/pending socket 所有权；
- `MpvCore::unload()` 释放 proxy。

HTTP Range 现支持：

```text
bytes=0-15       -> 206
bytes=-16        -> 206，最后 16 字节
越界             -> 416
start > end      -> 416
非法 Range       -> 416
```

### 3.10 冷启动时 Open 必须等待 PlaybackUiReady

提交：

```text
e4acc83 fix(protocol): keep cold-start open pending until UI ready
```

真实故障：

```text
23:24:20.295 SOAP AVTransport#SetAVTransportURI
23:24:20.941 Pending DLNA open submitted after reconnect
23:24:21.053 HLS compatibility plan: wrapped transport stream
23:24:21.053 DLNA request failed: playback UI is not ready
23:24:21.932 Playback UI ready
```

根因不是 MPV 加载完成消息丢失，而是 `PlaybackPipeGateway::snapshot()` 在 UI
首帧前已经报告 `playerAvailable=true`。Protocol 的 `PlanningPlaybackGateway`
因此提前启动兼容性规划；规划完成后，上游才返回 `WaitingForPlayer`。此时
DLNA Session 只收到异步的 `CommandCompleted/Rejected`，无法再把“暂时未就绪”
与真正失败区分开，最终清除了 pending Open。

修复只改变 Protocol 消费该连接时的可用性投影：

```text
PlaybackPipe connected
  + UI player snapshot present
  + PlaybackUiReady not received
  -> snapshot.playerAvailable=false

PlaybackUiReady received
  -> snapshot.playerAvailable follows the authoritative Ui snapshot
```

因此顺序固定为：

```text
SetAVTransportURI
  -> Open held as pending
  -> PlaybackUiReady
  -> compatibility planning starts once
  -> Open
  -> resulting Play in deferred order
```

不改变 pipe 名称、消息类型、wire 字段、generation/stateVersion、owner、
PlaybackUiReady 含义或 Open/Play 顺序。Ui 快照本身仍是播放事实权威；
Protocol 只是在命令门尚未打开时把它投影为不可接收命令。

回归覆盖：

- UI 快照先到、`PlaybackUiReady` 后到时，命令提交必须返回等待；
- `PlaybackUiReady` 到达后，同一快照恢复为可接收命令；
- UI 未就绪时不得启动兼容性规划；
- 同一 Open 在就绪后只启动一次规划并到达上游；
- PlaybackPipe 重连、DLNA Session、SOAP、Runtime 和 Qt-free PlaybackBridge
  既有行为保持不变。

## 4. 真实交互

lavfi 60 fps 视频经真实三进程 DLNA shadow E2E：

- 视频上屏；
- Pause/Play 在约 1 秒内反馈；
- Cast -> Settings -> Cast 保持暂停帧，无黑屏；
- 最大化、还原和真实 Size 模态循环正常；
- 关闭后 Ui 进程退出；
- Settings 活动时 Cast 控件离开 UIA 树；
- 返回 Cast 后控件恢复。

## 5. 测试结果

Debug：

```text
50/50 tests passed
```

Release：

```text
50/50 tests passed
```

命令：

```powershell
ctest --test-dir out/build/x64-Debug --output-on-failure
ctest --test-dir out/build/x64-Release --output-on-failure
```

新增故障/模式测试：

```text
mcast_control_pipe_fail_closed
mcast_playback_pipe_reconnect
mcast_opengl_context_lost_fail_closed
mcast_confirmation_only_smoke
```

## 6. Release 产物

目录：

```text
out/build/x64-Release
```

关键文件：

```text
MCast.Shell.exe
MCast.Protocol.exe
MCast.Ui.exe
MCast.AvaloniaHost.Native.dll
libmpv-2.dll
```

`dumpbin /DEPENDENTS` 检查：

- Shell、Protocol、Ui 和 native host 无 Qt6 import；
- native host 依赖 `libmpv-2.dll`；
- `libmpv-2.dll` 与 native host 已在同一发布目录。

## 7. 提交清单

本次收口主要提交：

```text
c6f1222 fix(ui): publish render target transitions
6a22002 fix(ui): freeze rendering during move and resize
e6a8280 fix(ui): seal playback before window teardown
66329ac docs(avalonia): record redraw and close contracts
b65d1dc test(ui): require ControlPipe fail-closed exit
ccd44f0 fix(protocol): release old playback pipe before reconnect
f23a192 test(ui): verify PlaybackPipe reconnect invariants
f5ce2aa fix(ui): gate playback intents on player readiness
e8a152a test(mpv): reject stale summary after failed load
3e162cb fix(media): validate and stream local artwork
827705b fix(ui): fail closed on render context loss
bad4659 fix(wrapped-ts): honor suffix and invalid ranges
d6f5c4e test(ui): cover lightweight confirmation-only mode
c986d34 fix(protocol): skip reconnect after Ui exit
9643110 test(shell): align handoff wait with protocol timeout
e4acc83 fix(protocol): keep cold-start open pending until UI ready
```

## 8. 明确未纳入

- 纯 UI 本地化；
- 全文媒体栏；
- About / Legal / Licenses 页面；
- 旧 Qt/QML/FluentUI 删除、迁移和兼容；
- confirmation-only Shell 自动冷启动；
- 真实显卡驱动级 context loss、多显示器混合 DPI、触摸和完整讲述人；
- 视觉像素级还原。

这些项目不改变上述主链所有权，但仍是独立产品或后续验收项。
