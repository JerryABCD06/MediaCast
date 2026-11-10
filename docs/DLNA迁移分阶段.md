# DLNA 迁入无 Qt Protocol

最后更新：2026-11-10

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

### 5. Win32 网络运行层

建立 `mcast_dlna_net`：

- WSAStartup/WSACleanup
- 非阻塞 TCP/UDP
- 可唤醒关闭
- 定时器队列
- `GetAdaptersAddresses` 网卡快照

只做 localhost 和假 socket 测试。

### 6. HTTP + SOAP

保留现有 `Connection: close` 行为，重写 QTcpServer/QTcpSocket 层；增加
Content-Length 缺失、负数、溢出、超大 body、慢请求和连接数上限测试。
SOAP 全部经 `DlnaSession -> PlaybackGateway`。

### 7. SSDP

替换 Qt 网卡枚举、组播、定时器和 UUID 配置。UDN 最终由 Shell 提供；
迁移期可注入现有 `renderer.ini` 快照。默认测试不绑定 1900。

### 8. GENA

保留以下语义：

- 首条事件延迟 100 ms
- 同一订阅同一时刻只有一条 NOTIFY 在途
- 在途期间只记重发，结束后发最新状态
- SEQ 严格递增
- 连续五次连接失败才移除订阅
- 5 秒超时
- 不走系统代理

### 9. Protocol 影子集成

只通过显式测试参数启动影子 DLNA，使用独立端口，不绑定 8200/1900。
旧 `MCast.exe` 仍是生产路径。

端到端验收：

- M-SEARCH -> description
- SetAVTransportURI -> PlaybackCommand
- Ui StateDelta -> SOAP 查询
- SUBSCRIBE -> LastChange/后续 NOTIFY
- 暂停接收 -> 会话结束、byebye、停止监听
- Ui 退出/重启 -> generation 与重连

## 生产切换

影子集成稳定后单独提交：Shell 改为只启动新 Protocol，删除旧单体 DLNA
路径和重复托盘，再做真实手机长时间回归。
