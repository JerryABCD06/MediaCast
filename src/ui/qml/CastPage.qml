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
                visible: !Playback.idle
            }

            // ── 点画面 = 播放/暂停 ──────────────────────────────────────
            //
            // 位置有讲究：写在画面**之后** → 它在画面上层，点得到；
            // 写在控制栏**之前** → 控制栏在更上层，点按钮不会被它吃掉。
            MouseArea {
                anchors.fill: parent
                enabled: Playback.hasMedia
                onClicked: {
                    if (Playback.paused)
                        Playback.play()
                    else
                        Playback.pause()
                }
            }

            // ── 中间那块提示 ────────────────────────────────────────────
            //
            // 没有东西可看的时候盖在画面上。**显示哪一句由 Playback.castState
            // 决定** —— 那个值把"谁连着"和"在放什么"算好了，这里不做组合判断。
            //
            // 不透明，所以下面的黑画面看不见；用页面同色而不是纯白，是为了让它
            // 看起来是"这一块空着"，而不是贴了一张白纸上去。
            Rectangle {
                anchors.fill: parent
                color: FluTheme.backgroundColor
                visible: Playback.idle

                ColumnLayout {
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 80, 520)
                    spacing: 12

                    // 标题：连着没连着，说法不一样。
                    FluText {
                        Layout.alignment: Qt.AlignHCenter
                        text: Playback.castState === Playback.ViewerIdle
                              ? qsTr("已经连上了 —— 在手机上挑一个视频、音乐或图片")
                              : qsTr("把手机上的内容投到这里")
                        font: FluTextStyle.Title
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }

                    FluDivider { Layout.fillWidth: true }

                    FluText {
                        Layout.fillWidth: true
                        text: qsTr("① 手机连到和这台电脑同一个 Wi-Fi")
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: qsTr("② 在手机的视频或相册里点「投屏」「投射」")
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }
                    FluText {
                        Layout.fillWidth: true
                        text: qsTr("③ 在设备列表里选中这台电脑")
                        font: FluTextStyle.Body
                        textColor: FluTheme.fontSecondaryColor
                    }

                    FluText {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        text: qsTr("支持 DLNA 的应用都能用：手机自带的相册、BubbleUPnP 等")
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
