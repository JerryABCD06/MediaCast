# 核心改动提案：PlaybackPipe 重连前释放旧实例

日期：2026-11-14  
状态：已实施并验证

## 1. 原契约

```text
PlaybackPipe 单边断开
  -> Protocol 失败关闭旧 PlaybackPipeGateway
  -> 创建同名新 listener
  -> 经 Shell 通知现有 Ui
  -> Ui 单飞重连并发送完整状态
  -> Ui 再发 PlaybackUiReady

Ui PID、窗口、MpvCore、媒体、播放位置和 render context 不重建。
```

Ui 侧的断管检测、单飞重连入口和完整快照已经实现并有测试。

## 2. 真实故障证据

测试方法：

```text
真实 Shell + Protocol + Avalonia Ui
设置 MCAST_UI_TEST_DISCONNECT_PLAYBACK_MS=3000
UI 主动关闭 PlaybackPipe client
```

Ui 日志：

```text
Test fault: dropping PlaybackPipe
PlaybackBridge: Playback IPC 读取失败：
  GetOverlappedResult failed with Win32 error 995
```

Protocol 日志：

```text
Playback pipe disconnected:
  GetOverlappedResult failed with Win32 error 109
PlaybackPipe disconnected; requesting Ui reconnect
Ui playback reconnect attempt 1 failed: playback pipe listener failed
Ui playback reconnect attempt 2 failed: playback pipe listener failed
Ui playback reconnect attempt 3 failed: playback pipe listener failed
Ui playback reconnect failed: playback pipe listener failed
```

这说明失败发生在 Protocol 创建同名新 listener，而不是 Ui 握手。

## 3. 根因

`protocolPlaybackServer()` 使用
`FILE_FLAG_FIRST_PIPE_INSTANCE` 创建 Pipe listener。

`src/skeleton/SkeletonClient.cpp` 的 playback watcher 仍持有：

```text
[playback, ...]                         // lambda 的 by-value capture
auto currentPlayback = playback;       // 再复制一份
```

即使 `activePlayback` 已 reset，旧 gateway/connection 仍由这两个
`shared_ptr` 持有。随后创建同名 `FIRST_PIPE_INSTANCE` listener 时，旧 pipe
服务端实例尚未析构，所以三次尝试都会报：

```text
playback pipe listener failed
```

因此只 reset `currentPlayback` 还不充分；by-value capture `playback` 也必须
在确认旧实例不再是 active 后释放。

## 4. 提议改动

只调整 Protocol 内部释放顺序：

```text
playbackGateway->detach(waitReason)
确认 currentPlayback 仍是 activePlayback
从 activePlayback 移除
currentPlayback.reset()
playback.reset()
再创建同名新 listener
```

不改变：

- pipe 名称；
- message type；
- generation / stateVersion；
- snapshot；
- owner / claim；
- Ui 重连协议；
- Shell 转发方式；
- EndSession / Stop 语义。

## 5. 所有权不变

| 状态/动作 | 所有者 |
|---|---|
| PlaybackPipe listener | Protocol |
| 旧连接失败关闭 | Protocol |
| reconnect request 转发 | Shell |
| PlaybackBridge 重连 | Ui |
| 当前媒体、MpvCore、render context | 原 Ui 实例 |

## 6. 必须保持的旧行为

- Ui 不存在时不重连；
- reconnect request 重复只保留一个 owner；
- 新 listener ready 后 Ui 才能连接；
- 新连接收到 `PlaybackReconnected` 完整快照；
- 旧媒体、位置、窗口和 MPV 不重建；
- 旧 generation 的迟到消息不能穿过新连接；
- reconnect 期间 EndSession 不丢失；
- 所有失败继续 fail-closed。

## 7. 必须补的回归

```text
断管时正在播放视频
  -> Protocol listener 重建成功
  -> Ui reconnect completed
  -> Ui PID 不变
  -> MainWindow 不变
  -> renderContextGeneration 不变
  -> PlaybackReconnected 完整快照
  -> 再次收到 PlaybackUiReady
  -> 播放位置继续
```

还要覆盖：

- 连续两次断管；
- listener 仍创建失败；
- Ui 在 listener ready 前尝试连接；
- reconnect 期间关闭 Ui；
- reconnect 期间投送结束；
- ControlPipe 不可用。

## 8. 请确认

请批准修改：

```text
src/skeleton/SkeletonClient.cpp
```

只改变旧 PlaybackPipe 实例的释放时机。未批准前，不修改该文件。

## 9. 实施记录

提交：

```text
ccd44f0 fix(protocol): release old playback pipe before reconnect
f23a192 test(ui): verify PlaybackPipe reconnect invariants
c986d34 fix(protocol): skip reconnect after Ui exit
```

实施内容严格限制为释放时机：

```text
detach 旧 gateway
清除 activePlayback
currentPlayback.reset()
playback.reset()
再创建同名 listener
```

另外补齐了 UI 进程已经退出时不再发起无意义重连：若断开时 Ui PID 已终止，
Protocol 跳过 reconnect，重新进入持久模式的 `UiOpenReady` 门禁。

真实故障结果：

```text
PlaybackPipe disconnected
Playback UI ready
PlaybackPipe reconnected
Ui playback reconnect completed
```

验收同时证明：

- Ui PID 不变；
- render context 只 attach 一次；
- MainWindow 第一帧只出现一次；
- `PlaybackUiReady` 收到两次；
- 播放位置继续增长。

