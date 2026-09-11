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
// 两条约定：
//
// 一、**引擎是懒创建的。** 没人打开新界面就不建，省得程序一启动就多几百毫秒，
//     也省得 QML 里的错误拖累主程序启动。
//
// 二、**它现在什么都不做。** 不碰播放器、不碰 DLNA —— main() 里会把播放控制
//     接进来，到时候它也只持有"播放控制"这一层，不认识 mpv。
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

private:
    bool load();

    QQmlApplicationEngine *m_engine = nullptr;
    QQuickWindow *m_window = nullptr;
};
