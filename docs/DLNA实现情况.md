# DLNA 实现情况

这份文件是给**以后回来查的人**看的 —— 尤其是"这个东西为什么是这样"这类问题，
光看代码看不出来。

更新时间：2026-09-11

---

## 〇、目录结构（2026-09-12 重排）

```
src/
├── main.cpp                  组装：造零件、接日志、开窗口、挂托盘
├── core/                     播放核心（协议无关）
│   ├── MediaPlayer.h         播放器接口
│   ├── LibMpvPlayer.*        内嵌 libmpv（当前用的）
│   ├── MpvMediaPlayer.*      外部 mpv.exe（排查问题时备用）
│   └── NowPlaying.*          "现在在播什么"这份数据
├── protocols/dlna/           DLNA 这一套
├── platform/windows/         跟 Windows 打交道的地方
└── ui/                       界面
```

规矩：**跨目录的 include 写从 src 起算的全路径**（`"core/MediaPlayer.h"`），
同目录写短名字。`src` 本身在 CMake 里加进了包含路径。

以后加 AirPlay 之类，就是在 `protocols/` 下加一个平级目录。

**第二阶段要做的**（还没做）：把 `SoapHandler` / `DlnaRenderer` 里那些**与协议无关**
的播放状态（当前媒体、传输状态、队列、音量）抽到 `core/PlaybackController`，
让协议层只剩"翻译"和"用本协议的话报状态"。

---

## 一、已经实现的

### SSDP（让控制点发现我们）
- 上线/下线的 alive / byebye 广播
- 应答 M-SEARCH 搜索（带 0~200ms 随机延迟，规范要求）
- BOOTID.UPNP.ORG 递增
- 广播间隔默认 10 秒（界面上那个"高频广播"复选框控制），接上正经路由器后可以放宽

### HTTP
- `/description.xml` 设备描述
- 三份 SCPD（服务能力清单）

### SOAP — AVTransport（15 个动作）
`SetAVTransportURI`、`SetNextAVTransportURI`、`Play`、`Pause`、`Stop`、`Seek`、
`Next`、`Previous`、`SetPlayMode`、`GetTransportInfo`、`GetPositionInfo`、
`GetMediaInfo`、`GetDeviceCapabilities`、`GetTransportSettings`、
`GetCurrentTransportActions`

几个值得一提的点：
- 队列只有**三个位置**：上一条 / 当前 / 下一条。不做播放列表管理器。
- `Seek` 支持 `REL_TIME`、`TRACK_NR`（只有一条，跳 1 = 回到开头）、
  `X_DLNA_REL_BYTE`（按"总字节数 ÷ 总时长"按比例换算；拿不到大小就回 711）
- `SetPlayMode` 收 `NORMAL` / `REPEAT_ONE` / `REPEAT_ALL` / `DIRECT_1`，
  **`SHUFFLE` 回 701**（没有列表可打乱）
- 播放速度只支持 1 倍，别的回 717
- 只有 0 号实例，别的号回 718
- 手上没有内容时，`Play`/`Pause`/`Stop`/`Seek`/`Next`/`Previous` 一律回 701

### SOAP — RenderingControl（12 个动作）
`SetVolume`、`GetVolume`、`SetMute`、`GetMute`、
`SetBrightness`、`GetBrightness`、`SetContrast`、`GetContrast`、
`SetSharpness`、`GetSharpness`、`ListPresets`、`SelectPreset`

**约定要小心**：DLNA 这几项是 0~100，而且 **50 才是"没调过"**；播放器内部是
-100~100、0 是中性。换算写在 `SoapHandler::dlnaPictureValue` 和 `Set` 那个分支里，
只此一处。

`SelectPreset` 只认 `FactoryDefaults`（复位画面调节），别的名字回 701。

### SOAP — ConnectionManager（3 个动作）
`GetProtocolInfo`、`GetCurrentConnectionIDs`、`GetCurrentConnectionInfo`

只有一条连接（0 号），而且不是"建"出来的 —— 控制点直接 SetAVTransportURI 就开播了。

### GENA（状态变化主动推给控制点）
SUBSCRIBE / UNSUBSCRIBE / 续订。`LastChange` 里报什么，是**照着 Macast 来的**
（Macast 是成熟的 DLNA 渲染器，手机跟它配合是好的；源码在
`source\Macast-main`）：

- AVTransport：TransportState、TransportStatus、CurrentMediaDuration、
  CurrentTrackDuration、CurrentTrack、NumberOfTracks、CurrentPlayMode、
  CurrentTransportActions
- RenderingControl：Volume、Mute、Brightness、Contrast、Sharpness
- ConnectionManager：SourceProtocolInfo、SinkProtocolInfo

Volume/Mute 带 `channel="Master"`，画面那三项**不带** —— 规范如此，多了少了都不行。

> ⚠️ **不要在事件里放 DIDL 元数据。**
>
> 原来 AVTransport 的事件是一份"全量快照"，里面带着 AVTransportURI /
> CurrentTrackURI，以及**两整坨转义过两遍的 DIDL 元数据**（一条九百多字节）。
>
> 症状：手机端播放/暂停图标永远不变，进度条却正常。事件其实送到了、手机也回了
> `200 OK`、XML 也合法 —— 但它的解析器处理不了那么多东西，**整条事件被静默
> 丢掉**。改成只发几个标量之后，vivo 和 BubbleUPnP 立刻就对上了。
>
> 教训：事件是丢给控制点**解析**的，结构化内容越多，被丢掉的概率越大，而且这种
> 失败完全静默 —— HTTP 层照常 200，日志里只看到"对方回了 200 OK"。
>
> 代价：事件里不再有 URI/元数据，所以电脑上按「上一首/下一首」时手机不会跟着换
> 标题。那件事本来就没人跟（见"待定/待测"），先这么放着。

### 三条踩了很久才踩明白的规矩

1. **`CurrentTransportActions` 必须跟着状态走，`Play` 和 `Pause` 互斥。**
   控制点就是靠这个列表决定播放/暂停按钮长什么样的。永远两个都给，它只能推出
   "还在播"。见 `UpnpXml::transportActionsFor()`。

2. **同一个订阅，同一时刻只允许一条 NOTIFY 在途**，后面的先攒着、按最新状态补发。
   两条挨着发会乱序到达，而控制点按 SEQ **严格递增**处理：它等 5 却先收到 6 就要
   丢掉，而且丢掉之后期待值不会前进 —— **后面每一条都会被丢掉**，状态从此冻住。

3. **首条事件（SEQ=0）不能跟订阅应答抢跑。** 控制点得先从应答里拿到 SID 才认得
   那条事件；抢输了它就把 SEQ=0 丢掉，期待值停在 0，之后每一条都对不上。
   所以现在延后 100 毫秒再发首条 —— 让应答先出去。

---

## 二、有意不做的（不是漏了）

| 没做的 | 为什么不 |
|---|---|
| 录音相关动作（`Record`、`SetRecordQualityMode`） | 我们录不了。**连声明都不声明**，控制点不会来问 |
| `PrepareForConnection` / `ConnectionComplete` | 那是"先建连接、再送内容"的流程，我们不那么干。没声明；万一硬调，会收到正常的 401 |
| `ColorTemperature`（色温）、`Horizontal/VerticalKeystone`（梯形校正）、颜色增益 | DLNA 里有这些标准名字，但 **mpv 没有对应属性**（`mpv --list-options` 实测过）。要做得自己写 shader |
| `SHUFFLE` 播放模式 | 手上只有三个位置，没有列表可打乱 |
| AVTransport:2 的 `SetStateVariable` / `GetStateVariable` | 极少有控制点使用。**这条是唯一还欠着的协议项** |

---

## 三、待定 / 待测

### 1. 「上一首/下一首」两个按钮的定位（重要）

**现象**：投屏时在电脑上按这两个按钮，我们的播放器**确实会切到上一条/下一条**，
但手机那边的界面不动，于是两边错位。

**原因**：DLNA 里**列表归控制点**，渲染器只有"当前 + 下一条"两个位置
（`SetNextAVTransportURI`）。渲染器端的 Next/Previous 只有在控制点真的把队列
交给渲染器时才有意义。实测（2026-09-11）：

| 控制点 | `SetNextAVTransportURI` 调用次数 | `GetCurrentTransportActions` 调用次数 |
|---|---|---|
| vivo 相册 | 0 | 0 |
| BubbleUPnP（公认最规范的控制点） | 0 | 0 |

**结论**：规范里有这条路，生态里没人走。换片的正确入口是**控制点自己的界面**
（vivo、BubbleUPnP 那里都完全正常）。

**三个候选做法**（还没定，等条件成熟再测）：
1. 改成"片子内部 ±10 秒"的前进/后退 —— 永远是 Seek，两边都认，手机进度条会跟着走
2. 投屏时灰掉，只在本地播放时保留
3. 原样不动

### 2. 媒体面板里的程序名字和图标

**现象**：Windows 那个"按音量键弹出来"的媒体面板里，我们显示成"未知应用"+一串
可执行文件路径，没有图标。

**试过没成的**：给 exe 加图标资源和版本信息（这一步对资源管理器有效，对媒体面板
无效）；给进程和窗口设 AppUserModelID 并在
`HKCU\Software\Classes\AppUserModelId\...` 里登记名字和图标 —— **实测也没生效**。

**下一步的线索**：微软文档里，未打包程序要让系统认出名字，完整要求还包括
**一个带这个 AUMID 的开始菜单快捷方式**（通常由安装程序创建）。我们没做那一步。

**另外**：下次可以先找一个**已经能正常显示名字的同类程序**（VLC、foobar2000 之类），
把它注册表、快捷方式、窗口属性翻一遍 —— 比我们这样试要快。

---

## 四、一条踩坑记录（关于构建）

**改过任何 `.h` 之后，把构建目录整个删掉重建。**

理由不是玄学：加一个虚函数或成员会改变类的布局。如果构建系统漏编了某个依赖文件，
两个 `.o` 就会带着**不同的布局**链到一起 —— 一调用虚函数就崩。表现为"编译链接全都
成功，程序一跑就崩"，而且崩的位置经常在 `Qt6Core.dll` / `ntdll.dll` 里，看着像
系统或框架的问题。前后遇到过三次，每次都是删掉 build 目录重建就好。

另外：**看构建输出里有没有 `Building` 行**。只看到 `Linking` 就等于什么都没重编，
拿旧二进制测出来的结论全是废的。

---

## 五、验证脚本

开发时用的验证脚本（SOAP 序列、GENA 订阅、画面调节像素比对等）不在仓库里，
它们编在开发机的临时目录下。

其中值得知道的两个测法：
- **画面调节怎么验**：光看"设进去没报错"不算数 —— 要抓两张截图逐像素比。
  实测时 `--sharpen` 是**完全无效**的（差 0 个像素），换成视频滤镜链里的
  `unsharp` 才真的管用。
- **协议怎么验**：三份 SCPD 和设备描述抓下来当 XML 解析一遍。手改过 XML 之后
  务必跑一次 —— 编译通过不代表 XML 合法。
