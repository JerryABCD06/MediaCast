// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>
#include <QString>

class QQmlApplicationEngine;
class QQuickWindow;

// NewUiWindow —— 用 QML + FluentUI 搭的界面。**现在是唯一的界面**
// （旧的 Widgets 界面 2026-09-14 退场了）。
//
// 什么时候开：有东西投过来（或者用户在托盘里点「打开主界面」）。启动时不开窗口 ——
// 这台设备平时的样子就是"在托盘里待着、等手机投过来"。
//
// 三条约定：
//
// 一、**引擎是懒创建的。** 没人打开新界面就不建，省得程序一启动就多几百毫秒，
//     也省得 QML 里的错误拖累主程序启动。
//
// 二、**引擎一辈子只建一个，窗口可以建了又销毁。** 用户在界面上把窗口关掉是
//     "真的关掉"（不是藏起来），关掉之后下次还能再开一个 —— 但**不能**为此
//     新建第二个 QQmlApplicationEngine，那会崩在 Qt6Qml 里。所以引擎建成之后
//     就只反复 load()。
//
// 三、**它不认识 DLNA，也不认识播放器。** QML 那两件事都走信号：要断投送就把
//     意思发出去（main() 接在 DlnaRenderer::endSession 上），要读播放状态就
//     读已经注册给 QML 的 Playback。
class NewUiWindow : public QObject
{
    Q_OBJECT

public:
    explicit NewUiWindow(QObject *parent = nullptr);
    ~NewUiWindow() override;

    /** 第一次调用时建引擎、加载 QML；之后再调用就是把窗口叫到前面。 */
    void show();

    /** 界面是否已经真的建起来了。没建起来的话 show() 是空操作。 */
    bool isLoaded() const;

    /**
     * 「关掉窗口，顺便断开这次投送」—— 用户在关闭确认框里点了确定。
     *
     * 它自己不会断投送（那是协议层的事），只把意思发出去。窗口的销毁由 QML
     * 那边走 FluRouter.removeWindow() 完成，和 FluentUI 自己关窗的方式一致。
     */
    Q_INVOKABLE void endCasting();

    /**
     * 进 / 出全屏时要做的**窗口层**调整（QML 那边调不到 Win32 API）。
     *
     * 现在是两件：
     *   · 置顶（`WS_EX_TOPMOST`）—— 不全屏时任务栏会压在画面上；
     *   · 关掉 Win11 的窗口圆角 —— 不然四角露出桌面。
     *
     * **为什么不写在 QML 里**：置顶本来可以用 `Qt::WindowStaysOnTopHint`，但那
     * 会让 Qt 重建原生窗口，而本工程用的 OpenGL 后端在这一步会丢窗口表面（整窗
     * 全黑，见 NewUiWindow.qml 里全屏那一大段）。绕开窗口标志、直接改扩展样式
     * 就没有这个问题 —— 这是平台相关的活儿，收在 WindowFrame 里。
     */
    Q_INVOKABLE void setFullscreenWindowMode(bool on);

public slots:
    /**
     * 语言变了。
     *
     * QML 里的 qsTr() 是**翻译期绑定**，语言变了它自己不会重算 —— 界面上
     * 那些字会一直停在旧语言，除非让引擎整个重来一遍。这件事只有持有引擎
     * 的这一方做得了，所以 UiState 只发信号，具体动作落在我们头上。
     */
    void retranslate();

signals:
    void logMessage(const QString &text);

    /** 用户要求结束当前投送（不是退出程序，也不是挂断接收）。 */
    void castEndRequested();

private:
    bool load();

    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;

    /**
     * 正在加载 QML 的那一小会儿。
     *
     * **它是防重入的闸**，不是"加载完了没有"的标志 —— `m_engine->load()` 里面
     * 会把事件循环转起来（建窗口、建场景图、把"攒着的那条片子"放出去），
     * 期间别的信号又调回 `show()` 的话，两次都会看到"窗口还没建好"（`m_window`
     * 还没赋值），于是**建出两扇窗**。
     *
     * 后果不只是多一个窗口：每扇窗里各有一个 `MpvQmlItem`，而 mpv 的 render API
     * **只允许一个渲染者** —— 日志里那两行 `There is already a mpv_render_context
     * set.` 就是它，画面会打架。2026-09-14 从托盘开出第二个主窗口就是这么来的。
     */
    bool m_loading = false;
};
