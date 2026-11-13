// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "FullscreenWindowController.h"

#include "WindowFrame.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

struct FullscreenWindowController::State
#ifdef Q_OS_WIN
    : QAbstractNativeEventFilter
#endif
{
#ifdef Q_OS_WIN
    HWND hwnd = nullptr;
    WINDOWPLACEMENT placement{};
    bool filterInstalled = false;

    void installFilter()
    {
        if (filterInstalled)
            return;
        if (auto *app = QCoreApplication::instance()) {
            app->installNativeEventFilter(this);
            filterInstalled = true;
        }
    }

    void removeFilter()
    {
        if (!filterInstalled)
            return;
        if (auto *app = QCoreApplication::instance())
            app->removeNativeEventFilter(this);
        filterInstalled = false;
    }

    bool nativeEventFilter(
        const QByteArray &eventType, void *message, qintptr *result) override
    {
        Q_UNUSED(result);
        if (!active
            || (eventType != QByteArrayLiteral("windows_generic_MSG")
                && eventType != QByteArrayLiteral("windows_dispatcher_MSG"))) {
            return false;
        }

        const auto *msg = static_cast<const MSG *>(message);
        if (!msg || msg->hwnd != hwnd || msg->message != WM_NCCALCSIZE
            || msg->wParam != TRUE) {
            return false;
        }

        auto *params =
            reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        if (!params)
            return false;

        RECT &client = params->rgrc[0];
        if (client.bottom > client.top)
            --client.bottom;
        return false;
    }
#endif
    bool active = false;
};

FullscreenWindowController::FullscreenWindowController()
    : m_state(std::make_unique<State>())
{
}

FullscreenWindowController::~FullscreenWindowController()
{
    if (m_state && m_state->active)
        exit(m_window);
#ifdef Q_OS_WIN
    if (m_state)
        m_state->removeFilter();
#endif
}

bool FullscreenWindowController::enter(QWindow *window)
{
#ifdef Q_OS_WIN
    if (!window)
        return false;
    if (active())
        return m_window == window;

    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd || !::IsWindow(hwnd))
        return false;

    WINDOWPLACEMENT placement{};
    placement.length = sizeof(placement);
    if (!::GetWindowPlacement(hwnd, &placement))
        return false;

    const HMONITOR monitor =
        ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !::GetMonitorInfoW(monitor, &monitorInfo))
        return false;

    m_state->hwnd = hwnd;
    m_state->placement = placement;
    m_state->active = true;
    m_window = window;

#ifdef Q_OS_WIN
    m_state->installFilter();
#endif

    // A maximized window does not reliably accept a new target rectangle.
    // Restore first; the original WINDOWPLACEMENT above is enough to put it
    // back exactly after fullscreen.
    if (::IsZoomed(hwnd))
        ::ShowWindow(hwnd, SW_RESTORE);

    WindowFrame::setRoundedCorners(window, false);
    WindowFrame::setFullscreenBorderless(window, true);

    const RECT &target = monitorInfo.rcMonitor;

    // Do not use the exact monitor rectangle. DWM treats an exact-monitor
    // window as fullscreen and disables non-client rendering; when the window
    // returns to its normal rectangle, DWMWA_NCRENDERING_ENABLED can remain 0
    // and the system border/shadow never comes back. Extend only the bottom by
    // one pixel so the top and right media edges stay aligned with the monitor;
    // WM_NCCALCSIZE reserves that pixel as non-client area.
    constexpr int kOverscanPixels = 1;
    if (!::SetWindowPos(
            hwnd, HWND_TOPMOST,
            target.left, target.top,
            target.right - target.left,
            target.bottom - target.top + kOverscanPixels,
            SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
        ::SetWindowPlacement(hwnd, &m_state->placement);
#ifdef Q_OS_WIN
        m_state->removeFilter();
#endif
        WindowFrame::setTopMost(window, false);
        WindowFrame::applyFramelessWindowChrome(window);
        m_state->active = false;
        m_state->hwnd = nullptr;
        m_window.clear();
        return false;
    }
    WindowFrame::setTopMost(window, true);
    return true;
#else
    Q_UNUSED(window);
    return false;
#endif
}

bool FullscreenWindowController::exit(QWindow *window)
{
#ifdef Q_OS_WIN
    if (!m_state->active)
        return true;

    QWindow *targetWindow =
        window && m_window == window ? window : m_window.data();
    if (!targetWindow) {
#ifdef Q_OS_WIN
        m_state->removeFilter();
#endif
        m_state->active = false;
        m_state->hwnd = nullptr;
        return false;
    }

    const HWND hwnd = reinterpret_cast<HWND>(targetWindow->winId());
    if (!hwnd || hwnd != m_state->hwnd) {
#ifdef Q_OS_WIN
        m_state->removeFilter();
#endif
        m_state->active = false;
        m_state->hwnd = nullptr;
        m_window.clear();
        return false;
    }

    const BOOL restored =
        ::SetWindowPlacement(hwnd, &m_state->placement);
    if (!restored)
        return false;

#ifdef Q_OS_WIN
    // Remove the client-area inset before the normal chrome pass. Otherwise
    // that pass's SWP_FRAMECHANGED runs WM_NCCALCSIZE with the fullscreen
    // filter still active and leaves the ordinary window one pixel short.
    m_state->removeFilter();
#endif
    WindowFrame::setTopMost(targetWindow, false);
    WindowFrame::applyFramelessWindowChrome(targetWindow);

    m_state->active = false;
    m_state->hwnd = nullptr;
    m_window.clear();
    return true;
#else
    Q_UNUSED(window);
    return false;
#endif
}

bool FullscreenWindowController::active() const
{
    return m_state && m_state->active;
}
