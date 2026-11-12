# 语言包

一种语言就是一个目录：

```text
lang/
  en_US/
    manifest.json
    locale-aliases.txt
    shell.json
    ui.json
    terms.json

  zh_CN/
    manifest.json
    locale-aliases.txt
    shell.json
    ui.json
    terms.json
```

目录名就是语言代码，也是写进 `MediaCast.json` 的
`ui.language` 值。目录名只允许英文字母、数字和下划线，并且必须以字母开头。

五个文件缺任何一个，整个语言包都无效。程序不会加载半套语言包。

## `manifest.json`

```json
{
  "type": "MediaCast.LanguagePack",
  "schemaVersion": 1,
  "language": "zh_CN",
  "name": "简体中文"
}
```

要求：

- `type` 必须严格等于 `MediaCast.LanguagePack`。
- `schemaVersion` 当前必须是 `1`。
- `language` 必须和目录名完全一致。
- `name` 是语言选择列表里显示的名字，不能为空。

## `locale-aliases.txt`

这个文件列出 Windows 可能用来表示该语言的区域标识。

```text
# Windows GetUserPreferredUILanguages() 可能返回的写法
zh-CN
zh-Hans-CN
zh-SG
zh-Hans-SG
zh_CN
zh_Hans_CN
```

规则：

- UTF-8 编码。
- 一行一个标识。
- 空行忽略。
- `#` 开头是注释。
- 中划线和下划线等价，大小写不敏感。
- 文件可以为空；空文件表示只能手动选择，不参与“跟随系统”的自动匹配。

“跟随系统”由 Shell 解析。它会按 Windows 提供的首选语言顺序逐个匹配，
找到第一个可用的语言包。Ui 不读取这个文件，也不自行判断系统语言。

同一个区域标识不能同时属于两个语言包。发生冲突时，程序会记录警告并忽略该
别名，不允许最终结果取决于目录枚举的偶然顺序。

## `shell.json`

只放 Shell 与系统托盘使用的文字：

```json
{
  "tray_open_main": "打开主界面",
  "tray_pause_receiving": "暂停接收投送",
  "tray_resume_receiving": "恢复接收投送",
  "tray_quit": "退出",
  "tray_tooltip_receiving": "{app_short}",
  "tray_tooltip_paused": "{app_short}（已暂停接收投送）"
}
```

六个键都必须是非空字符串。Shell 直接读取这个文件，不依赖 Qt。
`{app_short}` 会由 Shell 从 `terms.json` 的 `term_app_short` 替换。

## `terms.json`

公共术语表。UI 和 Shell 都读取这份文件；Protocol 不读取、不本地化，只发送
稳定协议 ID。

```json
{
  "strings": {
    "term_app_full": "媒体投屏接收器",
    "term_app_short": "媒体投屏",
    "term_protocol_dlna_full": "数字生活网络联盟（DLNA）",
    "term_protocol_dlna_short": "DLNA"
  }
}
```

维护约定是每个已支持协议都提供 `term_protocol_<id>_full` 和
`term_protocol_<id>_short`，应用名称固定使用 `term_app_full` 和
`term_app_short`。当前加载器强制要求 `strings` 对象及合法值，但不会枚举全部
未来协议键逐个强制；缺键由 Ui 记录日志并回退英文。

## `ui.json`

只放 QML 界面文字：

```json
{
  "strings": {
    "ui_mediabar_play": "播放",
    "ui_mediabar_pause": "暂停"
  }
}
```

`ui.json` 和 `terms.json` 的顶层必须是 `strings` 对象；Shell 扫描语言包时会
检查其条目均为非空字符串，但不把正文转发给 Ui。Ui 按 Shell 指定的实际语言目录
读取 `ui.json` 和 `terms.json`；缺失键回退英文，多出的键忽略并记录日志。

## 启动顺序

```text
Shell 扫描 lang/*/
  -> 校验目录名、manifest、五个文件和区域别名
  -> 生成完整语言目录
  -> 根据设置和 Windows 首选语言选出实际语言
  -> 加载 shell.json
  -> 加载 terms.json
  -> Ui 连接
  -> Shell 发送 LanguageCatalog
  -> Shell 发送 SettingsSnapshot（含实际语言代码）
  -> Ui 读取对应目录的 ui.json + terms.json
  -> Ui 创建 QML
```

Ui 不扫描 `lang/`，也不知道机器上总共有多少语言。设置页里的语言列表完全来自
Shell 下发的语言目录。

## 跟随系统

设置文件里保存：

```json
{
  "ui": {
    "language": "System"
  }
}
```

本次运行实际使用的语言由 Shell 算出，例如 `zh_CN`。这个结果只存在于运行时
快照，不写回设置文件。

如果所有区域别名都没有匹配：

- Shell 和 Ui 使用基准语言 `en_US`。
- 设置仍保留 `System`。
- 日志记录 Windows 候选语言和回退原因。

如果用户明确选择的语言包后来被删除：

- 本次运行回退 `en_US`。
- 原设置值保留，语言包重新安装后可以恢复。

## 基准语言

`en_US` 是基准语言。即使整个 `lang` 目录消失，程序也必须可用：

- `lang/en_US/shell.json` 编进 Shell 作为英文托盘兜底。
- `lang/en_US/ui.json` 编进 Ui 作为英文界面兜底。
- `lang/en_US/terms.json` 编进 Ui 作为英文公共术语兜底；Shell 也有同样的
  硬编码英文回退。

磁盘上有效的 `en_US` 包优先于内置副本，因此修改英文文案通常不需要重新编译。

## 添加一种语言

1. 复制 `lang/en_US/` 为 `lang/<语言代码>/`。
2. 修改 `manifest.json` 的 `language` 和 `name`。
3. 修改 `locale-aliases.txt`。
4. 翻译 `shell.json`。
5. 翻译 `ui.json`。
6. 翻译 `terms.json`。
7. 重新启动程序。

不用重新编译。新目录通过校验后会自动出现在语言列表里。

## 键名规则

小写字母加下划线，用第一段表示所在位置：

| 前缀 | 用在哪 |
|---|---|
| `tray_` | `shell.json`，原生托盘 |
| `ui_` | `ui.json`，QML 主界面 |
| `media_` | `ui.json`，内容类型和来源，供界面及 Windows 媒体面板共用 |
| `term_` | `terms.json`，Ui 与原生 Shell 共用的专有名词 |

后面按“位置_东西”继续写，例如 `ui_settings_language`。

## 文案规则

- 中文和拉丁字母、数字之间留一个半角空格。
- 中文句子使用全角标点，英文句子使用半角标点。
- 按钮和状态标签尽量短。
- 同一个概念在所有位置使用同一个词。
- 日志不翻译。
- 参数占位符沿用 Qt 的 `%1` 形式。

某个键缺失时会落回内置英文；多出的键会被忽略。两种情况都会写日志。
