// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import MediaCast 1.0
import "../components"

// 设置 → 关于 → 第二页「法律」。
//
// 上面一段**通俗说明**（跟随语言），下面一行一份文件、点开看全文。
//
// 「通俗说明」不是装饰：法律文本没人会读，用户真正想知道的只有几件事 ——
// 这程序联不联网、在他电脑上留下什么、跟那些第三方协议是什么关系。那几句话
// 比下面任何一份原文都更有用，所以放在最上面、而且写得像人话。
//
// 行标题的规则见下面的 docTitle()。
Item {
    id: root

    /** 正在看全文的那一份（空 = 还在看列表） */
    property string viewingFile: ""
    property string viewingTitle: ""
    property string viewingBody: ""
    property string viewingError: ""

    // 离开这一页（切到别的标签）就把全文收起来，下次进来还是列表 ——
    // 不然"上次点开的那一份"会一直占着屏幕，得先按返回才看得见列表。
    onVisibleChanged: if (!visible) viewingFile = ""

    /**
     * 行标题：语言文件里给了本地化名就显示 `本地化名 (English Name)`，
     * 没给（英文就是这种情况）就只显示英文名、不带括号。
     *
     * **括号是这里加的，语言文件控制不了** —— 否则每种语言都得自己把括号和
     * 英文名手写一遍，改个名就得改 N 处，迟早不一致。
     *
     * 判断"有没有本地化名"靠 qsTr 的约定：查不到时它把键名原样还回来。
     */
    function docTitle(entry) {
        const localized = qsTr(entry.key)
        if (localized === entry.key || localized === "")
            return entry.canonical
        return localized + " (" + entry.canonical + ")"
    }

    function openDoc(entry) {
        root.viewingTitle = docTitle(entry)
        root.viewingBody = Legal.readDocument(entry.file)
        root.viewingError = root.viewingBody === ""
                            ? qsTr("ui_legal_read_failed").arg(Legal.lastError()) : ""
        root.viewingFile = entry.file
    }

    // ── 列表 ──────────────────────────────────────────────────────────────
    ColumnLayout {
        anchors.fill: parent
        visible: root.viewingFile === ""
        spacing: 12

        FluText {
            Layout.fillWidth: true
            text: qsTr("ui_legal_intro")
            font: FluTextStyle.Caption
            textColor: FluTheme.fontSecondaryColor
            wrapMode: Text.Wrap
        }

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: docColumn.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FluScrollBar {}

            ColumnLayout {
                id: docColumn
                width: scroller.width - 12
                spacing: 8

                Repeater {
                    model: Legal.documents

                    delegate: SettingsCard {
                        required property var modelData

                        clickable: true
                        icon: FluentIcons.Document
                        title: root.docTitle(modelData)
                        subtitle: modelData.file
                        onClicked: root.openDoc(modelData)

                        FluIcon {
                            iconSource: FluentIcons.ChevronRight
                            iconSize: 14
                            iconColor: FluTheme.fontSecondaryColor
                        }
                    }
                }
            }
        }
    }

    // ── 全文 ──────────────────────────────────────────────────────────────
    TextViewer {
        anchors.fill: parent
        visible: root.viewingFile !== ""
        title: root.viewingTitle
        body: root.viewingBody
        errorText: root.viewingError
        onBackRequested: root.viewingFile = ""
    }
}
