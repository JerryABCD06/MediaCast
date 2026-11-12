// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>
#include <QPointer>
#include <QList>
#include <QString>
#include <QVariantMap>

class QQmlApplicationEngine;
class QQmlComponent;
class QQuickWindow;

// MainWindow —— 用 QML + FluentUI 搭的界面。**现在是唯一的界面**
// （旧的 Widgets 界面 2026-09-14 退场了）。
//
// 什么时候开：普通启动、手机投送、或者用户在托盘里点「打开主界面」。
// `--background` 启动不会主动打开；此时从托盘或投送进来才显示。
//
// 三条约定：
//
// 一、**引擎是懒创建的。** 没人打开主窗口就不建，省得程序一启动就多几百毫秒，
//     也省得 QML 里的错误拖累主程序启动。
//
// 二、**引擎只建一次，QML 文档只编译一次，窗口实例可以反复创建。** 用户关窗后
//     实例会被释放；下次打开时从同一个 QQmlComponent 新建。不要改成反复调用
//     QQmlApplicationEngine::load()：Qt6Qml 会留下悬空访问（2026-11-10 的
//     0xC0000005 转储里访问的指针是 MSVC 的 0xFEEEFEEE 释放填充值）。
//
// 三、**它不认识 DLNA，也不认识播放器。** QML 那两件事都走信号：要断投送就把
//     意思发出去（UiMain/PlaybackBridge 转给 Protocol），要读播放状态就读已经
//     注册给 QML 的 Playback。
class MainWindow : public QObject
{
    Q_OBJECT

public:
    explicit MainWindow(QObject *parent = nullptr);
    ~MainWindow() override;

    /** 第一次调用时建引擎、加载 QML；之后再调用就是把窗口叫到前面。 */
    void show();

    /** 界面是否已经真的建起来了。没建起来的话 show() 是空操作。 */
    bool isLoaded() const;

    /**
     * 「关掉窗口，顺便断开这次投送」—— 用户在关闭确认框里点了确定。
     *
     * 它自己不会断投送（那是协议层的事），只把意思发出去。窗口随后由 QML
     * 那边走 FluRouter.removeWindow() 真正销毁，下次再从同一个组件新建。
     */
    Q_INVOKABLE void endCasting();
    Q_INVOKABLE void pauseCastingForWindowClose();

    /**
     * 进 / 出全屏时要做的**窗口层**调整（QML 那边调不到 Win32 API）。
     *
     * 现在是两件：
     *   · 置顶（`WS_EX_TOPMOST`）—— 不全屏时任务栏会压在画面上；
     *   · 关掉 Win11 的窗口圆角 —— 不然四角露出桌面。
     *
     * **为什么不写在 QML 里**：置顶本来可以用 `Qt::WindowStaysOnTopHint`，但那
     * 会让 Qt 重建原生窗口，而本工程用的 OpenGL 后端在这一步会丢窗口表面（整窗
     * 全黑，见 MainWindow.qml 里全屏那一大段）。绕开窗口标志、直接改扩展样式
     * 就没有这个问题 —— 这是平台相关的活儿，收在 WindowFrame 里。
     */
    Q_INVOKABLE void setFullscreenWindowMode(bool on);

    /**
     * 把一个**子窗口**（比如"显示效果"）登记成主窗口的从属窗口。
     *
     * 以后再加子窗口，只要建的时候走 QML 那边那个 `createChildWindow(component)`
     * 就够了（它会先按 `transientParent` 建、再调这里登记）——**不用再动全屏那段
     * 逻辑**。登记之后，主窗口切全屏时这边会统一替它处理：
     *
     *   · 主窗口的原生窗口会被拆掉重建（全屏那条路，见 setFullscreenWindowMode），
     *     而 Win32 的规矩是"**销毁 owner 会连带销毁 owned 窗口**"——所以先把它
     *     从主窗口上摘开，重建完再挂到新的句柄上；
     *   · 那一下系统还会把它一并藏起来，owner 回来时它不会自己出现 —— 原来露着
     *     的替它显式再显示一次；
     *   · 它的原生窗口这一趟也换了，所以样式位、阴影、几何都替它补回去。
     */
    Q_INVOKABLE void adoptChildWindow(QObject *child);

    /** QML 确认窗口把用户选择交回来。 */
    Q_INVOKABLE void respondCastConfirmation(
        const QString &requestId, bool accepted);

public slots:
    void showCastConfirmation(
        qulonglong requestId, const QVariantMap &request);
    void cancelCastConfirmation(qulonglong requestId);

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
    void castPauseRequested();

    /** 本次 QML 主窗口实例已经销毁。Ui 进程用它决定何时退出。 */
    void windowClosed();

    /** 当前窗口实例完成了一次实际帧交换。 */
    void firstFrameReady();

    /** 这是 UI 自己的按钮结果，不是最终仲裁结果。 */
    void castConfirmationResponded(qulonglong requestId, bool accepted);

    /** 当前独立确认窗口已经销毁。 */
    void castConfirmationWindowClosed(qulonglong requestId);

private:
    bool ensureEngine();
    bool load();

    /**
     * 登记过的子窗口（从属窗口）。
     *
     * 用 `QPointer` 存：子窗口哪天被销毁了，这里自己变空，遍历时跳过就行 ——
     * 不用连着"谁关了我"的通知一起维护（这个工程在裸指针上栽过，见 docs/待办.md）。
     */
    QList<QPointer<QObject>> m_childWindows;

    QQmlApplicationEngine *m_engine = nullptr;
    QQmlComponent *m_component = nullptr;
    QQuickWindow *m_window = nullptr;
    QQmlComponent *m_castComponent = nullptr;
    QPointer<QObject> m_castWindow;
    qulonglong m_castRequestId = 0;
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
