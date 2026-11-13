// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

import QtQuick
import FluentUI
import MediaCast 1.0

// Keyboard-accessible projection of the media bar.
//
// The media region is one focus surface rather than a list of Tab stops. This
// menu exposes the same operations to the Menu key, Shift+F10, and pointer
// context clicks. It is intentionally a presentation component: commands still
// flow through Playback and PlaybackBridge.
FluMenu {
    id: menu

    property Item anchorItem: null
    property var hostWindow: null

    signal pictureRequested()
    signal fullscreenRequested()

    width: 220
    modal: true

    onOpened: Interaction.openMediaMenu()
    onClosed: Interaction.closeOverlay()

    function showAt(x, y) {
        if (!anchorItem)
            return
        parent = anchorItem
        menu.x = Math.max(
            4, Math.min(Math.round(x), anchorItem.width - width - 4))
        menu.y = Math.max(
            4, Math.min(Math.round(y), anchorItem.height - height - 4))
        open()
    }

    function showCentered() {
        showAt((anchorItem ? anchorItem.width : 0) / 2 - width / 2,
               (anchorItem ? anchorItem.height : 0) / 2 - height / 2)
    }

    FluMenuItem {
        text: qsTr("ui_mediabar_previous")
        iconSource: FluentIcons.BackSolidBold
        enabled: Playback.hasPrevious
        onTriggered: Playback.previous()
    }

    FluMenuItem {
        readonly property bool showPlay: Playback.paused || Playback.idle
        text: showPlay ? qsTr("ui_mediabar_play") : qsTr("ui_mediabar_pause")
        iconSource: showPlay ? FluentIcons.PlaySolid : FluentIcons.PauseBold
        enabled: Playback.hasMedia
        onTriggered: Playback.togglePlayPause()
    }

    FluMenuItem {
        text: qsTr("ui_mediabar_next")
        iconSource: FluentIcons.ForwardSolidBold
        enabled: Playback.hasNext
        onTriggered: Playback.next()
    }

    FluMenuItem {
        text: qsTr("ui_media_menu_seek_back")
        iconSource: FluentIcons.Rewind
        enabled: Playback.hasMedia && Playback.duration > 0
        onTriggered: Playback.seekTo(
            Math.max(0, Playback.position - 10))
    }

    FluMenuItem {
        text: qsTr("ui_media_menu_seek_forward")
        iconSource: FluentIcons.FastForward
        enabled: Playback.hasMedia && Playback.duration > 0
        onTriggered: Playback.seekTo(
            Math.min(Playback.duration, Playback.position + 10))
    }

    FluMenuSeparator { }

    FluMenu {
        title: qsTr("ui_mediabar_volume")

        FluMenuItem {
            text: qsTr("ui_media_menu_volume_up")
            iconSource: FluentIcons.Volume3
            enabled: Playback.volumePercent < 100
            onTriggered: Playback.setVolumePercent(
                Math.min(100, Playback.volumePercent + 5))
        }

        FluMenuItem {
            text: qsTr("ui_media_menu_volume_down")
            iconSource: FluentIcons.Volume1
            enabled: Playback.volumePercent > 0
            onTriggered: Playback.setVolumePercent(
                Math.max(0, Playback.volumePercent - 5))
        }

        FluMenuItem {
            text: Playback.muted ? qsTr("ui_media_menu_unmute")
                                : qsTr("ui_media_menu_mute")
            iconSource: Playback.muted ? FluentIcons.Volume2 : FluentIcons.Mute
            checkable: true
            checked: Playback.muted
            onTriggered: Playback.setMuted(!Playback.muted)
        }
    }

    FluMenuItem {
        text: qsTr("ui_mediabar_subtitles")
        iconSource: FluentIcons.Subtitles
        enabled: false
    }

    FluMenuItem {
        text: qsTr("ui_mediabar_picture")
        iconSource: FluentIcons.Brightness
        onTriggered: menu.pictureRequested()
    }

    FluMenuItem {
        text: menu.hostWindow && menu.hostWindow.fullscreen
            ? qsTr("ui_mediabar_fullscreen_exit")
            : qsTr("ui_mediabar_fullscreen")
        iconSource: menu.hostWindow && menu.hostWindow.fullscreen
            ? FluentIcons.BackToWindow : FluentIcons.FullScreen
        checkable: true
        checked: menu.hostWindow ? menu.hostWindow.fullscreen : false
        enabled: !!menu.hostWindow
        onTriggered: menu.fullscreenRequested()
    }
}
