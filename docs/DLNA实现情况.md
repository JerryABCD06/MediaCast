# DLNA 实现情况

更新日期：2026-11-12

本文是当前 DLNA/UPnP AV 实现的权威入口。真实手机应用的行为和抓包结论见
[控制点行为.md](控制点行为.md)，仲裁权限见
[投送仲裁与确认.md](投送仲裁与确认.md)。九月首轮实现快照已冻结到
[history/DLNA实现情况-2026-09-11.md](history/DLNA实现情况-2026-09-11.md)。

## 1. 分层

```text
src/protocols/dlna/
  core/        HTTP/SSDP 报文和 XML 基础
  net/         TCP/UDP、网卡选择、HTTP Server、SSDP
  soap/        AVTransport / RenderingControl / ConnectionManager
  session/     当前、下一条、历史和逻辑 transport 状态
  claim/       中性 Claim、owner 权限和异步 HTTP
  runtime/     Runtime 组合、GENA、确认 worker
  playback/    HLS/分片预读和中性媒体规划
```

`MCast.Protocol.exe` 是这套实现的宿主，不链接 Qt。Ui 只接收中性媒体、命令和
播放计划，不解析 DIDL。

## 2. SSDP

- 发送六种身份的 alive/byebye；
- 响应 `M-SEARCH`，带 0–200 ms 随机错峰；
- BOOTID 随运行代次递增；
- 广播开关和间隔由 Shell 设置下发；
- 默认间隔 10 秒，可配置范围 1000–3,600,000 ms；
- 旧 UI 的“高频广播”复选框已经不存在，广播由现行设置页和托盘状态驱动；
- 暂停接收时，无活动会话可关闭网络面；有活动会话只隐藏发现并拒绝新媒体，
  当前播放和控制保留。

生产路径监听局域网 IPv4；测试路径会关闭 SSDP 或使用临时端口，避免占用真实
1900/8200。

## 3. HTTP 与设备描述

HTTP 服务提供：

- `/description.xml`
- AVTransport SCPD
- RenderingControl SCPD
- ConnectionManager SCPD
- SOAP POST
- GENA `SUBSCRIBE` / 续订 / `UNSUBSCRIBE`

HTTP Server 是非阻塞事件循环；异步 SOAP 请求可以保持 Pending。请求携带来源 IP
和实例内单调 request ID。网络 owner 线程负责 socket 完成，其他线程只能把 completion
放入有界队列。

生产端口 8200；测试可使用随机端口。HTTP Owner 线程仍有少数 Play/Stop 路径等待
Playback IPC 最多 5 秒，这是当前已记录的剩余拥塞风险。

## 4. AVTransport

当前声明并实现 15 个动作：

```text
SetAVTransportURI
SetNextAVTransportURI
Play
Pause
Stop
Seek
Next
Previous
SetPlayMode
GetTransportInfo
GetPositionInfo
GetMediaInfo
GetDeviceCapabilities
GetTransportSettings
GetCurrentTransportActions
```

关键语义：

- 只有 instance 0；其他实例返回 718。
- Protocol 保存上一条、当前和下一条，不维护完整控制点播放列表。
- 新媒体 Open 只有实际加载成功后才提交为 current。
- 加载失败、Open 超时和迟到命令不能覆盖旧记录；新媒体边界失败时清空 current/
  next/previous，避免 Protocol 声称旧媒体仍有效。
- `hasNext` / `hasPrevious` 完全来自 Protocol 记录，不读取 Ui 自行猜的队列。
- `Seek` 支持 `REL_TIME`、`TRACK_NR` 和 `X_DLNA_REL_BYTE`；缺大小/时长时返回
  对应 SOAP fault。
- 播放速度只支持 1 倍，其他返回 717。
- `SetPlayMode` 接受 `NORMAL` / `REPEAT_ONE` / `REPEAT_ALL` / `DIRECT_1`；
  `SHUFFLE` 因没有完整列表返回 701。
- `CurrentTransportActions` 随状态变化，Play 和 Pause 不总是同时出现。

错误码不能再用一句“无媒体一律 701”概括。实际还取决于是否有 owner、来源是否
为候选或未知：

- 未知来源 `Stop` 返回成功但忽略；
- 候选 `Play` 跟随 SetURI 等待确认；
- 未知 `Play` 返回 501；
- 未知 `Pause/Seek/Next/Previous` 通常返回 701；
- 已建立 owner 但确实无媒体时，常规 transport 动作返回 701。

### 同 URL 去重

同一 owner、相同 URI 与相同中性媒体字段的重复 `SetAVTransportURI` 只返回成功，
不重新 Open。抖音等应用会重复发送相同 URL；这是幂等行为，不是忽略所有换片。

## 5. RenderingControl

当前声明并实现 12 个动作：

```text
SetVolume
GetVolume
SetMute
GetMute
SetBrightness
GetBrightness
SetContrast
GetContrast
SetSharpness
GetSharpness
ListPresets
SelectPreset
```

DLNA 画面/音量值是 0–100，其中 50 是“未调整”的中性值；播放器内部画面参数是
-1000–1000、0 是中性。换算只在 SOAP 边界做一次。

`SelectPreset` 当前只支持 `FactoryDefaults`，用于复位画面参数。mpv 没有直接
对应属性的色温、梯形校正和颜色增益等项不声明；锐度使用视频滤镜链实现。

## 6. ConnectionManager

当前实现 3 个动作：

```text
GetProtocolInfo
GetCurrentConnectionIDs
GetCurrentConnectionInfo
```

只有 0 号连接。控制点不需要先 `PrepareForConnection`；直接
`SetAVTransportURI -> Play` 即可发起投送。

## 7. GENA

已实现：

- `SUBSCRIBE`、续订和 `UNSUBSCRIBE`；
- 首条事件在应答后至少延迟 100 ms，避免与 SID 应答抢跑；
- 同一订阅同一时刻只有一条 NOTIFY 在途；
- 繁忙期间只记住“稍后发送最新完整快照”；
- SEQ 只在成功发完整条事件后前进；失败重试不制造序号断层；
- 通知超时 5 秒；
- 连续 5 次连接失败才移除订阅，并上报 `Unreachable`；
- 退出/过期/主动暂停可立即断开；
- 订阅上限 32；
- 回调 URL 和主机解析有界，不走系统代理。

订阅总数归零且原因是 `ExplicitRemoval`、`Expired` 或 `Count` 时，这是完整会话
边界：Protocol 会发起 `EndSession` 并让 UI 进入 `NoMedia`，而不是只把
`peerConnected` 改成 false 后留下一个 `STOPPED` 媒体。`Unreachable` 仍先保留
60 秒宽限；宽限到期后同样结束会话并卸载媒体。这样电脑端不能再用本地 Play
按钮重新启动已经结束的投送。

`LastChange` 只发控制点能稳定处理的标量字段：

- AVTransport：状态、状态详情、时长、track、track 数、播放模式、
  `CurrentTransportActions`；
- RenderingControl：音量、静音、亮度、对比度、锐度；
- ConnectionManager：source/sink protocol info。

不要在 AVTransport 事件里塞整坨 DIDL。vivo/BubbleUPnP 等控制点会静默丢弃过大的
事件；HTTP 仍返回 200，看起来“发送成功”，但手机状态冻结。

## 8. Stop 与主动断开

### 手机只发 Stop

一些控制点退出投送时只发 `AVTransport#Stop`，之后完全静默。当前处理：

```text
Stop 成功 applied
  -> 启动 8 秒 stop-idle grace
  -> 只有当前 owner 同地址的后续 SOAP 活动会续命
  -> 其他设备查询、GENA 订阅/续订不续命
  -> 8 秒边界额外给新 SetAVTransportURI 250 ms 优先窗口
  -> 仍无活动：结束会话、清空记录、回到未投送
```

因此实际收口约 8.25 秒，不是任意 `STOPPED` 都立刻断开。

### 接收器主动断开

```text
准备结束当前会话
  -> EndSession
  -> 等播放进入 NO_MEDIA_PRESENT
  -> 最多等 1 秒最终 GENA 队列排空
  -> 以短间隔发送 3 轮 SSDP byebye
  -> 停止 SSDP / GENA / HTTP
  -> 延迟 1.5 秒重新 alive
```

主动断开总 deadline 8 秒。接收器侧主动断开后 stay disconnected，查询、续订和
普通 Play/Stop 不会恢复“已连接”；只有新的 `SetAVTransportURI` 开启下一会话。

当前已知缺口：`endSession()` 之后才设置断开的 drain 状态，尚未形成真正原子的
“先拒绝新 SetURI，再结束”屏障，存在很窄并发窗口；此项已进入待办。

## 9. Claim 与所有权

真实 DLNA `SetAVTransportURI -> Play` 已接入中性 Claim。完整权限矩阵和状态机见
[投送仲裁与确认.md](投送仲裁与确认.md)，这里只保留协议侧约束：

- 新媒体必须匹配完整 peer 身份；
- 当前会话的同 IP `Play/Pause/Seek` 允许，兼容身份分裂控制点；
- `Stop` 必须匹配完整 peer 身份；候选 Stop 只回成功且不改变播放，
  确认接受后才由 DLNA 换投事务先停止旧 owner；
- `Next/Previous/SetNext/SetPlayMode` 仍要求完整身份；
- RenderingControl 和查询不做 owner 写权限检查；
- candidate 命令不能操作旧 owner；
- owner epoch、mediaGeneration、operationId/requestId 必须一起校验。

`sessionRetired` 目前没有真实生产调用，因此“状态机支持退场完成通知”不等于
旧 DLNA 会话已有协议级 goodbye 闭环。

## 10. 媒体规划与兼容路线

LANERC 等来源可能把 MPEG-TS 伪装成 PNG 分片。识别和播放执行分开：

```text
Protocol: URI 以 .m3u8 结尾
  -> 后台读取清单和 EXTINF
  -> 分片扩展名以图片为主
  -> 对首段最多 Range 读取 256 KiB
     纯图片
       -> MediaRoute::ImageSequence
     PNG 壳后接 MPEG-TS
       -> MediaRoute::WrappedTransportStream
     其他
       -> MediaRoute::Native
```

不能按域名、User-Agent 或“LANERC”分支。当前入口仍要求清单 URL 以 `.m3u8` 结尾，
不是任意清单嗅探。

- `Native`：将原 URI 交给 MPV；
- `ImageSequence`：Ui 用 `mf://` 和按 `EXTINF` 计算的虚拟时间轴；
- `WrappedTransportStream`：Ui 的本地只读代理按需剥 PNG 壳并返回标准 TS。

普通 MP4/HLS 直接走 Native。`Packet corrupt` 连续告警来自源时间戳/连续性时，
不能据此误判成图片序列；日志会按开始、结束和总条数聚合。

纯图片序列当前用平均 FPS 驱动 MPV，而协议时间轴按逐段 `EXTINF` 计算。等时长序列
一致；明显不等长时仍可能有切换偏差，已列入后续细化项。

## 11. 有意不声明

- 录音 `Record` / `SetRecordQualityMode`；
- `PrepareForConnection` / `ConnectionComplete`；
- mpv 没有对应属性的色温、梯形校正、颜色增益；
- `SHUFFLE`；
- AVTransport:2 的 `SetStateVariable` / `GetStateVariable`。

当前设备描述声明 AVTransport:1。不要把标准里存在但未声明的动作写成“已兼容”。

## 12. 验证入口

当前相关 CTest 包括：

- `mcast_dlna_core_tests`
- `mcast_dlna_net_tests`
- `mcast_ssdp_service_tests`
- `mcast_http_server_tests`
- `mcast_dlna_session_tests`
- `mcast_dlna_playback_planner_tests`
- `mcast_soap_router_tests`
- `mcast_dlna_claim_adapter_tests`
- `mcast_dlna_claim_runtime_tests`
- `mcast_dlna_claim_http_handler_tests`
- `mcast_dlna_http_frontend_tests`
- `mcast_dlna_protocol_runtime_tests`
- `mcast_dlna_shadow_e2e`
- `mcast_gena_manager_tests`
- `mcast_dlna_claim_confirmation_worker_tests`
- `mcast_image_sequence_playlist_tests`
- `mcast_mpv_image_sequence_tests`

真实控制点记录必须继续写入 `控制点行为.md`，不要混入协议规范正文。
