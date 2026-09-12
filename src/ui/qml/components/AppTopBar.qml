import QtQuick
import QtQuick.Layouts
import FluentUI
// 状态胶囊读 Playback.castState —— 那是"谁连着 × 在放什么"算好的结果。
import MediaCast 1.0

// AppTopBar —— 窗口顶栏。照 Windows 11「照片」那条做的。
//
// ── 为什么是 FluAppBar，而不是自己写一个 Item ─────────────────────────────
//
//   一、那三个窗口按钮归它管。FluWindow / FluFrameless 认的就是它身上那三个别名
//       （buttonMinimize / buttonMaximize / buttonClose）—— 换成普通 Item，
//       最小化/最大化/关闭就全断了。
//   二、拖动窗口、双击最大化、右键系统菜单：FluFrameless 判断"这一点算不算
//       标题栏"用的正是这个 appBar。换成普通 Item，整条栏就拖不动了。
//   三、**它自己不画背景**（颜色本来就是全透明）。窗口自己的背景、亚克力、
//       模糊都能直接透上来 —— 这正是"顶栏不要背景"想要的，不用额外做什么。
//
// ── 一条必须知道的机制：按钮不能和别的混在一个容器里 ──────────────────────
//
// 顶栏默认**整条都是标题栏**：按在上面就是拖窗口。想让某个按钮能点，必须把它
// 单独登记进"这些不算标题栏"的名单（`window.setHitTestVisible(item)`）。
//
// 所以窗口那三个按钮是 FluAppBar 自己的一组装好的（FluWindow 已经替我们登记
// 过了），我们自己的按钮是**另一组**，在 NewUiWindow 里登记。混在同一个容器里
// 是不行的：登记整组的话，那一整块就都不能拖窗口了；不登记的话，按钮点不动。
//
// ── 布局（左 → 右）──────────────────────────────────────────────────────
//
//   [Logo][Media Cast][● 正在投屏]  ……  [ⓘ][⚙] │ [—][□][✕]
//    └────── 我们的 ──────┘              └ 我们的 ┘ └ 窗口的（不归我们动）
//
// 二级页（设置）时左边换成 [←][设置]，右边我们的按钮收起来（照照片应用的做法）。
//
// ── 悬停提示 ────────────────────────────────────────────────────────────
//
// 没有文字的按钮都有提示，而且**方向统一朝下**：这一条栏贴着窗口上沿，
// 提示往上弹会被窗口边界裁掉（Qt 的 Popup 默认画在窗口自己的图层里）。
// 连窗口那三个按钮也是 —— 库自带的提示正是朝上的，所以另挂了一份。
// 两个组件的说明在 components/TipIconButton.qml 和 TipTooltip.qml。
FluAppBar {
    id: bar

    /**
     * 顶栏的两种样子：
     *   0 = 主界面：Logo + 名称 + 状态胶囊
     *   1 = 二级页：返回箭头 + 页面标题
     */
    property int mode: 0

    /** mode = 1 时显示在返回箭头右边的标题。 */
    property string pageTitle: ""

    signal backClicked()
    signal infoClicked()
    signal settingsClicked()
    /** 用户点了「断开连接」—— 具体怎么断是 main() 那边接到门面上的事。 */
    signal disconnectClicked()

    /**
     * 顶栏把自己建好了。
     *
     * 存在的唯一原因是**躲开 Component.onCompleted**：窗口那边要调
     * `window.setHitTestVisible(我们的按钮)`，而这句话不能写在窗口自己的
     * `Component.onCompleted` 里 —— FluWindow 自己已经用了那个处理函数
     * （居中、登记进 FluRouter、决定要不要显示），实例上再写一个有可能把它
     * 顶掉，那样窗口就不显示了。发个信号出来让窗口接，就没这个风险。
     */
    signal ready()

    // 把自己的按钮暴露出去 —— NewUiWindow 要拿它们去登记"这些不是标题栏"。
    readonly property alias backButton: btn_back
    readonly property alias disconnectButton: btn_disconnect
    readonly property alias infoButton: btn_info
    readonly property alias settingsButton: btn_settings

    // 48 逻辑像素（Windows 11 那条的高度）。**QML 里的长度就是逻辑像素**，
    // 显示器缩放由 Qt 自己换算：100% 时它是 48 物理像素，150% 时是 72 ——
    // 这里不需要写任何换算代码，也不该写。
    height: 48

    // 不用它自带的那套"图标 + 标题"：我们的左边要放 Logo、名称和状态胶囊。
    titleVisible: false
    icon: ""
    // 那两个额外按钮也不要（亮度、置顶）。默认的 FluWindow 会替我们关掉，
    // 但那是**它自己那份 appBar** 的绑定；我们这份得自己写。
    showDark: false
    showStayTop: false

    // 窗口那三个按钮的提示文字（FluAppBar 的公开属性，默认是英文）。
    // 下面那三份 TipTooltip 直接读它们，所以改词只改这一处。
    minimizeText: qsTr("ui_window_minimize")
    restoreText: qsTr("ui_window_restore")
    maximizeText: qsTr("ui_window_maximize")
    closeText: qsTr("ui_window_close")

    Component.onCompleted: bar.ready()

    // ── 状态胶囊那两个值：从 castState 翻出来 ────────────────────────────
    //
    // 五种局面，三种颜色：没连着是灰的，连着但没在放是绿的，在放东西是主题蓝。
    // 图标/颜色之外的字也在这儿 —— 换文案只改这一处。
    readonly property color pillColor: {
        switch (Playback.castState) {
        case Playback.NoViewer:    return FluTheme.dark ? "#9A9A9A" : "#8A8A8A"
        case Playback.ViewerIdle:  return "#107C10"
        default:                   return FluTheme.primaryColor
        }
    }

    readonly property string pillText: {
        switch (Playback.castState) {
        case Playback.NoViewer:     return qsTr("ui_topbar_state_disconnected")
        case Playback.ViewerIdle:   return qsTr("ui_topbar_state_connected")
        case Playback.ViewerVideo:  return qsTr("ui_topbar_state_casting")
        case Playback.ViewerAudio:  return qsTr("ui_topbar_state_audio")
        case Playback.ViewerImage:  return qsTr("ui_topbar_state_image")
        }
        return qsTr("ui_topbar_state_disconnected")
    }

    // ── 左：主界面模式 ──────────────────────────────────────────────────
    RowLayout {
        id: leftGroup
        visible: bar.mode === 0
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 10

        Image {
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20
            sourceSize: Qt.size(40, 40)     // 高 DPI 下取大图，别糊
            source: "qrc:/icons/mcast-32.png"
            fillMode: Image.PreserveAspectFit
        }

        // 产品名。**不翻译**：这是名字，不是描述。
        FluText {
            Layout.alignment: Qt.AlignVCenter
            text: "Media Cast"
            font: FluTextStyle.Body
        }

        // 状态胶囊
        //
        // 显示什么完全由 Playback.castState 决定 —— **那个值已经是"谁连着 ×
        // 在放什么"算好的结果**，这里不做任何组合判断（组合逻辑只有一份，
        // 在 PlaybackController::castState 里）。
        Rectangle {
            id: pill
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: pillRow.implicitWidth + 18
            implicitHeight: 22
            radius: height / 2
            color: FluTools.withOpacity(bar.pillColor, 0.16)

            Row {
                id: pillRow
                anchors.centerIn: parent
                spacing: 6

                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: bar.pillColor
                }

                FluText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: bar.pillText
                    font: FluTextStyle.Caption
                }
            }
        }
    }

    // ── 左：二级页模式（返回 + 标题）────────────────────────────────────
    RowLayout {
        id: backGroup
        visible: bar.mode === 1
        anchors.left: parent.left
        anchors.leftMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 4

        // 顶栏的图标按钮都用 TipIconButton 且 **tipBelow: true** ——
        // 它们贴着窗口上沿，提示往上弹会被窗口边界裁掉。见那个文件里那段说明。
        TipIconButton {
            id: btn_back
            Layout.preferredWidth: 36
            Layout.preferredHeight: 30
            iconSource: FluentIcons.Back
            iconSize: 16
            contentDescription: qsTr("ui_topbar_back")
            tipBelow: true
            onClicked: bar.backClicked()
        }

        FluText {
            Layout.alignment: Qt.AlignVCenter
            text: bar.pageTitle
            font: FluTextStyle.Body
        }
    }

    // ── 右：信息 + 设置 ─────────────────────────────────────────────────
    //
    // 贴在窗口那三个按钮的左边（layoutStandardbuttons 是 FluAppBar 自带的、
    // 右边对齐的那一组）。**它们是两个独立的容器** —— 见文件头那段说明。
    RowLayout {
        id: rightGroup
        visible: bar.mode === 0
        anchors.right: layoutStandardbuttons.left
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2

        // 「断开连接」：带边框的文字按钮（不是强调色那版），只在真有活儿的
        // 时候出现 —— 没连着的时候摆一个"断开连接"出来只会让人犯嘀咕。
        FluButton {
            id: btn_disconnect
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: 30
            Layout.rightMargin: 6
            visible: Playback.castState !== Playback.NoViewer
            contentDescription: qsTr("ui_topbar_disconnect")
            onClicked: bar.disconnectClicked()

            // FluButton 的内容项本来就是一个 FluText，这里换成"图标 + 文字"。
            contentItem: Row {
                spacing: 6
                FluIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    iconSource: FluentIcons.DisconnectDisplay
                    iconSize: 14
                }
                FluText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("ui_topbar_disconnect")
                }
            }
        }

        TipIconButton {
            id: btn_info
            Layout.preferredWidth: 36
            Layout.preferredHeight: 30
            iconSource: FluentIcons.Info
            iconSize: 16
            contentDescription: qsTr("ui_topbar_info")
            tipBelow: true
            onClicked: bar.infoClicked()
        }

        TipIconButton {
            id: btn_settings
            Layout.preferredWidth: 36
            Layout.preferredHeight: 30
            iconSource: FluentIcons.Settings
            iconSize: 16
            contentDescription: qsTr("ui_topbar_settings")
            tipBelow: true
            onClicked: bar.settingsClicked()
        }
    }

    // ── 窗口那三个按钮的悬停提示 ─────────────────────────────────────────
    //
    // 它们归 FluAppBar 管（见文件头那段：这三键必须留在它手里），提示它也已经
    // 各挂了一句。但 FluIconButton 里那个 tooltip **只会往按钮上方弹** ——
    // 这几个按钮就贴着窗口上沿，提示跑到窗口外面被裁掉，看不见。
    //
    // 所以这里自己挂三份往**下**弹的。文字直接读按钮自己那句（上面那四个
    // *Text 属性），"最大化/还原"来回切的事由库里管，不用在这儿再算一遍。
    //
    // 库那份提示留着不管：FluIconButton 把 Accessible.name 绑在 text 上，
    // 清掉 text 读屏就念不出名字了。它照样会弹，只是永远在窗口外面，碍不着事。
    //
    // 顺带记一笔：悬停「最大化」时，Windows 11 会在同一个位置弹出**它自己的
    // 贴靠布局面板**（一块画着几种分屏样式的浮层）。因为窗口保留着原生框架
    // 标志（见 platform/windows/WindowFrame::ensureSnapFlags），系统认这块
    // 区域就是标题栏上的最大化键。那块浮层是独立顶层窗口，永远盖在我们上面 ——
    // 所以在 Win11 上「最大化」这条提示实际看不见，Win10 上（没有那个面板）能看见。
    TipTooltip {
        target: bar.buttonMinimize
        text: bar.buttonMinimize.text
        below: true
        visible: bar.buttonMinimize.visible && bar.buttonMinimize.hovered
    }

    TipTooltip {
        target: bar.buttonMaximize
        text: bar.buttonMaximize.text
        below: true
        visible: bar.buttonMaximize.visible && bar.buttonMaximize.hovered
    }

    TipTooltip {
        target: bar.buttonClose
        text: bar.buttonClose.text
        below: true
        visible: bar.buttonClose.visible && bar.buttonClose.hovered
    }
}
