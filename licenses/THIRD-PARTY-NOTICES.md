# 第三方组件声明

> 这份清单对应的是**发布包里实际装了什么**，不是"开发时用过什么"。
> 开发期用到的工具另见文末一节 —— 它们不进发布包，所以不需要随包附文本。
>
> 本文件由项目维护者手工维护。增删依赖时请一并更新它和 `deploy-mcast.ps1`。

---

## 一、随包分发的组件

### 1. libmpv / FFmpeg

| 项 | 内容 |
|---|---|
| 文件 | `libmpv-2.dll`（约 115 MB，**内含整个 FFmpeg**） |
| 版本 | **mpv v0.41.0-1023-g69e63f425**（shinchiro 的 Windows 构建） |
| 许可 | **GPLv2 或更高**（`mpv is distributed under the terms of the GNU General Public License Version 2 or later.`）—— 这份构建启用了 GPL 组件：特性表含 `gpl`，并带 `dvdnav`/`dvdread`/`libbluray` |
| 版权 | mpv 项目及各位贡献者；FFmpeg 项目及各位贡献者；以及它捆绑的各库作者 |
| 许可全文 | `licenses/GPL-2.0.txt`（按 v2+ 的选项也可用 `GPL-3.0.txt`） |
| 源码 | **我们没有修改 mpv / FFmpeg。** 对应源码：<https://github.com/mpv-player/mpv>（commit `69e63f425`）、<https://ffmpeg.org/download.html> |
| 注 | 该 DLL 里同时含 **LGPLv2.1+** 的组件（FFmpeg 的 libav* 等，见 `licenses/LGPL-2.1.txt`），以及 dav1d、libass、libbluray、OpenSSL 等 |

### 2. Qt 6

| 项 | 内容 |
|---|---|
| 文件 | `Qt6*.dll`（35 个）、插件目录 `platforms/` `imageformats/` `iconengines/` `styles/` `tls/` `networkinformation/` `generic/` `qmltooling/`、以及 `qml/` 下的 Qt 模块（含 Qt5Compat、QtQuick 各样式） |
| 版本 | Qt **6.11.2**（msvc2022_64 构建） |
| 许可 | **LGPLv3**（Qt 另有商业许可） |
| 版权 | The Qt Company Ltd. 及各贡献者 |
| 许可全文 | `licenses/LGPL-3.0.txt`（该文件同时含 GPLv3 正文 —— LGPLv3 即"GPLv3 + 附加许可"） |
| 源码 | <https://download.qt.io/official_releases/qt/6.11/6.11.2/> |
| **我们怎么用的** | **动态链接**：不静态链接、不修改 Qt。这满足 LGPLv3 关于"用户能够替换该库"的要求 |
| 注 | Qt 自身还随附一批第三方组件（LLVM、zlib、PCRE2 等），完整清单以 Qt 官方文档的 Licensing 页为准 |

### 3. FluentUI（QML 组件库）

| 项 | 内容 |
|---|---|
| 文件 | `qml/FluentUI/**`（含 `fluentuiplugin.dll`） |
| 许可 | **MIT** |
| 版权 | Copyright (c) 2023 zhuzichu |
| 许可全文 | `licenses/MIT-FluentUI.txt` |
| 源码 | <https://github.com/zhuzichu520/FluentUI> |

### 4. Qt 随附的第三方二进制

| 文件 | 说明 |
|---|---|
| `D3Dcompiler_47.dll` | 微软 DirectX 着色器编译器（可再分发运行时组件） |
| `dxcompiler.dll` / `dxil.dll` | 微软 DirectX Shader Compiler |
| `opengl32sw.dll` | Qt 构建的软件 OpenGL（Mesa/llvmpipe），给没有可用 GPU 驱动的机器兜底 |

这几份的许可与再分发条件跟随 Windows SDK / Qt 发行版，不在此重复收录。
要做严格的合规审查时，以 Qt 官方文档里那份第三方清单为准。

### 5. 本项目自己的内容

`MCast.exe`、`lang/*.json`、`res/` 里的图标、`src/ui/qml/**` 下的界面 ——
都是本项目自己的，按 **GPL-3.0-or-later** 授权，
版权 `Wang Yunzheng <wyz-mcast@outlook.com>`。

---

## 二、参考过的开源实现（需要留意的一条）

**Macast**（<https://github.com/xfangfang/Macast>，GPLv3）：

`src/protocols/dlna/GenaManager.cpp` 里 AVTransport 事件字段那一段，注释写着
"这一段是照 Macast 抄的"。**如果那部分是逐字照搬，就属于衍生作品**，按 GPLv3
第 5(a) 条应当在文件里注明来源和修改。我们的项目本来就是 GPL-3.0-or-later，
**许可上是兼容的**，不存在冲突；但出处该标清楚。

**待确认**：那段到底是"照着思路写的"（字段集合本身不受版权保护）还是"逐字抄的"。
如果确认是后者，就在该文件头补一行出处说明（或者干脆按自己的写法重写一遍）。

---

## 三、开发期用到、但不随包分发的

这些只存在于开发/测试机上，**不进发布包**，所以无需随包附许可文本（这里只作记录）：

| 用途 | 工具 |
|---|---|
| 构建 | CMake、Ninja、MSVC（Visual Studio）、windeployqt（Qt 附带） |
| 抓包与观察 | Wireshark / tshark（GPLv2+） |
| 模拟器测试台 | MuMu 模拟器 |
| 测试用控制点 / 参照渲染器 | BubbleUPnP、AirScreen（仅用于观察互操作行为，未使用其代码） |
