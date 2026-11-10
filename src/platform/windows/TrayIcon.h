// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace mcast::platform {

class NativeTrayIcon
{
public:
    using Action = std::function<void()>;

    NativeTrayIcon(Action openUi, Action toggleReceiving, Action quit);
    ~NativeTrayIcon();

    NativeTrayIcon(const NativeTrayIcon &) = delete;
    NativeTrayIcon &operator=(const NativeTrayIcon &) = delete;

    bool initialize(std::string &error);
    void setChinese(bool chinese);
    void setReceivingEnabled(bool enabled);
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
    bool useChinese() const;

    Action m_openUi;
    Action m_toggleReceiving;
    Action m_quit;
    HWND m_window = nullptr;
    HICON m_icon = nullptr;
    HMENU m_menu = nullptr;
    UINT m_taskbarCreatedMessage = 0;
    bool m_receivingEnabled = true;
    bool m_iconAdded = false;
    ULONGLONG m_lastOpenTick = 0;
    bool m_chinese = false;
};

} // namespace mcast::platform
