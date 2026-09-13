import QtQuick
import QtQuick.Layouts
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
// ── 现在哪些是真的、哪些还是壳子（2026-09-12）────────────────────────────
//
//   真的：进度条（可拖）、播放/暂停、时间文字
//   壳子：上一首、下一首、字幕、音量、画面调节、全屏
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
//   · 字幕 / 音量 / 画面调节 / 全屏 —— 功能要么没做，要么**形式还没定**
//     （音量要在这个按钮上方弹一条纵向滑块；画面调节要做成**独立窗口**；
//       全屏要处理 FluFrameless 那套边框助手）
//
// 接壳子的时候**只动这个文件**，别碰排版。
Item {
    id: bar

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

                // 壳子：等 PlaybackController 把 previous() 暴露给 QML
                TipIconButton {
                    iconSource: FluentIcons.Previous
                    iconSize: 18
                    iconColor: bar.overPicture ? "#FFFFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_previous")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
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
                // （"上一首 / 下一首"这套图标里**没有实心版** —— 只有空心的
                // `Previous` / `Next`，所以那两格保持空心。实心播放键 + 空心换曲键，
                // 正好就是系统播放器的样子。）
                TipIconButton {
                    readonly property bool showPlay: Playback.paused || Playback.idle
                    iconSource: showPlay ? FluentIcons.PlaySolid : FluentIcons.PauseBold
                    // **和其他键一样大。** 以前这里是 24（别的都是 18），一个键
                    // 显大一号，看着像忘了配。
                    iconSize: 18
                    // 压在主题色圆底上，所以永远是白的（不再跟着主题走）。
                    iconColor: "#FFFFFFFF"
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

                // 壳子：下一个
                TipIconButton {
                    iconSource: FluentIcons.Next
                    iconSize: 18
                    iconColor: bar.overPicture ? "#FFFFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_next")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
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

                // 壳子：音量。以后点它**在按钮上方**弹一条纵向的 FluSlider
                // （FluSlider 继承 T.Slider，本身支持 Qt.Vertical）。
                TipIconButton {
                    iconSource: FluentIcons.Volume
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_volume")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                }

                // 壳子：显示效果调节。以后开**独立窗口**（不是这里的弹层），
                // 把旧界面那十多项（亮度/对比度/锐度/色温/色增益/梯形校正…）搬过去。
                TipIconButton {
                    iconSource: FluentIcons.Brightness
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_picture")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                }

                // 壳子：全屏 / 退出全屏。图标是两态，接的时候按 window.visibility 换。
                TipIconButton {
                    iconSource: FluentIcons.FullScreen
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_fullscreen")
                    width: 34
                    height: 34
                    hoverColor: bar.itemHoverColor
                    pressedColor: bar.itemPressColor
                }
            }
        }
    }

}
