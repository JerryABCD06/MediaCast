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
 * 把"无边框窗口"该有的样式位打一遍。
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
 * 补在**我们自己的代码**里而不是改 third_party/FluentUI —— 那样 FluentUI 升级
 * 时不会跟我们的改动打架。全屏现在调整现有 HWND，不再借原生窗口重建来补样式。
 */
void reapplyFramelessStyle(QWindow *window);

/**
 * 全屏期间关掉 DWM 那条 1 像素窗口边框（退出时调回来）。
 *
 * 为什么：我们的"全屏"是一个**普通窗口**摆到整块屏上（不是 Win32 的
 * fullscreen/maximized），于是 Windows 11 照常给它画那圈窗口边框和阴影 ——
 * 它们正好落在屏幕最外圈，看着就是"四周漏一条缝"（量过：最外 1~2 像素是
 * 边框的白色 + 阴影的渐变）。真正的全屏/最大化窗口不会画这一圈（窗口矩形
 * 本来就探到屏幕外面去了）。
 *
 * 这里用 `DWMWA_BORDER_COLOR`（Win11 属性）把边框设成"不画"，阴影则用
 * `DwmExtendFrameIntoClientArea` 收回去。老系统上属性不存在，调用失败无所谓。
 */
void setFullscreenBorderless(QWindow *window, bool borderless);

/**
 * 设 / 清窗口的原生 owner（从属窗口关系）。
 *
 * Qt 那边对应的入口是 `QWindow::setTransientParent()`，但它只在窗口创建时
 * 生效一次。这里直接改原生 owner，用于登记和恢复已经创建的从属窗口。
 *
 * 直接改 `GWLP_HWNDPARENT` 就行，不重建窗口、不影响画面。
 */
void setWindowOwner(QWindow *window, QWindow *owner);

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
