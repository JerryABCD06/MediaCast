# Media Cast Receiver

> [!WARNING]
> **Early Development / 早期开发**
>
> Media Cast is currently in early development. Pre-built releases are not yet available, but you can build it from source.
> Media Cast 处于早期开发阶段，暂不提供预编译的可下载版本，您可以从源码自行编译。

Windows 上的媒体投送接收器：手机投过来，电脑这边播放。

生产入口是 `MCast.Shell.exe / MCast.Protocol.exe / MCast.Ui.exe` 三进程架构，
已覆盖原生托盘、启动屏幕、DLNA Protocol、按需 MainWindow、系统媒体控件和
语言包。仓库不再构建旧单进程入口。

- 接收 **DLNA / UPnP AV** 投送（用 vivo 相册、BubbleUPnP 实测过）
- 播放后端是 **libmpv**，画面用 render API 合进 QML 界面
- 常驻托盘：关掉窗口还在接收；「暂停接收投送」能让手机暂时搜不到这台电脑
- 界面是 **Qt 6 + QML**（FluentUI 风格，跟随系统深浅色与语言）

## 目录

```
src/core/        播放器抽象接口、mpv、状态机和语言
src/protocols/   无 Qt 的协议层（现在只有 dlna/）
src/platform/    Qt/系统集成端口（媒体控件、托盘、窗口和网络基础设施）
src/shell/       原生 Shell、配置权威和进程监督
src/ui_process/  Ui 进程、IPC、播放桥和设置快照
src/ui/          QML + FluentUI MainWindow
docs/            设计文档、待办、协议实现情况、法律声明
licenses/        随包分发的第三方许可全文 + 组件清单
```

## 构建

需要：**Qt 6.11+（MSVC 2022 64 位）**、Visual Studio 2022、CMake 3.21+、Ninja，
以及一份 **libmpv 开发包**（`CMakeLists.txt` 里的 `MPV_DEV_DIR` 指向它，
需要 `include/mpv/client.h` 和 `libmpv.lib`）。

> **第三方源码 FluentUI 已随仓库提供。** 上游 `.git-upstream/` 被忽略，实际
> 使用提交和本项目补丁记录在 **`third_party/PATCHES.md`**。

```powershell
# 配置 + 编译
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_PREFIX_PATH=<你的 Qt 路径>
cmake --build build
```

## 许可

**GPL-3.0-or-later** —— 全文见 `LICENSE`。
版权 `Wang Yunzheng <wyz-mcast@outlook.com>`。

本程序随包分发几个第三方组件，各自的许可见
**`licenses/THIRD-PARTY-NOTICES.md`**：libmpv / FFmpeg（GPLv2 或更高）、
Qt 6（LGPLv3，动态链接、未修改）、FluentUI（MIT）。

> ⚠️ 编解码器涉及的**专利**是另一回事（H.264 / H.265 / AAC 等有专利池），
> 软件许可不涵盖它。详见 `docs/legal/CODECS.md`。

## 文档从哪儿看起

| 文件 | 内容 |
|---|---|
| `docs/状态速览.md` | 一屏看完：路径、怎么构建怎么跑、分层、会咬人的约束 |
| `docs/待办.md` | 现在到哪儿了、下一步干什么、踩过的坑 |
| `docs/控制点行为.md` | 真实控制点（vivo 相册、BubbleUPnP）**实际**怎么行为 |
| `docs/DLNA实现情况.md` | 协议实现到什么程度 |
| `docs/多进程重构交接.md` | 当前三进程边界、启动/重连时序、验证入口和下一步顺序 |
| `docs/DLNA迁移分阶段.md` | DLNA 迁入无 Qt Protocol 的历史阶段记录 |
| `docs/配置权威与设置同步.md` | Shell 配置权威、JSON 结构和 UI 设置快照/IPC |
| `docs/legal/` | 隐私、法律、商标、编解码器声明 |
