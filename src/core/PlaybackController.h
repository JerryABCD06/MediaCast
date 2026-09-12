#pragma once

#include "MediaPlayer.h"
#include "NowPlaying.h"

#include <QObject>
#include <QString>
#include <QVector>

class QTimer;

/**
 * PlaybackController —— 协议无关的播放控制。
 *
 * ── 它解决什么问题 ────────────────────────────────────────────────────────
 *
 * 在这之前，"当前在放什么、处于什么状态、队列里有什么"这三本账是记在
 * `SoapHandler` 里的 —— 也就是记在 DLNA 协议层里。将来加 AirPlay 之类的协议时，
 * 那一层也得再记一遍同样三本账，两份账迟早对不上。
 *
 * 所以把这三本账抽出来放这儿，协议层只做"翻译"：
 *
 *     DLNA ──┐
 *     AirPlay┤ ──▶ PlaybackController ──▶ MediaPlayer ──▶ 后端（mpv）
 *     其它 ──┘
 *
 * ── 一条硬规矩：这个类不说任何协议的话 ────────────────────────────────────
 *
 * DLNA 的传输状态是一串规范定义好的字符串（`PLAYING` / `PAUSED_PLAYBACK` /
 * `TRANSITIONING` / `NO_MEDIA_PRESENT` …），播放模式也是（`NORMAL` /
 * `REPEAT_ALL` / …）。**这些词一个都不许出现在这个类里。**
 *
 * 它用自己的枚举说话（State / PlayMode），哪个协议用哪些字符串是那个协议自己的
 * 事 —— 各写各的映射。这样将来接一个状态词完全不同的协议时，改的是那个协议的
 * 翻译层，不是这里。
 *
 * ── 边界 ─────────────────────────────────────────────────────────────────
 *
 * 属于这里：状态机、加载看门狗、队列三格、当前媒体、命令入口、状态读取。
 * 不属于这里：XML、SOAP、SSDP、事件推送、DLNA 的字符串和数值换算。
 */
class PlaybackController : public QObject
{
    Q_OBJECT

    /**
     * 屏幕上是不是"空的"。
     *
     * 空 = 手上没内容，或者内容已经停住。**加载中、播放中、暂停中都不算空** ——
     * 那三种情况下用户看到的是同一条内容，画面该留着。
     *
     * 界面用它决定"显示画面还是显示投屏指引"。做成属性是因为 QML 读不了方法。
     */
    Q_PROPERTY(bool idle READ isIdle NOTIFY idleChanged)

    /**
     * 手上**装着内容**没有 —— 停着的也算。
     *
     * 和 idle 的区别值得说清楚：idle 问的是"屏幕上空不空"（停了也算空），
     * 这个问的是"那个会话还在不在"。关窗口之前要不要先警告，要看后者 ——
     * 停着的内容也是投送会话的一部分，关掉界面照样会把手机那边弄断，
     * 而用户完全看不出"我刚才那个操作把投送搞没了"。
     */
    Q_PROPERTY(bool hasMedia READ hasMedia NOTIFY hasMediaChanged)

    // 下面这几个是给界面用的：QML 读不了 C++ 的方法，只能读属性。
    // 通知信号直接用播放器转发上来的那几个（位置/时长/暂停），不另造。

    Q_PROPERTY(double position READ positionSeconds NOTIFY positionChanged)
    Q_PROPERTY(double duration READ durationSeconds NOTIFY durationChanged)
    Q_PROPERTY(bool paused READ isPaused NOTIFY pausedChanged)

public:
    // ── 两个中性枚举 ─────────────────────────────────────────────────────

    /** 播放状态。DLNA 那边各有各的字符串对应，见 SoapHandler 的映射。 */
    enum class State {
        /** 手上什么都没有。刚启动、或者投送已经结束。 */
        NoMedia,
        /** 装着内容，但停着 —— 随时能接着播。 */
        Stopped,
        /** 正在加载，还没放起来。 */
        Preparing,
        Playing,
        Paused,
    };
    Q_ENUM(State)

    /** 播放模式。 */
    enum class PlayMode {
        Normal,
        RepeatOne,
        RepeatAll,
        /** 只放这一条，不放队列里的下一条。 */
        Direct,
    };
    Q_ENUM(PlayMode)

    /** 上一次加载的结果。协议层要如实报给控制点，不能把失败说成"我自己停的"。 */
    enum class LoadStatus {
        Ok,
        Failed,
    };
    Q_ENUM(LoadStatus)

    /**
     * 一条要播的内容。
     *
     * 协议层负责把它那套元数据（DLNA 是 DIDL-Lite）翻成这几个字段 ——
     * 翻不出来的留空。**这个类不认识 DIDL**，它只认这几个字段。
     *
     * 关于 kind：拿不准就填 MediaKind::Unknown，控制器会从地址的扩展名去猜。
     * 标题同理 —— 空的、或者不像标题的，控制器有一整套回退规则（见 .cpp）。
     */
    struct MediaRequest
    {
        QString uri;
        QString metadata;      // 原样存着 —— 协议层要拿它回给控制点
        QString title;         // 协议层能拿到就填
        QString artist;
        QString album;
        MediaKind kind = MediaKind::Unknown;
    };

    /** 这条路是谁开的。只影响显示的副标题（"DLNA 投送" / "本地播放"）。 */
    using MediaSource = QString;

    explicit PlaybackController(MediaPlayer *player, QObject *parent = nullptr);

    // ── 命令 ─────────────────────────────────────────────────────────────
    //
    // 每种协议都翻译成这几个。**所有命令都必须从这里走** —— 直接命令播放器的话，
    // 状态机不知道，订阅过的控制点就收不到通知。

    /** 开一条新内容。换内容前会把当前这条记进历史，这样「上一首」退得回去。 */
    void openUri(const MediaRequest &request, const MediaSource &source);

    /** 只在装了内容时有效。没内容时什么也不做。 */
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();

    /**
     * 停下播放，但**会话还在** —— 内容仍然装着，随时能再按播放。
     * 控制点自己点「停止」走的就是这儿。
     */
    Q_INVOKABLE void stop();

    /**
     * 结束会话：内容、队列、播放模式全部归零。
     * 相当于从控制点那边"挂断"。
     */
    Q_INVOKABLE void endSession();

    Q_INVOKABLE void seekTo(double seconds);

    /** 0..100，和 DLNA RenderingControl 同一把尺子（这个换算是通用的，不是 DLNA 独有）。 */
    void setVolumePercent(int percent);
    void setMuted(bool muted);

    // ── 队列 ─────────────────────────────────────────────────────────────
    //
    // 只有三个位置：上一条、当前、下一条。够用，而且不会长成一个播放列表管理器
    // —— 那玩意儿一旦长出来，就得连"第几首""还剩几首"一起实现。

    /**
     * 把一条内容排在当前这条后面。uri 为空表示清空。
     *
     * 收的是整条 MediaRequest 而不是"地址 + 元数据"两个字符串 —— 因为协议层
     * 在**排队的这一刻**就把元数据解开了。只存原始字符串的话，过一会儿自动
     * 接上这一条时，这个类没法再解一遍（它不认识 DIDL），标题就只能退化成
     * 从文件名推 —— 那是看得见的退化。
     *
     * 队列三格记住的都是完整请求，所以「下一首」「上一首」「单曲循环」重播
     * 出来的标题和第一次放的时候一模一样。
     */
    void setNextUri(const MediaRequest &request);

    void next();
    void previous();

    bool hasNext() const { return !m_next.uri.isEmpty(); }
    bool hasPrevious() const { return !m_previous.uri.isEmpty(); }

    QString nextUri() const { return m_next.uri; }
    QString nextMetadata() const { return m_next.metadata; }

    void setPlayMode(PlayMode mode);
    PlayMode playMode() const { return m_playMode; }

    // ── 画面调节 ─────────────────────────────────────────────────────────
    //
    // 全部原样转发给播放器 —— 这里不做任何数值换算。DLNA 那套 0~100（50 才是
    // "原样"）是 DLNA 自己的规矩，换算写在协议层。

    QVector<PictureControlInfo> pictureControls() const;
    bool setPictureControl(const QString &name, int value);
    int  pictureControlValue(const QString &name) const;
    void resetPictureControls();

    // ── 状态读取（取缓存，立刻返回）──────────────────────────────────────

    State state() const { return m_state; }
    bool isIdle() const { return m_state == State::NoMedia || m_state == State::Stopped; }
    /** 装着内容没有（停着的也算）。见上面 hasMedia 那段。 */
    bool hasMedia() const { return m_state != State::NoMedia; }
    /** 界面读的"是不是暂停着"。就是状态机里那一个状态。 */
    bool isPaused() const { return m_state == State::Paused; }
    LoadStatus loadStatus() const { return m_loadStatus; }

    NowPlaying nowPlaying() const { return m_nowPlaying; }

    QString currentUri() const { return m_current.uri; }
    QString currentMetadata() const { return m_current.metadata; }

    double positionSeconds() const;
    double durationSeconds() const;
    int    volumePercent() const;
    bool   isMuted() const;

    /** 当前媒体的总字节数。拿不到返回 0 —— 按字节跳转靠它换算成时间。 */
    qint64 mediaSizeBytes() const;

    /**
     * 把画面画到某个窗口里。
     *
     * 严格说这是"画面输出"而不是"播放控制"，放在这个类里只是因为界面手上
     * 只有这一个门面。将来多协议的时候要是觉得别扭，可以往上挪一层。
     */
    void setVideoWindow(quintptr windowId);

signals:
    void logMessage(const QString &text);

    /** 状态变了。协议层拿它去翻成自己那套词。 */
    void stateChanged(State state);

    /** 上面那个 idle 变了。界面靠它切换"画面 / 投屏指引"。 */
    void idleChanged();

    /** 上面那个 hasMedia 变了（会话开始 / 结束）。 */
    void hasMediaChanged();

    /** "正在播放什么"变了（换片子，或者会话结束）。 */
    void nowPlayingChanged(const NowPlaying &info);

    /** 当前装的是哪个地址变了。 */
    void mediaChanged(const QString &uri, const QString &metadata);

    /**
     * 队列或播放模式变了。
     *
     * 一起报，是因为两个收件人各要一半：事件推送要把"下一条是什么、播放模式是什么"
     * 写进状态里，而界面和 Windows 媒体面板只关心前两个布尔值（那对「上一首/
     * 下一首」按钮亮不亮）。
     */
    void queueChanged(bool hasNext, bool hasPrevious,
                      const QString &nextUri, PlayMode playMode);

    /** 某个画面调节项的原始值变了（播放器的 -100~100，不是 DLNA 的 0~100）。 */
    void pictureControlChanged(const QString &name, int value);

    // ── 转发播放器的信号 ─────────────────────────────────────────────────
    //
    // 这几个是"播放器内部发生了什么"。转发出来，是为了让上层（DLNA、界面）
    // **只认这一个门面** —— 它们不该为了接个进度变化就去抓播放器。

    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void volumeChanged(int percent);
    void muteChanged(bool muted);
    void pausedChanged(bool paused);

    /** 播放引擎自身的状态文字（"正在启动"、"已就绪"之类），给界面显示用。 */
    void playerStatusChanged(const QString &text);

    /** 播放能力整个没了（进程退出、崩溃）。收到之后会话应当结束。 */
    void playerLost();

private:
    /** 真正开播的地方 —— openUri / next / previous / 自动接下一首都走它。 */
    void startPlaying(const MediaRequest &request, const MediaSource &source);

    /** 把当前这条记进历史。 */
    void pushCurrentIntoHistory();

    /** 当前这条是谁送来的（队列自动接上时要沿用同一个来源）。 */
    MediaSource sourceForCurrent() const;

    void emitQueueChanged();
    void setState(State state);

    /**
     * 把协议层给的字段补全成一条完整的 NowPlaying。
     *
     * 标题三层取值：协议层给的 → 从地址的文件名推 → 用类型名顶上。
     * 类型：协议层声明的 → 从扩展名推。
     */
    NowPlaying buildNowPlaying(const MediaRequest &request, const MediaSource &source);

    MediaPlayer *m_player = nullptr;

    /**
     * 「开始加载」之后等多久还没放起来就放弃。
     *
     * 这个看门狗是踩出来的：控制点发来的地址可能是它自己那台"临时媒体服务器"上的
     * 一条，而那种服务器往往只服务它当前认的那一条。我们从队列里跳回上一条时，
     * 那个地址已经取不到了 —— 对方既不回数据也不报错，播放器就那么干等着。
     *
     * 没有它的话，状态会永远停在 Preparing，控制点那边看到的是"一直正在准备"。
     */
    QTimer *m_loadWatchdog = nullptr;

    State m_state = State::NoMedia;
    LoadStatus m_loadStatus = LoadStatus::Ok;

    /// 当前这条。三格队列都存完整请求，理由见 setNextUri 上面那段。
    MediaRequest m_current;
    MediaRequest m_next;
    MediaRequest m_previous;

    PlayMode m_playMode = PlayMode::Normal;

    NowPlaying m_nowPlaying;
};
