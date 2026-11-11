// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/TrayIcon.h"

#include <windowsx.h>

#include <utility>

namespace mcast::platform {
namespace {

constexpr wchar_t kWindowClass[] = L"MCast.NativeTray";
constexpr UINT kTrayMessage = WM_APP + 0x41;
constexpr UINT kReceivingUpdateMessage = WM_APP + 0x42;
constexpr UINT kChineseUpdateMessage = WM_APP + 0x43;
constexpr UINT kOpenMainCommand = 1;
constexpr UINT kToggleReceivingCommand = 2;
constexpr UINT kQuitCommand = 3;

} // namespace

NativeTrayIcon::NativeTrayIcon(
    Action openUi, Action toggleReceiving, Action quit)
    : m_openUi(std::move(openUi))
    , m_toggleReceiving(std::move(toggleReceiving))
    , m_quit(std::move(quit))
{
    m_chinese =
        PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

NativeTrayIcon::~NativeTrayIcon()
{
    removeNotificationIcon();
    if (m_menu)
        DestroyMenu(m_menu);
    if (m_window)
        DestroyWindow(m_window);
}

bool NativeTrayIcon::initialize(std::string &error)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &NativeTrayIcon::windowProcedure;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)
        && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "RegisterClassExW failed: "
            + std::to_string(GetLastError());
        return false;
    }

    m_window = CreateWindowExW(
        WS_EX_TOOLWINDOW, kWindowClass, L"Media Cast Tray",
        WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
        GetModuleHandleW(nullptr), this);
    if (!m_window) {
        error = "CreateWindowExW(tray) failed: "
            + std::to_string(GetLastError());
        return false;
    }

    m_icon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));
    if (!m_icon)
        m_icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    m_taskbarCreatedMessage =
        RegisterWindowMessageW(L"TaskbarCreated");
    rebuildMenu();
    return addNotificationIcon(error);
}

void NativeTrayIcon::setChinese(bool chinese)
{
    if (m_chinese == chinese)
        return;
    m_chinese = chinese;
    rebuildMenu();
    setReceivingEnabled(m_receivingEnabled);
}

void NativeTrayIcon::requestChinese(bool chinese)
{
    if (m_window) {
        PostMessageW(
            m_window, kChineseUpdateMessage, 0,
            chinese ? 1 : 0);
        return;
    }
    setChinese(chinese);
}

void NativeTrayIcon::setReceivingEnabled(bool enabled)
{
    m_receivingEnabled = enabled;
    rebuildMenu();
    if (!m_window || !m_iconAdded)
        return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_window;
    data.uID = 1;
    data.uFlags = NIF_TIP;
    const std::wstring tooltip = useChinese()
        ? (enabled ? L"Media Cast" : L"Media Cast（已暂停接收投送）")
        : (enabled ? L"Media Cast" : L"Media Cast (receiving paused)");
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void NativeTrayIcon::requestReceivingEnabled(bool enabled)
{
    if (m_window) {
        PostMessageW(
            m_window, kReceivingUpdateMessage, 0,
            enabled ? 1 : 0);
        return;
    }
    m_receivingEnabled = enabled;
}

void NativeTrayIcon::pumpMessages()
{
    MSG message{};
    while (PeekMessageW(
        &message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

LRESULT CALLBACK NativeTrayIcon::windowProcedure(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
        auto *self = static_cast<NativeTrayIcon *>(create->lpCreateParams);
        SetWindowLongPtrW(
            window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(self));
        self->m_window = window;
    }
    auto *self = reinterpret_cast<NativeTrayIcon *>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self)
        return self->handleMessage(message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT NativeTrayIcon::handleMessage(
    UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == kTrayMessage) {
        switch (LOWORD(lParam)) {
        case NIN_SELECT:
        case WM_LBUTTONDBLCLK:
            activateOpenUi();
            return 0;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            showContextMenu();
            return 0;
        default:
            break;
        }
    } else if (message == m_taskbarCreatedMessage
               && m_taskbarCreatedMessage != 0) {
        m_iconAdded = false;
        std::string error;
        addNotificationIcon(error);
    } else if (message == kReceivingUpdateMessage) {
        setReceivingEnabled(lParam != 0);
        return 0;
    } else if (message == kChineseUpdateMessage) {
        setChinese(lParam != 0);
        return 0;
    } else if (message == WM_COMMAND) {
        switch (LOWORD(wParam)) {
        case kOpenMainCommand:
            activateOpenUi();
            return 0;
        case kToggleReceivingCommand:
            if (m_toggleReceiving)
                m_toggleReceiving();
            return 0;
        case kQuitCommand:
            if (m_quit)
                m_quit();
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

void NativeTrayIcon::activateOpenUi()
{
    const ULONGLONG now = GetTickCount64();
    if (m_lastOpenTick != 0 && now - m_lastOpenTick < 300)
        return;
    m_lastOpenTick = now;
    if (m_openUi)
        m_openUi();
}

void NativeTrayIcon::showContextMenu()
{
    if (!m_window || !m_menu)
        return;
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(m_window);
    const UINT command = TrackPopupMenuEx(
        m_menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        cursor.x, cursor.y, m_window, nullptr);
    if (command != 0)
        PostMessageW(m_window, WM_COMMAND, command, 0);
    PostMessageW(m_window, WM_NULL, 0, 0);
}

void NativeTrayIcon::rebuildMenu()
{
    if (m_menu)
        DestroyMenu(m_menu);
    m_menu = CreatePopupMenu();
    if (!m_menu)
        return;
    const bool chinese = useChinese();
    AppendMenuW(
        m_menu, MF_STRING, kOpenMainCommand,
        chinese ? L"打开主界面" : L"Open main window");
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        m_menu, MF_STRING, kToggleReceivingCommand,
        chinese
            ? (m_receivingEnabled ? L"暂停接收投送" : L"恢复接收投送")
            : (m_receivingEnabled ? L"Pause receiving" : L"Resume receiving"));
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m_menu, MF_STRING, kQuitCommand, chinese ? L"退出" : L"Quit");
}

bool NativeTrayIcon::addNotificationIcon(std::string &error)
{
    if (!m_window)
        return false;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_window;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = m_icon;
    wcsncpy_s(data.szTip, L"Media Cast", _TRUNCATE);
    if (!Shell_NotifyIconW(
            m_iconAdded ? NIM_MODIFY : NIM_ADD, &data)) {
        error = "Shell_NotifyIconW failed: "
            + std::to_string(GetLastError());
        return false;
    }
    m_iconAdded = true;
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    setReceivingEnabled(m_receivingEnabled);
    return true;
}

void NativeTrayIcon::removeNotificationIcon()
{
    if (!m_window || !m_iconAdded)
        return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = m_window;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    m_iconAdded = false;
}

bool NativeTrayIcon::useChinese() const
{
    return m_chinese;
}

} // namespace mcast::platform
