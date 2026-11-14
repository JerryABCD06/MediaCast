# 状态所有权与 IPC 适配

基线：`6c990a3`

## 1. 唯一所有权

| 状态 | 唯一所有者 | Avalonia 权限 |
|---|---|---|
| 语言、主题、强调色、Mica | Shell | 读取快照、提交 patch |
| 接收开关、广播、广播间隔 | Shell，运行态交给 Protocol | 读取快照、提交 patch |
| 设备名、UDN | Shell | 只读 |
| owner、pending claim、session epoch | Protocol | 只读 |
| `peerConnected`、当前投送方 | Protocol | 只读 |
| 当前/下一条媒体和协议历史 | Protocol | 只读 |
| 逻辑 transport | Protocol | 只读 |
| current media epoch/load generation | PlaybackController | 只读 |
| 实际播放状态 | PlaybackController | 发语义命令，读结果 |
| position、duration、media size | MPV | 发 Seek，读事实 |
| volume、mute、picture value | MPV | 发输入，读事实 |
| media tracks / codec / container | MPV | 只读 |
| MPV handle / render context | native UI host | Avalonia 只调用受控 render ABI |
| route、overlay、focus、input mode | Avalonia | 唯一写入者 |
| 全屏和窗口状态 | Avalonia 窗口层 | Avalonia 唯一写入者 |
| slider 手势 preview | Avalonia 手势层 | 手势结束即失效 |

一个事实只能有一个名字。以下都属于重复状态：

```text
PeerConnected
CastConnected
Presentation.Connected
ViewModel.IsConnected
```

以下也属于重复状态：

```text
PlaybackController.State
ViewModel.PlaybackState
MediaBar.LocalPlayingFlag
```

View 可以绑定最后一份只读投影，但不得把投影回写为播放事实。

## 2. 进程内结构

```text
MCast.Ui.exe
├─ Avalonia UI runtime
│  ├─ Views
│  ├─ immutable projection consumers
│  ├─ focus / route / overlay / gesture state
│  └─ Avalonia OpenGL surface
│
├─ Qt-free native host
│  ├─ UiSession
│  ├─ UiSettingsModel
│  ├─ PlaybackController
│  ├─ MpvCore
│  ├─ PlaybackBridge
│  ├─ UiConfirmationBridge
│  ├─ WindowsMediaControls
│  └─ projection / render ABI
│
└─ libmpv
   ├─ event thread
   └─ render context
```

原生宿主仍在 UI 进程内，不新建第四进程。它不拥有网络会话，也不替 Protocol
判断 owner。它只承担 Qt-free 生命周期编排、线程切换、播放核心和 ABI 投影。

## 3. 启动顺序

```text
Ui Control Hello
Shell HelloAck
Shell LanguageCatalog
Shell SettingsSnapshot
Shell ProtocolIdentityGrant
Ui Playback Hello
Ui Playback HelloAck
Ui SessionSnapshotRequest
Protocol SessionSnapshot
Create native playback core
Create Avalonia window
First Avalonia frame completed
UiFirstFrame
PlaybackUiReady
```

不可交换或合并的边界：

- `UiFirstFrame` 表示主窗口已经完成真实首帧;
- `PlaybackUiReady` 还要求 MPV renderer ready 和 PlaybackBridge ready;
- confirmation-only 不创建主窗口、MPV 或 PlaybackBridge，不能复用主窗口首帧门;
- 没有 PlaybackUiReady 时 Protocol 不得下发普通媒体命令。

## 4. Avalonia 到 native host

Avalonia 只通过语义 ABI 发送用户意图：

```text
TogglePlayPause
SeekAbsolute(seconds)
Previous
Next
SetVolume(percent)
SetMuted
SetPictureControl(name, value)
ResetPictureControls
BeginControlAdjustment
EndControlAdjustment
RequestEndCast
PauseAndFlushForWindowClose
SetLanguage
SetThemeMode
SetAccentMode
SetMica
SetAcceptNewCast
SetBroadcast
SetBroadcastInterval
RespondCastConfirmation
NotifyFirstFrame
RenderAttach / RenderFrame / RenderDetach
```

这些调用不回传自造的“成功”状态。命令结果从之后的
`PlaybackProjection`、`ShellProjection` 和日志观察。

## 5. native host 到 Avalonia

快照按域发布：

```text
ShellProjection
PlaybackProjection
RendererProjection
CastConfirmationProjection
```

每个快照必须自带版本或代次：

```text
settingsVersion
playback generation
playback stateVersion
renderContextGeneration
media/load epoch
```

Avalonia 必须丢弃旧代次事件，不能把上一个媒体或上一轮连接的迟到结果应用到
当前页面。

### 5.1 PlaybackProjection

最低字段集：

```text
state
loadStatus
playerAvailable
hasMedia
paused
peerConnected
castState
showsPicture
mediaIsVideo
currentMedia
nextMedia
hasNext
hasPrevious
castSession
positionSeconds
durationSeconds
volumePercent
muted
pictureControls
mediaInfo
generation
stateVersion
```

`peerConnected` 和 `castSession` 只来自 Protocol 驱动的事实。Avalonia 不能根据
“标题存在”“画面存在”或“按钮点过”反推连接。

### 5.2 RendererProjection

最低字段集：

```text
playerRunning
rendererReady
renderContextGeneration
contextLost
renderCalls
renderFailures
lastFramebuffer
lastWidth
lastHeight
glBackend
```

`contextLost=true` 是 fail-closed 事实。UI 不得继续显示“已就绪”，也不得自行
创建第二个播放器来掩盖。

## 6. Protocol 与 Playback 的冻结方向

Protocol 到 UI 的播放命令语义不因 Avalonia 改变：

```text
Open / Play / Pause / Stop / Seek / Next / Previous / EndSession
SetNext / SetPlayMode / SetVolume / SetMuted
SetPictureControl / ResetPictureControls
SetPeerConnected / SetCastSession
```

UI 到 Protocol 的播放事实不因 Avalonia 改变：

```text
PlaybackCommandResult
PlaybackStateDelta
PlaybackPlayerLost
PlaybackReconnected
PlaybackSnapshotResponse
UiEndCastRequest
PlaybackUiReady
```

UI 的本地按钮直接进入 PlaybackController，由既有桥接层回报 Protocol；它不能
伪造另一条“UI 播放命令”协议，也不能直接结束投送。

## 7. 命令顺序与合并

顺序边界：

```text
Open
Stop
Next
Previous
EndSession
SetNext
SetPlayMode
ResetPictureControls
SetPeerConnected
```

允许由既有 PlaybackPipe/Coalescer 合并：

```text
Play / Pause
Seek
Volume
Mute
同名 PictureControl
CastSession
```

Avalonia 不自行合并或重排协议命令。输入层只负责阻止同一手势重复提交，并把
高频调节交给既有合并边界。

## 8. 生命周期与失败路径

### 8.1 正常关闭

```text
用户关闭窗口
  -> 停止接收新的 UI 意图
  -> 必要时 pause + flush
  -> detach renderer
  -> 停止 PlaybackBridge 读/写线程
  -> 关闭 Control/Playback 连接
  -> 停止 mpv event thread
  -> 退出 Avalonia
```

不得先卸载 Avalonia 窗口再让 native host 访问已销毁的 render callback。

### 8.2 ControlPipe 断开

ControlPipe 是 UI 生命周期边界。读取失败或服务端关闭属于 fail-closed：

```text
停止接收 UI 写操作
停止 Projection 更新
断开 playback 协作
记录 PID/代次/错误
退出 Ui
```

不能把 ControlPipe 断开降级成“离线设置页”或继续播放。

### 8.3 PlaybackPipe 单边断开

```text
Protocol 检测旧连接断开
  -> 旧 gateway 释放连接所有权
  -> 创建新 listener
  -> ControlPipe 通知现有 Ui
  -> Ui 单飞重连 PlaybackPipe
  -> Ui 发 PlaybackReconnected
  -> Ui 再发 PlaybackUiReady
```

Ui PID、主窗口、MpvCore 和 render context 保持不重建。重连并发必须只有一项，
重复通知合并成一次待执行请求。

当前干净基线的 Protocol 重连释放顺序可能阻止同名 listener 立即重建。该项属于
Protocol 核心修改，必须先单独立项并获用户确认，不能以 UI workaround 绕过。

### 8.4 player lost

MPV 进程内播放能力丢失时：

- `playerAvailable=false`;
- 停止下发普通媒体命令;
- 只允许 EndSession 和必要的会话事实更新;
- 记录导致丢失的具体错误;
- 不自动创建第二个 renderer 或第二个 mpv handle。

### 8.5 renderer detach / context lost

surface 被移出视觉树或 GL context 丢失时，native host 必须：

```text
清除 update callback
停止新的 RenderFrame
释放该 context 的 mpv render context
rendererReady=false
PlaybackUiReady 不再成立
根据失败类型等待重新 attach 或失败关闭 Ui
```

页面切换不得制造这个状态。Cast/设置切换必须保留 Cast 页的渲染表面。

## 9. 设置与画面值

设置权威链：

```text
Avalonia intent
  -> native UiSettingsModel
  -> coalesced JSON patch
  -> ControlPipe
  -> Shell validation/persistence
  -> SettingsUpdateResult
  -> ShellProjection
```

画面值链：

```text
Avalonia gesture
  -> PlaybackController
  -> MPV property/filter
  -> MPV value event
  -> PlaybackProjection
  -> 持久化 patch 到 Shell
```

两者不能混成一条：

- Avalonia 不写设置 JSON;
- Shell 不直接操纵 MPV;
- 高频拖动可以在播放器内实时生效，但持久化必须合并;
- `SavedRuntimeFailed` 表示已保存、运行态应用失败，UI 不回滚为旧值;
- 只有 `Rejected` 才恢复最后一份 Shell 权威快照。

## 10. 文本边界

Protocol、wire、JSON 和 Avalonia 之间统一使用 UTF-8 字符串：

```text
std::string UTF-8
  -> native ABI UTF-8
  -> System.Text.Json / System.String
  -> Avalonia Text
```

禁止：

- 用本地 ANSI 代码页转换;
- 按 `QChar`/UTF-16 code unit 截断后再判断;
- 对 URI 做不必要的重新序列化;
- 在 UI 中重新解码 DIDL XML;
- 用显示文本倒推协议身份或媒体来源。

文本过滤只改变“是否展示”和“展示哪一份”，不能改变 Protocol 发来的原始事实。

## 11. 禁止模式

- Avalonia 内自定义播放快照作为权威;
- C# 再实现 MpvPlaybackEngine;
- C# 再实现图片序列或 Wrapped-TS 代理;
- 用 Fake gateway 充当生产回退;
- `peerConnected` 和任何 UI 连接状态并存;
- UI 解析 DLNA/DIDL;
- UI 自行推进下一首;
- UI 保存最终 volume/mute;
- 右键、键盘菜单键、指针事件各建一套菜单逻辑;
- 页面切换时重建 MpvCore 或 render context;
- 为局部视觉问题复制状态、绕开所有权或修改 wire 语义。

