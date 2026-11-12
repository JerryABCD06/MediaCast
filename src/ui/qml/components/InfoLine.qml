// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import FluentUI

// One label/value line in the information flyout. Empty values are omitted
// by the caller, so the panel never turns into a wall of em dashes.
Item {
    id: line

    property string label: ""
    property string value: ""
    property bool show: true

    visible: show && label.length > 0 && value.length > 0
    Layout.fillWidth: true
    Layout.preferredHeight: Math.max(labelText.implicitHeight,
                                     valueText.implicitHeight) + 2

    RowLayout {
        anchors.fill: parent
        spacing: 12

        FluText {
            id: labelText
            Layout.preferredWidth: 86
            text: line.label
            font: FluTextStyle.Caption
            textColor: FluTheme.fontSecondaryColor
        }

        FluText {
            id: valueText
            Layout.fillWidth: true
            text: line.value
            font: FluTextStyle.Body
            textColor: FluTheme.fontPrimaryColor
            wrapMode: Text.WrapAnywhere
        }
    }
}
