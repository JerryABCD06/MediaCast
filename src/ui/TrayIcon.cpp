// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "TrayIcon.h"

#include "protocols/dlna/DlnaRenderer.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>

TrayIcon::TrayIcon(DlnaRenderer *renderer, QObject *parent)
    : QObject(parent)
    , m_renderer(renderer)
{
    // 文字一律写键名 —— 真正的字在 lang/*.json 里，tr() 会去 Tr 那儿查。
    // 查不到就显示键名本身，那也是底（说明这个键漏了）。
    m_openMainAction = new QAction(tr("tray_open_main"), this);
    connect(m_openMainAction, &QAction::triggered, this, &TrayIcon::openMainUiRequested);

    m_openTestAction = new QAction(tr("tray_open_test"), this);
    connect(m_openTestAction, &QAction::triggered, this, &TrayIcon::openTestUiRequested);

    m_acceptAction = new QAction(tr("tray_pause"), this);
    connect(m_acceptAction, &QAction::triggered, this, &TrayIcon::toggleAccepting);

    m_quitAction = new QAction(tr("tray_quit"), this);
    connect(m_quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // 菜单没有窗口可以挂（托盘不持有任何窗口），所以由我们自己拿着、自己删。
    // QSystemTrayIcon 只是引用它，不接管所有权。
    m_menu = new QMenu();
    m_menu->addAction(m_openMainAction);
    m_menu->addAction(m_openTestAction);
    m_menu->addSeparator();
    m_menu->addAction(m_acceptAction);
    m_menu->addSeparator();
    m_menu->addAction(m_quitAction);

    m_tray = new QSystemTrayIcon(this);

    // 用程序自己的图标 —— main() 里已经设过一次了，这里不用再加载一遍。
    m_tray->setIcon(QApplication::windowIcon());
    m_tray->setToolTip(QStringLiteral("Media Cast"));
    m_tray->setContextMenu(m_menu);

    // 双击图标也把正式界面叫回来 —— 大家的习惯就是这样，不做反而别扭。
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
            emit openMainUiRequested();
    });

    // 菜单文字要跟着实际状态走，**不能只在构造时算一次**。
    //
    // 反例就是设置文件：`cast.newcast = false` 是在托盘建好之后（main() 收尾时）
    // 才把服务暂停的。不连这个信号的话，菜单会一直写着"暂停接收投送"，而用户
    // 点下去只会再暂停一次（空操作），得点两下才恢复。
    connect(m_renderer, &DlnaRenderer::acceptingChanged, this, &TrayIcon::refreshMenu);

    refreshMenu();
}

TrayIcon::~TrayIcon()
{
    // 菜单是个 QWidget，没有父窗口就不会有人替我们删它。
    delete m_menu;
}

bool TrayIcon::isAvailable() const
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayIcon::show()
{
    if (!m_tray)
        return;

    if (!isAvailable()) {
        // 没有托盘的机器（比如某些精简版系统）上图标挂了也不显示，说一声好排查。
        emit logMessage(QStringLiteral("这台机器上找不到系统托盘，托盘图标挂不上 —— "
                                       "关掉窗口之后只能用任务管理器退出"));
        return;
    }

    m_tray->show();
    emit logMessage(QStringLiteral("托盘图标已就绪（关掉窗口不会退出程序）"));
}

void TrayIcon::toggleAccepting()
{
    if (!m_renderer)
        return;

    if (m_renderer->isAccepting())
        m_renderer->pauseAccepting();
    else
        m_renderer->resumeAccepting();

    refreshMenu();
}

void TrayIcon::retranslate()
{
    // QMenu / QAction 拿到的是一句话，不是一个"会跟着翻译器变的东西" ——
    // 换语言不会自己重画，得有人把文字重新设一遍。main() 那边接到
    // UiState::languageChanged 就调这儿。
    m_openMainAction->setText(tr("tray_open_main"));
    m_openTestAction->setText(tr("tray_open_test"));
    m_quitAction->setText(tr("tray_quit"));

    // 这一句的文字跟状态走，refreshMenu 里一起管。
    refreshMenu();
}

void TrayIcon::refreshMenu()
{
    const bool accepting = m_renderer ? m_renderer->isAccepting() : false;

    // 不做成可勾选的：文字本身就说清楚了当前点下去会发生什么，不用再配个勾。
    //
    // 每次都重新取一遍 —— tr() 是即时查表的，不是构造时定死的，所以
    // "运行中切语言"这件事在这儿天然就对。
    m_acceptAction->setText(accepting ? tr("tray_pause") : tr("tray_resume"));
}
