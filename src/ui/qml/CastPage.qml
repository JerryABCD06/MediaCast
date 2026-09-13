import QtQuick
import QtQuick.Layouts
import FluentUI
import MediaCast 1.0
// 可复用的界面组件（MediaBar 等）。它们在 qrc 的 ui/components 下。
import "components"

// 投屏页。目前是骨架：视频区是空的，播放按钮还没接线。
//
// 这是个普通 Item，不认识 TopNav 也不认识窗口 —— 它只负责把自己这一屏画好。
// 以后它会长成真正的播放界面。
Item {
    id: page

    ColumnLayout {
        anchors.fill: parent
        // 页边距由外层（NewUiWindow 里的 FluPivot）统一给，这里不再加一层 ——
        // 两处各加一次的话，改一个地方另一个地方就对不上了。
        spacing: 12

        // 将来 mpv 的画面就画在这个框里。
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 8
            color: "#10000000"
            clip: true
            border.width: 1
            border.color: FluTheme.dividerColor

            // ── 画面 ────────────────────────────────────────────────────
            //
            // 视频，就是场景图里的一个普通图层 —— 可以被裁剪、被别的控件压住。
            // 旧的 Widgets 界面做不到这件事，那边视频是一个独立的原生子窗口。
            //
            // **它必须一直可见、一直在渲染**，连"空着的时候"也一样。
            //
            // 踩过的坑：一开始空着的时候把它 visible: false 隐藏掉，看着挺好，
            // 但 mpv 在 render API 模式下是靠我们每帧调一次渲染来推进画面的；
            // 一旦没有渲染调用，它的视频输出就停了 —— 再按播放也起不来。
            // 实测表现：状态显示"正在播放"，但位置一直是 0、画面全黑。
            //
            // 所以空着的时候是**盖住**它（见下面那块面板），不是藏起它。
            MpvQmlItem {
                anchors.fill: parent
                // Player 是 main.cpp 注册进来的播放器。画面往哪出由 C++ 那边的
                // 输出模式决定，这里只负责"把它画出来"。
                core: Player

                // **这里故意没有 visible 绑定。** 以前写的是 `visible: !Playback.idle`，
                // 结果片子自然播完（状态变 Stopped）时它自己藏了起来 —— 恰好踩中上面
                // 说的那个坑：一停止渲染，mpv 的视频输出就死了，再点播放只会得到一块
                // 纯黑。空着的时候靠下面那块面板**盖住**它就行，它自己一直画着。
            }

            // ── 点画面 = 播放/暂停 ──────────────────────────────────────
            //
            // 位置有讲究：写在画面**之后** → 它在画面上层，点得到；
            // 写在控制栏**之前** → 控制栏在更上层，点按钮不会被它吃掉。
            MouseArea {
                anchors.fill: parent
                enabled: Playback.hasMedia
                // 「该播还是该暂停」是个状态机问题，规则在 PlaybackController 里
                // （togglePlayPause）。界面上有两处要用它，各写一份的话迟早只有
                // 一处被改到 —— 这次就是这么来的：片子播完之后这里还在判 paused，
                // 于是点一下变成"暂停"，什么都没发生。
                onClicked: Playback.togglePlayPause()
            }

            // ── 中间那块提示 ────────────────────────────────────────────
            //
            // **没有画面可看的时候盖在画面上。** 什么时候算"没画面"由
            // `Playback.showsPicture` 说（在 C++ 那边算的），这里不做组合判断。
            //
            // 为什么连"在放音乐"也要盖：mpv 没有画面可输出的时候，渲染出来的是
            // **纯黑**（实测放音频就是这样）。那一块黑露在外面，底下那条控制栏
            // 跟主题走的深色字就没法看了。盖上页面底色，看着才是"这一块空着"。
            //
            // **不显示画面 ≠ 什么都没在放**：所以引导文字只看 idle ——
            // 放音乐的时候这块只当底色，不摆"请在手机上选择…"那几句。
            //
            // 不透明，所以下面的黑画面看不见；用页面同色而不是纯白，是为了让它
            // 看起来是"这一块空着"，而不是贴了一张白纸上去。
            Rectangle {
                anchors.fill: parent
                color: FluTheme.backgroundColor
                visible: !Playback.showsPicture

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 80, 520)
                    spacing: 12
                    // 只有真的没东西可放的时候才摆这几句；放音乐时这块只是底色。
                    visible: Playback.idle

                    // 标题：连着没连着，说法不一样。
                    FluText {
                        Layout.alignment: Qt.AlignHCenter
                        text: Playback.castState === Playback.ViewerIdle
                              ? qsTr("ui_cast_hint_connected")
                              : qsTr("ui_cast_hint_idle")
                        font: FluTextStyle.Title
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }

                    FluDivider { Layout.fillWidth: true }

                    FluText {
                        Layout.fillWidth: true
                        text: qsTr("ui_cast_hint_step1")
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: qsTr("ui_cast_hint_step2")
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: qsTr("ui_cast_hint_step3")
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }

                    FluText {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        text: qsTr("ui_cast_hint_apps")
                        font: FluTextStyle.Caption
                        textColor: FluTheme.fontTertiaryColor
                    }
                }
            }

            // ── 播放控制栏 ──────────────────────────────────────────────
            //
            // 压在画面下沿的**内侧**。它挂在容器上（不是挂在 mpv 上），只是画在
            // 画面之上 —— 能做到这件事正是因为 render API 把视频变成了场景图里的
            // 一个普通图层。旧方案（wid）那边视频是独立的原生子窗口，永远盖在
            // 所有 QML 之上，这种栏根本做不出来。
            //
            // 容器上的 clip: true 保证它不会溢出圆角。
            MediaBar {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: Playback.hasMedia
            }
        }

        // （原来这一排占位按钮没了：播放/暂停搬进画面下沿那条控制栏。
        //   「打开文件」暂时没有落脚点 —— 以后按 Windows 11 播放器的做法放进
        //   控制栏右侧的「…」菜单里。）

        // （原来这里有一排"外观/语言"单选框，是当时的临时脚手架 ——
        //   现在它们搬进设置页了：顶栏 ⚙ -> 界面。投屏页只剩投屏这件事。）
    }
}
