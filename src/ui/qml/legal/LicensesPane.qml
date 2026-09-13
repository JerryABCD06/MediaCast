// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import MediaCast 1.0
import "../components"

// 设置 → 关于 → 第三页「开源许可」。
//
// 排序和"谁在最上面"**不在这儿决定** —— 那是 Legal 那边的规矩（本项目置顶、
// 第三方按首字母），界面拿到什么顺序就画什么顺序。规矩只有一处来源。
//
// 每一行右边两个东西：
//   「查看许可证书」按钮 —— 当场读 licenses/ 下的全文
//   ↗ 小图标          —— 用系统浏览器打开它的项目页
//
// **为什么两者都要**：查看证书是让用户确认"它的条款允许我这么用"；打开项目页
// 是让他看"这是什么东西、源码在哪"。GPL 和 LGPL 要求分发时提供**对应源码的
// 获取方式**，所以项目页那一栏不是装饰，是义务。
Item {
    id: root

    property string viewingFile: ""
    property string viewingTitle: ""
    property string viewingBody: ""
    property string viewingError: ""

    // 同上：离开就把全文收起来，回来时看到的是列表。
    onVisibleChanged: if (!visible) viewingFile = ""

    function openLicense(entry) {
        root.viewingTitle = entry.name + " — " + entry.license
        root.viewingBody = Legal.readLicense(entry.licenseFile)
        root.viewingError = root.viewingBody === ""
                            ? qsTr("ui_legal_read_failed").arg(Legal.lastError()) : ""
        root.viewingFile = entry.licenseFile
    }

    // 一行的样子在这儿统一定义 —— 本项目那条和第三方那几条长得一样，
    // 区别只在数据来自哪儿。
    component EntryCard: Item {
        id: entryItem
        required property var entry

        Layout.fillWidth: true
        implicitHeight: card.implicitHeight

        SettingsCard {
            id: card
            anchors.fill: parent
            icon: entryItem.entry.isSelf === true ? FluentIcons.Library : FluentIcons.Certificate
            title: entryItem.entry.name
            // 版权方 · 许可 —— 两项都按他说的标出来
            subtitle: entryItem.entry.copyright + " · " + entryItem.entry.license

            RowLayout {
                spacing: 4

                FluButton {
                    text: qsTr("ui_legal_view_license")
                    onClicked: root.openLicense(entryItem.entry)
                }

                TipIconButton {
                    iconSource: FluentIcons.OpenInNewWindow
                    contentDescription: qsTr("ui_legal_open_homepage")
                    onClicked: Qt.openUrlExternally(entryItem.entry.homepage)
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        visible: root.viewingFile === ""
        spacing: 8

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: listColumn.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FluScrollBar {}

            ColumnLayout {
                id: listColumn
                width: scroller.width - 12
                spacing: 8

                // 本项目自己 —— 单独置顶
                EntryCard { entry: Legal.project }

                // 和下面的第三方留出间隔
                Item { Layout.preferredHeight: 16 }

                Repeater {
                    model: Legal.components
                    delegate: EntryCard {
                        required property var modelData
                        entry: modelData
                    }
                }
            }
        }
    }

    TextViewer {
        anchors.fill: parent
        visible: root.viewingFile !== ""
        title: root.viewingTitle
        body: root.viewingBody
        errorText: root.viewingError
        onBackRequested: root.viewingFile = ""
    }
}
