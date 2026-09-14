// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "NewUiWindow.h"

#include "platform/windows/WindowFrame.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QUrl>

NewUiWindow::NewUiWindow(QObject *parent)
    : QObject(parent)
{
}

NewUiWindow::~NewUiWindow() = default;

bool NewUiWindow::isLoaded() const
{
    return m_window != nullptr;
}

bool NewUiWindow::load()
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
    // 日志里两条"新界面已打开"时间戳一模一样，同时冒出两行
    // `mpv: There is already a mpv_render_context set.` —— 两扇窗里各有一个
    // MpvQmlItem，而 mpv 的 render API 只允许一个渲染者。从托盘再开一个主窗口
    // 也是同一条路。
    //
    // 让路是对的：这一趟本来就要把那扇窗建出来，第二趟什么都不用做。
    if (m_loading) {
        // 留一行日志：这条路径平时不出现，一旦出现就是"投送和托盘撞在一起"的
        // 现场，排查时序问题时有它比没有强得多。
        emit logMessage(QStringLiteral("新界面正在打开，这次的请求并进去（不重复建窗）"));
        return false;
    }

    m_loading = true;
    // 不管从哪条 return 出去，都要把闸放开（下面的分支有好几个 return）。
    const auto loadingGuard = qScopeGuard([this] { m_loading = false; });

    // **引擎一辈子只建一个。** 窗口可以建了又销毁（用户关掉它），引擎不能 ——
    // 在这个进程里加载第二个 QQmlApplicationEngine 会崩在 Qt6Qml 里（踩过）。
    // 所以引擎建好之后只反复 load()，永远不销毁它重来。
    if (!m_engine) {
        m_engine = new QQmlApplicationEngine(this);

        // FluentUI 那个 QML 模块被 CMake 拷到了 exe 旁边的 qml/ 下。
        // 引擎默认不会往那儿找，得说一声。发布版也是同样的目录结构，
        // 所以这里不用按 Debug/Release 分支。
        m_engine->addImportPath(QCoreApplication::applicationDirPath() + QStringLiteral("/qml"));

        // QML 的报错默认只往 stderr 吐，界面和日志里都看不见 —— 出了事只能猜。
        // 接到日志上，"哪一行、什么东西找不到"一眼就能看见。
        connect(m_engine, &QQmlApplicationEngine::warnings, this,
                [this](const QList<QQmlError> &errors) {
            for (const QQmlError &error : errors)
                emit logMessage(QStringLiteral("QML 报错：%1").arg(error.toString()));
        });
    }

    m_engine->load(QUrl(QStringLiteral("qrc:/ui/NewUiWindow.qml")));

    // 取**最后**一个根对象：重新 load 出来的窗口是追加在后面的，前面那些
    // 是已经被销毁的旧窗口（引擎未必及时把它们从表里摘掉）。
    const QList<QObject *> roots = m_engine->rootObjects();
    if (roots.isEmpty()) {
        emit logMessage(QStringLiteral("新界面加载失败 —— 看上面的 QML 报错"));
        return false;
    }

    // 根对象得是个窗口，否则后面 show()/raise() 都是空谈。
    m_window = qobject_cast<QQuickWindow *>(roots.constLast());
    if (!m_window) {
        emit logMessage(QStringLiteral("新界面的根对象不是窗口，加载失败"));
        return false;
    }

    // ── 窗口被销毁时把指针清掉 ───────────────────────────────────────────
    //
    // **这一步不能省。** m_window 是个裸指针，窗口销毁之后它不会自己变空，
    // 下一次 show() 就会去碰已经释放的内存 —— 崩在 Qt6Gui 内部读一个非法
    // 地址，栈上什么线索都没有（踩过一次：Qt6Guid+0x34BA22）。
    //
    // 现在"关窗"是真的销毁（用户在关闭确认框里点了确定，或者当时没有投送），
    // 所以这条路径是常规路径，不是兜底。清掉指针之后，下一次 load() 会因为
    // m_window 为空而重新 load 出一个窗口。
    connect(m_window, &QObject::destroyed, this, [this] {
        m_window = nullptr;
        emit logMessage(QStringLiteral("新界面窗口已关闭（需要时会重新打开）"));
    });

    // FluentUI 建这个窗口的时候少设了两个样式位，补上。
    // 为什么在这儿补、补的是哪两个，见 WindowFrame.h 的注释。
    WindowFrame::ensureSnapFlags(m_window);

    return true;
}

void NewUiWindow::show()
{
    if (!load())
        return;

    // load() 里窗口没建出来的话已经 return 了，这里只是把话说死：
    // 宁愿什么都不做，也不要再去碰一个可能已经没了的窗口。
    if (!m_window)
        return;

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

    m_window->show();
    m_window->raise();
    m_window->requestActivate();

    emit logMessage(QStringLiteral("新界面已打开"));
}

void NewUiWindow::retranslate()
{
    // 界面还没建起来的话，等它建起来时本来就用的新语言，不用管。
    if (m_engine)
        m_engine->retranslate();
}

void NewUiWindow::endCasting()
{
    // 只把话传出去。具体怎么做（结束会话、推事件、让设备在网络里消失一下
    // 再回来）是协议层的事，main() 把这个信号接在 DlnaRenderer::endSession 上。
    emit castEndRequested();
}
