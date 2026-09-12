#pragma once

#include <QObject>
#include <QString>

class QAction;
class QMenu;
class QSystemTrayIcon;

class DlnaRenderer;

// TrayIcon —— 托盘图标和它的菜单。
//
// 这个程序的形态是"常驻后台的媒体接收器"：关掉窗口不等于退出，它还要继续在网络上
// 待着、继续接受投送。所以需要一个一直在那儿的东西，让用户能：
//
//   打开主界面      新的 QML 界面（正式的那套）
//   打开测试界面    旧的 Widgets 界面。它现在是调试/回归用的观测窗口，
//                   等新界面长齐了就连它一起删掉
//   暂停接收投送    暂时从网络上消失（"勿扰"），再点一次恢复
//   退出            真的退出
//
// 一个约定：
//
// **它只认识门面，而且不碰任何窗口。** 手上只有 DlnaRenderer —— "暂停接收"这种事
// 该怎么做是 DLNA 那一层的事；"打开界面"也不是它去操作窗口，而是发个信号，
// 由 main() 决定那个界面是谁、怎么开。所以它连一个 QWidget 都不持有。
class TrayIcon : public QObject
{
    Q_OBJECT

public:
    explicit TrayIcon(DlnaRenderer *renderer, QObject *parent = nullptr);
    ~TrayIcon() override;

    /** 这台机器上有没有系统托盘。没有的话图标是挂不上去的。 */
    bool isAvailable() const;

    /** 把图标亮出来。 */
    void show();

signals:
    void logMessage(const QString &text);

    /** 用户要打开正式界面（新的 QML 界面）。 */
    void openMainUiRequested();

    /** 用户要打开测试界面（旧的 Widgets 界面）。 */
    void openTestUiRequested();

private:
    /** 暂停 / 恢复接收投送。 */
    void toggleAccepting();

    /** 按当前状态把菜单项的文字对上。 */
    void refreshMenu();

    DlnaRenderer *m_renderer = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    /** 菜单没有窗口可以挂，所以由我们自己拿着、自己删。 */
    QMenu *m_menu = nullptr;

    QAction *m_openMainAction = nullptr;
    QAction *m_openTestAction = nullptr;
    QAction *m_acceptAction = nullptr;
};
