#include "TrayIcon.h"

#include "protocols/dlna/DlnaRenderer.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QWidget>

TrayIcon::TrayIcon(QWidget *window, DlnaRenderer *renderer, QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_renderer(renderer)
{
    m_openAction = new QAction(tr("打开主界面"), this);
    connect(m_openAction, &QAction::triggered, this, &TrayIcon::openWindow);

    m_acceptAction = new QAction(tr("暂停接收投送"), this);
    connect(m_acceptAction, &QAction::triggered, this, &TrayIcon::toggleAccepting);

    // 临时项：新的 QML 界面还在旁边长，先留个手工入口。
    // 新界面长齐、旧界面退场之后，这一项就删掉。
    m_newUiAction = new QAction(tr("打开新界面（实验）"), this);
    connect(m_newUiAction, &QAction::triggered, this, &TrayIcon::openNewUiRequested);

    auto *quitAction = new QAction(tr("退出"), this);
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    // 菜单挂在窗口底下，由窗口负责销毁。QSystemTrayIcon 只是引用它，不接管所有权。
    auto *menu = new QMenu(m_window);
    menu->addAction(m_openAction);
    menu->addSeparator();
    menu->addAction(m_acceptAction);
    menu->addAction(m_newUiAction);
    menu->addSeparator();
    menu->addAction(quitAction);

    m_tray = new QSystemTrayIcon(this);

    // 用程序自己的图标 —— main() 里已经设过一次了，这里不用再加载一遍。
    m_tray->setIcon(QApplication::windowIcon());
    m_tray->setToolTip(QStringLiteral("Media Cast"));
    m_tray->setContextMenu(menu);

    // 双击图标也把窗口叫回来 —— 大家的习惯就是这样，不做反而别扭。
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
            openWindow();
    });

    // 菜单文字要跟着实际状态走，**不能只在构造时算一次**。
    //
    // 反例就是设置文件：`cast.newcast = false` 是在托盘建好之后（main() 收尾时）
    // 才把服务暂停的。不连这个信号的话，菜单会一直写着"暂停接收投送"，而用户
    // 点下去只会再暂停一次（空操作），得点两下才恢复。
    connect(m_renderer, &DlnaRenderer::acceptingChanged, this, &TrayIcon::refreshMenu);

    refreshMenu();
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

void TrayIcon::openWindow()
{
    if (!m_window)
        return;

    if (m_window->isMinimized())
        m_window->showNormal();
    else
        m_window->show();

    m_window->raise();
    m_window->activateWindow();
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

void TrayIcon::refreshMenu()
{
    const bool accepting = m_renderer ? m_renderer->isAccepting() : false;

    // 不做成可勾选的：文字本身就说清楚了当前点下去会发生什么，不用再配个勾。
    //
    // 这两个字符串每次状态变都要重新取一遍 —— tr() 是即时查表的，
    // 不是构造时定死的。以后要是支持"运行中切语言"，这里天然就对。
    m_acceptAction->setText(accepting ? tr("暂停接收投送")
                                      : tr("恢复接收投送"));
}
