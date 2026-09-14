// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import MediaCast 1.0

// 「显示效果」那一屏的内容：一列滑块，一项一行。
//
// ── 它不认识窗口 ────────────────────────────────────────────────────────
//
// 现在装在一个独立窗口里（同目录的 PictureWindow.qml）。将来要把它塞进设置页
// 当一个类目、或者做成弹层，换个宿主就行，这个文件不用改 —— 「关于」那套标签页
// （legal/）也是这个路数。
//
// ── 界面上没有一项是写死的 ──────────────────────────────────────────────
//
// 有几项、每项什么量程、复位到哪儿，全从 `Playback.pictureControlList` 来
// （那张表的源头在 MpvCore 里，是照 mpv 的选项表抄的）。后端哪天多一项能调的，
// 这里一行都不用动 —— 界面上自己会多一行。
//
// ── 名字是查出来的，不是写死的 ──────────────────────────────────────────
//
// 后端只给中性的短名（"brightness"），译文按键名 `ui_picture_brightness` 去
// 语言文件里找。所以 C++ 那边一个中文都没有 —— 早先中文写死在 C++ 的表里
// （"亮度""对比度"……），界面切到英文，这几行照样蹦中文。
Item {
    id: pane

    /** 后端报上来的调节项。空数组 = 后端一项都不支持（那就什么都不画）。 */
    readonly property var controls: Playback.pictureControlList

    /**
     * 一项的显示名。
     *
     * 名字里的短横线要换成下划线：语言文件的键名只用小写字母和下划线
     * （规矩见 lang/README.md），而调节项的短名里带短横线（"pan-x"）。
     * 这个换算只有这一处，别在别处再写一遍。
     */
    function labelOf(name) {
        return qsTr("ui_picture_" + name.replace(/-/g, "_"))
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        // ── 顶部那条：说明一句 + 全部复位 ────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: 16

            FluText {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("ui_picture_hint")
                font: FluTextStyle.Caption
                textColor: FluTheme.fontSecondaryColor
                wrapMode: Text.Wrap
            }

            FluButton {
                Layout.alignment: Qt.AlignVCenter
                text: qsTr("ui_picture_reset")
                contentDescription: qsTr("ui_picture_reset_desc")
                onClicked: Playback.resetPictureControls()
            }
        }

        // ── 下面那一列滑块 ───────────────────────────────────────────────
        Flickable {
            id: scroller

            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: rows.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FluScrollBar { }

            ColumnLayout {
                id: rows
                // 右边留出滚动条的位置，免得滑块右端被它压住。
                width: scroller.width - 12
                spacing: 14

                Repeater {
                    model: pane.controls

                    delegate: ColumnLayout {
                        id: row

                        required property var modelData

                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 12

                            FluText {
                                Layout.fillWidth: true
                                text: pane.labelOf(modelData.name)
                                font: FluTextStyle.Body
                                elide: Text.ElideRight
                            }

                            FluText {
                                // **固定宽度 + 右对齐。** 不固定的话，"0" 和 "100"
                                // 宽度不一样，拖的时候右边这条数会左右跳。
                                // （进度条那边的时间文字是同一个道理。）
                                Layout.preferredWidth: 40
                                horizontalAlignment: Text.AlignRight
                                text: Math.round(slider.value)
                                font: FluTextStyle.Caption
                                textColor: FluTheme.fontSecondaryColor
                            }
                        }

                        FluSlider {
                            id: slider

                            Layout.fillWidth: true
                            from: modelData.min
                            to: modelData.max
                            stepSize: 1
                            // 值就在右边那条数上显示着，再挂一个跟着鼠标跑的
                            // 小气泡反而挡手。
                            tooltipEnabled: false

                            // **别写成 `value: Playback.pictureControlValue(...)`。**
                            // 用户一拖，控件自己会写 value，绑定当场就断了 ——
                            // 之后再也跟不回真实的值（这条规矩进度条那边也写着）。
                            // 平时跟着后端走，用户动的时候界面说了算。
                            Component.onCompleted: {
                                value = Playback.pictureControlValue(modelData.name)
                            }

                            /**
                             * 写回后端。**拖的过程中就写** ——
                             * 这一类调节要的就是"一边拖一边看画面变"。
                             *
                             * 进度条那边正相反（拖的时候每一帧都发会淹掉播放器，
                             * 所以那边松手才写）：这里只是设一个 mpv 属性，便宜。
                             */
                            function push() {
                                Playback.setPictureControl(modelData.name, Math.round(value))
                            }

                            onMoved: push()
                            // 在轨道上点一下也算改 —— 有的情况这一下不发 moved，
                            // 所以松手时再兜一次（同一个值写两遍无害）。
                            onPressedChanged: {
                                if (!pressed)
                                    push()
                            }

                            // 反方向：控制点（手机）改的、或者按了「全部复位」
                            // 之后，滑块要跟着回来。**拖着的时候别抢。**
                            Connections {
                                target: Playback

                                function onPictureControlChanged(name, value) {
                                    if (name === modelData.name && !slider.pressed)
                                        slider.value = value
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
