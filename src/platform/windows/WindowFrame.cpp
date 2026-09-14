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

void reapplyFramelessStyle(QWindow *window)
{
#ifdef Q_OS_WIN
    const HWND hwnd = nativeHandle(window);
    if (!hwnd)
        return;

    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    // 这一套 = 库里创建时打的那三个 + 它少打的那两个。硬要求见头注释。
    const LONG_PTR wanted = style | WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX
                                  | WS_MINIMIZEBOX | WS_SYSMENU;
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

void reapplyDwmShadow(QWindow *window)
{
#ifdef Q_OS_WIN
    const HWND hwnd = nativeHandle(window);
    if (!hwnd)
        return;

    // 和 FluFrameless::setShadow() 用的是同一组参数：左边留 1 像素。
    const MARGINS shadow = { 1, 0, 0, 0 };
    ::DwmExtendFrameIntoClientArea(hwnd, &shadow);
#else
    Q_UNUSED(window);
#endif
}

void setFullscreenBorderless(QWindow *window, bool borderless)
{
#ifdef Q_OS_WIN
    const HWND hwnd = nativeHandle(window);
    if (!hwnd)
        return;

    // DWMWA_BORDER_COLOR = 34（Win11 才有）
    //   DWMWA_COLOR_DEFAULT = 0xFFFFFFFF（系统默认那条边框）
    //   DWMWA_COLOR_NONE    = 0xFFFFFFFE（不画）
    const DWORD color = borderless ? 0xFFFFFFFEu : 0xFFFFFFFFu;
    ::DwmSetWindowAttribute(hwnd, 34, &color, sizeof(color));

    // 阴影：靠"把 DWM 的框伸进客户区 1 像素"实现。全屏时把它收回 0 ——
    // 那 1 像素同样会落在屏幕边上（左边缘那条）。
    const MARGINS shadow = borderless ? MARGINS{ 0, 0, 0, 0 } : MARGINS{ 1, 0, 0, 0 };
    ::DwmExtendFrameIntoClientArea(hwnd, &shadow);
#else
    Q_UNUSED(window);
    Q_UNUSED(borderless);
#endif
}

void setWindowOwner(QWindow *window, QWindow *owner)
{
#ifdef Q_OS_WIN
    const HWND hwnd = nativeHandle(window);
    if (!hwnd)
        return;

    // GWLP_HWNDPARENT 对顶层窗口就是"owner"；设成 0 就是摘下来。
    // 取 owner 的句柄前先用 handle() 问一句"它现在有没有原生窗口"，
    // **别用 winId() 直接取** —— 没有原生窗口时它会顺手去创建一个（踩过）。
    HWND ownerHandle = nullptr;
    if (owner && owner->handle())
        ownerHandle = reinterpret_cast<HWND>(owner->winId());

    ::SetWindowLongPtrW(hwnd, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerHandle));
    // 让系统按新关系重算一次（z 序、任务栏那一套）。
    // **别带 SWP_FRAMECHANGED** —— 那是"边框变了、客户区重算一次"，会把窗口
    // 的非客户区重新算一遍（实测从属窗口来回一次会四周各胖 16 像素）。改 owner
    // 跟边框没关系，不需要它。
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER);
#else
    Q_UNUSED(window);
    Q_UNUSED(owner);
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
