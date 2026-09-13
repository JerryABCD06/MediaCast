// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import FluentUI

// SettingsCard —— 设置页里的一张卡片。照 Windows 11 设置应用的样子：
//
//   [图标]  标题                                [右边放控件]
//           一句话说清这一项是干什么的、改了会怎样
//
// **副标题不是装饰。** 想让不懂的人自己能看懂，靠的就是那一句 ——
// 把选项藏起来只会让人更懵，"把后果说清楚"才是正经办法。
//
// 右边放什么由使用方决定（开关、下拉框、按钮，或者什么都不放，那就退化成一条
// 说明）。所以这里用一个默认属性把内容透传出去：
//
//     SettingsCard {
//         icon: FluentIcons.Globe
//         title: qsTr("ui_settings_language")
//         subtitle: qsTr("ui_settings_language_desc")
//         FluComboBox { model: [...] }
//     }
//
// 注意 title / subtitle 走的是**键名**，不是中文原文 —— 键名和译文在
// lang/*.json 里，规矩见 lang/README.md。
FluFrame {
    id: card

    property int icon: 0
    property string title: ""
    property string subtitle: ""

    /** 整张卡片可点（点了发 clicked）。默认关着 —— 只有"点一下有下文"的才开。 */
    property bool clickable: false
    signal clicked()

    /** 右边那个控件（用默认属性直接塞进来）。 */
    default property alias control: controlHost.data

    Layout.fillWidth: true
    // 高度自己算：一行约 40，副标题折行时能长高，但不至于矮得挤。
    implicitHeight: Math.max(72, rowLayout.implicitHeight + 24)
    padding: 0

    // **必须声明在 RowLayout 之前**：Qt Quick 的兄弟节点里后声明的在上面，
    // 所以铺在下面的这一层只管接"没被别人接走的"点击 —— 右边那些按钮照旧
    // 优先拿到事件，空白处的点击才落到这儿。
    //
    // 早先试过在卡片外面套一层 Item 挂 TapHandler：不行。卡片（FluFrame）盖在
    // 上面把事件接走了，点击根本到不了外层。让卡片自己可点才对。
    MouseArea {
        anchors.fill: parent
        enabled: card.clickable
        cursorShape: Qt.PointingHandCursor
        onClicked: card.clicked()
    }

    RowLayout {
        id: rowLayout
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        spacing: 16

        FluIcon {
            Layout.alignment: Qt.AlignVCenter
            visible: card.icon !== 0
            iconSource: card.icon
            iconSize: 18
            iconColor: FluTheme.fontPrimaryColor
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 2

            FluText {
                Layout.fillWidth: true
                text: card.title
                font: FluTextStyle.Body
                elide: Text.ElideRight
                maximumLineCount: 1
            }

            FluText {
                Layout.fillWidth: true
                visible: card.subtitle !== ""
                text: card.subtitle
                font: FluTextStyle.Caption
                textColor: FluTheme.fontSecondaryColor
                wrapMode: Text.Wrap
            }
        }

        // 右边控件的容器。宽度跟着内容走 —— 卡片不去猜控件该多宽。
        Item {
            id: controlHost
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: childrenRect.width
            implicitHeight: childrenRect.height
        }
    }
}
