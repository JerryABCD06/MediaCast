# DLNA 迁入无 Qt Protocol

最后更新：2026-11-11

目标是把现有 `MCast.exe` 中的 DLNA/SSDP/HTTP/SOAP/GENA 逐层迁进
`MCast.Protocol.exe`，同时保持旧入口可回退。生产切换必须是最后一个独立提交。

## 总原则

- `PlaybackController` 继续留在 Ui，不搬进 Protocol。
- Protocol 只持有版本化的 DLNA 会话视图，通过 Playback IPC 驱动 Ui。
- 每个阶段都必须可独立构建、测试和回滚；默认生产行为不变。
- 测试不得默认占用生产端口 `8200` / `1900`。
- 网络 XML 不使用无界回溯正则；所有输入必须有尺寸和连接数上限。
- SSDP、HTTP、GENA 的等待和关闭必须可取消。

## 阶段

### 0. 线级基准

从旧实现固化 description、三份 SCPD、协议能力表、transport actions、
SOAP 动作/Fault、SSDP 报文、GENA propertyset/LastChange 的 golden 样本。

验收：旧入口不变，后续端口化能逐字节对比。

### 1. 无 Qt DLNA 核心

新增 `mcast_dlna_core`，只包含纯数据和解析：

- UPnP 描述与 SCPD
- DIDL-Lite
- DLNA 状态词汇
- HTTP 请求字节解析
- SSDP/GENA 报文构造

不做 socket、线程、Win32 消息泵或 Playback 调用。

当前进展：`UpnpXml` 已移除 Qt 类型，改为 `std::string`，并加入独立测试。

### 2. Playback 契约与假实现

定义协议无关的 `PlaybackGateway`：

- Open / Play / Pause / Stop / Seek / Next / Previous / EndSession
- SetNext / SetPlayMode
- Volume / Mute / Picture controls
- PeerConnected

快照包含 state、load status、URI/metadata、队列、position/duration、
volume/mute、picture controls、generation/version 和 playerLost。

先用 fake gateway 测试，不接真实 Ui。

当前进展：已建立异步 `PlaybackGateway` 合同与 `FakePlaybackGateway`；
命令带 ID/generation，完成事件和状态事件分离，fake 支持重新连接、
playerLost、队列清理和上一首/下一首切换测试。

### 3. 无 Qt DLNA 会话状态机

从 `DlnaRenderer` 提取 `DlnaSession`：

- 接收/暂停状态
- GENA 订阅和 peerConnected
- 主动断开标记
- 60 秒不可达宽限
- 当前媒体和 DLNA 事件缓存
- 无 Ui 时请求 Shell 激活

使用假时钟、假网络、假 PlaybackGateway 测试。

当前进展：已建立无 Qt `DlnaSession`，覆盖接收暂停/恢复、订阅连接、
主动挂断、60 秒宽限、Ui 激活请求和 Playback 事件缓存。

### 4. Playback IPC 数据面

扩展 IPC：

- PlaybackCommand / Result
- PlaybackSnapshot / StateDelta
- PeerConnectionChanged / PlayerLost

命令必须带 correlation ID 和有界确认；position/volume 等遥测 latest-wins。
Snapshot 必须带 generation/version，保留原始 metadata、media size 和
picture controls。

验收：真实 Ui 首帧和空快照继续通过。

当前进展：中性 Playback 类型已迁到 `mcast::playback`；IPC codec 已支持
命令、结果、快照、状态增量、playerLost 和 reconnected，大 metadata 使用
原始 Bytes，不重复 XML 转义。

Protocol 侧 `PlaybackPipeGateway` 已支持首快照门控、pending 命令超时、
重复/未知结果去重、generation/stateVersion 去重、断线拒绝 pending 命令，
以及 listener 析构等待。Ui 侧 `PlaybackBridge` 已接入真实
`PlaybackController`，命令在 Qt 主线程执行，Pipe 写入由独立 writer 线程完成。

PlaybackPipe 命令现在还受 `PlaybackUiReady` 门控：UI 只有在窗口首帧、
MpvQmlItem 完成首次实际 OpenGL 渲染、libmpv 已启动且 PlaybackBridge 已连接后
才上报。Protocol 可先交换快照，但收到这条消息前不会下发 Open/Play。render
context 重建或首次加载失败时，会保留最后 URI 并重试。

Ui 侧 `PlaybackBridge` 已接入真实 `PlaybackController`：activation ID 作为
generation，命令在 Qt 主线程执行，Pipe 写入由独立 writer 线程完成；完整
current/next 请求和 playerLost/本地 endSession 均会同步到协议快照。

### 5. Win32 网络运行层

建立 `mcast_dlna_net`：

- WSAStartup/WSACleanup
- 非阻塞 TCP/UDP
- 可唤醒关闭
- 定时器队列
- `GetAdaptersAddresses` 网卡快照

只做 localhost 和假 socket 测试。

当前进展：`mcast_net` 已提供 Winsock RAII、非阻塞 TCP/UDP、loopback
绑定、可唤醒 socket 事件循环、保留 connect/close 错误码的激活结果、
唯一 ID 定时器队列，以及带 ifType/multicast/prefix/scope 属性的 IPv4
网卡枚举；IPv4 接口同时保留路由 metric。自动定时器 ID 不再永久累积，
显式 ID 与自动/活跃/取消状态交叉去重。默认测试不占用 1900/8200。

### 6. HTTP + SOAP

保留现有 `Connection: close` 行为，重写 QTcpServer/QTcpSocket 层；增加
Content-Length 缺失、负数、溢出、超大 body、慢请求和连接数上限测试。
SOAP 全部经 `DlnaSession -> PlaybackGateway`。

当前进展：已加入无 Qt loopback `HttpServer`，使用非阻塞 TCP 与
`WSAEventSelect`，支持分片请求、单请求 `Connection: close`、每连接
输入/输出缓冲、连接数与请求/慢读超时、半关闭后继续排空响应，以及可重入
`stop()` 的安全延迟收尾。请求体默认上限 1 MiB，超限 body 回 413、
超限/过多 header 回 431；响应按完整序列化大小限制，禁止覆盖 framing
header 或注入 CR/LF。handler 当前要求非阻塞。

`HttpServer::listenAny()` 已补上局域网 IPv4 绑定，单元测试仍只使用临时端口和
loopback 客户端，不占用 8200。`DlnaHttpFrontend` 已覆盖设备描述、三份 SCPD
和 SOAP POST；`DlnaClaimHttpHandler` 已成为 SOAP 写动作的统一异步入口。

已加入无 Qt `SoapRouter`，覆盖 AVTransport、RenderingControl、
ConnectionManager 三份 SCPD 的全部动作，并严格校验 service 路径、
SOAPAction 与 body namespace/action 的一致性、`InstanceID`、枚举和必填
metadata。XML 使用 Windows XmlLite 解析，禁用 DTD，并限制文档深度和节点数；
DIDL-Lite 元数据已提取为与 DLNA 协议层绑定的播放项描述。

`DlnaSession` 已用互斥锁保护状态，播放、UI、网络和状态回调全部在解锁态执行；
普通命令与 `EndSession` 的提交顺序串行化，并通过 command ID、deadline、
pending Open 和 generation 跟踪处理排队、超时、取消与重连。排队 Open 后的
Play/Pause，以及 `WaitingForPlayer` 期间的 deferred 调优命令均有回归测试。
HTTP 到 SOAP router 已由 `DlnaProtocolRuntime` 组合并由 `MCast.Protocol.exe`
实例化。Runtime 自己持有 HTTP 轮询线程和播放任务串行线程，播放器打开完成/失败
会回填 `mediaReady()`；ControlPipe 通过 `ProtocolDlnaConfirmationPort` 回传
确认结果。Ui 不存在时，`LateBindingPlaybackGateway` 让播放命令失败关闭，Ui
PlaybackPipe 建立后透明挂接。

“暂停接收”的语义是**不再接受新的投送**，不是替用户结束正在进行的投送。
没有活动会话时，暂停会关闭 HTTP/SSDP、停止确认 worker 并结束空会话；已有活动
会话时，只停 SSDP 广播，HTTP/GENA 和当前媒体的播放/暂停/音量/进度控制继续
服务，同时拒绝新的 `SetAVTransportURI`。恢复时重新开启 SSDP；如果此前已完整
暂停，才重新开启 HTTP/GENA 并恢复 session。HTTP 轮询线程异常退出时仍会整体
降级为 `receiving=false`，并通过 ControlPipe 主动同步 Shell。

UI 顶栏的「断开连接」通过 PlaybackPipe 的 `UiEndCastRequest` 回到 Protocol，
由同一套 `DlnaSession::endSession` 清媒体、断 peer 并产生 GENA 状态变化；UI
不直接操作协议状态。主动断开是一个粘性状态：后续查询、续订、Play/Stop 都不会
把它重新标成“已连接”，只有新的 `SetAVTransportURI` 才开启下一次会话。
考虑部分控制点只认设备从网络消失，主动断开还会把 HTTP/GENA/SSDP 整体停约
1.5 秒：先由 SSDP 发 `byebye`，再停 GENA 和 HTTP，EndSession 收尾后按
HTTP/GENA、SSDP `alive` 的顺序重新上线；接收开关本身保持开启。

关闭窗口确认框提供「仅关闭窗口 / 取消 / 关闭并断开」。选择仅关闭窗口时，UI
先同步把 `PAUSED` 快照经 PlaybackPipe 交给 Protocol，GENA 随即将暂停通知手机，
然后只销毁 UI 窗口；连接与接收服务保留，手机下一次操作会重新拉起主窗口。

### 7. SSDP

替换 Qt 网卡枚举、组播、定时器和 UUID 配置。UDN 最终由 Shell 提供；
迁移期可注入现有 `renderer.ini` 快照。默认测试不绑定 1900。

当前进展：已加入无 Qt `mcast_dlna_net`。网卡选择器保留旧实现
“Wi-Fi > 有线 > 其他”的优先级，并用 IPv4 metric、接口 index、名称和地址
形成稳定顺序；接口信息带 hardware/filter 标志，明确排除回环、隧道、软件网卡、
vEthernet、VPN/TAP/WinTun 等不可靠出口。`SsdpSocket` 支持 `SO_REUSEADDR`
非阻塞绑定、指定组播出口、加入/离开组播组，以及 16 KiB 有界 UDP 收发；
超限包、UDP reset 和 would-block 作为可恢复状态返回。测试只使用临时端口，
并覆盖实体网卡组播发收。

`SsdpService` 已负责完整发现生命周期：选择网卡、绑定 1900、发六种身份
`alive`/`byebye`、响应 `M-SEARCH`、200 ms 内有界随机错峰，以及可取消停线程。
M-SEARCH 回应队列有上限，搜索类型按 HTTP 规矩大小写不敏感。测试用临时端口和
回环接口完成真实 UDP 收发，不占生产 1900。`DlnaProtocolRuntime` 在
`listenAny()` 的生产路径上组合 HTTP 与 SSDP；loopback 和进程测试路径不启动
组播。Shell 启动 Protocol 时会直接携带持久化的初始接收开关，因此上次暂停后
重启不会再出现“先在线 250 ms、再被暂停”的窗口。

### 8. GENA

保留以下语义：

- 首条事件延迟 100 ms
- 同一订阅同一时刻只有一条 NOTIFY 在途
- 在途期间只记重发，结束后发最新状态
- SEQ 严格递增
- 连续五次连接失败才移除订阅
- 5 秒超时
- 不走系统代理

当前进展：Qt-free `GenaManager` 已接入 `DlnaProtocolRuntime` 和
`DlnaHttpFrontend`，完整处理 `SUBSCRIBE` / 续订 / `UNSUBSCRIBE`。订阅上限、
回调 URL 校验、1800 秒有效期和前台 HTTP 应答均有界；回调主机通过
`getaddrinfo` 解析，支持 DNS、IPv4 和带方括号的 IPv6 地址。新订阅在应答后
100 ms 收到第一条完整状态。每个订阅同一时刻最多一条 `NOTIFY`，繁忙期间只保留
“需要补发最新状态”的标记。

网络异常按“可能只是暂时送不到”处理：连接失败不消耗 `SEQ`，按
250/500/1000/2000 ms 退避重试当前完整快照；恢复后用原序号补发，不会因为失败
尝试制造序号断层。只有连续五次连不上才移除订阅并以 `Unreachable` 上报，交给
`DlnaSession` 的 60 秒宽限；连上但没及时返回 200 不算失败。明确退订、过期或
主动暂停才立即断开。连接恢复后的完整 `LastChange` 会重新对齐状态、时长、队列
能力、音量/静音和画面参数。

测试覆盖：回调服务器晚于订阅启动时，前两次连接失败后仍以 `SEQ=0` 成功补发；
成功后下一条为 `SEQ=1` 并包含最新 `PLAYING`；连续五次死回调最终上报
`Unreachable`；显式退订立即上报断开。GENA 集成测试不访问外网。

### 9. Protocol 影子集成

只通过显式测试参数启动影子 DLNA，使用独立端口，不绑定 8200/1900。
旧 `MCast.exe` 仍是生产回退路径。HTTP/SOAP/SSDP/GENA 已全部在无 Qt Protocol
中具备实现；运行时级测试和真实控制点首轮回归已经完成。尚未完成的是一次从
外部进程驱动、同时覆盖发现面和播放面的自动化影子 E2E。

已由 `DlnaProtocolRuntime` 运行时测试覆盖：

- description 与三份 SCPD；
- `SetAVTransportURI` 经媒体就绪门控后完成；
- 首投、确认、拒绝、加载失败和候选取消；
- `SUBSCRIBE` 建立 peer、暂停接收只阻止新投送；
- 主动断开清会话、短暂停网络面、重新上线，以及粘性断开状态；
- 新 `SetAVTransportURI` 开启下一次会话。

真实控制点已确认首条媒体不再黑屏、代理暂停不终止当前会话、主动断开成功；
自动测试仍需补齐从 `M-SEARCH` 发现开始的跨组件链路。

端到端验收：

- M-SEARCH -> description
- SetAVTransportURI -> PlaybackCommand
- Ui StateDelta -> SOAP 查询
- SUBSCRIBE -> LastChange/后续 NOTIFY
- 暂停接收 -> 无活动会话时结束空会话并停监听；有活动会话时保留当前控制，
  只发 SSDP byebye 并拒绝新的媒体 URI
- Ui 退出/重启 -> generation 与重连

## 生产切换

影子集成稳定后单独提交：Shell 改为只启动新 Protocol，删除旧单体 DLNA
路径和重复托盘，再做真实手机长时间回归。
