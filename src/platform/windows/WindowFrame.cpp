// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "WindowFrame.h"

#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace WindowFrame
{

namespace {

/** 取原生窗口句柄；顺手保证它已经建出来。拿不到就 nullptr。 */
HWND nativeHandle(QWindow *window)
{
    if (!window)
        return nullptr;
    return reinterpret_cast<HWND>(window->winId());
}

} // namespace

void ensureSnapFlags(QWindow *window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;

    // winId() 会顺手把原生窗口建出来（如果还没建），所以这个调用不挑时机。
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;

    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wanted = style | WS_SYSMENU | WS_MINIMIZEBOX;
    if (wanted == style)
        return;

    ::SetWindowLongPtrW(hwnd, GWL_STYLE, wanted);

    // 光改样式位不够。系统是在处理 WM_NCCALCSIZE 的时候重算非客户区的，
    // 不主动喊它一声，新样式要等到用户下次拖窗口大小才生效 —— 中间那段时间
    // 窗口处于一种"样式位说一套、实际样子是另一套"的错位状态。
    // SWP_FRAMECHANGED 就是"边框变了，重算一次"。
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE
                       | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    Q_UNUSED(window);
#endif
}

void setTopMost(QWindow *window, bool onTop)
{
#ifdef Q_OS_WIN
    const HWND hwnd = nativeHandle(window);
    if (!hwnd)
        return;

    // HWND_TOPMOST / HWND_NOTOPMOST 是约定的特殊句柄值（-1 / -2）。
    ::SetWindowPos(hwnd, onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    Q_UNUSED(window);
    Q_UNUSED(onTop);
#endif
}

void setRoundedCorners(QWindow *window, bool rounded)
{
#ifdef Q_OS_WIN
    const HWND hwnd = nativeHandle(window);
    if (!hwnd)
        return;

    // DWMWA_WINDOW_CORNER_PREFERENCE = 33（Win11 才有这个属性）
    // DWMWCP_DEFAULT = 0（交回系统）/ DWMWCP_DONOTROUND = 1
    const DWORD preference = rounded ? 0u : 1u;
    // 老系统上这个属性不存在，调用会失败 —— 失败就随它去，不是要紧事。
    ::DwmSetWindowAttribute(hwnd, 33, &preference, sizeof(preference));
#else
    Q_UNUSED(window);
    Q_UNUSED(rounded);
#endif
}

}
