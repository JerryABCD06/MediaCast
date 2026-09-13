// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>
#include <QString>

class QQmlApplicationEngine;
class QQuickWindow;

// NewUiWindow —— 用 QML + FluentUI 搭的新界面。
//
// 现阶段的定位是**并存**：旧的 Widgets 界面照常工作，这个是旁路加进来的，
// 由托盘菜单手工打开。等它长齐了，旧界面才退场。
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
};
