#include "PlaybackController.h"

#include <QTimer>

namespace {

/**
 * 「开始加载」之后等多久还没放起来就放弃。
 *
 * 这个看门狗是踩出来的：控制点发来的地址可能是它自己那台"临时媒体服务器"上的
 * 一条，而那种服务器往往只服务它当前认的那一条。我们从队列里跳回上一条时，
 * 那个地址已经取不到了 —— 对方既不回数据也不报错，播放器就那么干等着。
 *
 * 没有它的话，状态会永远停在 Preparing，控制点那边看到的是"一直正在准备"。
 * 规范里没有"渲染器可以无限期卡住"这一条。
 */
constexpr int kLoadTimeoutSeconds = 10;

} // namespace

PlaybackController::PlaybackController(MediaPlayer *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
{
    if (!m_player)
        return;

    m_loadWatchdog = new QTimer(this);
    m_loadWatchdog->setSingleShot(true);
    connect(m_loadWatchdog, &QTimer::timeout, this, [this] {
        if (m_state != State::Preparing)
            return;

        emit logMessage(QStringLiteral("等了 %1 秒也没放起来，放弃这一条 —— 十有八九是"
                                       "那个地址已经取不到了（控制点的临时媒体服务器"
                                       "往往只服务它当前认的那一条）")
                            .arg(kLoadTimeoutSeconds));

        // 播不出来要如实说。只把状态归成"停了"的话，控制点会以为是我们自己停的。
        m_loadStatus = LoadStatus::Failed;
        if (m_player)
            m_player->stop();   // 把那个挂着的请求丢掉
        setState(State::Stopped);
    });

    // 播放器说"文件好了"，才把状态从"正在准备"翻成"正在播放"。
    connect(m_player, &MediaPlayer::ready, this, [this] {
        m_loadWatchdog->stop();
        m_loadStatus = LoadStatus::Ok;
        setState(State::Playing);
    });

    // 一条内容播完之后干什么 —— 队列的"自动接上"和两种循环都在这里。
    connect(m_player, &MediaPlayer::ended, this, [this] {
        // 只在"正在播放"时才动手。加载新片子时，旧片子的结束事件也会来一次，
        // 那时候状态是 Preparing：既不能被打回去，也不该触发"接下一首"。
        if (m_state != State::Playing)
            return;

        // 单曲循环：重放这一条，队列原样留着。
        if (m_playMode == PlayMode::RepeatOne && !m_current.uri.isEmpty()) {
            emit logMessage(QStringLiteral("单曲循环，重放这一条"));
            startPlaying(m_current, sourceForCurrent());
            return;
        }

        // 排了下一条就自动接上 —— 这才是"排队列"这件事的意义所在。
        if (hasNext()) {
            emit logMessage(QStringLiteral("这一条放完了，自动接上队列里的下一条"));
            next();
            return;
        }

        // 全部循环，可手上就这一条：那就从头再来。
        if (m_playMode == PlayMode::RepeatAll && !m_current.uri.isEmpty()) {
            emit logMessage(QStringLiteral("全部循环：队列里只有这一条，重放"));
            startPlaying(m_current, sourceForCurrent());
            return;
        }

        // 没有下一条，也没有循环 —— 停在这儿。
        setState(State::Stopped);
    });

    // 画面值一变就往外报。这里连的是**播放器**的信号，所以不管是界面拖的、
    // 控制点设的、还是一口气全改的，都会走到 —— 只连自己那条路的话，
    // 电脑上拖一下亮度，手机那边就不会知道。
    connect(m_player, &MediaPlayer::pictureControlChanged,
            this, &PlaybackController::pictureControlChanged);

    // ── 转发播放器的信号 ────────────────────────────────────────────────
    // 上层只认这个门面，不该为了接个进度变化就去抓播放器。
    connect(m_player, &MediaPlayer::positionChanged,
            this, &PlaybackController::positionChanged);
    connect(m_player, &MediaPlayer::durationChanged,
            this, &PlaybackController::durationChanged);
    connect(m_player, &MediaPlayer::volumeChanged,
            this, &PlaybackController::volumeChanged);
    connect(m_player, &MediaPlayer::muteChanged,
            this, &PlaybackController::muteChanged);
    connect(m_player, &MediaPlayer::pausedChanged,
            this, &PlaybackController::pausedChanged);
    connect(m_player, &MediaPlayer::statusChanged,
            this, &PlaybackController::playerStatusChanged);

    // 播放器自己的日志（mpv 的警告之类）也要往外走 —— 上面那些转发把它漏了，
    // 会表现为"mpv 报了什么错，日志里查不到"。
    connect(m_player, &MediaPlayer::logMessage,
            this, &PlaybackController::logMessage);

    // 播放器整个没了（进程退出、崩溃）—— 这是状态机的事，不该让上层去记。
    // 结束会话，控制点那边才会把投屏界面收起来。
    connect(m_player, &MediaPlayer::lost, this, [this] {
        emit playerLost();
        endSession();
    });
}

// ── 状态出口 ─────────────────────────────────────────────────────────────

void PlaybackController::setState(State state)
{
    if (m_state == state)
        return;

    m_state = state;

    // 日志里那串英文是给排查用的，和协议无关 —— 谁都能看懂 PLAYING 是什么。
    static const auto nameOf = [](State s) {
        switch (s) {
        case State::NoMedia:   return "NO_MEDIA";
        case State::Stopped:   return "STOPPED";
        case State::Preparing: return "PREPARING";
        case State::Playing:   return "PLAYING";
        case State::Paused:    return "PAUSED";
        }
        return "?";
    };

    emit logMessage(QStringLiteral("传输状态 -> %1").arg(QLatin1String(nameOf(state))));
    emit stateChanged(state);
}

// ── 命令 ─────────────────────────────────────────────────────────────────

void PlaybackController::play()
{
    if (m_player)
        m_player->play();
    setState(State::Playing);
}

void PlaybackController::pause()
{
    if (m_player)
        m_player->pause();
    setState(State::Paused);
}

void PlaybackController::stop()
{
    if (m_player)
        m_player->stop();

    // 地址**不清**：媒体还装着，只是停了。控制点随时可以再按播放。
    setState(State::Stopped);
}

void PlaybackController::seekTo(double seconds)
{
    if (m_player)
        m_player->seekTo(seconds);
}

void PlaybackController::setVolumePercent(int percent)
{
    if (m_player)
        m_player->setVolumePercent(percent);
}

void PlaybackController::setMuted(bool muted)
{
    if (m_player)
        m_player->setMuted(muted);
}

void PlaybackController::openUri(const MediaRequest &request, const MediaSource &source)
{
    // 换内容之前先记一笔历史，这样「上一首」退得回去。
    //
    // 队列（下一条）**不动**：控制点排歌单就是"设当前、再设下一条"，
    // 在这里清掉等于把它的意图抹了。
    pushCurrentIntoHistory();
    startPlaying(request, source);
}

// ── 队列 ─────────────────────────────────────────────────────────────────

void PlaybackController::pushCurrentIntoHistory()
{
    if (m_current.uri.isEmpty())
        return;   // 还没放过东西，"刚才那条"不存在

    m_previous = m_current;
    emitQueueChanged();
}

PlaybackController::MediaSource PlaybackController::sourceForCurrent() const
{
    return m_nowPlaying.senderName.isEmpty() ? QStringLiteral("DLNA 投送")
                                             : m_nowPlaying.senderName;
}

void PlaybackController::emitQueueChanged()
{
    emit queueChanged(hasNext(), hasPrevious(), m_next.uri, m_playMode);
}

void PlaybackController::setNextUri(const MediaRequest &request)
{
    m_next = request;

    emit logMessage(request.uri.isEmpty()
                        ? QStringLiteral("队列：清空")
                        : QStringLiteral("队列：下一条是 %1").arg(request.uri));
    emitQueueChanged();
}

void PlaybackController::next()
{
    if (m_next.uri.isEmpty()) {
        emit logMessage(QStringLiteral("收到「下一首」，但队列里没有下一条 —— 不动"));
        return;
    }

    // 现在这条退到"上一条"的位置，这样按「上一首」还回得来。
    m_previous = m_current;

    const MediaRequest request = m_next;
    m_next = MediaRequest();

    emitQueueChanged();
    startPlaying(request, sourceForCurrent());
}

void PlaybackController::previous()
{
    if (m_previous.uri.isEmpty()) {
        emit logMessage(QStringLiteral("收到「上一首」，但没有上一条 —— 不动"));
        return;
    }

    // 当前这条退回队列，这样按「下一首」还回得来。
    m_next = m_current;

    const MediaRequest request = m_previous;
    m_previous = MediaRequest();

    emitQueueChanged();
    startPlaying(request, sourceForCurrent());
}

void PlaybackController::setPlayMode(PlayMode mode)
{
    if (m_playMode == mode)
        return;

    m_playMode = mode;

    static const auto nameOf = [](PlayMode m) {
        switch (m) {
        case PlayMode::Normal:    return "NORMAL";
        case PlayMode::RepeatOne: return "REPEAT_ONE";
        case PlayMode::RepeatAll: return "REPEAT_ALL";
        case PlayMode::Direct:    return "DIRECT";
        }
        return "?";
    };
    emit logMessage(QStringLiteral("播放模式 -> %1").arg(QLatin1String(nameOf(mode))));

    emitQueueChanged();
}

// ── 开播 ─────────────────────────────────────────────────────────────────

void PlaybackController::startPlaying(const MediaRequest &request, const MediaSource &source)
{
    if (!m_player || request.uri.isEmpty())
        return;

    m_current = request;

    // 换了内容就报一声。控制点靠这个知道渲染器现在装的是哪一条 ——
    // 少了它，我们这边的「上一首/下一首」在手机看来就像没发生过。
    emit mediaChanged(m_current.uri, m_current.metadata);

    const NowPlaying info = buildNowPlaying(request, source);

    emit logMessage(QStringLiteral("开始播放 %1").arg(request.uri));
    emit logMessage(QStringLiteral("   来源：%1    类型：%2")
                        .arg(source,
                             mediaKindLabel(info.kind).isEmpty() ? QStringLiteral("未知")
                                                                 : mediaKindLabel(info.kind)));
    if (info.hasTitle())
        emit logMessage(QStringLiteral("   标题：%1").arg(info.title));

    m_nowPlaying = info;
    emit nowPlayingChanged(info);

    // 先报"正在准备"，等播放器说文件好了再翻成"正在播放"。
    setState(State::Preparing);

    // 这条如果一直放不起来，看门狗会把它收掉。
    m_loadStatus = LoadStatus::Ok;
    m_loadWatchdog->start(kLoadTimeoutSeconds * 1000);
    m_player->load(request.uri);
}

NowPlaying PlaybackController::buildNowPlaying(const MediaRequest &request, const MediaSource &source)
{
    NowPlaying info;
    info.senderName = source;

    // ── 类型 ────────────────────────────────────────────────────────────
    // 协议层声明了就用它的；没声明就从地址的扩展名猜。
    //
    // 这不只是显示问题：Windows 媒体面板按类型决定卡片是音乐样式还是视频样式，
    // 一律报成音乐的话，投过来的图片在面板里显示得驴唇不对马嘴。
    info.kind = request.kind;
    if (info.kind == MediaKind::Unknown)
        info.kind = mediaKindFromUri(request.uri);

    info.title = request.title;
    info.artist = request.artist;
    info.album = request.album;

    // ── 标题的回退 ──────────────────────────────────────────────────────
    // 协议层给的就是空的（或者它压根没有标题这个概念），那就从文件名推。
    // 推出来的也要过"像不像标题"那道筛子 —— 很多 App 拿内部编号当文件名。
    if (info.title.isEmpty()) {
        const QString guess = titleFromUri(request.uri);
        if (looksLikeATitle(guess)) {
            info.title = guess;
            emit logMessage(QStringLiteral("没给标题，从文件名推出「%1」").arg(guess));
        } else if (!guess.isEmpty()) {
            emit logMessage(QStringLiteral("文件名「%1」不像标题，跳过").arg(guess));
        }
    }

    // 还是没有，就用类型名顶上。标题空着在 Windows 媒体面板里会显示成"未知"，
    // 比一个中性的类型名还难懂 —— 至少"图片"这两个字说清了现在在放什么。
    if (info.title.isEmpty()) {
        info.title = mediaKindLabel(info.kind);
        if (!info.title.isEmpty())
            emit logMessage(QStringLiteral("没有可用标题，用类型名「%1」顶上").arg(info.title));
    }

    return info;
}

void PlaybackController::endSession()
{
    // 「挂断」：相当于在电脑这一端主动结束这次投送。
    //
    // 这里归成 NoMedia 而不是 Stopped，区别很要紧：Stopped 的含义是"媒体还装着，
    // 只是停着"，控制点收到它会认为会话还在、只是没在播 —— 手机上的投屏界面就会
    // 一直挂着。NoMedia 才是"我这儿什么都没有了"。
    if (m_player)
        m_player->stop();              // 播放器回到空闲，下次投送不用重启

    m_loadWatchdog->stop();            // 手都断了，别再等那条片子了
    m_loadStatus = LoadStatus::Ok;

    m_current = MediaRequest();
    emit mediaChanged(QString(), QString());   // 没内容了，也报一声

    // 队列和播放模式一并归零。会话都断了还留着"下一条"没有任何意义，
    // 更要紧的是：下次投送时它会莫名其妙地自动接上。
    m_next = MediaRequest();
    m_previous = MediaRequest();
    m_playMode = PlayMode::Normal;
    emitQueueChanged();

    setState(State::NoMedia);

    // 投送结束了，界面上那块"正在播放"也该清掉。
    m_nowPlaying = NowPlaying();
    emit nowPlayingChanged(m_nowPlaying);

    emit logMessage(QStringLiteral("已在电脑端结束投送"));
}

// ── 画面调节（原样转发）──────────────────────────────────────────────────

QVector<PictureControlInfo> PlaybackController::pictureControls() const
{
    return m_player ? m_player->pictureControls() : QVector<PictureControlInfo>();
}

bool PlaybackController::setPictureControl(const QString &name, int value)
{
    return m_player ? m_player->setPictureControl(name, value) : false;
}

int PlaybackController::pictureControlValue(const QString &name) const
{
    return m_player ? m_player->pictureControlValue(name) : 0;
}

void PlaybackController::resetPictureControls()
{
    if (m_player)
        m_player->resetPictureControls();
}

// ── 状态读取 ─────────────────────────────────────────────────────────────

double PlaybackController::positionSeconds() const
{
    return m_player ? m_player->positionSeconds() : 0.0;
}

double PlaybackController::durationSeconds() const
{
    return m_player ? m_player->durationSeconds() : 0.0;
}

int PlaybackController::volumePercent() const
{
    return m_player ? m_player->volumePercent() : 0;
}

bool PlaybackController::isMuted() const
{
    return m_player ? m_player->isMuted() : false;
}

qint64 PlaybackController::mediaSizeBytes() const
{
    return m_player ? m_player->mediaSizeBytes() : 0;
}

void PlaybackController::setVideoWindow(quintptr windowId)
{
    if (m_player)
        m_player->setVideoWindow(windowId);
}
