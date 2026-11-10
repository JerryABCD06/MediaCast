// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import FluentUI
import MediaCast 1.0
import "components"

// 独立确认窗口。它不依赖主窗口是否已经显示，也不主动拉起主窗口。
FluWindow {
    id: win

    property string requestId
    property int protocol: 0
    property string peerName: ""
    property string mediaTitle: ""
    property int timeoutMs: 10000
    property bool resolved: false
    property double deadline: Date.now() + timeoutMs
    property int remainingMs: timeoutMs

    title: qsTr("ui_cast_takeover_title")
    width: 520
    height: 390
    minimumWidth: 520
    minimumHeight: 390
    maximumWidth: 520
    maximumHeight: 390
    fixSize: true
    autoVisible: false
    autoCenter: true
    stayTop: true
    showMinimize: false
    showMaximize: false
    showClose: true
    effect: "normal"

    function protocolName() {
        switch (protocol) {
        case 1: return "DLNA"
        case 2: return "Google Cast"
        case 3: return "AirPlay"
        case 4: return "Miracast"
        case 5: return "LeLink"
        case 6: return qsTr("ui_cast_takeover_vendor")
        default: return qsTr("ui_cast_takeover_unknown")
        }
    }

    function finish(accepted) {
        if (resolved)
            return
        resolved = true
        Shell.respondCastConfirmation(requestId, accepted)
        FluRouter.removeWindow(win)
    }

    function cancelFromShell() {
        if (resolved)
            return
        resolved = true
        FluRouter.removeWindow(win)
    }

    closeListener: function(event) {
        if (resolved) {
            event.accepted = true
            return
        }
        event.accepted = false
        finish(false)
    }

    background: Component {
        MicaBackdrop {
            isSource: true
        }
    }

    Timer {
        interval: 100
        running: !win.resolved
        repeat: true
        onTriggered: {
            win.remainingMs = Math.max(
                0, Math.ceil(win.deadline - Date.now()))
            if (win.remainingMs === 0)
                win.finish(false)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 16

        FluText {
            Layout.fillWidth: true
            text: qsTr("ui_cast_takeover_heading")
            font: FluTextStyle.Title
            wrapMode: Text.Wrap
        }

        FluText {
            Layout.fillWidth: true
            text: qsTr("ui_cast_takeover_description")
            font: FluTextStyle.Body
            textColor: FluTheme.fontSecondaryColor
            wrapMode: Text.Wrap
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: details.implicitHeight + 32
            radius: 8
            color: FluTheme.dark
                   ? Qt.rgba(255, 255, 255, 0.055)
                   : Qt.rgba(0, 0, 0, 0.035)

            ColumnLayout {
                id: details
                anchors.fill: parent
                anchors.margins: 16
                spacing: 8

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 18
                    rowSpacing: 8

                    FluText {
                        text: qsTr("ui_cast_takeover_protocol")
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontSecondaryColor
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: win.protocolName()
                        font: FluTextStyle.BodyStrong
                        textFormat: Text.PlainText
                    }

                    FluText {
                        text: qsTr("ui_cast_takeover_device")
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontSecondaryColor
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: win.peerName
                              || qsTr("ui_cast_takeover_unknown")
                        font: FluTextStyle.Body
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }

                    FluText {
                        text: qsTr("ui_cast_takeover_media")
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontSecondaryColor
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: win.mediaTitle
                              || qsTr("ui_cast_takeover_no_media")
                        font: FluTextStyle.Body
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        FluText {
            Layout.fillWidth: true
            text: qsTr("ui_cast_takeover_timeout")
            font: FluTextStyle.Caption
            textColor: FluTheme.fontSecondaryColor
        }

        FluProgressBar {
            Layout.fillWidth: true
            from: 0
            to: Math.max(1, win.timeoutMs)
            value: win.remainingMs
            indeterminate: false
            strokeWidth: 4
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            Item { Layout.fillWidth: true }

            FluButton {
                text: qsTr("ui_cast_takeover_keep")
                onClicked: win.finish(false)
            }

            FluFilledButton {
                text: qsTr("ui_cast_takeover_allow")
                onClicked: win.finish(true)
            }
        }
    }
}
