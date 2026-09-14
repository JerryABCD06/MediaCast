// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import FluentUI
// 自己画的那块云母底在 components/ 里。**相对路径**是相对本文件所在目录
// （qrc 里就是 qrc:/ui/picture/ → ../components 指到 qrc:/ui/components）。
import "../components"

// 「显示效果」那个**独立窗口** —— 控制栏右边那个太阳图标开的就是它。
//
// 内容全在 PicturePane.qml 里，这里只办"当窗口"该办的三件事：标题、尺寸、
// 以及关掉之后怎么收场。
//
// ── 为什么是独立窗口，不是弹出层 ────────────────────────────────────────
//
// 这一屏幕要调的项有十来条，弹出层做不了那么高，而且调的时候用户得**看着画面**
// 一边拖一边比 —— 窗口能挪开、能摆在画面旁边，弹出层一松手就没了。
//
// ── 关掉之后不销毁 ──────────────────────────────────────────────────────
//
// `autoDestroy: false`：FluWindow 默认关一次就把自己销毁，而这个窗口是"用完
// 还留着"的东西 —— 反复开关时重建一次要一秒多（FluentUI 那一套的构造开销），
// 而且销毁之后外面那份引用就成了悬空的（主窗口那边为这个栽过一次，见
// docs/待办.md 里"关窗 = 真的销毁窗口对象"那条）。
//
// 关掉只是隐藏，下次再点那个按钮，它原样回来（位置也是上次那个位置）。
FluWindow {
    id: win

    title: qsTr("ui_picture_title")

    width: 420
    height: 560
    minimumWidth: 360
    minimumHeight: 320

    autoDestroy: false

    // 系统那个 backdrop 常关，理由和主窗口一样（见 NewUiWindow.qml 里那段）。
    effect: "normal"

    // ── 窗口的底：和主窗口一样的那块"自己画的云母" ───────────────────────
    //
    // **不能用 FluWindow 自带那份**（默认的 `com_background`）：它内部走的是
    // `window.screen.virtualX`，那是 Qt 5 的 API，Qt 6 里恒为 undefined，
    // 于是整块底变成一张空图（这条记在 docs/待办.md 的坑里）。
    //
    // 所以摆一块 MicaBackdrop。**这一层是它自己那张图的"造"的那一层**
    // （`isSource: true`）—— 换句话说这扇窗有自己一张整屏云母，不跟主窗口共用。
    //
    // 为什么不共用主窗口那张（明明那张早就算好了）：**跨窗口取纹理这条路 Qt 不给**
    // —— `ShaderEffectSource` 的 `sourceItem` 必须在同一个窗口里，指过去只会拿到
    // 一张空图，而空图的表现正好是"窗口透过去"（这个 bug 刚踩过一次）。
    // 代价是多算一遍模糊（只在打开这扇窗的期间）；换来的好处是这扇窗的底和主窗口
    // **天然对齐**：同一张壁纸、同一个模糊半径和染色、同样钉在屏幕原点上，
    // 裁的又是屏幕坐标里自己那一格，所以两扇窗重叠的地方逐点一致，没有色差。
    //
    // 关掉云母（设置里那个开关）、壁纸读不到、还没解码完 —— 一律退回那块灰
    // （#F3F3F3 / #1A1A1A），这一层永远是实心的。判断的事在 MicaBackdrop 里。
    background: Component {
        MicaBackdrop {
            id: winBackdrop

            isSource: true
        }
    }

    PicturePane {
        anchors.fill: parent
    }
}
