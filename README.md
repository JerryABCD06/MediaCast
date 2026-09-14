# Media Cast Receiver

> [!WARNING]
> **Early Development / 早期开发**
>
> Media Cast is currently in early development. Pre-built releases are not yet available, but you can build it from source.
> Media Cast 处于早期开发阶段，暂不提供预编译的可下载版本，您可以从源码自行编译。

Windows 上的媒体投送接收器：手机投过来，电脑这边播放。

- 接收 **DLNA / UPnP AV** 投送（用 vivo 相册、BubbleUPnP 实测过）
- 播放后端是 **libmpv**，画面用 render API 合进 QML 界面
- 常驻托盘：关掉窗口还在接收；「暂停接收投送」能让手机暂时搜不到这台电脑
- 界面是 **Qt 6 + QML**（FluentUI 风格，跟随系统深浅色与语言）

## 目录

```
src/core/        播放器抽象接口、mpv 那一层、状态机、设置、语言
src/protocols/   协议层（现在只有 dlna/；加别的协议就再加一个同级目录）
src/ui/          QML + FluentUI 界面（唯一那套；旧 Widgets 界面已退场）
docs/            设计文档、待办、协议实现情况、法律声明
licenses/        随包分发的第三方许可全文 + 组件清单
```

## 构建

需要：**Qt 6.11+（MSVC 2022 64 位）**、Visual Studio 2022、CMake 3.21+、Ninja，
以及一份 **libmpv 开发包**（`CMakeLists.txt` 里的 `MPV_DEV_DIR` 指向它，
需要 `include/mpv/client.h` 和 `libmpv.lib`）。

> **第三方源码 FluentUI 不在本仓库里。** 它是一个独立的 clone（自己带 `.git`），
> 所以被 `.gitignore` 排除了。用的哪个提交、我们改了哪几行，都记在
> **`third_party/PATCHES.md`** —— 照着那份文档准备好它，才能开始编译。

```powershell
# 1) 按 third_party/PATCHES.md 准备好 third_party/FluentUI
# 2) 配置 + 编译
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
| `docs/legal/` | 隐私、法律、商标、编解码器声明 |
