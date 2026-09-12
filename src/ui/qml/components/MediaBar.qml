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

    // 半透明黑。压在视频上，所以不看主题深浅 —— 底下永远是画面，
    // 白字 + 70% 黑是最稳的搭配。
    Rectangle {
        anchors.fill: parent
        color: "#B3000000"
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
            }

            FluText {
                Layout.alignment: Qt.AlignVCenter
                text: bar.timeText(Playback.position) + " / " + bar.timeText(Playback.duration)
                font: FluTextStyle.Caption
                textColor: "#E6FFFFFF"
            }
        }

        // ── 下：左信息 / 中三键 / 右四键 ─────────────────────────────────
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 40

            // 左：投屏信息。
            //
            // 标题/副标题的兜底（文件名推标题、占位符跳过、类型名顶上）在 C++ 那边
            // 就算好了，等 NowPlaying 暴露给 QML 之后，把这两行占位文字换成真的就行。
            Column {
                anchors.left: parent.left
                anchors.right: centerGroup.left
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                FluText {
                    width: parent.width
                    // 还没接上真标题。**这是兜底，不是占位符** —— 以后接上
                    // NowPlaying 之后，读不到标题时才落回这一句，别把它删了。
                    text: qsTr("ui_mediabar_title_unknown")
                    font: FluTextStyle.BodyStrong
                    textColor: "#FFFFFFFF"
                    elide: Text.ElideRight
                    maximumLineCount: 1
                }

                FluText {
                    width: parent.width
                    text: qsTr("ui_mediabar_subtitle_unknown")
                    font: FluTextStyle.Caption
                    textColor: "#B3FFFFFF"
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
                    iconColor: "#FFFFFFFF"
                    contentDescription: qsTr("ui_mediabar_previous")
                }

                // 真的：播放 / 暂停
                //
                // 图标的两态看的是"现在按下去会发生什么"，**不是 paused 一个标志**：
                // 停着（Stopped）的时候 paused 也是 false，但那时该显示"播放"，
                // 显示"暂停"是骗人的 —— 点下去什么都不会发生。
                TipIconButton {
                    readonly property bool showPlay: Playback.paused || Playback.idle
                    iconSource: showPlay ? FluentIcons.Play : FluentIcons.Pause
                    iconSize: 24
                    iconColor: "#FFFFFFFF"
                    contentDescription: showPlay ? qsTr("ui_mediabar_play")
                                                 : qsTr("ui_mediabar_pause")
                    // 按下去该干什么由状态机说了算（规则只有一份，在
                    // PlaybackController::togglePlayPause）。上面那个 showPlay
                    // 只管画哪个图标。
                    onClicked: Playback.togglePlayPause()
                }

                // 壳子：下一个
                TipIconButton {
                    iconSource: FluentIcons.Next
                    iconSize: 18
                    iconColor: "#FFFFFFFF"
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
                    iconColor: "#E6FFFFFF"
                    contentDescription: qsTr("ui_mediabar_subtitles")
                }

                // 壳子：音量。以后点它**在按钮上方**弹一条纵向的 FluSlider
                // （FluSlider 继承 T.Slider，本身支持 Qt.Vertical）。
                TipIconButton {
                    iconSource: FluentIcons.Volume
                    iconSize: 18
                    iconColor: "#E6FFFFFF"
                    contentDescription: qsTr("ui_mediabar_volume")
                }

                // 壳子：显示效果调节。以后开**独立窗口**（不是这里的弹层），
                // 把旧界面那十多项（亮度/对比度/锐度/色温/色增益/梯形校正…）搬过去。
                TipIconButton {
                    iconSource: FluentIcons.Brightness
                    iconSize: 18
                    iconColor: "#E6FFFFFF"
                    contentDescription: qsTr("ui_mediabar_picture")
                }

                // 壳子：全屏 / 退出全屏。图标是两态，接的时候按 window.visibility 换。
                TipIconButton {
                    iconSource: FluentIcons.FullScreen
                    iconSize: 18
                    iconColor: "#E6FFFFFF"
                    contentDescription: qsTr("ui_mediabar_fullscreen")
                }
            }
        }
    }

}
