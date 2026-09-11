#pragma once

#include <QObject>
#include <QString>

class QAction;
class QSystemTrayIcon;
class QWidget;

class DlnaRenderer;

// TrayIcon —— 托盘图标和它的菜单。
//
// 这个程序的形态是"常驻后台的媒体接收器"：关掉窗口不等于退出，它还要继续在网络上
// 待着、继续接受投送。所以需要一个一直在那儿的东西，让用户能：
//
//   打开主界面      把窗口叫回来
//   暂停接收投送    暂时从网络上消失（"勿扰"），再点一次恢复
//   退出            真的退出
//
// 另外暂时多一项「打开新界面（实验）」—— 新的 QML 界面正在旁边长出来，
// 旧的 Widgets 界面还没退场，两个并存。等新界面长齐了，这一项会并进
// 「打开主界面」，这行注释也会一起删掉。
//
// 两个约定：
//
// 一、**它只认识门面。** 和窗口一样，手上只有 DlnaRenderer —— "暂停接收"这种事
//     该怎么做是 DLNA 那一层的事，托盘只管把用户的意思传过去。
//
// 二、**打开主界面暂时是直接操作窗口。** 这是本次的临时状态：以后换正式界面时，
//     这里会改成"请求打开"的信号，由新的界面自己决定怎么响应。
class TrayIcon : public QObject
{
    Q_OBJECT

public:
    TrayIcon(QWidget *window, DlnaRenderer *renderer, QObject *parent = nullptr);

    /** 这台机器上有没有系统托盘。没有的话图标是挂不上去的。 */
    bool isAvailable() const;

    /** 把图标亮出来。 */
    void show();

signals:
    void logMessage(const QString &text);

    /** 用户要打开新界面。托盘不自己建它 —— 那是 main() 的活儿。 */
    void openNewUiRequested();

private:
    /** 把窗口叫回来：显示、还原、抬到前面。 */
    void openWindow();

    /** 暂停 / 恢复接收投送。 */
    void toggleAccepting();

    /** 按当前状态把菜单项的文字对上。 */
    void refreshMenu();

    QWidget *m_window = nullptr;
    DlnaRenderer *m_renderer = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_openAction = nullptr;
    QAction *m_acceptAction = nullptr;
    QAction *m_newUiAction = nullptr;
};
