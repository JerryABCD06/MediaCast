#pragma once

#include "core/NowPlaying.h"

#include <QObject>
#include <QString>

class MediaPlayer;
class QTimer;

/**
 * SoapHandler —— 把控制点发来的 SOAP 请求翻译成对播放器的命令。
 *
 * 前面几步做的是"让控制点找到我们、看清我们有哪些服务"。这一步做的是让它真的能
 * 指挥我们：点播放就播、点暂停就停、拖进度条就跳。
 *
 * 结构上这里只做三件事：解析 XML 取参数、调用播放器、拼一段 XML 回过去。
 * 至于"当前在播什么、处于什么状态"，那是 DLNA 自己的账本（transportState /
 * currentUri），不由 mpv 负责 —— mpv 只知道自己播没播，不知道怎么用 DLNA 的话说。
 */
class SoapHandler : public QObject
{
    Q_OBJECT

public:
    explicit SoapHandler(MediaPlayer *player, QObject *parent = nullptr);

    /**
     * 处理一次 SOAP 调用，返回完整的响应 XML（出错时是 SOAP Fault）。
     *
     * service 是 "AVTransport" / "RenderingControl" / "ConnectionManager"；
     * action 是动作名，来自 SOAPACTION 头；body 是请求体原文。
     */
    QString handle(const QString &service, const QString &action, const QString &body);

    /** 当前的 DLNA 传输状态，供事件推送（GENA）稍后使用。 */
    QString transportState() const { return m_transportState; }

    /** 当前正在播放的内容。界面和 Windows 媒体面板都读它。 */
    NowPlaying nowPlaying() const { return m_nowPlaying; }

    /**
     * 「停止」：停下播放，但**会话还在**。
     *
     * 按 UPnP 的规矩，停止之后媒体仍然装着 —— 控制点接下来还能直接按"播放"接着看。
     * 控制点自己点「停止」时发生的就是这件事。
     */
    void stopTransport();

    /**
     * 「播放」/「暂停」。和 SOAP 的 Play / Pause 是同一套状态机。
     *
     * 电脑上的按钮也必须走这里。直接叫播放器暂停的话，DLNA 这一层不知道状态变了，
     * 订阅过的控制点收不到通知，手机上就会一直显示"暂停中"—— 而进度条却还在动，
     * 因为位置是控制点主动来问的，不经过状态机。那种自相矛盾的画面看着很莫名其妙。
     */
    void play();
    void pause();

    /** 控制点指定一个地址开始播放（SOAP 的 SetAVTransportURI 走这里）。 */
    void openUri(const QString &uri, const QString &metadata = QString());

    /**
     * 电脑上自己放一个东西（界面上那个地址输入框）。
     *
     * 和 openUri 是**同一条路**：状态机、事件推送、断开会话全一样，只有"谁送来的"
     * 不一样。做成两个入口而不是加个 bool 参数，是因为副标题必须分得清
     * 「DLNA 投送」和「本地播放」—— 而这件事最容易被顺手糊过去。
     */
    void openLocalUri(const QString &uri);

    /**
     * 「断开投屏」：在电脑这一端主动结束当前投送 —— 相当于"挂断"。
     *
     * 电脑上的按钮必须走这里，**不要**直接叫播放器停。直接停播放器的话，DLNA 这一层
     * 根本不知道，状态还停在 PLAYING，订阅过的控制点什么都不会收到，手机上的投屏
     * 界面会一直挂着。
     */
    void endSession();

    // ── 队列（三件套）─────────────────────────────────────────────────────

    /**
     * SOAP 的 SetNextAVTransportURI：把一条内容排在当前这条后面。
     *
     * 控制点排歌单靠的就是它：设好当前这条，再把下一条排上，我们放完自动接上。
     * 传空地址表示清空队列。
     */
    void setNextUri(const QString &uri, const QString &metadata = QString());

    /** 「下一首」/「上一首」。没有可去的地方就什么也不做（并写日志）。 */
    void next();
    void previous();

    /** 有没有下一条、有没有上一条。界面和媒体面板靠它决定按钮亮不亮。 */
    bool hasNext() const { return !m_nextUri.isEmpty(); }
    bool hasPrevious() const { return !m_previousUri.isEmpty(); }

    /**
     * 播放模式：NORMAL / REPEAT_ONE / REPEAT_ALL / DIRECT_1 认，SHUFFLE 认不了。
     *
     * 返回 false 表示这个模式做不到，调用方应当回一个 SOAP 错误 ——
     * 收下了却按普通模式放，等于骗控制点。
     */
    bool setPlayMode(const QString &mode);
    QString playMode() const { return m_playMode; }

signals:
    void logMessage(const QString &text);
    void transportStateChanged(const QString &state);

    /** "正在播放什么"变了（换片子，或者投送结束）。 */
    void nowPlayingChanged(const NowPlaying &info);

    /**
     * 队列或播放模式变了。
     *
     * 四个值一起报，是因为两个收件人各要一半：GENA 要把 NextAVTransportURI 和
     * CurrentPlayMode 写进 LastChange 推给控制点，Windows 媒体面板只关心
     * 前两个（那对「上一首/下一首」按钮亮不亮）。
     */
    void queueChanged(bool hasNext, bool hasPrevious,
                      const QString &nextUri, const QString &playMode);

    /**
     * 当前装的是哪个地址变了。
     *
     * 这个信号只给 GENA 用：AVTransport 的 LastChange 里本来就该有
     * AVTransportURI / CurrentTrackURI，我们以前一条都没报。
     *
     * 少了它会出一件很别扭的事：**电脑上按「上一首」跳回上一条，手机那边完全不知道**
     * ——它的界面还显示着原来那条，下次一划就从它自己的位置继续发。
     * 报出去至少给了它一个跟上的机会。
     */
    void mediaChanged(const QString &uri, const QString &metadata);

    /**
     * 画面调节的三个标准值变了（DLNA 单位，0~100，50 = 原样）。
     *
     * 三个一起报，是因为 RenderingControl 的事件本来就是一份 LastChange 全量推。
     */
    void pictureControlsChanged(int brightness, int contrast, int sharpness);

private:
    /** openUri / openLocalUri 真正干活的地方 —— 两个入口共用这一份。 */
    void startPlaying(const QString &uri, const QString &metadata, const QString &senderName);

    /**
     * 换新内容之前，把"刚才那条"记进历史。
     *
     * 有了它，电脑上连着放几个文件之后「上一首」才退得回去 —— 否则那两个按钮
     * 只有等控制点排队时才会亮，平时一直是灰的，等于白做。
     */
    void pushCurrentIntoHistory();

    /** 当前这条是谁送来的（队列自动接上时要沿用同一个来源）。 */
    QString senderNameForCurrent() const;

    /** 队列相关的东西变了，统一从这里报出去。 */
    void emitQueueChanged();

    /**
     * 后端那个 -100~100 的值，换成 DLNA 的 0~100（**50 才是"原样"**）。
     *
     * 这道换算只许有一处，所以 GetXxx 和事件推送都走它。
     */
    int dlnaPictureValue(const char *control) const;

    QString handleAvTransport(const QString &action, const QString &body);
    QString handleRenderingControl(const QString &action, const QString &body);
    QString handleConnectionManager(const QString &action, const QString &body);

    /** 统一的状态出口，保证任何一个分支改状态都会发出通知。 */
    void setTransportState(const QString &state);

    MediaPlayer *m_player = nullptr;

    /**
     * 「开始加载」之后等多久还没放起来就放弃。
     *
     * 这个看门狗是踩出来的：控制点发来的地址可能是它自己那台"临时媒体服务器"上的
     * 一条，而那种服务器往往只服务它当前认的那一条。我们从队列里跳回上一条时，
     * 那个地址已经取不到了 —— 对方既不回数据也不报错，mpv 就那么干等着。
     *
     * 没有它的话，传输状态会永远停在 TRANSITIONING，控制点那边看到的是"一直正在准备"。
     * 规范里没有"渲染器可以无限期卡住"这一条。
     */
    QTimer *m_loadWatchdog = nullptr;

    /**
     * 一上来是 NO_MEDIA_PRESENT，不是 STOPPED。
     *
     * 这两个的差别很要紧：STOPPED 的意思是"媒体还装着，只是停着"。刚启动时我们手上
     * 什么都没有，报 STOPPED 等于跟控制点说"我这有片子"—— 它会因此认为 ConnectionManager
     * 里有一条连接、GetCurrentTransportActions 里 Play/Pause/Stop 都能用，全是假的。
     */
    QString m_transportState = QStringLiteral("NO_MEDIA_PRESENT");

    /**
     * 传输状态字（GetTransportInfo 里的 CurrentTransportStatus）。
     *
     * 只有两个值：OK 和 ERROR_OCCURRED。上面那个看门狗放弃时会把这里置成后者 ——
     * 播不出来要如实说，光把状态归成 STOPPED 会让控制点以为"我自己停的"。
     */
    QString m_transportStatus = QStringLiteral("OK");

    QString m_currentUri;
    QString m_currentMetadata;

    // 队列只有三个位置：上一条、当前、下一条。够用，而且不会长成一个播放列表
    // 管理器 —— 那玩意儿一旦长出来，就得连"第几首""还剩几首"一起实现。
    QString m_nextUri;
    QString m_nextMetadata;
    QString m_previousUri;
    QString m_previousMetadata;

    QString m_playMode = QStringLiteral("NORMAL");

    NowPlaying m_nowPlaying;
};
