// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/**
 * SplashWindow —— 主界面加载期间显示的原生启动屏幕。
 *
 * 它故意不认识 Qt：
 *
 *   · 冷启动时可以在 QApplication 之前创建；
 *   · 窗口和动画在自己的线程里，QML 同步加载时不会被冻住；
 *   · 用 D3D11 + Direct2D + DirectComposition 绘制，窗口底色继续交给 DWM，
 *     图形栈取自 NoUninstallIcon（MIT）。
 *
 * 所有方法都可以从主线程调用。窗口操作会投递到绘图线程，调用本身不等 D3D
 * 初始化，因此不会把主线程卡住。
 */
class SplashWindow
{
public:
    enum class State {
        Shown,
        Hidden,
        Failed,
    };

    using StateCallback =
        std::function<void(std::uint64_t token, State state)>;

    explicit SplashWindow(StateCallback callback = {});
    ~SplashWindow();

    SplashWindow(const SplashWindow &) = delete;
    SplashWindow &operator=(const SplashWindow &) = delete;

    /** 显示启动屏幕。重复调用不会创建第二个窗口。 */
    /** Returns true only after the drawing thread has rendered and shown it. */
    bool show(std::uint64_t token = 0);

    /**
     * 设置实际主题。
     *
     * followSystem 为真时，启动屏幕在每次显示和收到 WM_SETTINGCHANGE 时会
     * 重新读取系统主题，避免主界面还没开始跑事件循环时拿到过时状态；
     * 强制浅/深时只使用 dark，完全忽略系统主题变化。
     */
    void setTheme(bool followSystem, bool dark);
    void setTitle(const std::wstring &title);

    /** 浅色/深色界面各自使用的强调色，0xRRGGBB。 */
    void setAccent(unsigned int lightRgb, unsigned int darkRgb);

    /** 主窗口露面时再压到它上面一次；启动屏幕始终是 topmost、不激活窗口。 */
    void bringToFront(std::uint64_t token = 0);

    /**
     * 主界面已经可以显示。绘图线程会等最短展示时间（280 ms）结束后，
     * 再用 150 ms 淡出。
     */
    void dismiss(std::uint64_t token = 0);

    /** 主界面加载失败或程序退出时立即收起。 */
    void hideImmediately(std::uint64_t token = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
