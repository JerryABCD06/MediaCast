// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <windows.h>

#include <atomic>
#include <functional>
#include <string>

namespace mcast::platform {

struct TrayTexts {
    std::wstring openMain;
    std::wstring pauseReceiving;
    std::wstring resumeReceiving;
    std::wstring quit;
    std::wstring tooltipReceiving;
    std::wstring tooltipPaused;
};

class NativeTrayIcon
{
public:
    using Action = std::function<void()>;

    NativeTrayIcon(
        Action openUi, Action toggleReceiving, Action quit,
        Action themeChanged = {});
    ~NativeTrayIcon();

    NativeTrayIcon(const NativeTrayIcon &) = delete;
    NativeTrayIcon &operator=(const NativeTrayIcon &) = delete;

    bool initialize(std::string &error);
    void setTexts(const TrayTexts &texts);
    void requestTexts(TrayTexts texts);
    void setDark(bool dark);
    void requestDark(bool dark);
    void setReceivingEnabled(bool enabled);
    void requestReceivingEnabled(bool enabled);
    void pumpMessages();
    HWND window() const noexcept { return m_window; }

private:
    static LRESULT CALLBACK windowProcedure(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void showContextMenu();
    void rebuildMenu();
    void activateOpenUi();
    bool addNotificationIcon(std::string &error);
    void removeNotificationIcon();
    void applyDarkMode();

    Action m_openUi;
    Action m_toggleReceiving;
    Action m_quit;
    Action m_themeChanged;
    HWND m_window = nullptr;
    HICON m_icon = nullptr;
    HMENU m_menu = nullptr;
    UINT m_taskbarCreatedMessage = 0;
    std::atomic_bool m_receivingEnabled{true};
    bool m_iconAdded = false;
    ULONGLONG m_lastOpenTick = 0;
    TrayTexts m_texts;
    bool m_dark = false;
};

} // namespace mcast::platform
