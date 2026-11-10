// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/CastConfirmationDialog.h"

#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <iterator>
#include <utility>

namespace mcast::platform {
namespace {

struct DialogContext {
    ULONGLONG deadline = 0;
    std::chrono::milliseconds total{0};
    bool timedOut = false;
    bool cancelled = false;
    HANDLE cancelEvent = nullptr;
};

std::wstring fallback(std::wstring value, const wchar_t *emptyText)
{
    return value.empty() ? std::wstring(emptyText) : std::move(value);
}

std::wstring singleLine(std::wstring value)
{
    for (wchar_t &ch : value) {
        if (ch < 0x20 || ch == 0x7f || ch == 0x061c
            || ch == 0x200e || ch == 0x200f
            || (ch >= 0x202a && ch <= 0x202e)
            || ch == 0x2028 || ch == 0x2029
            || (ch >= 0x2066 && ch <= 0x2069)) {
            ch = L' ';
        }
    }
    return value;
}

std::wstring promptContent(
    const CastConfirmationPrompt &prompt)
{
    const std::wstring protocol = singleLine(fallback(
        prompt.protocolName,
        prompt.chinese ? L"未知协议" : L"Unknown protocol"));
    const std::wstring peer = singleLine(fallback(
        prompt.peerName,
        prompt.chinese ? L"未知设备" : L"Unknown device"));
    const std::wstring title = singleLine(fallback(
        prompt.mediaTitle,
        prompt.chinese ? L"未提供媒体标题" : L"No media title"));
    if (prompt.chinese) {
        return L"设备：" + peer + L"\n协议：" + protocol + L"\n内容：" + title
            + L"\n\n允许后，当前投送会结束并切换到新的投送。";
    }
    return L"Device: " + peer + L"\nProtocol: " + protocol
        + L"\nMedia: " + title
        + L"\n\nAllowing this request ends the current cast.";
}

HRESULT CALLBACK dialogCallback(
    HWND dialog, UINT notification, WPARAM wParam, LPARAM,
    LONG_PTR contextValue)
{
    auto *context = reinterpret_cast<DialogContext *>(contextValue);
    if (!context)
        return S_OK;
    switch (notification) {
    case TDN_CREATED:
        if (context->cancelEvent
            && WaitForSingleObject(context->cancelEvent, 0)
                == WAIT_OBJECT_0) {
            context->cancelled = true;
            PostMessageW(dialog, TDM_CLICK_BUTTON, IDNO, 0);
            break;
        }
        SendMessageW(dialog, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(dialog, TDM_SET_PROGRESS_BAR_POS, 100, 0);
        SetWindowPos(
            dialog, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        ShowWindow(dialog, SW_SHOWNORMAL);
        if (!SetForegroundWindow(dialog)) {
            const HWND foreground = GetForegroundWindow();
            const DWORD foregroundThread = foreground
                ? GetWindowThreadProcessId(foreground, nullptr) : 0;
            const DWORD currentThread = GetCurrentThreadId();
            if (foregroundThread != 0
                && foregroundThread != currentThread
                && AttachThreadInput(
                    currentThread, foregroundThread, TRUE)) {
                SetForegroundWindow(dialog);
                AttachThreadInput(currentThread, foregroundThread, FALSE);
            }
        }
        {
            FLASHWINFO flash{
                sizeof(FLASHWINFO), dialog, FLASHW_ALL | FLASHW_TIMERNOFG,
                0, 0,
            };
            FlashWindowEx(&flash);
        }
        SetFocus(dialog);
        break;
    case TDN_TIMER:
        {
            if (context->cancelEvent
                && WaitForSingleObject(context->cancelEvent, 0)
                    == WAIT_OBJECT_0) {
                context->cancelled = true;
                SendMessageW(dialog, TDM_CLICK_BUTTON, IDNO, 0);
                break;
            }
            const ULONGLONG now = GetTickCount64();
            const ULONGLONG total = context->total.count();
            const ULONGLONG remaining = now < context->deadline
                ? context->deadline - now : 0;
            const int percent = total == 0
                ? 0
                : static_cast<int>(
                    std::min<ULONGLONG>(100, remaining * 100 / total));
            SendMessageW(dialog, TDM_SET_PROGRESS_BAR_POS, percent, 0);
            if (remaining == 0) {
                context->timedOut = true;
                SendMessageW(dialog, TDM_CLICK_BUTTON, IDNO, 0);
            }
        }
        break;
    case TDN_BUTTON_CLICKED:
        if (context->cancelEvent
            && WaitForSingleObject(context->cancelEvent, 0)
                == WAIT_OBJECT_0) {
            context->cancelled = true;
        } else if (GetTickCount64() >= context->deadline) {
            context->timedOut = true;
        }
        break;
    default:
        break;
    }
    return S_OK;
}

} // namespace

CastConfirmationChoice showCastConfirmationDialog(
    HWND owner, const CastConfirmationPrompt &prompt)
{
    INITCOMMONCONTROLSEX controls{
        sizeof(INITCOMMONCONTROLSEX),
        ICC_STANDARD_CLASSES,
    };
    InitCommonControlsEx(&controls);

    const std::wstring content = promptContent(prompt);
    const TASKDIALOG_BUTTON buttons[] = {
        {IDYES, prompt.chinese ? L"允许投送" : L"Allow cast"},
        {IDNO, prompt.chinese ? L"继续当前投送" : L"Keep current cast"},
    };
    DialogContext context;
    context.total = std::max(
        prompt.timeout, std::chrono::milliseconds{100});
    context.deadline = GetTickCount64()
        + static_cast<ULONGLONG>(context.total.count());
    context.cancelEvent = prompt.cancelEvent;

    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.hwndParent = owner;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION
        | TDF_SHOW_PROGRESS_BAR | TDF_CALLBACK_TIMER
        | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle =
        prompt.chinese ? L"Media Cast — 投送确认" : L"Media Cast — Cast request";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = prompt.chinese
        ? L"另一个设备想接管投送"
        : L"Another device wants to take over";
    config.pszContent = content.c_str();
    config.pszFooter = prompt.chinese
        ? L"未在倒计时内选择时，将继续当前投送。"
        : L"If no choice is made, the current cast continues.";
    config.pButtons = buttons;
    config.cButtons = static_cast<UINT>(std::size(buttons));
    config.nDefaultButton = IDNO;
    config.pfCallback = &dialogCallback;
    config.lpCallbackData = reinterpret_cast<LONG_PTR>(&context);
    config.cxWidth = 520;

    int button = IDNO;
    const HRESULT result = TaskDialogIndirect(
        &config, &button, nullptr, nullptr);
    if (FAILED(result))
        return CastConfirmationChoice::Failed;
    if (context.cancelled)
        return CastConfirmationChoice::Cancelled;
    if (context.timedOut)
        return CastConfirmationChoice::TimedOut;
    if (button == IDYES)
        return CastConfirmationChoice::Accepted;
    return CastConfirmationChoice::Declined;
}

} // namespace mcast::platform
