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
 * 把"无边框窗口"该有的样式位打一遍（创建时调一次，**原生窗口重建之后也要调**）。
 *
 * ── 为什么创建时要调 ────────────────────────────────────────────────────
 *
 * FluentUI 的无边框助手在创建窗口时只往样式里加了
 * WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX，少了 WS_SYSMENU 和
 * WS_MINIMIZEBOX。这件事不用猜 —— 把它的窗口和一个普通 Qt 窗口并排量一下
 * 就看得出来：
 *
 *     FluentUI 无边框窗口   0x96C50000
 *     普通 Qt 窗口          0x96CF0000      差的正是这两个位
 *
 * 按业界那篇被反复引用的分析（Dmitriy Kubyshkin《Win32 Window Custom Title
 * Bar (Caption)》），WS_SYSMENU 是 Win+左/右 贴靠的前提、WS_MINIMIZEBOX 是
 * Win+下 的前提。而 **WS_MAXIMIZEBOX 是"鼠标悬停在最大化键上弹出快速贴靠"的
 * 前提** —— 少了它，Windows 根本不给那套贴靠布局。
 *
 * ── 为什么重建之后还要再调一次 ──────────────────────────────────────────
 *
 * 库那几句 `SetWindowLongPtr` 只跑一次（在它自己的初始化里）。而这个工程的
 * "全屏"会在切换时**把原生窗口拆掉重建**（`QWindow::destroy()` + `show()`，
 * 见 NewUiWindow::setFullscreenWindowMode）—— 重建出来的窗口是 Qt 按窗口标志
 * 造的全新原生窗口，**库里那套样式位一个都没有**。症状就是他 2026-09-14 报的：
 * 退出全屏后**最大化键悬停不再出快速贴靠**，而且"顶栏上面莫名多出一小块"
 * （客户区的算法跟着变了：`isMaximized` 那条会给客户区留 8 像素，正常最大化的
 * 窗口因为窗口矩形探到屏幕外看不见，重建出来的却看得见）。
 *
 * 所以：**创建时调、原生窗口重建之后也调**。补在**我们自己的代码**里而不是改
 * third_party/FluentUI —— 那样 FluentUI 升级时不会跟我们的改动打架。
 */
void reapplyFramelessStyle(QWindow *window);

/**
 * 重新给窗口挂上 DWM 阴影。
 *
 * 和上一条一个道理：库在创建时用 `DwmExtendFrameIntoClientArea(hwnd, {1,0,0,0})`
 * 挂过一层（左边留 1 像素，DWM 才会给窗口阴影），原生窗口一重建就没了 ——
 * 新窗口看着就像"没影子"的一张贴纸。参数和库里那处保持一致。
 */
void reapplyDwmShadow(QWindow *window);

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
