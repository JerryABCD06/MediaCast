// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QPointer>

#include <memory>

class QWindow;

/**
 * Adjusts the existing native window for borderless fullscreen.
 *
 * The controller never destroys or recreates the QWindow. The QML tree, the
 * QQuickWindow scene graph and the mpv render context therefore survive every
 * fullscreen transition.
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
    struct State;

    QPointer<QWindow> m_window;
    std::unique_ptr<State> m_state;
};
