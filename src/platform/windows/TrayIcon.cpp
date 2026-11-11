// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/TrayIcon.h"

#include <windowsx.h>
#include <winternl.h>

#include <memory>
#include <utility>

namespace mcast::platform {
namespace {

constexpr wchar_t kWindowClass[] = L"MCast.NativeTray";
constexpr UINT kTrayMessage = WM_APP + 0x41;
constexpr UINT kReceivingUpdateMessage = WM_APP + 0x42;
constexpr UINT kTextsUpdateMessage = WM_APP + 0x43;
constexpr UINT kDarkUpdateMessage = WM_APP + 0x44;
constexpr UINT kOpenMainCommand = 1;
constexpr UINT kToggleReceivingCommand = 2;
constexpr UINT kQuitCommand = 3;

enum PreferredAppMode : int {
    DefaultMode = 0,
    AllowDarkMode = 1,
    ForceDarkMode = 2,
    ForceLightMode = 3,
};

using SetPreferredAppModeFn = int(WINAPI *)(int);
using AllowDarkModeForWindowFn = bool(WINAPI *)(HWND, bool);
using FlushMenuThemesFn = void(WINAPI *)();

struct DarkModeApi {
    SetPreferredAppModeFn setPreferredAppMode = nullptr;
    AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    FlushMenuThemesFn flushMenuThemes = nullptr;
};

bool knownDarkModeOrdinalRange()
{
    using RtlGetVersionFn = LONG(WINAPI *)(PRTL_OSVERSIONINFOW);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    if (!module)
        return false;
    const auto getVersion = reinterpret_cast<RtlGetVersionFn>(
        GetProcAddress(module, "RtlGetVersion"));
    if (!getVersion)
        return false;
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (getVersion(&version) != 0)
        return false;
    return version.dwBuildNumber >= 17763
        && version.dwBuildNumber <= 26100;
}

DarkModeApi darkModeApi()
{
    static const DarkModeApi api = [] {
        DarkModeApi result;
        HMODULE module = LoadLibraryW(L"uxtheme.dll");
        if (!module)
            return result;
        result.setPreferredAppMode =
            reinterpret_cast<SetPreferredAppModeFn>(
                GetProcAddress(module, "SetPreferredAppMode"));
        const bool useOrdinals = knownDarkModeOrdinalRange();
        if (!result.setPreferredAppMode && useOrdinals) {
            result.setPreferredAppMode =
                reinterpret_cast<SetPreferredAppModeFn>(
                    GetProcAddress(module, MAKEINTRESOURCEA(135)));
        }
        result.allowDarkModeForWindow =
            reinterpret_cast<AllowDarkModeForWindowFn>(
                GetProcAddress(module, "AllowDarkModeForWindow"));
        if (!result.allowDarkModeForWindow && useOrdinals) {
            result.allowDarkModeForWindow =
                reinterpret_cast<AllowDarkModeForWindowFn>(
                    GetProcAddress(module, MAKEINTRESOURCEA(133)));
        }
        result.flushMenuThemes =
            reinterpret_cast<FlushMenuThemesFn>(
                GetProcAddress(module, "FlushMenuThemes"));
        if (!result.flushMenuThemes && useOrdinals) {
            result.flushMenuThemes =
                reinterpret_cast<FlushMenuThemesFn>(
                    GetProcAddress(module, MAKEINTRESOURCEA(136)));
        }
        return result;
    }();
    return api;
}

bool highContrastEnabled()
{
    HIGHCONTRASTW contrast{sizeof(contrast)};
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST, sizeof(contrast),
               &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

} // namespace

NativeTrayIcon::NativeTrayIcon(
    Action openUi, Action toggleReceiving, Action quit,
    Action themeChanged)
    : m_openUi(std::move(openUi))
    , m_toggleReceiving(std::move(toggleReceiving))
    , m_quit(std::move(quit))
    , m_themeChanged(std::move(themeChanged))
{
    m_texts = {
        L"Open main window",
        L"Pause receiving",
        L"Resume receiving",
        L"Quit",
        L"Media Cast",
        L"Media Cast (receiving paused)",
    };
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
    applyDarkMode();
    return addNotificationIcon(error);
}

void NativeTrayIcon::setTexts(const TrayTexts &texts)
{
    m_texts = texts;
    rebuildMenu();
    setReceivingEnabled(m_receivingEnabled);
}

void NativeTrayIcon::requestTexts(TrayTexts texts)
{
    if (m_window) {
        auto *payload = new TrayTexts(std::move(texts));
        if (!PostMessageW(
                m_window, kTextsUpdateMessage, 0,
                reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
        return;
    }
    setTexts(texts);
}

void NativeTrayIcon::setDark(bool dark)
{
    m_dark = dark;
    if (m_window)
        applyDarkMode();
}

void NativeTrayIcon::requestDark(bool dark)
{
    if (m_window) {
        PostMessageW(
            m_window, kDarkUpdateMessage, 0,
            dark ? 1 : 0);
        return;
    }
    m_dark = dark;
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
    const std::wstring &tooltip = enabled
        ? m_texts.tooltipReceiving : m_texts.tooltipPaused;
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
    } else if (message == kTextsUpdateMessage) {
        std::unique_ptr<TrayTexts> texts(
            reinterpret_cast<TrayTexts *>(lParam));
        if (texts)
            setTexts(*texts);
        return 0;
    } else if (message == kDarkUpdateMessage) {
        setDark(lParam != 0);
        return 0;
    } else if (message == WM_SETTINGCHANGE || message == WM_THEMECHANGED) {
        if (m_themeChanged)
            m_themeChanged();
        applyDarkMode();
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
    applyDarkMode();
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
    AppendMenuW(
        m_menu, MF_STRING, kOpenMainCommand,
        m_texts.openMain.c_str());
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        m_menu, MF_STRING, kToggleReceivingCommand,
        (m_receivingEnabled
             ? m_texts.pauseReceiving
             : m_texts.resumeReceiving).c_str());
    AppendMenuW(m_menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(
        m_menu, MF_STRING, kQuitCommand, m_texts.quit.c_str());
    SetMenuDefaultItem(m_menu, kOpenMainCommand, FALSE);
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
    wcsncpy_s(
        data.szTip, m_texts.tooltipReceiving.c_str(), _TRUNCATE);
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

void NativeTrayIcon::applyDarkMode()
{
    const bool highContrast = highContrastEnabled();
    const bool useDark = m_dark && !highContrast;
    const DarkModeApi api = darkModeApi();
    if (api.setPreferredAppMode) {
        api.setPreferredAppMode(
            highContrast
                ? DefaultMode
                : (useDark ? ForceDarkMode : ForceLightMode));
    }
    if (m_window && api.allowDarkModeForWindow)
        api.allowDarkModeForWindow(m_window, useDark);
    if (api.flushMenuThemes)
        api.flushMenuThemes();
}

} // namespace mcast::platform
