// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "MpvCore.h"

/**
 * LibMpvPlayer —— MpvCore 的 **wid 外壳**。
 *
 * 它只做一件事：告诉 mpv"画面画到这个原生窗口里"。其余全在 MpvCore 里。
 *
 * 为什么不把这点差异也塞进 MpvCore：mpv 的 wid 选项和它的 render API 是两条
 * 完全不同的画面输出路径，而**除了"画面往哪出"，两者要的东西一模一样**。
 * 所以共用的四百多行待在 MpvCore，这里只留差异的那十几行。
 *
 * 谁在用：旧的 Widgets 界面（MainWindow 把画面区的窗口号传进来）。
 * 新的 QML 界面不用它 —— 那边走 render API。
 */
class LibMpvPlayer : public MpvCore
{
    Q_OBJECT

public:
    explicit LibMpvPlayer(QObject *parent = nullptr);
    ~LibMpvPlayer() override;

    /**
     * 把画面画到这个窗口里。
     *
     * 什么时候给都行，两条路都留着：
     *   start() 之前给 → 记下来，start() 里当启动选项设
     *   start() 之后给 → 直接改 mpv 的属性
     *
     * 之所以两条都要，是因为**窗口的创建时机不由我们决定**：窗口先建好就先给，
     * 后建好就后给。原来的代码就是这么用的，搬过来一行没改。
     */
    void setVideoWindow(quintptr windowId) override;

protected:
    /** 初始化之前把 wid 设上 —— 这个选项只能在 mpv_initialize() 之前设。 */
    void applyStartupOptions(mpv_handle *mpv) override;

private:
    /** 记住窗口号，等 start() 的时候用。 */
    quintptr m_windowId = 0;
};
