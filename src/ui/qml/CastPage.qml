import QtQuick
import QtQuick.Layouts
import FluentUI
import MediaCast 1.0

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

            // 视频，就是场景图里的一个普通图层 —— 可以被裁剪、被别的控件压住。
            // 旧的 Widgets 界面做不到这件事，那边视频是一个独立的原生子窗口。
            MpvQmlItem {
                anchors.fill: parent
                // Player 是 main.cpp 注册进来的播放器。画面往哪出由 C++ 那边的
                // 输出模式决定，这里只负责"把它画出来"。
                core: Player
            }

            FluText {
                anchors.centerIn: parent
                visible: !Player.running
                text: qsTr("播放器未就绪")
                font: FluTextStyle.Body
                textColor: FluTheme.fontTertiaryColor
            }
        }

        // ── 播放控制（还没接上）──────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            FluButton { text: qsTr("打开文件") }
            FluFilledButton { text: qsTr("播放") }
            FluButton { text: qsTr("暂停") }
            FluButton { text: qsTr("停止") }

            Item { Layout.fillWidth: true }
        }

        // ── 界面状态 ─────────────────────────────────────────────────────
        //
        // 临时脚手架 —— 以后有了用户设置文件，它们会挪进设置页，这里会删掉。
        // 现在放在这儿是为了让"语言和深浅只有一份状态"这件事看得见摸得着。
        //
        // 用 Repeater + FluRadioButton 是照着 FluentUI 自己的设置页写的：
        // checked 是一个纯查询表达式（谁也不用改它），用户点了走 clickListener
        // 去改状态。这样不会出现"控件和状态各说各话"的绑定打架。
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            FluText {
                text: qsTr("外观")
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
            }

            Row {
                spacing: 14
                Repeater {
                    model: [
                        { title: qsTr("跟随系统"), mode: UiState.System },
                        { title: qsTr("浅色"), mode: UiState.Light },
                        { title: qsTr("深色"), mode: UiState.Dark }
                    ]
                    delegate: FluRadioButton {
                        text: modelData.title
                        checked: UiState.themeMode === modelData.mode
                        clickListener: function() {
                            UiState.themeMode = modelData.mode
                        }
                    }
                }
            }

            Item { Layout.fillWidth: true }

            FluText {
                text: qsTr("语言")
                font: FluTextStyle.Body
                textColor: FluTheme.fontSecondaryColor
            }

            Row {
                spacing: 14
                Repeater {
                    // "中文"和"English"故意不套 qsTr：语言名就该写成它自己的
                    // 语言，翻了反而看不懂。要翻的只有"跟随系统"。
                    model: [
                        { title: qsTr("跟随系统"), code: "" },
                        { title: "中文", code: "zh_CN" },
                        { title: "English", code: "en_US" }
                    ]
                    delegate: FluRadioButton {
                        text: modelData.title
                        checked: UiState.language === modelData.code
                        clickListener: function() {
                            UiState.language = modelData.code
                        }
                    }
                }
            }
        }
    }
}
