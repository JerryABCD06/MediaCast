// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import FluentUI
import MediaCast 1.0

// Diagnostic flyout anchored under the information button. The upper half is
// protocol-owned connection information; the lower half is Ui-local MPV data.
Item {
    id: root

    property var hostWindow: null
    signal closeRequested()

    anchors.fill: parent
    visible: false
    z: 1000

    function close() {
        closeRequested()
    }

    function translated(key, fallback) {
        const value = qsTr(key)
        return value === key ? fallback : value
    }

    function normalizedId(value) {
        return String(value || "").toLowerCase().replace(/[^a-z0-9]+/g, "_")
    }

    function protocolName(value) {
        if (!value)
            return ""
        const revision = Tr.languageRevision
        void revision
        return Tr.resolve(
            "term_protocol_" + normalizedId(value) + "_full",
            String(value))
    }

    function codecName(value) {
        if (!value)
            return ""
        return translated("codec_" + normalizedId(value), String(value))
    }

    function containerName(value) {
        if (!value)
            return ""
        const text = String(value).toLowerCase()
        if (text.indexOf("mp4") >= 0 || text.indexOf("isom") >= 0)
            return translated("container_mp4", String(value))
        if (text.indexOf("matroska") >= 0)
            return translated("container_matroska", String(value))
        if (text.indexOf("webm") >= 0)
            return translated("container_webm", String(value))
        if (text.indexOf("mov") >= 0)
            return translated("container_mov", String(value))
        if (text.indexOf("mpegts") >= 0)
            return translated("container_mpegts", String(value))
        if (text.indexOf("flac") >= 0)
            return translated("container_flac", String(value))
        if (text.indexOf("mp3") >= 0)
            return translated("container_mp3", String(value))
        if (text.indexOf("ogg") >= 0)
            return translated("container_ogg", String(value))
        if (text.indexOf("wav") >= 0)
            return translated("container_wav", String(value))
        if (text.indexOf("hls") >= 0 || text.indexOf("mpegurl") >= 0)
            return translated("container_hls", String(value))
        return String(value)
    }

    function durationText(seconds) {
        const value = Number(seconds || 0)
        if (!(value > 0))
            return ""
        const total = Math.round(value)
        const hours = Math.floor(total / 3600)
        const minutes = Math.floor((total % 3600) / 60)
        const secs = total % 60
        const mm = String(minutes).padStart(2, "0")
        const ss = String(secs).padStart(2, "0")
        return hours > 0 ? String(hours) + ":" + mm + ":" + ss : mm + ":" + ss
    }

    function bitrateText(value) {
        const bits = Number(value || 0)
        if (!(bits > 0))
            return ""
        return bits >= 1000000
            ? (bits / 1000000).toFixed(2) + " Mbps"
            : Math.round(bits / 1000) + " kbps"
    }

    function resolutionText(item) {
        const width = Number(item && item.width || 0)
        const height = Number(item && item.height || 0)
        if (!(width > 0 && height > 0))
            return ""
        return Math.round(width) + " × " + Math.round(height)
    }

    function aspectText(item) {
        let aspect = Number(item && item.aspect || 0)
        if (!(aspect > 0)) {
            const width = Number(item && item.width || 0)
            const height = Number(item && item.height || 0)
            if (width > 0 && height > 0)
                aspect = width / height
        }
        return aspect > 0 ? aspect.toFixed(2) + ":1" : ""
    }

    function fpsText(value) {
        const fps = Number(value || 0)
        return fps > 0 ? fps.toFixed(fps >= 100 ? 0 : 3) + " fps" : ""
    }

    function rateText(value) {
        const rate = Number(value || 0)
        return rate > 0 ? Math.round(rate / 100) / 10 + " kHz" : ""
    }

    function channelText(item) {
        const channels = Number(item && item.channels || 0)
        const layout = String(item && item.channel_layout || "")
        if (channels === 2 && (!layout || layout.toLowerCase().indexOf("stereo") >= 0))
            return "2 · " + translated("channel_stereo", "Stereo")
        if (channels === 1)
            return "1 · " + translated("channel_mono", "Mono")
        return channels > 0 ? String(channels) : ""
    }

    readonly property var cast: Playback.castSession
    readonly property var media: Player.mediaInfo
    readonly property var video: media && media.video ? media.video : null
    readonly property var audio: media && media.audio ? media.audio : null
    readonly property string mediaKind: media && media.kind ? String(media.kind) : ""

    Shortcut {
        sequences: ["Esc"]
        enabled: root.visible
        onActivated: root.close()
    }

    MouseArea {
        anchors.fill: parent
        onClicked: root.close()
    }

    Rectangle {
        id: panel
        width: Math.min(420, root.width - 32)
        height: Math.min(contentColumn.implicitHeight + 32, root.height - 16)
        x: Math.max(16, root.width - width - 16)
        y: 8
        radius: 8
        color: FluTheme.windowActiveBackgroundColor
        border.width: 1
        border.color: FluTheme.dividerColor
        clip: true

        FluShadow { anchors.fill: parent; radius: 18 }

        MouseArea {
            anchors.fill: parent
        }

        Flickable {
            id: scroller
            anchors.fill: parent
            anchors.margins: 16
            contentWidth: width
            contentHeight: contentColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: FluScrollBar {}

            ColumnLayout {
                id: contentColumn
                width: scroller.width
                spacing: 4

                FluText {
                    Layout.fillWidth: true
                    visible: !cast.connected
                    text: qsTr("ui_info_not_cast")
                    font: FluTextStyle.BodyStrong
                    textColor: FluTheme.fontPrimaryColor
                }

                InfoLine {
                    show: cast.connected
                    label: qsTr("ui_info_protocol")
                    value: root.protocolName(cast.protocol)
                }
                InfoLine {
                    show: cast.connected
                    label: qsTr("ui_info_control_point")
                    value: cast.controlPointName || qsTr("media_unknown")
                }
                InfoLine {
                    show: cast.connected && !!cast.controlPointAddress
                    label: qsTr("ui_info_address")
                    value: cast.controlPointAddress || ""
                }
                InfoLine {
                    show: cast.connected
                    label: qsTr("ui_info_control_mode")
                    value: cast.remoteControlRegistered
                           ? qsTr("ui_info_registered")
                           : qsTr("ui_info_control_unsubscribed")
                }

                FluDivider {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.bottomMargin: 8
                }

                FluText {
                    Layout.fillWidth: true
                    text: qsTr("ui_info_media")
                    font: FluTextStyle.BodyStrong
                    textColor: FluTheme.fontPrimaryColor
                }

                FluText {
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    visible: !Playback.hasMedia
                    text: qsTr("ui_info_not_playing")
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                }

                FluText {
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    visible: Playback.hasMedia && !video && !audio
                    text: qsTr("ui_info_unrecognized")
                    font: FluTextStyle.Body
                    textColor: FluTheme.fontSecondaryColor
                }

                InfoLine {
                    show: !!video || !!audio
                    label: qsTr("ui_info_container")
                    value: root.containerName(media.container)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: !!video || !!audio
                    label: qsTr("ui_info_duration")
                    value: root.durationText(media.duration_seconds)
                           || qsTr("media_unknown")
                }

                FluText {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: mediaKind === "video"
                    text: qsTr("ui_info_video")
                    font: FluTextStyle.BodyStrong
                    textColor: FluTheme.fontPrimaryColor
                }
                InfoLine {
                    show: mediaKind === "video"
                    label: qsTr("ui_info_codec")
                    value: root.codecName(video && video.codec)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: mediaKind === "video"
                    label: qsTr("ui_info_profile")
                    value: video && video.profile
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: mediaKind === "video"
                    label: qsTr("ui_info_resolution")
                    value: root.resolutionText(video)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: mediaKind === "video"
                    label: qsTr("ui_info_aspect_ratio")
                    value: root.aspectText(video)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: mediaKind === "video"
                    label: qsTr("ui_info_frame_rate")
                    value: root.fpsText(video && video.fps)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: mediaKind === "video"
                    label: qsTr("ui_info_bitrate")
                    value: root.bitrateText(video && video.bitrate)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: mediaKind === "video"
                    label: qsTr("ui_info_pixel_format")
                    value: video && video.pixel_format
                           || qsTr("media_unknown")
                }

                FluText {
                    Layout.fillWidth: true
                    Layout.topMargin: video ? 8 : 0
                    visible: mediaKind === "image"
                    text: qsTr("ui_info_image")
                    font: FluTextStyle.BodyStrong
                    textColor: FluTheme.fontPrimaryColor
                }
                InfoLine {
                    show: mediaKind === "image"
                    label: qsTr("ui_info_codec")
                    value: root.codecName(video && video.codec)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: mediaKind === "image"
                    label: qsTr("ui_info_resolution")
                    value: root.resolutionText(video)
                           || qsTr("media_unknown")
                }

                FluText {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: mediaKind === "audio" || (mediaKind === "video" && !!audio)
                    text: qsTr("ui_info_audio")
                    font: FluTextStyle.BodyStrong
                    textColor: FluTheme.fontPrimaryColor
                }
                InfoLine {
                    show: !!audio
                    label: qsTr("ui_info_codec")
                    value: root.codecName(audio && audio.codec)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: !!audio
                    label: qsTr("ui_info_profile")
                    value: audio && audio.profile
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: !!audio
                    label: qsTr("ui_info_sample_rate")
                    value: root.rateText(audio && audio.sample_rate)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: !!audio
                    label: qsTr("ui_info_channels")
                    value: root.channelText(audio)
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: !!audio
                    label: qsTr("ui_info_channel_layout")
                    value: audio && audio.channel_layout
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: !!audio
                    label: qsTr("ui_info_sample_format")
                    value: audio && audio.sample_format
                           || qsTr("media_unknown")
                }
                InfoLine {
                    show: !!audio
                    label: qsTr("ui_info_bitrate")
                    value: root.bitrateText(audio && audio.bitrate)
                           || qsTr("media_unknown")
                }
            }
        }
    }
}
