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

    /** 栏有多高。压在画面上时外面不用管，将来要是改成占位排布会用得上。 */
    implicitHeight: 74

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

    // 压在画面上时铺一层半透明黑；不然整条栏是透的，底下的东西直接露出来。
    Rectangle {
        anchors.fill: parent
        color: bar.overPicture ? "#B3000000" : "transparent"
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

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 2
        anchors.bottomMargin: 6
        spacing: 0

        // ── 上：进度条 + 时间 ────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            spacing: 10

            FluSlider {
                id: posSlider
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                from: 0
                to: Math.max(1, Playback.duration)
                tooltipEnabled: false

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
                text: bar.timeText(Playback.position) + " / " + bar.timeText(Playback.duration)
                font: FluTextStyle.Caption
                textColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontSecondaryColor
            }
        }

        // ── 下：左信息 / 中三键 / 右四键 ─────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 40

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
                    width: 30
                    height: 30
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
                }
            }

            // 右：四个键，最右对齐。
            Row {
                id: rightGroup
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // 壳子：字幕开关。功能还没做，先占个位置。
                TipIconButton {
                    iconSource: FluentIcons.Subtitles
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_subtitles")
                }

                // 壳子：音量。以后点它**在按钮上方**弹一条纵向的 FluSlider
                // （FluSlider 继承 T.Slider，本身支持 Qt.Vertical）。
                TipIconButton {
                    iconSource: FluentIcons.Volume
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_volume")
                }

                // 壳子：显示效果调节。以后开**独立窗口**（不是这里的弹层），
                // 把旧界面那十多项（亮度/对比度/锐度/色温/色增益/梯形校正…）搬过去。
                TipIconButton {
                    iconSource: FluentIcons.Brightness
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_picture")
                }

                // 壳子：全屏 / 退出全屏。图标是两态，接的时候按 window.visibility 换。
                TipIconButton {
                    iconSource: FluentIcons.FullScreen
                    iconSize: 18
                    iconColor: bar.overPicture ? "#E6FFFFFF" : FluTheme.fontPrimaryColor
                    contentDescription: qsTr("ui_mediabar_fullscreen")
                }
            }
        }
    }

}
