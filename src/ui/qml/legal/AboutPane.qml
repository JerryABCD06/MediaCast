// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import MediaCast 1.0
import "../components"

// 设置 → 关于 → 第一页「关于」。
//
// 这四张卡片是从 SettingsPage.qml **原样搬过来的** —— 只去掉了原来那句
// `visible: page.category === 2`（现在由标签页决定谁显示），内容一个字没改。
// 所以卡片的外观、间距、滚动条都还和以前一致。
//
// 数据来自 Device（就是 DlnaRenderer 那个门面，只暴露了这几项只读属性）。
// 界面拿不到 DlnaRenderer 的任何操作能力。
Item {
    id: root

    Flickable {
        anchors.fill: parent
        clip: true
        contentWidth: width
        contentHeight: cardColumn.height
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: FluScrollBar {}

        ColumnLayout {
            id: cardColumn
            width: root.width - 12          // 给滚动条让位
            spacing: 8

            SettingsCard {
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
