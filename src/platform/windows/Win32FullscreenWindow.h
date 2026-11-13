// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <memory>

class QWindow;

/**
 * All Windows-native state for one borderless fullscreen session.
 *
 * This class owns Win32 styles, WINDOWPLACEMENT, DWM chrome, system-menu
 * locking, native input filtering and QWindow/native geometry synchronization.
 * It never destroys or recreates the QWindow.
 */
class Win32FullscreenWindow final
{
public:
    explicit Win32FullscreenWindow(QWindow *window);
    ~Win32FullscreenWindow();

    Win32FullscreenWindow(const Win32FullscreenWindow &) = delete;
    Win32FullscreenWindow &operator=(const Win32FullscreenWindow &) = delete;

    bool enter();
    bool exit();
    bool active() const;
    QWindow *window() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
