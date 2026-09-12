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

    /** 语言列表：显示名 + 传给 UiState 的代码。空字符串 = 跟随系统。 */
    readonly property var languages: [
        { title: qsTr("跟随系统"), code: "" },
        { title: "中文", code: "zh_CN" },
        { title: "English", code: "en_US" }
    ]

    /** 深浅模式：显示名 + UiState.ThemeMode 的值。 */
    readonly property var themeModes: [
        { title: qsTr("跟随系统"), mode: UiState.System },
        { title: qsTr("浅色"), mode: UiState.Light },
        { title: qsTr("深色"), mode: UiState.Dark }
    ]

    /** 广播间隔的几档。数字是毫秒。 */
    readonly property var broadcastIntervals: [5000, 10000, 30000, 60000]

    function intervalLabel(ms) {
        return qsTr("%1 秒").arg(ms / 1000)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 24

        // ── 左：类目 ─────────────────────────────────────────────────────
        Column {
            Layout.preferredWidth: 176
            Layout.fillHeight: true
            spacing: 2

            Repeater {
                model: [
                    { title: qsTr("界面"), icon: FluentIcons.Personalize },
                    { title: qsTr("投送"), icon: FluentIcons.Project }
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
                    title: qsTr("语言")
                    subtitle: qsTr("界面上的文字用哪种语言。跟随系统就是跟 Windows 走")

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
                    title: qsTr("深浅模式")
                    subtitle: qsTr("界面的亮暗。跟随系统就是跟 Windows 走")

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
                    title: qsTr("接受新的投送")
                    subtitle: qsTr("关掉之后，这台电脑会从手机的可投屏设备里消失（等于勿扰）")

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
                    text: qsTr("高级 —— 一般不用改")
                    font: FluTextStyle.Caption
                    textColor: FluTheme.fontSecondaryColor
                }

                SettingsCard {
                    visible: page.category === 1
                    icon: FluentIcons.Sync
                    title: qsTr("定期广播")
                    subtitle: qsTr("定期喊一声「我在」。关掉只是不主动喊，手机主动搜还是能找到这台电脑")

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
                    title: qsTr("广播间隔")
                    subtitle: qsTr("越短越容易被搜到，网络上也越吵。手机上搜不到这台电脑时，先往短里调")
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
            }
        }
    }
}
