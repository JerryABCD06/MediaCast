// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import FluentUI

// TipTooltip —— 跟着任意一个 Item 弹的悬停提示。
//
// ── 为什么不直接用 FluTooltip ─────────────────────────────────────────────
//
// 它的位置是写死的：`y: -implicitHeight - 3`，**永远在目标上方**。
//
// 贴着窗口上沿的那一排按钮（顶栏、窗口三键）就撞在这上面：提示弹到窗口外面去
// 了。Qt 的 Popup 默认画在窗口自己的图层里，出了窗口边界就被裁掉 —— 看不见。
// 所以这里把方向做成可选的。
//
// ── 怎么用 ────────────────────────────────────────────────────────────────
//
// 把它放在**你自己那个 Item 里面**（它的 parent 就是坐标基准），再告诉它跟谁：
//
//     TipTooltip {
//         target: btn_something
//         text: qsTr("ui_topbar_settings")   // 键名，译文在 lang/*.json
//         below: true                      // 贴着窗口上沿的才需要
//         visible: btn_something.hovered   // 什么时候弹，由调用方说了算
//     }
//
// 「什么时候弹」故意不做成自动的：目标可能是 Button（有 hovered），也可能不是，
// 与其在这里猜，不如让调用方写一行。
FluTooltip {
    id: control

    /** 跟着哪个 Item 弹。空着就什么也不显示。 */
    property Item target: null

    /** 弹在目标**下面**（默认在上面）。 */
    property bool below: false

    // 库自带的是 1000 毫秒，比 Windows 自己的悬停提示慢半拍。
    delay: 600

    // ── 位置 ────────────────────────────────────────────────────────────
    //
    // 用绑定而不是一次性算好：顶栏那一排按钮在窗口宽度变化、状态胶囊变长变短
    // 的时候都会重新排布，提示得跟着走。
    //
    // mapToItem(parent, ...) 给出的是 **parent 坐标系**里的位置，而 Popup 的
    // x/y 正好就是相对 parent 的 —— 两边对上，中间隔了几层都无所谓。
    x: control.target
       ? (control.target.mapToItem(control.parent, 0, 0).x
          + (control.target.width - implicitWidth) / 2)
       : 0

    y: {
        if (!control.target)
            return 0
        const p = control.target.mapToItem(control.parent, 0, 0)
        return control.below ? (p.y + control.target.height + 3)
                             : (p.y - implicitHeight - 3)
    }
}
