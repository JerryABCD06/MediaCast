// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "MainWindow.h"

#include "platform/windows/FullscreenWindowController.h"
#include "platform/windows/WindowFrame.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlError>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QUrl>

MainWindow::MainWindow(QObject *parent)
    : QObject(parent)
    , m_fullscreen(std::make_unique<FullscreenWindowController>())
{
}

MainWindow::~MainWindow() = default;

bool MainWindow::isLoaded() const
{
    return m_window != nullptr;
}

bool MainWindow::ensureEngine()
{
    if (m_engine)
        return true;
    m_engine = new QQmlApplicationEngine(this);
    m_engine->addImportPath(
        QCoreApplication::applicationDirPath() + QStringLiteral("/qml"));
    connect(m_engine, &QQmlApplicationEngine::warnings, this,
            [this](const QList<QQmlError> &errors) {
        for (const QQmlError &error : errors)
            emit logMessage(
                QStringLiteral("QML 报错：%1").arg(error.toString()));
    });
    return true;
}

bool MainWindow::load()
{
    // 窗口还在就什么都不用做。
    if (m_window)
        return true;

    // ── 防重入：正在建的时候，第二次进来直接让路 ────────────────────────
    //
    // `m_engine->load()` 不是"与世隔绝"的一步 —— 它建窗口、建场景图，期间
    // **会把事件循环转起来**（窗口创建本身要过一遍消息循环；投送那条路还会
    // 在渲染面就绪时"把攒着的那条片子放出去"，那又会发信号）。
    // 于是别的信号（`mediaChanged`、托盘那条）能在第一扇窗**还没登记到
    // m_window** 的时候调回 `show()` —— 两次都看到"窗口不在"，各建一扇。
    //
    // 实测就是这么来的（2026-09-14）：一次投送让 show() 被调两次，
    // 日志里两条"主窗口已打开"时间戳一模一样，同时冒出两行
    // `mpv: There is already a mpv_render_context set.` —— 两扇窗里各有一个
    // MpvQmlItem，而 mpv 的 render API 只允许一个渲染者。从托盘再开一个主窗口
    // 也是同一条路。
    //
    // 让路是对的：这一趟本来就要把那扇窗建出来，第二趟什么都不用做。
    if (m_loading) {
        // 留一行日志：这条路径平时不出现，一旦出现就是"投送和托盘撞在一起"的
        // 现场，排查时序问题时有它比没有强得多。
        emit logMessage(QStringLiteral("主窗口正在打开，这次的请求并进去（不重复建窗）"));
        return false;
    }

    m_loading = true;
    // 不管从哪条 return 出去，都要把闸放开（下面的分支有好几个 return）。
    const auto loadingGuard = qScopeGuard([this] { m_loading = false; });

    // **引擎只建一次。** 在这个进程里加载第二个 QQmlApplicationEngine 会崩在
    // Qt6Qml 里。QML 文档则由一个持久 QQmlComponent 只编译一次。
    if (!ensureEngine())
        return false;

    // 不要用 QQmlApplicationEngine::load() 反复建根对象。那条路会让 Qt6Qml
    // 在窗口销毁后继续访问旧对象；改用持久组件 create()，每次实例的生命周期
    // 边界清楚，而且关闭窗口能释放整棵 QML 树和它持有的图形资源。
    if (!m_component) {
        m_component = new QQmlComponent(
            m_engine, QUrl(QStringLiteral("qrc:/ui/MainWindow.qml")),
            QQmlComponent::PreferSynchronous, this);
    }
    if (m_component->isError()) {
        for (const QQmlError &error : m_component->errors())
            emit logMessage(QStringLiteral("QML 报错：%1").arg(error.toString()));
        emit logMessage(QStringLiteral("主窗口加载失败 —— 看上面的 QML 报错"));
        return false;
    }

    QObject *root = m_component->create();
    if (!root) {
        emit logMessage(QStringLiteral("主窗口加载失败 —— 组件没有创建出根对象"));
        return false;
    }

    m_window = qobject_cast<QQuickWindow *>(root);
    if (!m_window) {
        delete root;
        emit logMessage(QStringLiteral("主窗口的根对象不是窗口，加载失败"));
        return false;
    }

    // ── 窗口被销毁时把指针清掉 ───────────────────────────────────────────
    //
    // **这一步不能省。** m_window 是个裸指针，窗口销毁之后它不会自己变空，
    // 下一次 show() 就会去碰已经释放的内存 —— 崩在 Qt6Gui 内部读一个非法
    // 地址，栈上什么线索都没有（踩过一次：Qt6Guid+0x34BA22）。
    //
    // 正常关窗会通过 FluRouter 延迟销毁根对象，所以这里负责及时清掉裸指针。
    connect(m_window, &QObject::destroyed, this, [this] {
        m_window = nullptr;
        emit logMessage(QStringLiteral("主窗口已关闭（需要时会重新打开）"));
        emit windowClosed();
    });

    // FluentUI 建这个窗口的时候少设了两个样式位，补上。
    // 为什么在这儿补、补的是哪几个，见 WindowFrame.h 的注释。
    WindowFrame::reapplyFramelessStyle(m_window);

    return true;
}

void MainWindow::show()
{
    // 已经有一趟加载在跑：把这次请求并进去，等那一趟完成。
    if (m_loading) {
        emit logMessage(QStringLiteral("主窗口正在打开，这次的请求并进去（不重复建窗）"));
        return;
    }

    // 窗口已经露着就只是把它叫到前面。Windows 托盘一次双击可能同时送来
    // Trigger 和 DoubleClick，不去重的话会在这里把同一扇窗连续 show 两遍。
    if (m_window && m_window->isVisible()) {
        m_window->raise();
        m_window->requestActivate();
        return;
    }

    if (!load()) {
        return;
    }

    // load() 里窗口没建出来的话已经 return 了，这里只是把话说死：
    // 宁愿什么都不做，也不要再去碰一个可能已经没了的窗口。
    if (!m_window) {
        return;
    }

    // ── 先让它离屏渲染一帧，再露出来 ─────────────────────────────────────
    //
    // 窗口从"显示"到"画上第一帧"之间那段时间，Windows 合成的是窗口表面里
    // **已有的内容** —— 而那时候里面什么都没有（一次都没渲染过），于是那一段
    // 是**透明的**：用户看到窗口的边框和阴影先出来，里面还是后面那些窗口。
    //
    // 实测这段有一秒多（探针 work/cap-flash.ps1：窗口阴影出现之后，内部的颜色
    // 还要 1.4 秒才变）。这段主要是图形这一套的第一次初始化 —— 建渲染目标、
    // 现编着色器（Debug 构建下没有预编译着色器）。它躲不掉，但可以**挪到窗口
    // 露面之前**：grabWindow() 会在离屏渲染一帧，把这份开销先付掉。
    //
    // 图片本身不要，只是借它把第一帧逼出来。
    if (!m_window->isVisible())
        m_window->grabWindow();

    // 必须在 show() 之前接上：窗口首次显示时可能在同一轮事件处理中完成
    // 帧交换，晚一步连接会把这次唯一可信的首帧通知漏掉。
    connect(m_window, &QQuickWindow::frameSwapped, this,
            [this] { emit firstFrameReady(); },
            Qt::SingleShotConnection);

    m_window->show();
    m_window->raise();
    m_window->requestActivate();

    emit logMessage(QStringLiteral("主窗口已打开"));

}

void MainWindow::showCastConfirmation(
    qulonglong requestId, const QVariantMap &request)
{
    if (requestId == 0)
        return;
    const auto fail = [this, requestId] {
        emit castConfirmationResponded(requestId, false);
        if (!m_window)
            emit castConfirmationWindowClosed(requestId);
    };
    if (m_castWindow && m_castRequestId != 0)
        cancelCastConfirmation(m_castRequestId);
    if (!ensureEngine()) {
        emit logMessage(
            QStringLiteral("投送确认窗口：QML 引擎初始化失败"));
        fail();
        return;
    }
    if (!m_castComponent) {
        m_castComponent = new QQmlComponent(
            m_engine,
            QUrl(QStringLiteral("qrc:/ui/CastTakeoverWindow.qml")),
            QQmlComponent::PreferSynchronous, this);
    }
    if (m_castComponent->isError()) {
        for (const QQmlError &error : m_castComponent->errors())
            emit logMessage(
                QStringLiteral("QML 报错：%1").arg(error.toString()));
        emit logMessage(
            QStringLiteral("投送确认窗口加载失败 —— 看上面的 QML 报错"));
        fail();
        return;
    }

    const QVariantMap properties{
        {QStringLiteral("requestId"),
         QString::number(requestId)},
        {QStringLiteral("protocol"),
         request.value(QStringLiteral("protocol"))},
        {QStringLiteral("peerName"),
         request.value(QStringLiteral("peerName"))},
        {QStringLiteral("mediaTitle"),
         request.value(QStringLiteral("mediaTitle"))},
        {QStringLiteral("timeoutMs"),
         request.value(QStringLiteral("timeoutMs"))},
    };
    QObject *root = m_castComponent->createWithInitialProperties(properties);
    auto *window = qobject_cast<QQuickWindow *>(root);
    if (!window) {
        delete root;
        emit logMessage(
            QStringLiteral("投送确认窗口的根对象不是窗口"));
        fail();
        return;
    }

    m_castWindow = root;
    m_castRequestId = requestId;
    connect(window, &QObject::destroyed, this, [this, requestId] {
        const bool wasCurrent = m_castRequestId == requestId;
        if (wasCurrent) {
            m_castRequestId = 0;
            m_castWindow.clear();
            emit logMessage(QStringLiteral("投送确认窗口已关闭"));
            emit castConfirmationWindowClosed(requestId);
        }
    });
    window->show();
    window->raise();
    window->requestActivate();
    emit logMessage(QStringLiteral("投送确认窗口已打开"));
}

void MainWindow::cancelCastConfirmation(qulonglong requestId)
{
    if (requestId == 0 || requestId != m_castRequestId)
        return;
    if (!m_castWindow)
        return;
    if (!QMetaObject::invokeMethod(
            m_castWindow, "cancelFromShell", Qt::DirectConnection)
        && qobject_cast<QQuickWindow *>(m_castWindow.data())) {
        qobject_cast<QQuickWindow *>(m_castWindow.data())->close();
    }
}

void MainWindow::respondCastConfirmation(
    const QString &requestIdText, bool accepted)
{
    bool valid = false;
    const qulonglong requestId = requestIdText.toULongLong(&valid);
    if (!valid || requestId == 0 || requestId != m_castRequestId)
        return;
    emit logMessage(
        accepted
        ? QStringLiteral("投送确认：允许")
        : QStringLiteral("投送确认：继续当前投送"));
    emit castConfirmationResponded(requestId, accepted);
}

void MainWindow::retranslate()
{
    // 界面还没建起来的话，等它建起来时本来就用的新语言，不用管。
    if (m_engine)
        m_engine->retranslate();
}

void MainWindow::endCasting()
{
    // 只把话传出去。UiMain 会把它交给 PlaybackBridge，再沿 PlaybackPipe
    // 请求 Protocol 结束会话；窗口层不认识 DLNA。
    emit castEndRequested();
}

void MainWindow::pauseCastingForWindowClose()
{
    emit castPauseRequested();
}

void MainWindow::adoptChildWindow(QObject *child)
{
    if (!child)
        return;

    for (const QPointer<QObject> &entry : std::as_const(m_childWindows)) {
        if (entry == child)
            return;                 // 已经登记过
    }
    m_childWindows.append(QPointer<QObject>(child));

    // 从属关系本身是靠建窗口时的 `transientParent` 生效的（见 MainWindow.qml
    // 里 createChildWindow）。这里再显式设一次原生 owner，是为了让这条登记
    // **不管调用方怎么建窗口**都能成立。
    if (m_window) {
        if (auto *childWindow = qobject_cast<QWindow *>(child))
            WindowFrame::setWindowOwner(childWindow, m_window);
    }
}

bool MainWindow::setFullscreenWindowMode(bool on)
{
    if (!m_window || !m_fullscreen)
        return false;

    if (on == m_fullscreen->active())
        return true;

    if (on) {
        // Owned windows are always painted above their owner. Hide the picture
        // settings window instead of destroying or reparenting it; the same
        // QWindow objects can be shown again after fullscreen.
        m_fullscreenHiddenChildren.clear();
        for (const QPointer<QObject> &entry :
             std::as_const(m_childWindows)) {
            auto *child = qobject_cast<QWindow *>(entry.data());
            if (!child || !child->isVisible())
                continue;
            m_fullscreenHiddenChildren.append(child);
            child->hide();
        }

        if (!m_fullscreen->enter(m_window)) {
            for (const QPointer<QObject> &entry :
                 std::as_const(m_fullscreenHiddenChildren)) {
                if (auto *child = qobject_cast<QWindow *>(entry.data()))
                    child->show();
            }
            m_fullscreenHiddenChildren.clear();
            return false;
        }
        return true;
    }

    const bool exited = m_fullscreen->exit(m_window);
    for (const QPointer<QObject> &entry :
         std::as_const(m_fullscreenHiddenChildren)) {
        if (auto *child = qobject_cast<QWindow *>(entry.data()))
            child->show();
    }
    m_fullscreenHiddenChildren.clear();
    return exited;
}
