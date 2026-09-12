import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import MediaCast 1.0
import "components"

// 设置页。左右两栏，照 Windows 11 设置应用：
//
//   ┌──────────┬──────────────────────────────────────────────┐
//   │ 界面      │ [图标] 语言          说明一句        [下拉框] │
//   │ 投送      │ [图标] 深浅模式      说明一句        [下拉框] │
//   └──────────┴──────────────────────────────────────────────┘
//
// ── 类目为什么这么分 ────────────────────────────────────────────────────
//
// 三条原则：
//
//   一、**按"用户想干什么"分，不按"程序里有什么模块"分。** 没人关心 GENA 在
//       哪一层，他们关心的是"手机能不能找到我"。
//   二、**绕的、危险的选项下沉。** 每个类目第一张卡片永远是"不懂的人也需要
//       知道的那个"；其余归到「高级」标题底下。不看的人根本不用往下看。
//   三、**宁可有 2 个类目，也不要造空壳类目。** 一个类目里孤零零一条会显得敷衍，
//       用户还会以为漏了什么。
//
// 现在只有 2 个类目，因为设置一共就五项。将来长出来的（播放、网络、开机自启）
// 各自独立成类目就行，这里的结构不用动。
//
// **「关于」不在这儿** —— 它在顶栏那个 ⓘ 里（设备名、设备标识、版本、日志位置）。
// 那是"看"的地方，不是"改"的地方；混进设置页只会让这一页变长。
Item {
    id: page

    /** 现在看的是哪个类目：0 = 界面，1 = 投送。 */
    property int category: 0

    /**
     * 语言列表：显示名 + 传给 UiState 的代码。空字符串 = 跟随系统。
     *
     * **这些是扫出来的，不是写死的。** Tr 启动时扫过 exe 旁边的 lang/ 目录，
     * 往那个目录里丢一个 json 文件，这里就多一项 —— 加语言不用改代码。
     *
     * 语言名用文件里写的那句原文（中文文件里写"简体中文"，英文文件里写
     * "English"），**不翻译**：翻成 "Chinese" 反而让不懂英文的人认不出来。
     */
    readonly property var languages: {
        var list = [ { title: qsTr("ui_settings_language_system"), code: "" } ]
        var found = Tr.languages()
        for (var i = 0; i < found.length; ++i)
            list.push({ title: found[i].name, code: found[i].code })
        return list
    }

    /** 深浅模式：显示名 + UiState.ThemeMode 的值。 */
    readonly property var themeModes: [
        { title: qsTr("ui_settings_theme_system"), mode: UiState.System },
        { title: qsTr("ui_settings_theme_light"), mode: UiState.Light },
        { title: qsTr("ui_settings_theme_dark"), mode: UiState.Dark }
    ]

    /** 广播间隔的几档。数字是毫秒。 */
    readonly property var broadcastIntervals: [5000, 10000, 30000, 60000]

    function intervalLabel(ms) {
        return qsTr("ui_settings_interval_seconds").arg(ms / 1000)
    }

    RowLayout {
        anchors.fill: parent
        // 页面从窗口边缘开始铺（投屏页要画面铺满，这一层不许留边），
        // 所以内边距在这儿自己加。
        anchors.margins: 24
        spacing: 24

        // ── 左：类目 ─────────────────────────────────────────────────────
        Column {
            Layout.preferredWidth: 176
            Layout.fillHeight: true
            spacing: 2

            Repeater {
                model: [
                    { title: qsTr("ui_settings_category_ui"), icon: FluentIcons.Personalize },
                    { title: qsTr("ui_settings_category_cast"), icon: FluentIcons.Project },
                    { title: qsTr("ui_settings_category_about"), icon: FluentIcons.Info }
                ]

                delegate: Rectangle {
                    id: navItem
                    required property var modelData
                    required property int index

                    width: parent.width
                    height: 40
                    radius: 5
                    color: page.category === index
                           ? FluTheme.itemPressColor
                           : (hoverHandler.hovered ? FluTheme.itemHoverColor
                                                   : Qt.rgba(0, 0, 0, 0))

                    // 选中时左边那道强调竖线 —— Windows 11 就是这么做标记的。
                    Rectangle {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: 16
                        radius: 1.5
                        visible: page.category === index
                        color: FluTheme.primaryColor
                    }

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 12

                        // 单色图标，从库的图标表里挑。
                        FluIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            iconSource: modelData.icon
                            iconSize: 16
                        }
                        FluText {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.title
                            font: FluTextStyle.Body
                        }
                    }

                    HoverHandler {
                        id: hoverHandler
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: page.category = navItem.index
                    }
                }
            }
        }

        // ── 右：当前类目的卡片 ───────────────────────────────────────────
        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: cardColumn.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FluScrollBar {}

            ColumnLayout {
                id: cardColumn
                width: scroller.width - 12      // 给滚动条让位
                spacing: 8

                // ══ 类目：界面 ═══════════════════════════════════════════
                //
                // 这两项是"每个人都会碰"的，所以放在最前面、不加任何层级。

                SettingsCard {
                    visible: page.category === 0
                    icon: FluentIcons.Globe
                    title: qsTr("ui_settings_language")
                    subtitle: qsTr("ui_settings_language_desc")

                    FluComboBox {
                        Layout.preferredWidth: 150
                        model: page.languages.map(function (item) { return item.title })
                        currentIndex: {
                            for (var i = 0; i < page.languages.length; ++i)
                                if (page.languages[i].code === UiState.language)
                                    return i
                            return 0
                        }
                        onActivated: function (index) {
                            UiState.language = page.languages[index].code
                        }
                    }
                }

                SettingsCard {
                    visible: page.category === 0
                    icon: FluentIcons.Personalize
                    title: qsTr("ui_settings_theme")
                    subtitle: qsTr("ui_settings_theme_desc")

                    FluComboBox {
                        Layout.preferredWidth: 150
                        model: page.themeModes.map(function (item) { return item.title })
                        currentIndex: {
                            for (var i = 0; i < page.themeModes.length; ++i)
                                if (page.themeModes[i].mode === UiState.themeMode)
                                    return i
                            return 0
                        }
                        onActivated: function (index) {
                            UiState.themeMode = page.themeModes[index].mode
                        }
                    }
                }

                // ══ 类目：投送 ═══════════════════════════════════════════
                //
                // 第一张是小白需要的（一个开关，一句话说清后果）；下面两条归到
                // 「高级」。不懂的人看完第一条就可以走了。

                SettingsCard {
                    visible: page.category === 1
                    icon: FluentIcons.Send
                    title: qsTr("ui_settings_accept")
                    subtitle: qsTr("ui_settings_accept_desc")

                    // 不写 `checked: Settings.castNewCast` 这种绑定：开关自己点一下
                    // 就会写 checked，绑定当场被打断，之后（比如托盘点勿扰）就再也
                    // 跟不回来了。所以平时靠 Connections 跟，点的时候写回 C++。
                    FluToggleSwitch {
                        id: switchNewCast
                        Component.onCompleted: checked = Settings.castNewCast
                        Connections {
                            target: Settings
                            function onAcceptNewCastChanged() {
                                switchNewCast.checked = Settings.castNewCast
                            }
                        }
                        clickListener: function () {
                            Settings.castNewCast = !Settings.castNewCast
                        }
                    }
                }

                FluText {
                    visible: page.category === 1
                    Layout.topMargin: 12
                    text: qsTr("ui_settings_advanced")
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontSecondaryColor
                }

                SettingsCard {
                    visible: page.category === 1
                    icon: FluentIcons.Sync
                    title: qsTr("ui_settings_broadcast")
                    subtitle: qsTr("ui_settings_broadcast_desc")

                    FluToggleSwitch {
                        id: switchBroadcast
                        Component.onCompleted: checked = Settings.castBroadcast
                        Connections {
                            target: Settings
                            function onBroadcastChanged() {
                                switchBroadcast.checked = Settings.castBroadcast
                            }
                        }
                        clickListener: function () {
                            Settings.castBroadcast = !Settings.castBroadcast
                        }
                    }
                }

                SettingsCard {
                    visible: page.category === 1
                    icon: FluentIcons.Ringer
                    title: qsTr("ui_settings_interval")
                    subtitle: qsTr("ui_settings_interval_desc")
                    enabled: Settings.castBroadcast

                    FluComboBox {
                        Layout.preferredWidth: 150
                        model: page.broadcastIntervals.map(function (ms) {
                            return page.intervalLabel(ms)
                        })
                        currentIndex: Math.max(0,
                            page.broadcastIntervals.indexOf(Settings.castBroadcastInterval))
                        onActivated: function (index) {
                            Settings.castBroadcastInterval =
                                page.broadcastIntervals[index]
                        }
                    }
                }

                // ══ 类目：关于 ═══════════════════════════════════════════
                //
                // 这一栏**全是只读的** —— 它是"看"的地方：排错、报问题、或者想确认
                // "手机上一堆设备里哪个是我"的时候用。所以右边不放控件，只放值。
                //
                // 数据来自 Device（就是 DlnaRenderer 那个门面，只暴露了这几项只读
                // 属性）。界面拿不到 DlnaRenderer 的任何操作能力。

                SettingsCard {
                    visible: page.category === 2
                    icon: FluentIcons.TVMonitor
                    title: qsTr("ui_about_device_name")
                    subtitle: qsTr("ui_about_device_name_desc")

                    FluText {
                        text: Device.deviceName
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }
                }

                SettingsCard {
                    visible: page.category === 2
                    icon: FluentIcons.Link
                    title: qsTr("ui_about_device_address")
                    subtitle: qsTr("ui_about_device_address_desc")

                    FluText {
                        text: Device.locationUrl
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontSecondaryColor
                    }
                }

                SettingsCard {
                    visible: page.category === 2
                    icon: FluentIcons.Info
                    title: qsTr("ui_about_device_id")
                    subtitle: qsTr("ui_about_device_id_desc")

                    FluText {
                        text: Device.udn
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontSecondaryColor
                    }
                }

                SettingsCard {
                    visible: page.category === 2
                    icon: FluentIcons.Settings
                    title: qsTr("ui_about_version")

                    FluText {
                        text: "Media Cast " + Device.appVersion
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }
                }
            }
        }
    }
}
