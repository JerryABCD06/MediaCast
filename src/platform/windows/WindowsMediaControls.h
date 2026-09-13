#pragma once

#include <QObject>
#include <QString>
#include <QtGlobal>

#include "core/NowPlaying.h"

// WindowsMediaControls —— 把"这台电脑正在放什么"讲给 Windows 听。
//
// 讲给谁听？按音量键（或者点任务栏右下角的音量）弹出来的那个面板，系统里叫 SMTC。
// 接上之后：面板里能看到正在投送的标题，面板上的播放/暂停/停止按钮能指挥我们，
// 拖动面板里的进度条也能跳转。
//
// 三条约定，前两条和 DLNA 那边是同一个道理 —— 都是踩过坑才定下来的：
//
// 一、面板按钮不能直接命令播放器。面板上按了暂停，要和手机上按的暂停**走同一条路**
//     （也就是经过控制器，再到底下的播放器）。绕过去的话，状态机不知道状态变了，
//     订阅过的控制点收不到通知，手机上就会显示成另一副样子 —— 这个坑踩过三次。
//
// 二、**这个类不许认识任何具体协议，也不许认识播放器。** 它只发信号（用户按了什么）
//     和收数据（要显示什么），两头都由 main() 接线。
//
//     （早先这里写的是"手上只有 DlnaRenderer"—— 那是老版本的样子。现在它一个
//       协议对象都不持有：命令信号接到哪儿、状态从哪儿来，全在 main() 里。这样加
//       第二个协议时，面板不用改一行。）
//
// 三、头文件里不许出现 WinRT。C++/WinRT 的头文件又大又挑编译开关，让它传染到界面和
//     DLNA 那边不值得。所以实现全藏在 .cpp 的 Impl 里，这里只留一个指针。
//
// 另外还有一条不属于设计、属于现实的：按钮回调未必在界面线程上。回调里不直接干活，
// 先把动作甩回界面线程，再去动 Qt 的东西。

class WindowsMediaControls : public QObject
{
    Q_OBJECT

public:
    explicit WindowsMediaControls(QObject *parent = nullptr);
    ~WindowsMediaControls() override;

    /**
     * 挂到某个窗口上。必须在窗口显示之后调用 —— 窗口号要等控件真的变成系统里的一个
     * 窗口才拿得到。
     *
     * 返回 false 表示这台 Windows 上没接上（很老的系统，或者系统组件出了问题）。
     * 接不上不影响投屏，只是媒体面板里看不到。
     */
    bool attachToWindow(quintptr windowId);

    /** 摘掉会话，让程序从媒体面板里消失。 */
    void detach();

    bool isAttached() const { return m_attached; }

public slots:
    /** 正在放什么。界面上显示的和这里显示的必须是同一份数据。 */
    void setNowPlaying(const NowPlaying &info);

    /** DLNA 的传输状态（PLAYING / PAUSED_PLAYBACK / STOPPED / ...）。 */
    void setTransportState(const QString &state);

    /** 进度。位置和时长分两次给，内部自己记着，凑齐了再报给系统。 */
    void setPosition(double seconds);
    void setDuration(double seconds);

    /**
     * 队列前后有没有地方可去 —— 决定面板上那对「上一首/下一首」亮不亮。
     *
     * 没地方可去的时候就保持灰着。点得亮却按不动，比灰着更让人恼火，
     * 这条和 DLNA 那边往 GetCurrentTransportActions 里报什么是同一个道理。
     */
    void setQueueAvailability(bool hasNext, bool hasPrevious);

signals:
    /** 面板上按了播放。接到这个的人负责走门面，不要直接指挥播放器。 */
    void playRequested();
    void pauseRequested();
    void stopRequested();
    void nextRequested();
    void previousRequested();
    void seekRequested(double seconds);

    /** 接不上的时候说一声，好写进日志。 */
    void logMessage(const QString &text);

private:
    struct Impl;

    /** 把当前记着的标题/歌手/专辑推给系统。 */
    void pushDisplay();

    /** 把当前记着的进度推给系统。 */
    void pushTimeline();

    Impl *m_impl = nullptr;
    bool  m_attached = false;
};
