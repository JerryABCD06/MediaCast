// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import "../components"

// 看全文的一页：一行"返回 + 标题"，下面一大块可滚动的纯文本。
//
// ── 为什么按纯文本显示，不做 Markdown 渲染 ────────────────────────────────
//
// 那几份声明是 .md，`#`、`-`、`**` 会原样露出来。这是**有意的选择**：
// 为了这一个页面引入 Markdown 渲染（外加它自己的安全面）不值得，而法律文本
// 在乎的从来是内容、不是排版。所以 textFormat 明确钉死 Text.PlainText ——
// 不这么写的话，正文里一个 `<` 会被当成标签解析，轻则排版乱、重则吞掉后面文字。
//
// 它不认识窗口，也不知道自己是被标签页装着的还是被独立窗口装着的 —— 谁装它，
// 谁把 backRequested 接上就行。
Item {
    id: root

    /** 顶部那行标题（外面已经合并好"本地化名 (English Name)"） */
    property string title: ""
    /** 正文 */
    property string body: ""
    /** 读不到文件时显示这个（非空时正文不显示） */
    property string errorText: ""

    signal backRequested()

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            TipIconButton {
                iconSource: FluentIcons.Back
                contentDescription: qsTr("ui_legal_back")
                onClicked: root.backRequested()
            }

            FluText {
                Layout.fillWidth: true
                text: root.title
                font: FluTextStyle.Body
                elide: Text.ElideRight
            }
        }

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: bodyText.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FluScrollBar {}

            FluText {
                id: bodyText
                width: scroller.width - 12      // 给滚动条让位
                visible: root.errorText === ""
                text: root.body
                font: FluTextStyle.Caption
                wrapMode: Text.Wrap
                // 正文里出现 `<` 是常事（比如 XML 片段），别让它被当成标签。
                textFormat: Text.PlainText
            }

            FluText {
                width: scroller.width - 12
                visible: root.errorText !== ""
                text: root.errorText
                font: FluTextStyle.Body
                wrapMode: Text.Wrap
            }
        }
    }
}
