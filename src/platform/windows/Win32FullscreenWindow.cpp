// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "Win32FullscreenWindow.h"

#include "WindowFrame.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWindow>

#include <windows.h>

struct Win32FullscreenWindow::Impl final : QAbstractNativeEventFilter
{
    explicit Impl(QWindow *target)
        : window(target)
    {
        repairTimer.setInterval(500);
        repairTimer.setTimerType(Qt::CoarseTimer);
        QObject::connect(
            &repairTimer, &QTimer::timeout,
            &repairTimer, [this] { repairGeometryIfNeeded(); });
    }

    ~Impl() override
    {
        repairTimer.stop();
        removeFilter();
    }

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

    void lockSystemMenu()
    {
        const HMENU menu = ::GetSystemMenu(hwnd, FALSE);
        if (!menu)
            return;
        ::DeleteMenu(menu, SC_RESTORE, MF_BYCOMMAND);
        ::DeleteMenu(menu, SC_MOVE, MF_BYCOMMAND);
        ::DeleteMenu(menu, SC_SIZE, MF_BYCOMMAND);
        ::DeleteMenu(menu, SC_MINIMIZE, MF_BYCOMMAND);
        ::DeleteMenu(menu, SC_MAXIMIZE, MF_BYCOMMAND);
        ::EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
        ::DrawMenuBar(hwnd);
    }

    void restoreSystemMenu()
    {
        if (::GetSystemMenu(hwnd, TRUE))
            ::DrawMenuBar(hwnd);
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
        if (!msg || msg->hwnd != hwnd)
            return false;

        if (msg->message == WM_NCHITTEST) {
            *result = HTCLIENT;
            return true;
        }

        if (msg->message == WM_INITMENU
            || msg->message == WM_INITMENUPOPUP) {
            const HMENU menu = reinterpret_cast<HMENU>(msg->wParam);
            if (menu) {
                ::EnableMenuItem(
                    menu, SC_RESTORE, MF_BYCOMMAND | MF_GRAYED);
                ::EnableMenuItem(
                    menu, SC_MOVE, MF_BYCOMMAND | MF_GRAYED);
                ::EnableMenuItem(
                    menu, SC_SIZE, MF_BYCOMMAND | MF_GRAYED);
                ::EnableMenuItem(
                    menu, SC_MINIMIZE, MF_BYCOMMAND | MF_GRAYED);
                ::EnableMenuItem(
                    menu, SC_MAXIMIZE, MF_BYCOMMAND | MF_GRAYED);
                ::EnableMenuItem(
                    menu, SC_CLOSE, MF_BYCOMMAND | MF_ENABLED);
            }
            return false;
        }

        if (msg->message == WM_WINDOWPOSCHANGING) {
            if (applyingGeometry)
                return false;
            auto *position =
                reinterpret_cast<WINDOWPOS *>(msg->lParam);
            if (!position)
                return false;
            position->flags |= SWP_NOMOVE | SWP_NOSIZE;
            *result = ::DefWindowProcW(
                msg->hwnd, msg->message, msg->wParam, msg->lParam);
            return true;
        }

        switch (msg->message) {
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONUP:
        case WM_NCLBUTTONDBLCLK:
        case WM_NCRBUTTONDOWN:
        case WM_NCRBUTTONUP:
        case WM_NCRBUTTONDBLCLK:
            *result = 0;
            return true;
        case WM_SYSCOMMAND: {
            const WPARAM command = msg->wParam & 0xFFF0;
            if (command == SC_SIZE || command == SC_MOVE
                || command == SC_MINIMIZE || command == SC_MAXIMIZE
                || command == SC_RESTORE) {
                *result = 0;
                return true;
            }
            return false;
        }
        default:
            break;
        }

        if (msg->message != WM_NCCALCSIZE || msg->wParam != TRUE)
            return false;

        auto *params =
            reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        if (!params)
            return false;

        RECT &client = params->rgrc[0];
        if (client.bottom > client.top)
            --client.bottom;
        return false;
    }

    bool applyGeometry()
    {
        QWindow *targetWindow = window.data();
        if (!active || !targetWindow || !hwnd)
            return false;

        applyingGeometry = true;
        if (::IsZoomed(hwnd))
            ::ShowWindow(hwnd, SW_RESTORE);

        constexpr int kOverscanPixels = 1;
        if (!::SetWindowPos(
                hwnd, HWND_TOPMOST,
                monitorRect.left, monitorRect.top,
                monitorRect.right - monitorRect.left,
                monitorRect.bottom - monitorRect.top + kOverscanPixels,
                SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
            applyingGeometry = false;
            return false;
        }

        if (QScreen *screen = targetWindow->screen()) {
            targetWindow->setGeometry(screen->geometry());
            RECT adjusted{};
            if (::GetWindowRect(hwnd, &adjusted)) {
                if (!::SetWindowPos(
                        hwnd, HWND_TOPMOST,
                        monitorRect.left, monitorRect.top,
                        adjusted.right - adjusted.left,
                        monitorRect.bottom - monitorRect.top
                            + kOverscanPixels,
                        SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
                    applyingGeometry = false;
                    return false;
                }
            }

            if (targetWindow->size() != screen->geometry().size()) {
                applyingGeometry = false;
                return false;
            }
        }

        RECT finalRect{};
        if (!::GetWindowRect(hwnd, &finalRect)
            || finalRect.left > monitorRect.left
            || finalRect.top > monitorRect.top
            || finalRect.right < monitorRect.right
            || finalRect.bottom < monitorRect.bottom) {
            applyingGeometry = false;
            return false;
        }

        WindowFrame::setFullscreenBorderless(targetWindow, true);
        WindowFrame::setRoundedCorners(targetWindow, false);
        WindowFrame::setTopMost(targetWindow, true);
        applyingGeometry = false;
        return true;
    }

    void repairGeometryIfNeeded()
    {
        if (!active || applyingGeometry)
            return;

        QWindow *targetWindow = window.data();
        if (!targetWindow || !hwnd)
            return;

        const HMONITOR monitor =
            ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (!monitor || !::GetMonitorInfoW(monitor, &monitorInfo))
            return;

        RECT actual{};
        const bool hasActual = ::GetWindowRect(hwnd, &actual);
        const bool coversMonitor =
            hasActual && actual.left <= monitorInfo.rcMonitor.left
            && actual.top <= monitorInfo.rcMonitor.top
            && actual.right >= monitorInfo.rcMonitor.right
            && actual.bottom >= monitorInfo.rcMonitor.bottom;
        const bool monitorMatches =
            ::EqualRect(&monitorRect, &monitorInfo.rcMonitor);
        const QScreen *screen = targetWindow->screen();
        const bool qtMatches =
            screen
            && targetWindow->size() == screen->geometry().size()
            && qFuzzyCompare(
                targetWindow->devicePixelRatio(),
                screen->devicePixelRatio());

        if (coversMonitor && monitorMatches && qtMatches)
            return;

        monitorRect = monitorInfo.rcMonitor;
        repairTimer.stop();
        applyGeometry();
        if (active)
            repairTimer.start();
    }

    QPointer<QWindow> window;
    HWND hwnd = nullptr;
    WINDOWPLACEMENT placement{};
    RECT monitorRect{};
    LONG_PTR normalStyle = 0;
    bool active = false;
    bool filterInstalled = false;
    bool styleLocked = false;
    bool applyingGeometry = false;
    QTimer repairTimer;
};

Win32FullscreenWindow::Win32FullscreenWindow(QWindow *window)
    : m_impl(std::make_unique<Impl>(window))
{
}

Win32FullscreenWindow::~Win32FullscreenWindow()
{
    if (m_impl->active)
        exit();
}

bool Win32FullscreenWindow::enter()
{
    Impl &state = *m_impl;
    QWindow *targetWindow = state.window.data();
    if (!targetWindow)
        return false;
    if (state.active)
        return true;

    const HWND hwnd = reinterpret_cast<HWND>(targetWindow->winId());
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

    state.hwnd = hwnd;
    state.placement = placement;
    state.monitorRect = monitorInfo.rcMonitor;
    state.normalStyle = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    state.active = true;
    state.installFilter();

    if (::IsZoomed(hwnd))
        ::ShowWindow(hwnd, SW_RESTORE);

    const LONG_PTR fullscreenStyle =
        (state.normalStyle
         & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX))
        | WS_POPUP;
    ::SetWindowLongPtrW(hwnd, GWL_STYLE, fullscreenStyle);
    state.styleLocked = true;
    state.lockSystemMenu();

    WindowFrame::setRoundedCorners(targetWindow, false);
    WindowFrame::setFullscreenBorderless(targetWindow, true);

    if (!state.applyGeometry()) {
        state.repairTimer.stop();
        state.removeFilter();
        state.restoreSystemMenu();
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, state.normalStyle);
        ::SetWindowPlacement(hwnd, &state.placement);
        WindowFrame::setTopMost(targetWindow, false);
        WindowFrame::applyFramelessWindowChrome(targetWindow);
        state.styleLocked = false;
        state.active = false;
        state.hwnd = nullptr;
        return false;
    }
    state.repairTimer.start();
    return true;
}

bool Win32FullscreenWindow::exit()
{
    Impl &state = *m_impl;
    if (!state.active)
        return true;

    QWindow *targetWindow = state.window.data();
    if (!targetWindow) {
        state.repairTimer.stop();
        state.removeFilter();
        state.active = false;
        state.hwnd = nullptr;
        return false;
    }

    const HWND hwnd = reinterpret_cast<HWND>(targetWindow->winId());
    if (!hwnd || hwnd != state.hwnd) {
        state.repairTimer.stop();
        state.removeFilter();
        state.active = false;
        state.hwnd = nullptr;
        return false;
    }

    state.repairTimer.stop();
    state.removeFilter();
    if (state.styleLocked)
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, state.normalStyle);
    state.styleLocked = false;
    state.restoreSystemMenu();

    if (!::SetWindowPlacement(hwnd, &state.placement)) {
        // Placement failed after the lock was removed. Rebuild the complete
        // fullscreen state so active() never claims success over a half-restored
        // ordinary window.
        const LONG_PTR fullscreenStyle =
            (state.normalStyle
             & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX
                 | WS_MAXIMIZEBOX))
            | WS_POPUP;
        ::SetWindowLongPtrW(hwnd, GWL_STYLE, fullscreenStyle);
        state.styleLocked = true;
        state.installFilter();
        state.lockSystemMenu();
        state.applyGeometry();
        state.repairTimer.start();
        return false;
    }

    WindowFrame::setTopMost(targetWindow, false);
    WindowFrame::applyFramelessWindowChrome(targetWindow);
    state.active = false;
    state.hwnd = nullptr;
    return true;
}

bool Win32FullscreenWindow::active() const
{
    return m_impl->active;
}

QWindow *Win32FullscreenWindow::window() const
{
    return m_impl->window.data();
}
