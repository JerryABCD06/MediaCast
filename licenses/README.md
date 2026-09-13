# licenses —— 随包分发的第三方许可文本

这个目录里的文件**会跟着发布包一起发出去**（`work\deploy-mcast.ps1` 负责拷），
不是只放在仓库里看的。

## 里面有什么

| 文件 | 是什么 | 谁需要它 |
|---|---|---|
| `THIRD-PARTY-NOTICES.md` | **组件清单**：包里装了哪些第三方、各自什么许可、源码在哪 | 先看这个 |
| `GPL-2.0.txt` | GNU 通用公共许可 第 2 版全文 | libmpv（它是 GPLv2 **或更高**，所以 v2 这份也得给） |
| `GPL-3.0.txt` | GNU 通用公共许可 第 3 版全文（和仓库根目录的 `LICENSE` 同一份） | 本项目自身（`GPL-3.0-or-later`），也是 libmpv 可选的更高版本 |
| `LGPL-2.1.txt` | GNU 宽通用公共许可 2.1 全文 | libmpv 里捆进来的那些 LGPL 组件（FFmpeg 的 libav* 等） |
| `LGPL-3.0.txt` | GNU 宽通用公共许可 3.0 全文 | Qt 6 |
| `MIT-FluentUI.txt` | MIT 许可全文 + 版权行 | FluentUI（QML 组件库） |

## 两点要注意

1. **`LGPL-3.0.txt` 里同时含 GPLv3 的正文。** 这不是弄错了 —— LGPLv3 本身就是
   "GPLv3 + 一组附加许可"，官方发布的文本把两者拼在一起。我们直接从 SPDX 的
   权威镜像取的，没做任何改动。
2. **许可正文一律不改。** 能"填"的只有项目自己的声明（版权行 + "or later" 那句），
   那些写在源文件的 SPDX 头、`NOTICE.md` 和这里，不写进正文里。

## 加了新依赖时怎么办

三件事一起做，别漏：

1. 把它的许可全文放进本目录；
2. 在 `THIRD-PARTY-NOTICES.md` 里加一行（组件 / 版本 / 许可 / 版权 / 源码在哪）；
3. 确认 `deploy-mcast.ps1` 会把它带到发布包里。
