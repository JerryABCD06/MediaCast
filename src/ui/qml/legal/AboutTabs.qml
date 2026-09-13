// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import FluentUI

// 设置 → 关于 里的那套标签页：关于 / 法律 / 开源许可。
//
// **它不是全局导航。** 只有"设置 → 关于"这一个位置有它 —— 投屏页、设置页的
// 其它类目都看不到。
//
// 三个页面本身都是**不含边框、不认识窗口**的普通 Item（AboutPane / DocPane /
// LicensesPane）。装在这里只是它们的一种用法；以后首启向导要链接过来时，
// 换成装进独立窗口就行，**页面本身一行都不用改**。
//
// ── 标签条为什么自己画、没用 FluPivot ─────────────────────────────────────
//
// FluPivot 的内容区是一个 `anchors.fill: parent` 的容器，会连标签条一起盖住
// （它的 FluLoader 铺满整页）。与其跟它绕，不如自己摆这 40 像素。
// 样式照它来，免得两处看着不一样：高 40、选中项底下 3 像素圆角指示条、
// 颜色取主题强调色、动画 167 毫秒（库自己的标准）。
Item {
    id: root

    /** 0 = 关于，1 = 法律，2 = 开源许可 */
    property int currentIndex: 0

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── 标签条 ────────────────────────────────────────────────────────
        Item {
            id: strip
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            Layout.bottomMargin: 8

            Row {
                id: tabRow
                anchors.left: parent.left
                height: parent.height
                spacing: 20                       // 和 FluPivot 的 headerSpacing 一致

                Repeater {
                    id: tabRepeater
                    model: [
                        qsTr("ui_legal_tab_about"),
                        qsTr("ui_legal_tab_legal"),
                        qsTr("ui_legal_tab_licenses")
                    ]

                    delegate: Item {
                        id: tabItem
                        required property int index
                        required property string modelData

                        width: label.implicitWidth
                        height: strip.height

                        FluText {
                            id: label
                            anchors.centerIn: parent
                            text: tabItem.modelData
                            font: FluTextStyle.Body
                            textColor: root.currentIndex === tabItem.index
                                       ? (FluTheme.dark ? FluColors.Grey10 : FluColors.Black)
                                       : FluTheme.fontSecondaryColor
                        }

                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.currentIndex = tabItem.index
                        }
                    }
                }
            }

            // 选中项下面那道线。跟着当前标签走，换页时滑过去。
            Rectangle {
                id: indicator
                height: 3
                radius: 1.5
                color: FluTheme.primaryColor
                y: strip.height - 3

                readonly property Item currentTab: tabRepeater.itemAt(root.currentIndex)
                x: currentTab ? currentTab.x : 0
                width: currentTab ? currentTab.width : 0

                Behavior on x {
                    NumberAnimation { duration: FluTheme.animationEnabled ? 167 : 0 }
                }
                Behavior on width {
                    NumberAnimation { duration: FluTheme.animationEnabled ? 167 : 0 }
                }
            }
        }

        // ── 内容区 ────────────────────────────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            AboutPane    { anchors.fill: parent; visible: root.currentIndex === 0 }
            DocPane      { anchors.fill: parent; visible: root.currentIndex === 1 }
            LicensesPane { anchors.fill: parent; visible: root.currentIndex === 2 }
        }
    }
}
