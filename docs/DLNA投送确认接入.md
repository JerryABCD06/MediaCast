# DLNA 投送确认接入

最后更新：2026-11-12

本文固定真实 DLNA `SetAVTransportURI -> Play` 接入投送仲裁时的职责、时序和
超时边界。上层原则、生产策略和仲裁状态见
[投送仲裁与确认.md](投送仲裁与确认.md)。

## 1. 当前分层

```text
HTTP socket 线程
  HttpServer
    -> 解析 HTTP、记录来源 IP、分配 requestId
    -> async handler 返回 Completed 或 Pending

Protocol 单一仲裁执行上下文
  ProtocolControlDispatcher
    -> 唯一读取 ControlPipe
    -> 统一分配消息 ID 和等待 activation/confirmation

  DlnaClaimAdapter
    -> 判断动作是否明确要求开始投送
    -> 保存 SetURI/Play 候选事务；候选 Stop 不登记为可执行命令
    -> 唯一调用 CastClaimCoordinator

  DlnaClaimRuntime
    -> 保存 HTTP requestId 与 DLNA 事务的关联
    -> 等媒体打开完成后才放行 SetURI/Play

  DlnaClaimHttpHandler
    -> 复用 SoapRouter 的校验与查询路径
    -> 只异步接管 SetURI/Play/Stop 和受所有权约束的 AVTransport 动作

DLNA 协议执行层
  SoapRouter / DlnaSession
    -> Claim 已允许后才提交 Open、Play、Stop 等播放命令
```

`DlnaClaimAdapter` 不解析 SOAP，不持有 socket，也不直接发送 IPC。它只接受已经
抽象后的设备身份、来源地址和 `MediaRequest`。

`DlnaClaimRuntime` 同样不拥有 socket。它返回 HTTP completion、media execution
和 confirmation prompt，由 Protocol 组合根分别交给 `HttpServer`、播放层和
ControlPipe。

## 2. HTTP 请求与完成

`HttpServer` 现在支持两种 handler：

- 原有同步 handler 返回 `HttpResponse`；
- 异步 handler 返回 `HttpDispatchResult`，状态为 `Completed` 或 `Pending`。

HTTP 请求携带：

```text
requestId       当前 HttpServer 实例内非零单调 ID
peerAddress     由 getpeername() 得到的来源 IPv4
```

handler 返回 `Pending` 后，连接保持。确认线程或媒体准备线程只能调用
`completeRequest(requestId, response)` 把结果放入线程安全队列并唤醒网络事件循环；
真正的 requestId 校验、deadline 校验和 socket 写入都在调用 `pollOnce()` 的
网络 owner 线程完成。

网络 owner 线程合同：

- `listenLoopback`、`stop`、`pollOnce`、`localPort` 和 `activeConnections`
  必须由同一 owner 线程串行调用；
- `completeRequest` 可以从其他线程调用；
- 销毁或重启 `HttpServer` 前必须先停止所有 completion producer，再停止网络
  owner 线程；
- completion 队列最多容纳 `maxConnections` 项；
- 合法的 HTTP 半关闭不会被当作彻底断开；Pending 响应仍可在客户端
  `shutdown(SD_SEND)` 后写回。

`completeRequest()` 返回 true 只表示结果已被 completion 队列接受。对应请求可能
已经超时、断开、完成过或属于上一代服务器；这些最终状态只有网络 owner 线程能
判定。业务层仍必须使用自己的 Claim deadline 主动完成请求。

## 3. 超时分层

| 超时 | 默认值 | 负责范围 |
|---|---:|---|
| HTTP `requestTimeout` | 10 秒 | 接收完整请求，以及后续响应写回 |
| HTTP `pendingRequestTimeout` | 60 秒 | handler 已挂起后的网络层兜底 |
| Claim `confirmationTimeout` | 10 秒 | 用户确认窗口 |
| 旧会话 `retirementTimeout` | 5 秒 | 旧协议退场跟踪 |

主窗口不存在时，从手机建立联系到 UI/MPV 真正就绪可能需要数秒。因此：

- 不能把 HTTP 接收超时直接当作“确认 + 启动主窗口 + 准备 MPV”的总超时；
- 用户确认窗口仍严格受 Claim deadline 控制；
- 确认前挂起的 `SetAVTransportURI` 由 DLNA runtime 保存 requestId；
- Claim 拒绝或超时后，runtime 必须主动完成对应 HTTP，不能等 60 秒网络兜底；
- 用户允许后，应先完成所有权切换并继续 Open，再由协议适配层决定何时把
  `SetAVTransportURI` 的成功响应写回。

真实控制点能等待多久仍未实测，因此 60 秒只是保护值，不是目标等待时长。

## 3.1 ControlPipe 单 reader

`ProtocolControlDispatcher` 在 Protocol Hello 握手完成后接管整个
`PipeConnection`。此后它是该连接唯一允许调用 `readMessage()` 的组件。

- heartbeat、`UiOpenReady`、UI 激活和确认请求共用一个非零消息 ID 序列；
- 握手已使用 message ID 1，因此 dispatcher 从 2 开始；
- UI activation 分为 `AwaitingDecision / AwaitingGrant / Done`；
- `UiActivateDecision` 按 correlation ID 匹配，`PeerIdentityGrant` 按
  activation ID 匹配；
- cast confirmation 按 correlation ID 匹配，并再次校验 protocol 和 claimId；
- 已完成的响应不能被迟到或冲突的重复消息翻转；
- `UiOpenRequest` 只增加可等待事件，不在 reader 线程执行外部业务或 callback；
- read/write 失败统一进入一次性的 terminal failure，清空所有等待并唤醒
  `waitForTermination()`；
- terminal 后 `start()` 不会假装成功。

控制通道断开时，非 reader 的 Protocol 串行上下文必须调用
`DlnaClaimRuntime::controlLost()`，拒绝当前确认候选和正在准备媒体的全部 HTTP。
不能依赖日志 callback 代替这次语义通知。

## 3.2 HTTP requestId 与媒体打开

`DlnaClaimRuntime` 维护两层关联：

- 待确认候选：保存 SetURI 和 Play 的 HTTP requestId；
- 正在打开的媒体：保存 operation ID、session、epoch、mediaGeneration，以及
  SetURI/Play requestId。

行为：

- 首次投送即使不需要确认，HTTP 也等到 `mediaReady(operationId, true)` 后才成功；
- 慢 Open 期间到达的 Play 附着到 operation；
- 同一 owner 的新媒体会拒绝旧 operation 的 HTTP，并创建新的 operation；
- operation 完成后迟到的 `mediaReady` 返回 `staleOperation`；
- sessionReady 失败时 HTTP 必须失败，不能因为 mpv 已经完成打开就报告成功；
- ControlPipe 丢失时，确认候选和正在准备的媒体都 fail-closed。

## 3.3 SOAP 异步接管

`SoapRouter::parseRequest()` 与 `handleParsed()` 把原有校验和动作执行拆开。
`DlnaClaimHttpHandler` 复用同一个解析结果，不复制路径、SOAPAction、
namespace、InstanceID 和 XML 校验逻辑。

`DlnaHttpFrontend` 是完整 HTTP 面的纯路由层：GET 提供
`/description.xml` 和三份 SCPD，POST 原样交给 `DlnaClaimHttpHandler`。
`net::HttpServer` 同时提供 loopback 和 `listenAny()` 两种绑定方式；生产测试不再
依赖修改代码才能从局域网访问。

异步接管范围：

| 动作 | 行为 |
|---|---|
| `SetAVTransportURI` | 进入 Claim runtime，HTTP 保持 Pending |
| `Play` | owner 直接播放；候选跟随 SetURI 等待确认；未知来源 701 |
| `Stop` | owner 正常停止；候选/未知来源只返回成功但不停止旧投送 |
| `Pause/Seek/Next/Previous/SetNext/SetPlayMode` | owner 且无慢 Open 时同步执行，否则 701 |
| `Get*` 查询 | 保持原有同步 SoapRouter 行为 |
| RenderingControl / ConnectionManager | 保持原有同步行为 |

确认或媒体准备完成后，handler 根据原 requestId 生成对应 SOAP success/fault，
再通过 `HttpServer::completeRequest()` 放行。确认窗口和媒体打开不能在该
callback 内同步进行，也不能创建 UI；但当前生产实现仍允许 `playNow/stopNow`
在 HTTP owner 线程内等待 Playback IPC 最多 5 秒。这个等待是已知拥塞风险，
后续应改成“提交命令 + HTTP Pending + completion 异步回填”。

handler 内部串行化 `DlnaClaimRuntime` 和 requestId 映射；外部 callback 在锁外
执行，因此可以从 callback 安全地再次投递 handler 事件。入口另有一把递归
“操作锁”，覆盖“授权判断 -> callback”整段，防止两个并发请求在授权后插入新的
换片或抢占；runtime 状态锁只保护短临界区。

`DlnaClaimConfirmationWorker` 把确认等待移出 HTTP 线程。handler 的
`requestConfirmation` 只投递提示，后台线程通过可替换的
`DlnaClaimConfirmationPort` 等待 Shell/UI 决定，再调用
`confirmationDecision()`。Port 可以接 ControlPipe，而 DLNA 层本身仍不依赖 IPC。

Runtime 还会在媒体打开完成时复核 owner session、epoch 和 mediaGeneration；
所有权在这期间变化时，HTTP 必须失败，不能把迟到 Open 的成功算到新 owner 上。
慢 Open 有 10 秒 Runtime deadline，`tick()` 到期会结束 owner、拒绝关联 HTTP
并释放 operation，避免永久 Pending。

## 4. 明确 Claim

只有控制点明确表达“开始播放这个媒体”时才产生 Claim：

| 动作 | 是否 Claim |
|---|---|
| `SetAVTransportURI` | 是 |
| 播放器准备完成后的 `Play` | 属于已有事务，不再创建第二个 Claim |
| `SetNextAVTransportURI` | 否 |
| 发现、GET、`Get*` 查询、GENA 订阅 | 否 |
| 找不到明确所有者的 `Stop/Pause/Seek/Next/Previous` | 否，且不能操作旧 owner |

来源 IP 只用于 DLNA 内的 best-effort 关联。新建媒体仍要求完整 `peer.id` 和来源
IP，空值 fail-closed。`peer.id` 由协议 runtime 从来源地址和控制通道能稳定取得的
信息形成，不能由 adapter 把 IP 自动冒充成控制点 ID。

当前会话的兼容矩阵更细：

- 同一 owner 地址的 `Play/Pause/Stop/Seek` 允许，兼容腾讯视频本地模式等控制身份
  分裂的控制点；
- 新媒体和 `Next/Previous/SetNext/SetPlayMode` 仍要求完整 peer 身份；
- RenderingControl 和查询不做 owner 写权限检查。

这不是密码学认证。同一 IP 上的多个控制应用仍可能碰撞；无 User-Agent 且地址下
存在多个缓存身份时不能猜测。

## 5. 同一控制点换片

`DlnaClaimAdapter` 先比较当前 owner：

- 当前 owner 是 DLNA，且 `peer.id` 与来源 IP 都相同：这是已有投送会话内的
  正常换片，直接返回 `Execute`，不创建新 Claim，不重复弹确认窗口；
- 没有 owner：创建首个 DLNA Claim；
- owner 是其他协议或另一个来源：按仲裁策略返回 `Busy`、请求确认或直接抢占；
- 已有待确认候选：只有同一来源的重发可以更新该候选媒体；其他来源返回
  `PendingClaimExists`。

只有 URI、metadata 和标题都完全相同的 `SetAVTransportURI` 重发，才复用原
claimId、原确认 deadline 和原确认窗口。媒体内容一旦变化，旧候选立即作废，
新内容使用新 claimId 和新确认窗口；旧窗口上的“允许”不可能授权新媒体。

各入口在分发前会检查候选 deadline 和本地事务投影。过期候选会返回
`invalidatedPending`，供 runtime 主动完成旧 HTTP；新的 `SetURI` 随后可以按新
Claim 继续，而不是继续挂在旧 payload 上。换媒体时也会返回同一个显式失效结果，
其中包含旧 claim key、decision 和旧媒体，runtime 可以据此完成旧 SetURI/Play
请求并关闭旧确认窗口。

## 6. Stop/SetURI/Play 事务

待确认事务由 `DlnaClaimAdapter` 保存：

```text
claimId
peer
latest MediaRequest
```

行为：

- 候选来源的 `Play` 返回 `Pending`，与 `SetURI` 一起等待确认；
- 候选来源的 `Stop` 返回成功但不执行，不能停止当前 owner；
- 当前 owner 的 `Play` 可走同 IP 控制兼容，`Stop` 必须匹配完整 peer 身份；
- 未知来源的 `Stop` 返回 `Ignored`；
- 未知来源的 `Play` 返回 `Rejected`；
- 用户拒绝或确认超时：清除候选事务，旧 owner 和 epoch 完全不变；
- 用户允许：存在 displaced DLNA owner 时，runtime 先在事务内 Stop 旧 owner；
  Stop 成功后才返回候选媒体和 `executeMedia=true`。adapter 对同一 Claim 只发出
  一次媒体 outcome，runtime 还必须用 owner session ID、epoch 和
  `mediaGeneration` 保证 Open 幂等并拒绝迟到的旧 Open；
- 重复的 accepted decision 返回 duplicate，`executeMedia` 必须为 false，禁止
  adapter 第二次发出媒体；
- 特定断线取消必须携带原 claim key；协议整体换代才使用 `cancelAllPending()`，
  迟到的旧取消不能误伤新候选。

## 7. 当前边界与剩余接线

当前已完成并测试：

- 异步 HTTP Pending/完成/超时/半关闭；
- 跨线程 completion 唤醒阻塞的网络 poll；
- `HttpRequest` 来源地址和 requestId；
- `DlnaClaimAdapter` 首投、同一 owner 换片、跨来源确认、拒绝、超时、重复决定、
  禁用抢占和候选期间命令隔离；
- `ProtocolControlDispatcher`：单 reader、activation/grant、
  confirmation、terminal failure、重复响应隔离和真实命名管道测试；
- `DlnaClaimRuntime`：HTTP requestId 生命周期、慢 Open 替换、媒体就绪后完成
  SetURI/Play、controlLost 批量失败；
- `DlnaClaimHttpHandler`：异步 SetURI/Play/Stop、候选慢 Open 控制隔离、
  同步查询透传和 SOAP 错误映射；
- `DlnaHttpFrontend`：设备描述、三份 SCPD 与 SOAP 的统一 HTTP 路由；
- `DlnaClaimConfirmationWorker`：HTTP 线程之外的确认等待和决定回填；
- `DlnaProtocolRuntime`：把 HTTP、SOAP、SSDP、GENA、Claim、确认 worker 和
  播放任务线程组合进 `MCast.Protocol.exe`；
- Protocol ControlPipe 断开时拒绝待确认候选和正在准备的媒体；
- 确认后继续 Open，并按媒体就绪/失败结果完成原 HTTP 请求；
- 暂停接收只阻止新投送，主动断开使用短暂网络离线兼容非标准控制点；
- `MCast.Protocol` 生产入口已经使用 dispatcher 完成 heartbeat、UI Open、
  activation 和 PlaybackPipe 握手；
- HTTP、DLNA Claim、dispatcher、runtime 和进程级测试；
- 真实控制点的首条媒体、暂停接收、主动断开和窗口重新唤起回归。

冷启动接线已完成且已接入：

- MainWindow 不存在时，由 Shell 按需启动 confirmation-only UI；
- confirmation-only Ui 保持 ControlPipe 常驻，在确认请求到达后显示独立窗口；
- 确认请求在 native host 信号订阅前到达时由 bridge 暂存，不会丢失；

尚未完成：

- 跨进程 UDP 的 M-SEARCH 仍受本机防火墙影响，当前由
  `DlnaProtocolRuntime` 动态 SSDP 端口测试覆盖；三进程 E2E 已覆盖 description、
  GENA、SetURI、真实 UI 播放和查询；
- 不同真实控制点在确认窗口前的等待时长实测；
- 接收/确认策略完全收归 Shell 的持久配置数据面；
- 三进程生产入口的长稳和故障注入。

MainWindow 已存在时和不存在时都可以进入确认路径。无 UI 场景由
confirmation-only 进程承接；确认允许后必须由 Shell 完成 confirmation-only 到
Normal Ui 的切换，不能把辅助 Ui 误认为主窗口。
