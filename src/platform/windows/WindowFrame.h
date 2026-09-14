// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

class QWindow;

// WindowFrame —— 窗口边框里那些"该由 Windows 自己管的事"。
//
// 和它的邻居 WindowsMediaControls 一样：平台相关的活儿都收在这个目录里，
// 上层（界面、协议、播放器）只调接口，不知道底下调的是哪个 Win32 API。
//
// 这里目前只有一件事。之所以单独开一个文件而不是塞进别处，是因为它属于
// "窗口外壳"这一类事情，和播放、和界面都没有关系 —— 将来要是还有窗口圆角、
// 阴影、DWM 属性之类的调整，都往这儿放。
namespace WindowFrame
{

/**
 * 把 FluentUI 无边框窗口少设的两个样式位补上。
 *
 * 为什么需要：FluentUI 的无边框助手在创建窗口时只往样式里加了
 * WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX，少了 WS_SYSMENU 和
 * WS_MINIMIZEBOX。这件事不用猜 —— 把它的窗口和一个普通 Qt 窗口并排量一下
 * 就看得出来：
 *
 *     FluentUI 无边框窗口   0x96C50000
 *     普通 Qt 窗口          0x96CF0000      差的正是这两个位
 *
 * 按业界那篇被反复引用的分析（Dmitriy Kubyshkin《Win32 Window Custom Title
 * Bar (Caption)》），WS_SYSMENU 是 Win+左/右 贴靠的前提、WS_MINIMIZEBOX 是
 * Win+下 的前提。
 *
 * **我们没有验证过缺了它到底会不会出问题** —— 沙箱里注入的 Win 键触发不了
 * shell 热键，连普通 Qt 窗口都不动，所以测不出结论。补上它只是让窗口更接近
 * 标准窗口，不会有坏处。
 *
 * 补在**我们自己的代码**里而不是改 third_party/FluentUI —— 那样 FluentUI
 * 升级时不会跟我们的改动打架。代价是每次创建这样的窗口都要调一次。
 */
void ensureSnapFlags(QWindow *window);

/**
 * 让窗口压在最上层（或取消）。
 *
 * **为什么不改 Qt 的窗口标志**（`Qt::WindowStaysOnTopHint`）：在 Windows 上改
 * 窗口标志会让 Qt **把原生窗口销毁重建**，而这个工程用的 OpenGL 后端在这一步会
 * 把窗口表面丢掉 —— 表现是整扇窗全黑（2026-09-14 查实：一个最朴素的 `qml.exe`
 * 窗口，只要图形后端是 OpenGL，全屏来回一次也黑；D3D11 就没事）。这里直接改
 * `WS_EX_TOPMOST`，只碰扩展样式，不重建窗口。
 */
void setTopMost(QWindow *window, bool onTop);

/**
 * Win11 窗口圆角开/关。
 *
 * "自己铺满"那种全屏下窗口还是个普通窗口，Win11 会给它留四个圆角 —— 那四个角
 * 会把桌面露出来。全屏期间关掉它。非 Win11 或属性设不上就静默忽略。
 */
void setRoundedCorners(QWindow *window, bool rounded);

}
