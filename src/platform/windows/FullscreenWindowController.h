// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <memory>

class QWindow;
class Win32FullscreenWindow;

/**
 * Window-layer facade for entering and leaving borderless fullscreen.
 *
 * The QML tree, QQuickWindow scene graph and mpv render context survive every
 * transition. All Win32 state is owned by Win32FullscreenWindow.
 */
class FullscreenWindowController final
{
public:
    FullscreenWindowController();
    ~FullscreenWindowController();

    FullscreenWindowController(const FullscreenWindowController &) = delete;
    FullscreenWindowController &operator=(const FullscreenWindowController &) =
        delete;

    bool enter(QWindow *window);
    bool exit(QWindow *window);
    bool active() const;

private:
    std::unique_ptr<Win32FullscreenWindow> m_native;
};
