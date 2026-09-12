#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QMultiMap>
#include <QObject>
#include <QString>

/**
 * GenaManager —— 事件订阅与推送（GENA）。
 *
 * 前面做的都是"控制点问、我们答"。GENA 反过来：控制点先留下一个回调地址订阅，
 * 之后我们这边状态一变就主动推给它。这样在电脑上停止播放时，手机上的投屏界面
 * 才知道该收起来 —— 否则手机只会一直显示"正在投屏"，直到用户自己关掉。
 *
 * 流程：
 *   SUBSCRIBE   → 记下回调地址、分配订阅号（SID），并**立刻推一条当前状态**
 *   状态变化     → 按 SID 推 NOTIFY，序号（SEQ）递增
 *   UNSUBSCRIBE → 删掉
 *
 * "立刻推一条"不是可选项：VLC 这类客户端在收到第一条事件之前会把设备当成不可用，
 * 压根不显示。
 *
 * 要提醒一句：能不能收到推送，取决于控制点有没有订阅。有些客户端（实测 B 站 App
 * 就是）从头到尾一个 SUBSCRIBE 都不发，也不轮询 —— 那种情况下我们没有任何办法
 * 让它知道状态变了，因为两边根本没建立过联系。
 */
class GenaManager : public QObject
{
    Q_OBJECT

public:
    explicit GenaManager(QObject *parent = nullptr);

    /** 处理 SUBSCRIBE，返回完整的 HTTP 应答（含状态行与头）。 */
    QByteArray handleSubscribe(const QString &service, const QMultiMap<QString, QString> &headers);

    /** 处理 UNSUBSCRIBE。 */
    QByteArray handleUnsubscribe(const QMultiMap<QString, QString> &headers);

    int subscriptionCount() const { return m_subscriptions.size(); }

    /** 传输状态变了：更新缓存并推给订阅了 AVTransport 的客户端。 */
    void pushTransportState(const QString &state);

    /**
     * 媒体总时长（秒）。只缓存，不单独推送 —— 之后每一条 AVTransport 事件都会
     * 带上它。控制点靠它画进度条、判断"现在到底有没有东西可放"。
     */
    void setMediaDuration(double seconds);

    /** 音量或静音变了。 */
    void pushRendering(int volume, bool muted);

    /**
     * 画面调节变了（DLNA 单位，0~100，50 = 原样）。
     *
     * 不给控制点报这个的话，电脑上拖一下亮度，手机那边的界面不会知道。
     */
    void pushPictureControls(int brightness, int contrast, int sharpness);

    /**
     * 队列或播放模式变了。
     *
     * 这两个也走 AVTransport 的 LastChange 推出去：
     *   NextAVTransportURI —— 控制点靠它确认"我排的下一条你收到了"
     *   CurrentPlayMode    —— 它那边的循环/随机按钮要照这个更新
     */
    void pushQueueState(bool hasNext, bool hasPrevious,
                        const QString &nextUri, const QString &playMode);

    /**
     * 现在装的是哪一条变了。
     *
     * LastChange 里本来就该有 AVTransportURI / CurrentTrackURI 这几项 —— 控制点靠它们
     * 知道渲染器换了内容。我们以前一条都没报，于是电脑上按「上一首」跳回上一条时，
     * 手机那边完全不知道，界面还显示着原来那条。
     */
    void pushMediaState(const QString &uri, const QString &metadata);

signals:
    void logMessage(const QString &text);

    /**
     * 订阅数变了（有人订阅 / 全都取消了 / 超时被清掉）。
     *
     * 界面靠它判断"有没有投送方连着" —— 这是**唯一可靠**的那个信号：
     * 控制点一旦开始投送就会订阅我们的状态，走了会取消订阅。只是搜到我们、
     * 看两眼设备描述那种不算连上。
     */
    void subscriptionCountChanged(int count);

private:
    struct Subscription {
        QString   service;        // AVTransport / RenderingControl / ConnectionManager
        QString   callbackUrl;    // 控制点留下的回调地址
        int       sequence = 0;   // SEQ，从 0 开始递增
        QDateTime expiresAt;
        /**
         * 连着推不出去几次了。
         *
         * 控制点的回调服务器是会消失的 —— App 退出了、崩了、或者干脆换了
         * 一个端口重新订阅。那些死地址如果我们一直捶下去，日志里全是
         * "连接被拒绝"，真正有用的那几条会被淹掉，也是白费连接。
         * 连着失败够多次就把它丢掉；推成功一次就清零。
         *
         * **注意"失败"的定义：连不上（或者连上之前就超时）才算。**
         * 连上了、NOTIFY 也写进去了，只是对方没回 200 —— 那不算失败。
         * 规范要求订阅者回一条 200，但现实里有的是闷头收、一声不吭的；
         * 把它当失败的话，会在第 5 条之后把一个**好好的**订阅扔掉，
         * 那种"越修越坏"的改动最要命。
         */
        int       failures = 0;

        /**
         * 这个订阅现在**有一条 NOTIFY 正在路上**。
         *
         * 这一条是关键。GENA 的规矩是订阅者按 SEQ **严格递增**来处理事件：
         * 它等 5 却先收到 6，就必须丢掉；而丢掉之后它的期待值还是 5，
         * 于是**后面每一条都对不上、全被丢掉** —— 手机那边的状态从此冻住，
         * 界面上播放/暂停再也不变（进度条倒是照常，因为那是它自己轮询
         * GetPositionInfo 问来的）。
         *
         * 而我们原来是每变一次状态就新开一条 TCP 连接，两条挨着发的时候
         * 谁先到就说不准了 —— 实测真的乱过（SEQ=6 比 SEQ=5 先到）。
         *
         * 所以：同一个订阅同一时刻只允许一条在途，后面的先攒着。
         */
        bool      busy = false;

        /** 在途那条发完之后，还要不要再发一条（按**那时候**的最新状态）。 */
        bool      resend = false;
    };

    void prune();
    void pushToService(const QString &service);
    void sendEvent(const QString &sid, const QByteArray &body);
    /** 这一次推成功 / 失败了。失败够多次就把订阅丢掉。 */
    void noteSendOk(const QString &sid);
    void noteSendFailed(const QString &sid);
    /** 在途的那条结束了（成功、失败、超时都算），该发的下一条在这儿发。 */
    void finishSend(const QString &sid);
    /** 订阅数变了就报一声。所有增删订阅的地方都要在末尾叫一次。 */
    void notifyCountIfChanged();
    QByteArray eventBodyFor(const QString &service) const;

    QHash<QString, Subscription> m_subscriptions;

    /** 上一次报出去的订阅数，用来"变了才报"。 */
    int m_lastReportedCount = 0;

    // 缓存最近一次的状态，这样新订阅者一来就能立刻收到一份"现在是什么样"。
    // 和 SoapHandler 那边保持一致：一上来是"什么都没有"，不是"停着"。
    QString m_lastTransportState = QStringLiteral("NO_MEDIA_PRESENT");
    int     m_lastVolume = 100;
    bool    m_lastMuted = false;
    int     m_brightness = 50;
    int     m_contrast = 50;
    int     m_sharpness = 50;

    // 队列那几项也要缓存，理由和上面一样：新订阅者一来就得收到一份完整的现状。
    bool    m_hasNext = false;
    bool    m_hasPrevious = false;
    /** 缓存的总时长，形如 "0:00:27"。见 setMediaDuration()。 */
    QString m_mediaDuration = QStringLiteral("0:00:00");
    QString m_nextUri;
    QString m_playMode = QStringLiteral("NORMAL");

    QString m_mediaUri;
    QString m_mediaMetadata;
    bool    m_hasMedia = false;
};
