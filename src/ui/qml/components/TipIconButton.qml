import QtQuick
import FluentUI

// TipIconButton —— 只有图标、鼠标停上去给一句小提示的按钮。
//
// ── 为什么不直接用 FluIconButton ──────────────────────────────────────────
//
// 它**自带**一个 tooltip（认的是 text，只在 IconOnly 且 hovered 时显示），
// 但位置写死在按钮**上方**。顶栏那三个按钮贴着窗口上沿，提示会跑到窗口外面去
// 被裁掉，等于没有。所以这里换成 TipTooltip —— 方向可挑：贴着上沿的往下弹，
// 压在画面下沿的控制栏往上弹。
//
// ── 提示的文字从哪来 ──────────────────────────────────────────────────────
//
// 默认取 contentDescription —— 那本来就是"这个按钮是干什么的"（给读屏用的），
// 悬停提示要说的正是同一件事。**两处分开写迟早只改一处**，所以默认绑过去；
// 哪天某个按钮想让提示和读屏名不一样，再单独给 tip 赋值。
//
// ── 用法 ──────────────────────────────────────────────────────────────────
//
//     TipIconButton {
//         iconSource: FluentIcons.Settings
//         contentDescription: qsTr("设置")     // 提示的文字就是从这儿来的
//         tipBelow: true                       // 只有贴着窗口上沿的才需要
//         onClicked: ...
//     }
//
// 其余属性（iconSource / iconSize / iconColor / radius / padding…）跟
// FluIconButton 一模一样 —— 它就是这个类型。
FluIconButton {
    id: control

    /** 悬停时显示的那句话。默认跟 contentDescription 走。空字符串 = 不显示。 */
    property string tip: contentDescription

    /**
     * 提示弹在按钮**下面**（默认在上面）。
     *
     * 需要它的场合只有一种：按钮贴着窗口上沿（顶栏）。别的地方用默认值 ——
     * 压在画面下沿的控制栏往上弹才看得见。
     */
    property bool tipBelow: false

    // FluIconButton 自己那个 tooltip 认的是 text：text 是空串它就不显示。
    // 我们从来不给 text 赋值，正好把显示提示这件事完全让给下面这一个 ——
    // 不然鼠标一上去会同时冒出两个。
    TipTooltip {
        target: control
        text: control.tip
        below: control.tipBelow
        visible: control.tip !== "" && control.hovered
    }
}
