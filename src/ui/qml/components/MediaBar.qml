// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
// Popup（音量那个浮出控件）在 QtQuick.Controls 里 —— 这个文件以前用不上它。
import QtQuick.Controls
import FluentUI
// Playback 是 main.cpp 注册进来的播放控制（MediaCast 模块的单例）。
// **每个 QML 文件都要自己 import 一次**，不是页面 import 了组件就跟着有。
import MediaCast 1.0

// MediaBar —— 压在画面下沿的那条播放控制栏。
//
// ── 它只认一件事：Playback ───────────────────────────────────────────────
//
// 不认识窗口、不认识 mpv、不认识 DLNA。谁把它放进哪个容器，它就在那儿画一条栏。
// 以后要做独立的播放页（或者设置页里的预览），整个文件原样搬过去就行。
//
// ── 排版照 Windows 11 的媒体播放器 ───────────────────────────────────────
//
//   [──────── 进度条 ────────]  0:12 / 0:27
//   [标题 / 副标题]         [⏮ ⏯ ⏭]         [字 音 效 全]
//
// ── 两套颜色，按"底下有没有画面"切 ───────────────────────────────────────
//
//   底下露着画面（在放视频 / 图片）—— 半透明黑底 + 白字。画面的内容不可控，
//     只能固定成深色，不然浅色主题下黑图标压在暗画面上就没了。
//   底下没画面（放音乐、没在放、或者盖着投屏引导）—— **不铺黑底**，颜色跟
//     主题走，看起来就像控件直接画在那上头。
//
// 判据是 `Playback.showsPicture`（在 C++ 那边算的，见下面的 overPicture）。
//
// 中间那三个键**锚在正中间**，不是"左中右三格等宽" —— 左右两边的内容宽度差得
// 多，等宽会把中组挤偏。窗口变窄时先牺牲左边：标题走省略号。
//
// ── 现在哪些是真的、哪些还是壳子（2026-09-14 更新）──────────────────────
//
//   真的：进度条（可拖）、播放/暂停、时间文字、全屏、音量
//   壳子：上一首、下一首、字幕、画面调节
//
// ── 那一排图标键的悬停提示从哪来 ─────────────────────────────────────────
//
// 用的是 components/TipIconButton.qml，不是 FluIconButton —— 提示的文字**直接
// 取按钮的 contentDescription**。所以加按钮时别忘了写 contentDescription：
// 写了，读屏和悬停提示就都有了，不用把同一句话抄两遍。
//
// 壳子分成两类原因，接的时候各自的着落点不一样：
//
//   · 上一首 / 下一首 —— PlaybackController 还没把 next()/previous() 暴露给 QML
//   · 字幕 / 画面调节 —— 功能要么没做，要么**形式还没定**
//     （画面调节要做成**独立窗口**，把旧界面那十多项搬过来）
//
// 接壳子的时候**只动这个文件**，别碰排版。
Item {
    id: bar

    /**
     * 「显示效果」那个按钮被按了。
     *
     * **这条栏自己不窗口、也不开窗口** —— 那个调节窗口是个独立窗口，归窗口
     * 外壳管（见 NewUiWindow.qml 里 openPictureWindow 那段）。这里只把"用户
     * 要调画面"这件事说出去，至于开在哪儿、怎么开，是宿主的事。
     */
    signal pictureRequested()

    /**
     * 栏有多高。压在画面上时外面不用管，将来要是改成占位排布会用得上。
     *
     * = 上边距 4 + 进度条那行 28 + 按键那行 48 + 下边距 11。
     * （原来是 2 + 26 + 40 + 6 = 74：按键那行只有 40，而按钮本身 30 高，
     *   上下各剩 5 像素，看着挤。）
     *
     * **下边距 11 是量出来的，不是凑的。** 播放键上方那段留白是从进度条轨道
     * 的下沿算的（轨道 6 高，居中在那一行里，所以它下沿在离栏顶 21 的地方），
     * 到按钮上沿（离栏顶 39）—— 18。下方那段是按钮下沿到栏底：按键那行里
     * 留 7，再加下边距。两边要看起来一样高，下边距就得 11（18 = 7 + 11）。
     * 截图量过：改之前上是 23 物理像素、下是 17，改之后两边都是 23。
     */
    implicitHeight: 91

    /**
     * 底下是不是真的露着画面（在放视频 / 图片）。
     *
     * **判据在 C++ 那边**（`PlaybackController::showsPicture()`），这里只管照着画 ——
     * "在放着、是视频或图片、而且不看谁连着"这三条合起来只有一份。
     *
     * 两套颜色，各管各的场合：
     *
     *   true  —— 压在画面上：半透明黑底 + 白字。画面的内容不可控（白的黑的都
     *            可能有），跟着主题走的话，浅色主题下黑图标压在暗画面上就没了。
     *   false —— 底下是页面底色（放音乐、没在放）或者投屏引导面板：**不铺黑底**，
     *            颜色跟主题走，看起来就像控件直接画在那上头。
     */
    readonly property bool overPicture: Playback.showsPicture

    /**
     * 这一条栏现在是不是"深色那套"。
     *
     * 压着画面的时候**永远是深色**（画面内容不可控）；不然跟着主题走。
     * 图标和文字各自用带透明度的白表达这件事，进度条按它选整组颜色 ——
     * 以前进度条漏了这条规矩，见下面 FluSlider 里那段。
     */
    readonly property bool darkStyle: overPicture || FluTheme.dark

    // ── 什么时候露面 ─────────────────────────────────────────────────────
    //
    // 规则（2026-09-13 和他当面定的，改动之前先回去问）：
    //
    //   空闲 / 放音乐        常显
    //   视频**在放**          鼠标进"下沿那条热区"就显示，挪开立刻收
    //   视频暂停 / 放图片     静止画面：鼠标在**整个画面框**里动一下就显示；
    //                         停手、或者挪到窗口外，3 秒后收
    //   刚投上来的视频         开头先露 5 秒，这 5 秒里不管鼠标在哪都不收
    //   刚投上来的图片         没有那 5 秒，上来就是干净的画面
    //
    // 两条兜底：鼠标**停在栏上**时不收（正要按它、它自己没了很别扭）；
    // 播放↔暂停切换的那一瞬规则直接跟着换，不做额外过渡。
    //
    // 为什么图片算"静止"那一档：图片也是不动的画面，鼠标在框里动一下就出来，
    // 比"只有下沿那条能唤出来"好用得多（他认了这一条）。

    /** 鼠标在下沿那条热区里。投屏页量好了告诉它（见 CastPage 里那块热区）。 */
    property bool stripHot: false

    /** 鼠标在画面框里动过 —— 静止画面那一档靠它。3 秒不动就自己落回去。 */
    property bool areaMoved: false

    /** 投屏页在"鼠标于画面框内移动"时叫它一下。 */
    function poke() {
        areaMoved = true
        idleTimer.restart()
    }

    Timer {
        id: idleTimer
        interval: 3000
        repeat: false
        onTriggered: bar.areaMoved = false
    }

    /** 新视频开头那 5 秒。 */
    Timer {
        id: holdTimer
        interval: 5000
        repeat: false
    }

    /**
     * 换了内容没有 —— 用来认出"新视频"。
     *
     * **不能直接听 nowPlayingChanged**：文件标签到了也会报一次（标题从地址推的
     * 变成文件里写的），那样 5 秒会被拉长。所以拿 mediaChanged 先记一笔
     * （它先来、带 URI），等 nowPlayingChanged 到的时候看这一笔还在不在 ——
     * 在，就说明这次是**新内容**，不是同一条的标签更新。
     */
    property bool freshMedia: false

    /**
     * 弹出层（音量那条滑块）开着的时候**不许把栏收起来**。
     *
     * 少了它会出现这种怪事：点开音量、鼠标往上挪到滑块上 —— 那一瞬间鼠标既不在
     * 栏上、也不在下沿热区里，栏按规矩淡出，滑块跟着一起没了。
     */
    property bool popupOpen: false

    /**
     * 窗口是**这一下投送才打开**的时候，上面那两个信号在控制栏建起来之前就发完了
     * —— 它收不到，"新视频先露 5 秒"这条就丢了（实测报过来的就是这个：窗口没开
     * 时投视频，唤起的窗口里控制栏干脆不出来）。
     *
     * 所以开场自己补一次。**补在"窗口真的露出来"那一下**，不是补在对象建好的
     * 那一下：这个窗口从建好到露面之间还隔着"离屏渲染一帧"（见 NewUiWindow::show），
     * 差着一秒多 —— 补早了，用户看到的就不是 5 秒。
     */
    function startHoldIfVideo() {
        if (Playback.showsPicture && Playback.mediaIsVideo)
            holdTimer.restart()
    }

    /**
     * "屏幕上放着一条视频"这个事实。
     *
     * **它由假变真的时候补一次那 5 秒** —— 这是最兜底的那条：窗口是投送之后才
     * 建起来的时候，投送的信号早就发完了、而控制栏建好、窗口露出来那两个时刻
     * 视频往往还没真正开始放（实测就是这么漏的）。不管先后怎么变，只要这条
     * 视频最终放出来了，事实一变，5 秒就到。
     *
     * 暂停/继续不会让它变（那个看的是"在放什么"，不是"放没在放"），所以不会
     * 平白无故又弹一次。
     */
    readonly property bool videoShowing: Playback.showsPicture && Playback.mediaIsVideo
    onVideoShowingChanged: {
        if (videoShowing)
            holdTimer.restart()
    }

    readonly property var win: Window.window

    Component.onCompleted: startHoldIfVideo()

    Connections {
        target: bar.win
        function onVisibleChanged() {
            if (bar.win && bar.win.visible)
                bar.startHoldIfVideo()
        }
    }

    Connections {
        target: Playback
        function onMediaChanged(uri, metadata) {
            bar.freshMedia = (uri !== "")
        }
        function onNowPlayingChanged() {
            if (bar.freshMedia && Playback.mediaIsVideo)
                holdTimer.restart()
            bar.freshMedia = false
        }
    }

    /** 鼠标在栏自己身上（兜底：停在栏上不收）。 */
    HoverHandler {
        id: barHover
    }

    /** 鼠标是不是正停在栏上 —— 静止画面那一档靠它兜底（停在栏上不收）。 */
    readonly property bool pointerOnBar: barHover.hovered

    /** 静止画面那一档：图片，或者暂停着的视频。 */
    readonly property bool stillMode: overPicture && (!Playback.mediaIsVideo || Playback.paused)

    /** 这一条栏现在该不该露着。 */
    readonly property bool wanted: {
        if (popupOpen)                        // 弹出层开着：不许收
            return true
        if (!overPicture)                     // 空闲 / 放音乐：常显
            return true
        if (holdTimer.running)                // 新视频那 5 秒
            return true
        if (stillMode)                        // 静止画面：动过鼠标，或者鼠标正停在栏上
            return areaMoved || pointerOnBar
        // 视频在放：鼠标在下沿那条里就显示。
        //
        // **`pointerOnBar` 这一半不能省。** 栏压在热区上面，鼠标移到栏上的时候
        // 那条热区的 HoverHandler 收不到（上层赢了，实测读出来 stripHot=0）——
        // 只判 stripHot 的话，用户把鼠标移到栏上、栏反而缩掉了。
        return stripHot || pointerOnBar
    }

    // 淡入淡出 167 毫秒 —— Fluent 这套库自己用的就是它（库里 39 处动画都是），
    // 这一条栏里进度旋钮的悬停动画也已经是它。
    opacity: wanted ? 1 : 0
    visible: opacity > 0
    Behavior on opacity {
        NumberAnimation {
            duration: 167
            easing.type: Easing.OutCubic
        }
    }

    /**
     * 这一条栏该用的主题色（蓝色）。
     *
     * **它跟 darkStyle 走，不跟 FluTheme.dark 走。**
     *
     * 因为 `FluTheme.primaryColor` 是按**程序主题**取值的：浅色主题给
     * `accentColor.dark`（深一点的蓝），深色主题给 `accentColor.lighter`
     * （亮一点的蓝）。而这一条栏有它自己的明暗规矩 —— 于是会出现
     * "**两条看起来一模一样的深色控制栏，蓝色却是两种**"：
     *
     *   浅色主题 + 放视频（栏走深色那套）  用的是浅色主题的蓝 ✗
     *   深色主题 + 放视频（栏也走深色那套）用的是深色主题的蓝
     *
     * 所以这里照 FluTheme.cpp 里那句自己取一档：
     *
     *     primaryColor = 深色 ? accentColor->lighter() : accentColor->dark()
     *
     * （库哪天改了那个算法，这儿要跟着改。）
     */
    readonly property color accentColor: darkStyle ? FluTheme.accentColor.lighter
                                                   : FluTheme.accentColor.dark

    /**
     * 图标按钮悬停 / 按下时那层底色。
     *
     * **和 accentColor 是同一个坑。** `FluTheme.itemHoverColor` 是按**程序主题**
     * 取的：浅色主题给"压深"（黑 3%）、深色主题给"提亮"（白 6%）。这一条栏
     * 强制走深色那套的时候，鼠标一上去反而**变暗** —— 和周围的白字白图标对不上，
     * 看着像凹下去一块。
     *
     * ── 深色那一档用多少 ────────────────────────────────────────────────
     *
     * 参照物是 Windows 11 那个媒体播放器，用户在纯黑底上量出来的：
     *
     *     悬停 #222222（34）   按下 #1D1D1D（29）
     *
     * 这两个数反推出微软那套是怎么搭的：它的**栏底是 #141414（20）**，
     * 按钮叠加标准的白 6% / 4% ——
     *
     *     悬停 0.06×255 + 0.94×20 = 34.1  ✓
     *     按下 0.04×255 + 0.96×20 = 29.4  ✓
     *
     * 严丝合缝。而**我们这条栏的底是纯黑**（70% 黑叠在黑的画面上还是黑），
     * 同样 6% 只能得到 #0F0F0F —— 难怪"太不明显"。所以这里不套那两档，
     * 直接按**净值**补：白色的 34/255 和 29/255。
     *
     * 注意**按下比悬停暗**（29 < 34）—— 这不是笔误，微软那边就是这样，
     * Fluent 的"透明按钮"本来就是 pressed 比 hover 淡。别顺手改过来。
     *
     * 浅色那一档继续问 FluTheme —— 那本来就是它该给的（浅底上叠白等于没叠）。
     *
     * **底保持纯黑是有意的**（2026-09-13 和他当面确认过）：把栏底抬到 #141414
     * 能让参数回到标准的 6% / 4%（那时悬停结果仍是 #222222，手感不变），
     * 代价是整条栏亮一档 —— 他觉得没必要，就不改。
     *
     * 所以**这两档浓度和底色是一对**：哪天真把底抬亮了，这里必须同时改回
     * 0.06 / 0.04，否则等于叠两遍，按钮会比参照物亮一截。
     */
    readonly property color itemHoverColor: darkStyle ? Qt.rgba(1, 1, 1, 34 / 255)
                                                      : FluTheme.itemHoverColor
    readonly property color itemPressColor: darkStyle ? Qt.rgba(1, 1, 1, 29 / 255)
                                                      : FluTheme.itemPressColor

    /**
     * 图标键**不可用**时的图标颜色（灰掉）。
     *
     * 和上面两个是同一个理由：**必须是这条栏自己那套**。库给的那个"一眼灰"
     * （`FluTheme` 里那对 130/161）是按**程序主题**算的，而这条栏压在画面上时
     * 永远用深色那套 —— 浅色主题 + 暗画面的时候，那两个灰图标会糊在画面里看不清。
     *
     * 0.36 是 Windows 的"禁用前景色"那一档（前景色叠 36%），照它来。
     */
    readonly property color disabledIconColor: darkStyle ? Qt.rgba(1, 1, 1, 0.36)
                                                         : Qt.rgba(0, 0, 0, 0.36)

    // 压在画面上时铺一层半透明黑；不然整条栏是透的，底下的东西直接露出来。
    Rectangle {
        anchors.fill: parent
        color: bar.overPicture ? "#B3000000" : "transparent"
    }

    // **把这一条栏上的点击吃掉。**
    //
    // 栏下面就是投屏页那个"点画面 = 播放/暂停"的 MouseArea。而 Rectangle 本身
    // **不吃鼠标事件**，于是点在按钮之间的空隙、栏两端的留白上，事件会穿过去 ——
    // 用户点的是控制栏，片子却被暂停了。
    //
    // 空实现就够，这里要的只是"事件到此为止"。声明在所有按钮**之前**：
    // 后声明的在上面，所以按钮、进度条照样点得到。
    MouseArea {
        anchors.fill: parent
    }

    /** 秒 -> "0:12"；超过一小时才带小时位。 */
    function timeText(seconds) {
        if (!isFinite(seconds) || seconds < 0)
            seconds = 0
        var total = Math.floor(seconds)
        var h = Math.floor(total / 3600)
        var m = Math.floor((total % 3600) / 60)
        var s = total % 60
        var pad = function (n) { return (n < 10 ? "0" : "") + n }
        return h > 0 ? (h + ":" + pad(m) + ":" + pad(s)) : (m + ":" + pad(s))
    }

    /**
     * 时间那一格要留多宽 —— **量出来的，不是写死的数字**。
     *
     * 长度是会变的：`0:03 / 0:27` 和 `2:59:59 / 3:00:00` 差着好几十像素。
     * 右边那一格如果跟着变宽变窄，进度条就会跟着一伸一缩（秒数每进一位抖一下），
     * 右边线也跟着跳。所以**固定住**，让进度条去吃掉这点变化。
     *
     * 取样的那串是**最长的那种排法**：两边都带上"时:分:秒"，
     * 也就是能装下 99 小时的片子 —— 再长就不必迁就了。
     *
     * 为什么用 TextMetrics 而不是写个数字：字号、字体（以后换语言换字体）、
     * 屏幕缩放进一位，写死的数字就不准了 —— 我这轮先写了个 96，结果按
     * 12 号字量下来，超过一小时的串要 111，早就顶出去了。
     */
    TextMetrics {
        id: timeMetrics
        font: FluTextStyle.Caption
        text: "00:00 / 00:00:00"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 4
        anchors.bottomMargin: 11
        spacing: 0

        // ── 上：进度条 + 时间 ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 28
            spacing: 10

            FluSlider {
                id: posSlider
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                from: 0
                to: Math.max(1, Playback.duration)
                tooltipEnabled: false

                // **把内部那 6 像素内边距去掉。**
                //
                // FluSlider 自己带 `padding: 6`，于是"轨道的可见起点"比这一条栏
                // 的内容左边线往里缩了 6 —— 上面对齐的是时间文字的右端（贴着内容
                // 右边线），下面一行标题、按钮也都在内容边线上，只有进度条左端缩
                // 进去一截，看着就是左右不对称。
                //
                // 归零之后：position=0 时手柄的左边缘正好落在内容左边线上，
                // position=max 时右边缘落在右边线上，两端都和上下两行对齐。
                padding: 0

                // ── 这条进度条的颜色得自己来 ────────────────────────────────
                //
                // FluSlider 把轨道和手柄的颜色**写死在组件内部**（`FluTheme.dark`
                // 两档），而且一个属性都不暴露 —— 拿它没办法，只能把 handle 和
                // background 整个换掉。
                //
                // 为什么非改不可：这一条栏有它**自己的**明暗规矩（压着画面时永远
                // 是深色那套，见上面 darkStyle），而组件内部只认 FluTheme。两者会
                // 对不上：浅色主题下压在黑底上，轨道是浅灰、手柄是白的，看着像贴
                // 上去的。
                //
                // 这一段的形状、尺寸、悬停放大都照抄库里那份，只把 `FluTheme.dark`
                // 换成 `posSlider.darkStyle`。
                readonly property bool darkStyle: bar.darkStyle

                // **不要写成 value: Playback.position 这种绑定。**
                // 用户一拖，控件自己会写 value，绑定当场被打断 —— 之后它再也
                // 跟不回播放器的位置（表现是"拖一次之后进度条就瞎了"）。
                //
                // 所以：平时（没按住）跟着 C++ 走，按住的时候 UI 说了算，松手写回去。
                Connections {
                    target: Playback
                    function onPositionChanged() {
                        if (!posSlider.pressed)
                            posSlider.value = Playback.position
                    }
                }
                Component.onCompleted: value = Playback.position
                onPressedChanged: {
                    if (!pressed)
                        Playback.seekTo(value)
                }

                handle: Rectangle {
                    x: posSlider.leftPadding
                       + posSlider.visualPosition * (posSlider.availableWidth - width)
                    y: posSlider.topPadding + (posSlider.availableHeight - height) / 2
                    implicitWidth: 20
                    implicitHeight: 20
                    radius: 10
                    color: posSlider.darkStyle ? Qt.rgba(69 / 255, 69 / 255, 69 / 255, 1)
                                               : Qt.rgba(1, 1, 1, 1)
                    FluShadow {
                        radius: 10
                    }
                    FluIcon {
                        width: 10
                        height: 10
                        anchors.centerIn: parent
                        iconSource: FluentIcons.FullCircleMask
                        iconSize: 10
                        iconColor: bar.accentColor
                        scale: posSlider.pressed ? 0.9 : (posSlider.hovered ? 1.2 : 1)
                        Behavior on scale {
                            NumberAnimation {
                                duration: 167
                                easing.type: Easing.OutCubic
                            }
                        }
                    }
                }

                background: Item {
                    x: posSlider.leftPadding
                    y: posSlider.topPadding + (posSlider.availableHeight - height) / 2
                    implicitWidth: 180
                    implicitHeight: 6
                    width: posSlider.availableWidth
                    height: implicitHeight
                    scale: posSlider.mirrored ? -1 : 1

                    // 没走过的那一段
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        radius: 2
                        color: posSlider.darkStyle ? Qt.rgba(162 / 255, 162 / 255, 162 / 255, 1)
                                                   : Qt.rgba(138 / 255, 138 / 255, 138 / 255, 1)
                    }

                    // 走过的那一段
                    Rectangle {
                        width: posSlider.position * parent.width
                        height: 6
                        radius: 3
                        color: bar.accentColor
                    }
                }
            }

            FluText {
                Layout.alignment: Qt.AlignVCenter
                // **固定宽度 + 右对齐。** 不固定的话，"0:03 / 0:27" 和
                // "10:03 / 1:27:00" 宽度不一样，进度条会跟着一伸一缩 ——
                // 时间每进一位，整条进度条就抖一下。右边线也不会跟着跳。
                //
                // 宽度按**最长的排法**量出来（最多两小时位 + 两分钟位 + 两秒位），
                // 见上面 timeMetrics；多给 2 像素防四舍五入。
                Layout.preferredWidth: timeMetrics.advanceWidth + 2
                horizontalAlignment: Text.AlignRight
                text: bar.timeText(Playback.position) + " / " + bar.timeText(Playback.duration)
                font: FluTextStyle.Caption
                textColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontSecondaryColor
            }
        }

        // ── 下：左信息 / 中三键 / 右四键 ─────────────────────────────────
        Item {
            Layout.fillWidth: true
            // 按键那行加高。按钮也一起从 30 长到 34 —— 整行留出上下各 7 像素的
            // 呼吸空间（原来是各 5），不再显得贴边。
            Layout.preferredHeight: 48

            // 左：投屏信息。
            //
            // 标题和副标题都读 Playback.nowPlaying —— **回退链在 C++ 那边**
            // （文件名推标题、占位符跳过、类型名顶上），这里不做第二套判断。
            Column {
                anchors.left: parent.left
                anchors.right: centerGroup.left
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                FluText {
                    width: parent.width
                    // 标题：真标题 → 从文件名推 → 类型名 →「未知」。
                    //
                    // **前三层在 C++ 那边已经拼好了**（回退链只此一份，见
                    // PlaybackController::buildNowPlaying）—— 这里拿到什么显示
                    // 什么，空着只可能是连类型都认不出来，那就写「未知」。
                    text: Playback.nowPlaying.title !== ""
                          ? Playback.nowPlaying.title
                          : qsTr("media_unknown")
                    font: FluTextStyle.BodyStrong
                    textColor: bar.overPicture ? "#FFFFFFFF" : FluTheme.fontPrimaryColor
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }

                FluText {
                    width: parent.width
                    // 副标题：**也是 C++ 那边算好的**（按类型分工：音频看歌手、
                    // 视频图片看副标题，拿不到就是「未知」）。这儿只管显示。
                    //
                    // 主界面上**不显示"投送"/"本地播放"** —— 那是"打哪儿来的"，
                    // 不是"这是什么"，在这儿写等于废话。Windows 媒体面板那边会
                    // 带上来源（那块面板是全局的，得分得清是谁在放）。
                    text: Playback.nowPlaying.subtitle
                    font: FluTextStyle.Caption
                    textColor: bar.overPicture ? "#B3FFFFFF" : FluTheme.fontSecondaryColor
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }
            }

            // 中：三个键，正中间。
            Row {
                id: centerGroup
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6

                // 上一首。**队列里没有上一条就灰着** —— 灰显和"按了有没有用"用的是
                // 同一份判据（Playback.hasPrevious），不会出现"亮着却点不动"。
                // 没有上一条是常态：控制点大多不排下一条、也不按上一首。
                TipIconButton {
                    readonly property bool canGo: Playback.hasPrevious
                    iconSource: FluentIcons.BackSolidBold
                    iconSize: 18
                    iconColor: canGo ? (bar.overPicture ? "#FFFFFFFF"
                                                        : FluTheme.fontPrimaryColor)
                                     : bar.disabledIconColor
                    contentDescription: qsTr("ui_mediabar_previous")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                    disabled: !canGo
                    onClicked: Playback.previous()
                }

                // 真的：播放 / 暂停
                //
                // 图标的两态看的是"现在按下去会发生什么"，**不是 paused 一个标志**：
                // 停着（Stopped）的时候 paused 也是 false，但那时该显示"播放"，
                // 显示"暂停"是骗人的 —— 点下去什么都不会发生。
                //
                // 三个键里只有它用**实心图标 + 主题色圆底**：那是这一条栏的主操作，
                // Windows 11 的媒体播放器也是这么把它突出出来的。
                //
                // （这里原来写着"上一首 / 下一首没有实心版，所以那两格保持空心" ——
                // 那句话是错的，只是名字不叫 `PreviousSolid` / `NextSolid`：
                // 实心那对叫 `BackSolidBold` / `ForwardSolidBold`。查证的办法在
                // `work\glyph-sheet.ps1` 里 —— 把码位渲染成一张对照表看形状，
                // 比对着名字猜靠谱。现在三个键都是实心的，和系统播放器一致。）
                TipIconButton {
                    readonly property bool showPlay: Playback.paused || Playback.idle
                    iconSource: showPlay ? FluentIcons.PlaySolid : FluentIcons.PauseBold
                    // **和其他键一样大。** 以前这里是 24（别的都是 18），一个键
                    // 显大一号，看着像忘了配。
                    iconSize: 18
                    // ── 圆底上的字形：**深色那套是黑的，浅色那套才是白的** ────
                    //
                    // 这不是我们定的，是这套库自己的规范 —— 见库里
                    // `FluFilledButton.qml`（主题色实心按钮）：
                    //
                    //     textColor: FluTheme.dark ? 黑 : 白
                    //
                    // 道理在配色本身：**深色那套的蓝是提亮过的**（`accentColor`
                    // 那个属性就是照库里 `FluTheme.primaryColor` 取的 —— 深色给
                    // `accentColor.lighter`，浅色给 `.dark`）。提亮过的蓝底上再压
                    // 白字，对比度只有 3.2:1，糊成一片；压黑字是 6.6:1。Windows
                    // 那套也是这么配的（WinUI 的 `TextOnAccentFillColorPrimary`：
                    // 浅色主题白、深色主题黑）。
                    //
                    // 原来是写死白的 —— 2026-09-14 他看出来了："深色样式下播放/
                    // 暂停键还是白的"。
                    //
                    // 判据必须和 **accentColor 用同一个** `darkStyle`，不能看
                    // `FluTheme.dark`：这条栏压着画面时永远是深色那套，而程序主题
                    // 可能还是浅的 —— 那就是两个字色对不上圆底的场合。
                    iconColor: bar.darkStyle ? Qt.rgba(0, 0, 0, 1)
                                             : Qt.rgba(1, 1, 1, 1)
                    contentDescription: showPlay ? qsTr("ui_mediabar_play")
                                                 : qsTr("ui_mediabar_pause")
                    // 圆底：常态就是主题色，悬停 / 按下各亮暗一档 —— 不这么做的话，
                    // 鼠标移上去只有一圈几乎看不见的底色，不像个"实心按钮"。
                    width: 34
                    height: 34
                    radius: width / 2
                    normalColor: bar.accentColor
                    hoverColor: Qt.lighter(bar.accentColor, 1.15)
                    pressedColor: Qt.darker(bar.accentColor, 1.15)
                    // 按下去该干什么由状态机说了算（规则只有一份，在
                    // PlaybackController::togglePlayPause）。上面那个 showPlay
                    // 只管画哪个图标。
                    onClicked: Playback.togglePlayPause()
                }

                // 下一个。同上 —— 判据是 Playback.hasNext。
                TipIconButton {
                    readonly property bool canGo: Playback.hasNext
                    iconSource: FluentIcons.ForwardSolidBold
                    iconSize: 18
                    iconColor: canGo ? (bar.overPicture ? "#FFFFFFFF"
                                                        : FluTheme.fontPrimaryColor)
                                     : bar.disabledIconColor
                    contentDescription: qsTr("ui_mediabar_next")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                    disabled: !canGo
                    onClicked: Playback.next()
                }
            }

            // 右：四个键，最右对齐。
            //
            // **往右多探出 8 像素**（`rightMargin: -8`）—— 这是"看起来对齐"需要的，
            // 不是手滑：
            //
            // 按钮是 34 宽的方格，里面的图标只有 18，所以**图标的右边缘比按钮的
            // 右边缘缩进 8**。按钮的右边缘对着内容右边线时（也就是 0 的样子），
            // 那排图标看上去就比上面那行时间文字往里缩了一截 —— 他看出来了：
            // "全屏按钮观感上有些偏左"。
            //
            // 量过：时间文字的右边缘在内容线内 2 物理像素，全屏图标的右边缘在
            // 里面 12 —— 差的正是那 8 逻辑像素。把整组往右挪这么一点，图标右边缘
            // 就落在 1228（和时间文字同一条竖线）。代价是这四个按钮的悬停圆圈
            // 会伸进右边距里 8 像素 —— 那正是图标按钮本来就该有的样子，
            // 点击区域本来就该比看得见的图标大一圈。
            Row {
                id: rightGroup
                anchors.right: parent.right
                anchors.rightMargin: -8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // 壳子：字幕开关。功能还没做，先占个位置。
                TipIconButton {
                    iconSource: FluentIcons.Subtitles
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_subtitles")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                }

                // 音量。点它在按钮**正上方**弹一条纵向滑块 —— 照 Windows 11 那个
                // 音量浮出控件的样子：上面一个数字，下面一条滑块。
                //
                // 数字放上面不是装饰：滑块本身看不出"现在是几"，调的时候得有个数可看。
                TipIconButton {
                    id: btn_volume

                    // ── 图标按**现在的状态**换 ───────────────────────────────────
                    //
                    // 五个状态，全是库里现成的字形（不是我们画的）：静音一个、
                    // 0 一个、然后按响度分三档。
                    //
                    //   `.muted`      带叉的喇叭（`Mute`）—— 和系统一样，静音是
                    //                 单独一个状态：音量为 0 和"被静音"不是一回事
                    //   `Volume0`     一个波都没有（音量 0，但没静音）
                    //   `Volume1..3`  一道 / 两道 / 三道波
                    //
                    // 分档照三分之一切（1-33 一道、34-66 两道、67-100 三道）。
                    // 名字怎么来的、长什么样，见 `work\glyph-sheet.ps1` 那张对照表。
                    readonly property int level: Playback.volumePercent
                    iconSource: Playback.muted ? FluentIcons.Mute
                              : level <= 0     ? FluentIcons.Volume0
                              : level <= 33    ? FluentIcons.Volume1
                              : level <= 66    ? FluentIcons.Volume2
                                               : FluentIcons.Volume3
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_volume")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                    // 面板开着的时候，这一下**到不了这儿** —— 面板自己摆了一块
                    // 透明的"静音键"盖在它上面（见 Popup 里那段）。所以这里只管开。
                    onClicked: volumePopup.open()

                    Popup {
                        id: volumePopup

                        // **挂在按钮自己身上**：这样 x/y 就是"相对按钮"算的，不用
                        // 去 mapToItem 换算 —— 那个在本工程里踩过坑（算出来恒为 0）。
                        parent: btn_volume
                        x: Math.round((parent.width - width) / 2)
                        y: -height - 8
                        padding: 10
                        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

                        // **必须模态**，虽然它看着不像个"对话框"。
                        //
                        // Qt 的 Popup 默认 `modal: false`：点外面会把它关掉，但**不
                        // 吃掉那一下点击** —— 于是那一下会漏到底下的画面上去，触发
                        // "点画面 = 播放/暂停"。实测过一次：弹出层开着时点画面空白处，
                        // 传输状态从 STOPPED 直接变成 PLAYING。
                        //
                        // 模态之后那一下被挡住，只起"关掉弹出层"的作用 —— 和 Windows
                        // 自己那个音量浮出控件的行为一致。
                        //
                        // **但默认那层遮罩会变暗**（实测：整窗明显暗下去一块），那是给
                        // 对话框用的。这里换成一个铺满窗口的 MouseArea —— 它不画任何
                        // 东西，但**照样吃点击**，于是"挡住了但看不见"。
                        //
                        // 于是"面板开着的时候，除了那个音量键，哪儿都点不动"这条就有了
                        // —— 点别处的效果是关面板（关自己由 closePolicy 负责，这层只
                        // 负责把点击吃掉、不放它穿到画面上去）。
                        modal: true
                        Overlay.modal: MouseArea { }

                        // 开着的时候告诉栏一声"别收"：鼠标从按钮挪到滑块上要往上走，
                        // 那一瞬间既不在栏上也不在下沿热区里，栏会直接淡出。
                        onOpened: bar.popupOpen = true
                        onClosed: bar.popupOpen = false

                        background: FluRectangle {
                            // **`radius` 要给四个角** —— 这个属性是 `QList<int>`
                            // （左上 / 右上 / 右下 / 左下，画的时候不够的补 0），
                            // 写成 `radius: 6` 只会圆左上角，另外三个角是直角。
                            // 库自己那几处也是这么写的：`[5,5,5,5]`、`[6,6,0,0]`。
                            radius: [6, 6, 6, 6]
                            color: bar.darkStyle ? Qt.rgba(43 / 255, 43 / 255, 43 / 255, 1)
                                                 : Qt.rgba(1, 1, 1, 1)
                            FluShadow {
                                radius: 6
                            }

                            // ── 盖在控制栏那个音量键上的"静音键" ────────────────────
                            //
                            // 面板开着的时候要能切静音，可遮罩（和面板本身）把下面那个
                            // 按钮挡住了 —— 所以在这一层摆一个**和它一模一样的按钮**，
                            // 正好盖在它上面：点它就是切静音，点面板以外别的地方才是关面板。
                            //
                            // 用**控制栏自己那个按钮组件**（TipIconButton），不是光秃秃的
                            // MouseArea：它悬停有底色、按下有动画，和栏上其它键一模一样 ——
                            // 底下的按钮被它盖住了，要是没有反馈，按下去就像"没反应"。
                            //
                            // **它自己不画图标**（`iconSource: 0` 就是"不画"，FluIcon 里
                            // 认这个值）：图标由下面那个真按钮画，它一直画着、看得见。
                            // 两块都画就是同一个位置、同一个字形叠两遍 —— 抗锯齿的边缘
                            // 会被叠一次，那个键看着比旁边几个粗一圈（他定的：上面这块不画）。
                            //
                            // 颜色**必须引用这条栏自己那套**（`bar.itemHoverColor` /
                            // `itemPressColor`）：栏压在画面上时永远是深色的，而
                            // `FluTheme` 那套是按程序主题算的，两边会对不上。
                            //
                            // 提示（tooltip）故意关掉：面板就开在旁边，再弹一句话会压在
                            // 滑块上。`tip: ""` 就是"不显示"。
                            //
                            // **位置反而不用算**：`volumePopup.x / .y` 本来就是"相对那个
                            // 按钮"的坐标（见上面 `parent: btn_volume` 那段），所以按钮在
                            // 这一层里的位置就是它的相反数。这比"给整窗遮罩挖个洞"省事
                            // 得多（那是他提的），而且**从根上绕开了那个坑**：挖洞得算出
                            // 按钮在窗口里的绝对坐标，这里根本不需要窗口坐标 —— 面板跟着
                            // 按钮走，它也就跟着走。
                            //
                            // 它会伸到面板外面去（按钮在面板下方 8 像素处）：这是有意的。
                            // Popup 不裁自己的子项（面板四周那圈阴影就是这么画出来的），
                            // 所以伸出去也点得到。
                            TipIconButton {
                                x: -volumePopup.x
                                y: -volumePopup.y
                                width: btn_volume.width
                                height: btn_volume.height
                                iconSource: 0
                                hoverColor: bar.itemHoverColor
                                pressedColor: bar.itemPressColor
                                tip: ""
                                // 静音 / 取消静音。图标会跟着变（见音量那个按钮里那段）。
                                onClicked: Playback.setMuted(!Playback.muted)
                            }
                        }

                        contentItem: ColumnLayout {
                            spacing: 8

                            FluText {
                                Layout.alignment: Qt.AlignHCenter
                                text: volumeSlider.volume
                                font: FluTextStyle.Caption
                                textColor: bar.overPicture ? "#E6FFFFFF"
                                                           : FluTheme.fontSecondaryColor
                            }

                            // ── 音量滑块：自己画 ──────────────────────────────────
                            //
                            // **为什么不用 FluSlider（或者任何现成的 Slider）**：
                            // Qt 的纵向 Slider 有两套互相矛盾的规则，实测了三轮：
                            //
                            //   · 显示侧认 from/to（`position = (value-from)/(to-from)`），
                            //     而且 `visualPosition = 1 - position`，本身又是倒的；
                            //   · 但**点击侧是归一化的** —— 点轨道顶部永远得到"最小值"，
                            //     把 from/to 倒过来也不管用（点顶部照样给 0）。
                            //
                            // 结果就是"点上面、手柄跑到下面"，怎么配都对不齐。
                            // 自己画之后，显示和输入都由这里算，方向不可能再反。
                            //
                            // 规矩只有一条：**volume 0..100，顶 = 100**（和 Windows 那根一致）。
                            Item {
                                id: volumeSlider
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 32
                                Layout.preferredHeight: 150

                                /** 音量 0..100。**顶 = 100**。 */
                                property int volume: 0

                                readonly property bool pressed: volumeMouse.pressed
                                readonly property bool hovered: volumeMouse.containsMouse

                                // 两条轨的粗细**不一样**：已经到的那一段（蓝）6、还
                                // 没到的（灰）4。这是这套界面的规矩，不是随手定的 ——
                                // 库自己那个 FluSlider 就这么画（灰的那条 `margins: 1`
                                // 缩掉一圈，蓝的占满 6），上面那条进度条也照抄了它。
                                readonly property int trackWidth: 6   // 已到的那一段（蓝）
                                readonly property int emptyWidth: 4   // 还没到的那一段（灰）
                                readonly property int handleSize: 20
                                // 手柄圆心能走的上下两端（各留半个手柄，手柄才不会越出控件）
                                //
                                // **别叫 top / bottom** —— 那是 Item 的 FINAL 属性（锚点用），
                                // 覆盖它会报 "Cannot override FINAL property"，而且整个
                                // 界面都加载不出来（实测踩过）。
                                readonly property real travelTop: handleSize / 2
                                readonly property real travelBottom: height - handleSize / 2
                                readonly property real handleCenterY:
                                    travelBottom - (travelBottom - travelTop) * (volume / 100)

                                /** 把控件内的 y 换算成音量（顶 = 100）。压到范围外就夹住。 */
                                function volumeAt(y) {
                                    const span = travelBottom - travelTop
                                    const t = Math.max(0, Math.min(1, (y - travelTop) / span))
                                    return Math.round(100 * (1 - t))
                                }

                                // 还没到的那一段（灰，细一圈）
                                Rectangle {
                                    width: volumeSlider.emptyWidth
                                    x: (parent.width - width) / 2
                                    y: volumeSlider.travelTop
                                    height: volumeSlider.travelBottom - volumeSlider.travelTop
                                    radius: width / 2
                                    color: bar.darkStyle ? Qt.rgba(162 / 255, 162 / 255, 162 / 255, 1)
                                                         : Qt.rgba(138 / 255, 138 / 255, 138 / 255, 1)
                                }

                                // 已经到的那一段：从底部往上长到手柄中心（蓝，粗的那条）
                                Rectangle {
                                    width: volumeSlider.trackWidth
                                    x: (parent.width - width) / 2
                                    y: volumeSlider.handleCenterY
                                    height: volumeSlider.travelBottom - volumeSlider.handleCenterY
                                    radius: width / 2
                                    color: bar.accentColor
                                }

                                // 手柄
                                Rectangle {
                                    width: volumeSlider.handleSize
                                    height: volumeSlider.handleSize
                                    radius: width / 2
                                    x: (parent.width - width) / 2
                                    y: volumeSlider.handleCenterY - height / 2
                                    color: bar.darkStyle ? Qt.rgba(69 / 255, 69 / 255, 69 / 255, 1)
                                                         : Qt.rgba(1, 1, 1, 1)
                                    FluShadow { radius: 10 }
                                    FluIcon {
                                        width: 10
                                        height: 10
                                        anchors.centerIn: parent
                                        iconSource: FluentIcons.FullCircleMask
                                        iconSize: 10
                                        iconColor: bar.accentColor
                                        scale: volumeSlider.pressed ? 0.9
                                               : (volumeSlider.hovered ? 1.2 : 1)
                                        Behavior on scale {
                                            NumberAnimation {
                                                duration: 167
                                                easing.type: Easing.OutCubic
                                            }
                                        }
                                    }
                                }

                                /**
                                 * 把当前值发给播放器。
                                 *
                                 * **拖的过程中就发**（不是等松手）—— 音量这一类调节，
                                 * 用户要的就是"一边拖一边听见响"。设一个属性很便宜。
                                 *
                                 * 进度条那边正相反（松手才 seek）：那里每发一次都是一次
                                 * 跳转，拖的时候一帧一条会把播放器淹掉。两条规矩不一样，
                                 * 因为底下干的事不一样 —— 别看着像就抄过去。
                                 */
                                function push() {
                                    Playback.setVolumePercent(volumeSlider.volume)
                                }

                                // 输入：点哪儿、拖哪儿，手柄就跟着去哪儿
                                MouseArea {
                                    id: volumeMouse
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor

                                    onPressed: {
                                        volumeSlider.volume = volumeSlider.volumeAt(mouse.y)
                                    }
                                    onPositionChanged: {
                                        if (pressed) {
                                            volumeSlider.volume = volumeSlider.volumeAt(mouse.y)
                                            volumeSlider.push()
                                        }
                                    }
                                    // 松手再补一次：拖的过程里要是漏了最后那一格（鼠标没动
                                    // 就抬手），这一步保证落点准。同一个值写两遍无害。
                                    onReleased: volumeSlider.push()
                                }

                                // 控制点（手机）改音量时跟着走。**拖着的时候别抢** —— 这是
                                // 双向同步里最容易打架的一处。
                                Connections {
                                    target: Playback
                                    function onVolumeChanged() {
                                        if (!volumeSlider.pressed)
                                            volumeSlider.volume = Playback.volumePercent
                                    }
                                }
                                Component.onCompleted: volume = Playback.volumePercent
                            }
                        }
                    }
                }

                // 显示效果调节。开的是**独立窗口**（不是这里的弹层）——
                // 十来条滑块塞不进一个浮出控件，而且调的时候得能一边看画面一边拖。
                // 窗口归窗口外壳管，这条栏只把请求发出去（见上面 pictureRequested）。
                TipIconButton {
                    iconSource: FluentIcons.Brightness
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_picture")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                    onClicked: bar.pictureRequested()
                }

                // 全屏 / 退出全屏。**两态图标按窗口自己的可见状态换**（见 NewUiWindow
                // 里那段"一个事实 + 一张表 + 一处执行"），这里不另记一个开关。
                TipIconButton {
                    readonly property bool isFull: bar.win ? bar.win.fullscreen : false
                    iconSource: isFull ? FluentIcons.BackToWindow : FluentIcons.FullScreen
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: isFull ? qsTr("ui_mediabar_fullscreen_exit")
                                               : qsTr("ui_mediabar_fullscreen")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                    onClicked: {
                        if (bar.win)
                            bar.win.toggleFullscreen()
                    }
                }
            }
        }
    }

}
