// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "FullscreenWindowController.h"

#include "Win32FullscreenWindow.h"

#include <QWindow>

FullscreenWindowController::FullscreenWindowController() = default;

FullscreenWindowController::~FullscreenWindowController() = default;

bool FullscreenWindowController::enter(QWindow *window)
{
    if (!window)
        return false;

    if (!m_native || m_native->window() != window) {
        if (m_native)
            m_native->exit();
        m_native = std::make_unique<Win32FullscreenWindow>(window);
    }
    return m_native->enter();
}

bool FullscreenWindowController::exit(QWindow *window)
{
    Q_UNUSED(window);
    if (!m_native)
        return true;

    const bool exited = m_native->exit();
    if (!m_native->active())
        m_native.reset();
    return exited;
}

bool FullscreenWindowController::active() const
{
    return m_native && m_native->active();
}
