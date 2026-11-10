# DLNA 投送确认接入

最后更新：2026-11-11

本文固定真实 DLNA `SetAVTransportURI -> Play` 接入投送仲裁时的职责、时序和
超时边界。上层原则见 `多协议投送与确认架构.md`，仲裁状态见
`投送仲裁状态机.md`。

## 1. 当前分层

```text
HTTP socket 线程
  HttpServer
    -> 解析 HTTP、记录来源 IP、分配 requestId
    -> async handler 返回 Completed 或 Pending

Protocol 单一仲裁执行上下文
  DlnaClaimAdapter
    -> 判断动作是否明确要求开始投送
    -> 保存 Stop/SetURI/Play 的 DLNA 跨请求事务
    -> 唯一调用 CastClaimCoordinator

DLNA 协议执行层
  SoapRouter / DlnaSession
    -> Claim 已允许后才提交 Open、Play、Stop 等播放命令
```

`DlnaClaimAdapter` 不解析 SOAP，不持有 socket，也不直接发送 IPC。它只接受已经
抽象后的设备身份、来源地址和 `MediaRequest`。

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

## 4. 明确 Claim

只有控制点明确表达“开始播放这个媒体”时才产生 Claim：

| 动作 | 是否 Claim |
|---|---|
| `SetAVTransportURI` | 是 |
| 播放器准备完成后的 `Play` | 属于已有事务，不再创建第二个 Claim |
| `SetNextAVTransportURI` | 否 |
| 发现、GET、`Get*` 查询、GENA 订阅 | 否 |
| 找不到明确所有者的 `Stop/Pause/Seek/Next/Previous` | 否，且不能操作旧 owner |

来源 IP 只用于 DLNA 内的 best-effort 关联。`peer.id` 和来源 IP 缺一不可，
空值一律 fail-closed。`peer.id` 应由协议 runtime 从本控制通道能稳定取得的
信息形成，例如来源地址与控制点 User-Agent/控制通道指纹的组合；adapter 不会
把 IP 自动冒充成控制点 ID。它仍然不是密码学身份认证，同一 IP 上的两个控制
应用也可能碰撞，只是不能仅凭 IP 获得当前 owner 的执行权限。

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
- 候选来源的 `Stop` 返回 `Ignored`，不能停止当前 owner；
- 当前 owner 的 `Play/Stop` 返回 `Execute`，确认期间继续正常工作；
- 未知来源的 `Stop` 返回 `Ignored`；
- 未知来源的 `Play` 返回 `Rejected`；
- 用户拒绝或确认超时：清除候选事务，旧 owner 和 epoch 完全不变；
- 用户允许：返回候选媒体和 `executeMedia=true`；adapter 对同一 Claim 只发出
  一次媒体 outcome，runtime 还必须用 owner session ID、epoch 和
  `mediaGeneration` 保证 Open 幂等并拒绝迟到的旧 Open；
- 重复的 accepted decision 返回 duplicate，`executeMedia` 必须为 false，禁止
  adapter 第二次发出媒体；
- 特定断线取消必须携带原 claim key；协议整体换代才使用 `cancelAllPending()`，
  迟到的旧取消不能误伤新候选。

## 7. 尚未完成的生产接线

当前已完成并测试：

- 异步 HTTP Pending/完成/超时/半关闭；
- 跨线程 completion 唤醒阻塞的网络 poll；
- `HttpRequest` 来源地址和 requestId；
- `DlnaClaimAdapter` 首投、同一 owner 换片、跨来源确认、拒绝、超时、重复决定、
  禁用抢占和候选期间命令隔离；
- HTTP 专项与 DLNA Claim 专项测试。

尚未完成：

- DLNA runtime 把 HTTP requestId 和 attached `Play` requestId 写入待确认事务；
- Protocol 在 ControlPipe 上发送 `CastConfirmationRequest` 并处理 decision；
- ControlPipe 断开时批量 fail-closed 所有待确认 HTTP；
- `SoapRouter::handle()` 或外层 adapter 返回异步 `Pending`；
- 确认后继续 Open，并把成功/失败映射为 SOAP 响应；
- 真实手机控制点等待时长实测；
- 将新 DLNA 网络栈接入生产 `MCast.Protocol.exe`。

在这些步骤完成前，不能宣称真实手机投送已经会触发 QML 确认窗口。
