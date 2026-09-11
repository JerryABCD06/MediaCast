#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "core/MediaPlayer.h"
#include "core/NowPlaying.h"

class GenaManager;
class HttpServer;
class SoapHandler;
class SsdpService;

/**
 * DlnaRenderer —— DLNA 这一整套对外只有这一个窗口。
 *
 * 它把四块东西包在里面：SSDP（被发现）、HTTP（把设备描述发出去）、SOAP（接受控制）、
 * GENA（把状态变化推回去），外加一个播放器。
 *
 * 为什么要包这一层，三条理由都是踩过坑才明白的：
 *
 * 一、内部连线不该由界面来管。"传输状态一变就推事件"、"音量一变就推事件"、
 * "播放器没了就结束会话"，这些是模块自己的事。散在界面代码里，改一处漏一处。
 *
 * 二、界面不该知道底下有几个对象。界面只需要知道"我要放这个地址""现在什么状态"，
 * 不需要知道背后是 SSDP 还是 GENA。
 *
 * 三、也是最要紧的一条：让界面绕不过状态机。之前连着三次出问题（停止、播放/暂停、
 * 载入地址），根因都是界面直接命令了播放器、跳过了状态机。现在界面手上只有这个门面，
 * 所有命令都必须从这里走 —— 那类错在结构上就不可能再犯。
 *
 * 所以连"读状态"也一并转发：界面读位置和音量也是从这里读。它不需要、也就不会去碰
 * 播放器。
 */
class DlnaRenderer : public QObject
{
    Q_OBJECT

public:
    explicit DlnaRenderer(MediaPlayer *player, QObject *parent = nullptr);
    ~DlnaRenderer() override;

    /** 起来干活：SSDP 广播 + HTTP 服务。返回 false 表示没能启动。 */
    bool start();
    void stop();
    bool isRunning() const { return m_running; }

    /**
     * 「勿扰」：结束当前投送，并从网络上消失。
     *
     * 具体做三件事：把当前投送结束掉（推事件，手机那边的投屏界面会收起来）、
     * 发一条 byebye、停掉 SSDP 和 HTTP。做完之后手机搜不到我们。
     *
     * 为什么要**先结束投送**：光让设备从网络上消失是不够的 —— 手机手上已经有我们的
     * 控制地址，照样能继续指挥播放。那种"半隐"状态比干脆说再见更让人糊涂。
     */
    void pauseAccepting();

    /** 从「勿扰」里出来：重新广播、重启服务，手机又能搜到我们。 */
    void resumeAccepting();

    /** 现在是不是在接收投送（没被勿扰）。 */
    bool isAccepting() const { return m_accepting; }

    // ── 控制 ─────────────────────────────────────────────────────────────
    // 这几个和远程控制点发来的命令走的是**同一套状态机**，从哪边操作都一样。

    /** 电脑上自己放一个新地址（和手机投过来走同一条状态机，只是来源不同）。 */
    void openUri(const QString &uri);

    void play();
    void pause();

    /** 停住，但会话保留 —— 控制点还能接着按播放。 */
    void stopTransport();

    /** 结束投送：状态归零、推事件，并让设备在网络里消失一下。 */
    void endSession();

    /** 「下一首」/「上一首」——和手机上按的是同一条路。 */
    void next();
    void previous();

    /** 队列前后有没有东西可去。界面和媒体面板靠它决定按钮亮不亮。 */
    bool hasNext() const;
    bool hasPrevious() const;

    void seekTo(double seconds);
    void setVolumePercent(int percent);
    void setMuted(bool muted);

    /** 让画面画到指定的窗口里。只有嵌入式播放器才用得上，现在是空操作。 */
    void setVideoWindow(quintptr windowId);

    // ── 画面调节 ─────────────────────────────────────────────────────────
    // 界面从这里读"有哪些项、当前值多少"，也从这里改 —— 它不需要认识播放器。

    QVector<PictureControlInfo> pictureControls() const;
    void setPictureControl(const QString &name, int value);
    int  pictureControlValue(const QString &name) const;
    void resetPictureControls();

    /**
     * 改 SSDP 广播间隔（毫秒）。
     *
     * 默认 10 秒一次。控制点可能收不到我们的搜索请求、只能靠听广播发现我们，所以
     * 默认就报得比较勤；接上正经路由器之后可以放宽到 60 秒，网络噪音会小很多。
     */
    void setAliveIntervalMs(int ms);

    // ── 状态 ─────────────────────────────────────────────────────────────

    QString deviceName() const;
    QString address() const;
    QString locationUrl() const;
    QString transportState() const;

    double positionSeconds() const;
    double durationSeconds() const;
    int    volumePercent() const;
    bool   isMuted() const;

    /** 当前有多少个控制点订阅了事件。 */
    int subscriptionCount() const;

signals:
    /** 一句话摘要：网卡、地址、HTTP 服务状态。 */
    void statusChanged(const QString &text);

    /** 过程日志，界面和日志文件都看着它。 */
    void logMessage(const QString &text);

    /** 播放引擎自身的状态文字（"正在启动 mpv"、"已就绪"之类）。 */
    void playerStatusChanged(const QString &text);

    void runningChanged(bool running);

    /** 「勿扰」开关变了。 */
    void acceptingChanged(bool accepting);

    /** 传输状态变化（STOPPED / PLAYING / ...）。 */
    void transportStateChanged(const QString &state);

    /** 队列前后有没有东西可去，变了。 */
    void queueChanged(bool hasNext, bool hasPrevious);

    /** 正在播放的内容变了。界面和 Windows 媒体面板都看着它。 */
    void nowPlayingChanged(const NowPlaying &info);

    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void volumeChanged(int percent);
    void muteChanged(bool muted);
    void pausedChanged(bool paused);

private:
    /** 把两个子模块的状态拼成一行，界面只需要认一个信号。 */
    void updateStatus();

    MediaPlayer *m_player = nullptr;
    SsdpService *m_ssdp = nullptr;
    HttpServer  *m_http = nullptr;
    SoapHandler *m_soap = nullptr;
    GenaManager *m_gena = nullptr;

    QString m_ssdpStatus;
    QString m_httpStatus;
    bool    m_running = false;

    /** 是不是在接收投送。托盘那个「暂停接收投送」就是它。 */
    bool    m_accepting = true;
};
