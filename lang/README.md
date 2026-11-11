# 语言包

一种语言就是一个目录：

```text
lang/
  en_US/
    manifest.json
    locale-aliases.txt
    shell.json
    ui.json

  zh_CN/
    manifest.json
    locale-aliases.txt
    shell.json
    ui.json
```

目录名就是语言代码，也是写进 `MediaCast.json` 的
`ui.language` 值。目录名只允许英文字母、数字和下划线，并且必须以字母开头。

四个文件缺任何一个，整个语言包都无效。程序不会加载半套语言包。

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
  "tray_tooltip_receiving": "Media Cast",
  "tray_tooltip_paused": "Media Cast（已暂停接收投送）"
}
```

六个键都必须是非空字符串。Shell 直接读取这个文件，不依赖 Qt。

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

Shell 只检查文件存在且是合法 JSON 对象，并把绝对路径下发给 Ui。Ui 收到后
自己读取 `ui.json`。Shell 不转发界面正文，也不解析其中的键值。

## 启动顺序

```text
Shell 扫描 lang/*/
  -> 校验目录名、manifest、四个文件和区域别名
  -> 生成完整语言目录
  -> 根据设置和 Windows 首选语言选出实际语言
  -> 加载 shell.json
  -> Ui 连接
  -> Shell 发送 LanguageCatalog
  -> Shell 发送 SettingsSnapshot（含实际语言代码）
  -> Ui 读取对应目录的 ui.json
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

磁盘上有效的 `en_US` 包优先于内置副本，因此修改英文文案通常不需要重新编译。

## 添加一种语言

1. 复制 `lang/en_US/` 为 `lang/<语言代码>/`。
2. 修改 `manifest.json` 的 `language` 和 `name`。
3. 修改 `locale-aliases.txt`。
4. 翻译 `shell.json`。
5. 翻译 `ui.json`。
6. 重新启动程序。

不用重新编译。新目录通过校验后会自动出现在语言列表里。

## 键名规则

小写字母加下划线，用第一段表示所在位置：

| 前缀 | 用在哪 |
|---|---|
| `tray_` | `shell.json`，原生托盘 |
| `ui_` | `ui.json`，QML 主界面 |
| `media_` | 内容类型和来源，供界面及 Windows 媒体面板共用 |

后面按“位置_东西”继续写，例如 `ui_settings_language`。

## 文案规则

- 中文和拉丁字母、数字之间留一个半角空格。
- 中文句子使用全角标点，英文句子使用半角标点。
- 按钮和状态标签尽量短。
- 同一个概念在所有位置使用同一个词。
- 日志不翻译。
- 参数占位符沿用 Qt 的 `%1` 形式。

某个键缺失时会落回内置英文；多出的键会被忽略。两种情况都会写日志。
