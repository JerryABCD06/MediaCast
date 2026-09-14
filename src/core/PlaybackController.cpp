// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "PlaybackController.h"

#include <QCoreApplication>
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

/**
 * 从文件自带的标签里按几个候选键名取值。**不分大小写。**
 *
 * 键名随容器变：同一个"标题"，FLAC 里存成 `title`、MKV 里存成 `TITLE`。
 * 所以候选键都列出来，逐个比小写 —— 一个都对不上就是空串，不报错。
 */
QString tagValue(const QVariantMap &tags, const QStringList &keys)
{
    for (const QString &key : keys) {
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
            if (it.key().compare(key, Qt::CaseInsensitive) == 0) {
                const QString value = it.value().toString().simplified();
                if (!value.isEmpty())
                    return value;
            }
        }
    }
    return QString();
}

/**
 * 歌词在标签里可能叫什么名字。
 *
 * 没有统一叫法：Vorbis 注释习惯写 `LYRICS`，ID3 那边是 USLT（不同程序又各自
 * 映射成 lyrics / unsyncedlyrics）。都试一遍，比只认一个强。
 */
const QStringList &lyricsTagKeys()
{
    static const QStringList keys = {
        QStringLiteral("lyrics"),
        QStringLiteral("unsyncedlyrics"),
        QStringLiteral("unsynchronisedlyrics"),
        QStringLiteral("syncedlyrics"),
        QStringLiteral("uslt"),
    };
    return keys;
}

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

        // **只在"还在准备"的时候才翻。**
        //
        // 加载要花时间（控制点的地址尤其慢），这中间用户或者控制点完全可能已经
        // 按了停止/暂停。那时候这一声"加载好了"就不该把对方的意思顶掉 ——
        // 实测表现是"手机上按了停止，电脑上立刻又开始放"（15 毫秒内翻回来）。
        //
        // 这一条同时管住了三种情况，不用各记各的标志：加载期间按了停止、
        // 按了暂停、以及看门狗等不下去先放弃了。
        if (m_state == State::Preparing)
            setState(State::Playing);
    });

    // 看门狗从"真正开始加载"那一刻开始跑，不是从调用 load() 那一刻。
    //
    // 差别在一件事上：render API 模式下，加载可能要等渲染面就绪才能发出去
    // （见 MpvCore 里那段），中间隔着"把界面拉起来"的时间。从 load() 开始算的话，
    // 界面起得慢一点就会被判成"加载失败"，而其实片子还没开始加载。
    connect(m_player, &MediaPlayer::loadStarted, this, [this] {
        m_loadWatchdog->start(kLoadTimeoutSeconds * 1000);
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

    // 文件自带的标签到了（或者换了）。**它是后到的** —— 投送一开始只有协议给的
    // 那份元数据，文件得等打开才知道里面写了什么。所以到这儿要重算一遍：
    // 标题、歌手可能是文件里才有的，歌词也只可能在这儿。
    connect(m_player, &MediaPlayer::metadataChanged, this,
            [this](const QVariantMap &tags) {
        if (m_fileTags == tags)
            return;

        m_fileTags = tags;

        // 把标签原样打进日志 —— "这首歌明明有歌词却没显示"这种问题，看一眼就知道
        // 是文件里没有、还是键名叫得不一样（键名随容器变，见下面 tagValue）。
        if (!tags.isEmpty()) {
            QStringList pairs;
            for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
                const QString value = it.value().toString();
                // 歌词可能很长，只报字数。
                pairs.append(value.size() > 60
                                 ? QStringLiteral("%1=（%2 字）").arg(it.key()).arg(value.size())
                                 : QStringLiteral("%1=%2").arg(it.key(), value));
            }
            emit logMessage(QStringLiteral("文件自带标签：%1")
                                .arg(pairs.join(QStringLiteral("；"))));
        }

        rebuildNowPlaying();
    });

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

    // 空/不空只在这几种状态之间跳变，所以要记下改之前的答案再比。
    const bool wasIdle = isIdle();
    // "装着东西没有"和"屏幕上空不空"不是一回事：Stopped 两个答案分别是
    // "装着"和"空"。所以也要单独记一份改之前的。
    const bool hadMedia = hasMedia();
    // 局面（谁连着 × 在放什么）只看"装没装内容"，所以这儿也得比一次。
    const CastState castBefore = castState();
    // 控制栏压在什么上，看的是"有没有画面露着"。
    const bool pictureBefore = showsPicture();

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

    if (isIdle() != wasIdle)
        emit idleChanged();

    if (hasMedia() != hadMedia)
        emit hasMediaChanged();

    if (castState() != castBefore)
        emit castStateChanged();

    if (showsPicture() != pictureBefore)
        emit showsPictureChanged();
}

void PlaybackController::setPeerConnected(bool connected)
{
    if (m_peerConnected == connected)
        return;

    const CastState castBefore = castState();

    m_peerConnected = connected;

    // ── 会话边界：播放列表作废 ──────────────────────────────────────────
    //
    //   断开 —— 对方那套"上一条 / 下一条"没意义了（那些地址多半在它自己那台
    //           临时媒体服务器上，会话一断就取不到）；
    //   连上 —— 新设备从零开始，不能接着上一台设备的列表。清完之后协议层再按
    //           自己的方式把列表填回来（DLNA 没有"查列表"这个动作，控制点要是
    //           有下一条，它会自己发 SetNextAVTransportURI）。
    //
    // **这一层只认"谁连着变了"。** 网络抖一下算不算断开，是协议层的事 ——
    // 那边已经留了宽限（见 DlnaRenderer::refreshPeerConnected 和 kPeerGraceMs），
    // 别把那种判断搬到这儿来，那会把协议特有的东西漏进来。
    clearPlaylist();

    emit peerConnectedChanged();

    if (castState() != castBefore)
        emit castStateChanged();
}

PlaybackController::CastState PlaybackController::castState() const
{
    // 规则就这十来行，**只此一份** —— 界面上不再各自去拼"连着没有 + 在放什么"。
    if (!m_peerConnected)
        return CastState::NoViewer;

    // 用 idle 而不是 !hasMedia：**停着的也算"没在放"**。
    //
    // 这两个判据的差别就在 Stopped 上：hasMedia 说"会话还在"，idle 说"屏幕上空了"。
    // 界面要的是后者 —— 片子自然播完之后，胶囊该回到"已连接"，而不是继续写着
    // "正在投屏"。
    //
    // **别把这一条和"关窗口要不要警告"混起来**：那一处看的是另一个概念 ——
    // "有没有投送方连着"（`peerConnected`）：关窗确认框（NewUiWindow.qml）和顶栏
    // 那个「断开连接」按钮的显隐**读的都是它**，三处问同一件事就写同一个属性。
    // （那两个地方早先分别写的是 hasMedia 和 `castState !== NoViewer`，都对、但
    // 都是绕着的说法，2026-09-14 统一了。）
    if (isIdle())
        return CastState::ViewerIdle;

    switch (m_nowPlaying.kind) {
    case MediaKind::Video:
        return CastState::ViewerVideo;
    case MediaKind::Audio:
        return CastState::ViewerAudio;
    case MediaKind::Image:
        return CastState::ViewerImage;
    case MediaKind::Unknown:
        break;
    }

    // 认不出类型时按视频算 —— 投屏这件事里绝大多数就是视频，而"按图片算"
    // 会让界面去等一张永远不会来的图。
    return CastState::ViewerVideo;
}

bool PlaybackController::showsPicture() const
{
    // 停着的时候容器上盖着投屏引导 —— 画面在底下，可用户看不见，
    // 所以"露着画面"不成立。这条和 idle 的定义是同一个口径，别改歪。
    if (isIdle())
        return false;

    return m_nowPlaying.kind == MediaKind::Video || m_nowPlaying.kind == MediaKind::Image;
}

bool PlaybackController::mediaIsVideo() const
{
    return m_nowPlaying.kind == MediaKind::Video;
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

void PlaybackController::togglePlayPause()
{
    // **手上什么都没有的时候按它不该有任何后果。**
    //
    // 以前碰不到这种调用：控制栏只在"装着内容"时才显示（界面那边是
    // `visible: Playback.hasMedia`）。现在那条栏**常显**（窗口一打开就在），
    // 没投送时也能按到它 —— 不挡住的话状态机会翻成 Playing，胶囊跟着写
    // "正在投送"，而 mpv 手上其实一个文件都没有。
    if (!hasMedia())
        return;

    // "停止"和"播完了"这两种情况下 paused 都是 false，但按下去该是**播放**
    // （播完了还会由播放器那边先回到 0，见 MpvCore::play）。所以判断的是
    // "现在屏幕上是不是在走"，不是 paused 取反。
    if (isIdle() || isPaused())
        play();
    else
        pause();
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
    // 只放新的，**两格队列都不动**：
    //   · "下一条"不动 —— 控制点排歌单就是"设当前、再设下一条"，在这里清掉
    //     等于把它的意图抹了；
    //   · "上一条"**不记** —— 早先这里会把刚才那条记成历史（"这样「上一首」
    //     退得回去"），那是我们替对方猜的。他定的规矩是：**只有协议明确说了
    //     前后有东西，那两个键才亮**（见 docs/待办.md 里"播放列表"那一节）。
    //     "上一条"只在真的用按键换曲时才有（next()/previous() 里那两行）。
    startPlaying(request, source);
}

// ── 队列 ─────────────────────────────────────────────────────────────────

MediaSource PlaybackController::sourceForCurrent() const
{
    // 当前这条是打哪儿来的，就照原样带回去（重放 / 上一首 / 下一首都用它）。
    return m_nowPlaying.source;
}

void PlaybackController::emitQueueChanged()
{
    emit queueChanged(hasNext(), hasPrevious(), m_next.uri, m_playMode);
}

void PlaybackController::clearPlaylist()
{
    if (m_next.uri.isEmpty() && m_previous.uri.isEmpty())
        return;   // 本来就是空的，不白报一次

    m_next = MediaRequest();
    m_previous = MediaRequest();

    emit logMessage(QStringLiteral("播放列表：清空（上一条 / 下一条都没了）"));
    emitQueueChanged();
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

    // **上一条的文件标签不能留到这一条。**
    //
    // 标签是后到的（文件打开了才有），所以换内容这一刻手上拿的还是上一条的 ——
    // 不清掉的话，新内容会先顶着上一条的标题/歌手显示一会儿，等新文件的标签
    // 到了才更正。本机文件是几十毫秒的事，局域网地址慢的时候那一会儿看得见。
    // （实测：放本地一首歌，界面先显示上一首的标题，20 毫秒后才更正。）
    m_fileTags.clear();

    // 换了内容就报一声。控制点靠这个知道渲染器现在装的是哪一条 ——
    // 少了它，我们这边的「上一首/下一首」在手机看来就像没发生过。
    emit mediaChanged(m_current.uri, m_current.metadata);

    const NowPlaying info = buildNowPlaying(request, source);

    emit logMessage(QStringLiteral("开始播放 %1").arg(request.uri));
    // 来源那句是**日志**，所以带上协议名（排查时有用），而且不翻译 ——
    // 界面上的来源是另一套，见 media_source_* 那几个键。
    //
    // 协议名**由协议层自己填**（MediaRequest::protocol），这儿一个字都不写死：
    // 加第二个协议时，这行日志不用改。
    const QString sourceText =
        source == MediaSource::Local
            ? QStringLiteral("本地播放")
            : (request.protocol.isEmpty()
                   ? QStringLiteral("投送")
                   : request.protocol + QStringLiteral(" 投送"));
    emit logMessage(QStringLiteral("   来源：%1    类型：%2")
                        .arg(sourceText,
                             mediaKindLabel(info.kind).isEmpty() ? QStringLiteral("未知")
                                                                 : mediaKindLabel(info.kind)));
    if (info.hasTitle())
        emit logMessage(QStringLiteral("   标题：%1").arg(info.title));

    // 局面（胶囊/容器显示什么）看的是"在放的是视频还是音乐还是图片"，
    // 所以换内容这一下也得比一次。
    const CastState castBefore = castState();
    const bool pictureBefore = showsPicture();

    m_nowPlaying = info;
    emit nowPlayingChanged(info);

    if (castState() != castBefore)
        emit castStateChanged();

    if (showsPicture() != pictureBefore)
        emit showsPictureChanged();

    // 先报"正在准备"，等播放器说文件好了再翻成"正在播放"。
    setState(State::Preparing);

    // 这条如果一直放不起来，看门狗会把它收掉。
    m_loadStatus = LoadStatus::Ok;

    // 看门狗不在这儿起 —— 由 loadStarted 信号起，理由见构造函数里那段。
    // 这里只把"现在要放这条"交代下去；加载可能被推迟（等渲染面就绪）。
    m_player->load(request.uri);
}

NowPlaying PlaybackController::buildNowPlaying(const MediaRequest &request,
                                               const MediaSource &source, bool quiet)
{
    NowPlaying info;
    info.source = source;

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
    // 协议给的"副标题"（视频/图片那一路用，音频不用它）。存成局部变量就够 ——
    // 算完落到 info.subtitle 里，界面上只要那一份。
    const QString description = request.description;

    // ── 协议没给的那几样，拿文件自带的标签补 ────────────────────────────
    //
    // **只补空着的**：协议层明确说了什么就以它为准（那是控制点眼里这条内容的
    // 名字），文件里的只在它没给的时候顶上。
    //
    // 这张标签表是后到的（见构造函数里那个 metadataChanged）：投送刚起来的时候
    // 它还是空的，等文件打开才会来，那时候会重算一遍。
    if (info.title.isEmpty()) {
        const QString fromFile = tagValue(m_fileTags, { QStringLiteral("title") });
        // 文件里的标题**也要过同一道筛子**。有些文件是被工具批量打过标签的，
        // title 里装的就是文件名本身（"Screenshot_20260905_232916"）——
        // 那种东西显示出来比"图片"两个字还糟，宁可往后退。
        if (looksLikeATitle(fromFile)) {
            info.title = fromFile;
        } else if (!fromFile.isEmpty() && !quiet) {
            emit logMessage(QStringLiteral("文件里的标题「%1」不像标题，跳过").arg(fromFile));
        }
    }
    if (info.artist.isEmpty())
        info.artist = tagValue(m_fileTags, { QStringLiteral("artist") });
    if (info.album.isEmpty())
        info.album = tagValue(m_fileTags, { QStringLiteral("album") });

    // 文件标签里的作者也可能是占位符（有些文件的标签就是随手写的）。
    if (looksLikePlaceholder(info.artist))
        info.artist.clear();

    // 歌词**只可能在文件里** —— DLNA 协议的元数据里根本没有这一项。
    info.lyrics = tagValue(m_fileTags, lyricsTagKeys());

    // ── 标题的回退 ──────────────────────────────────────────────────────
    // 协议层给的就是空的（或者它压根没有标题这个概念），那就从文件名推。
    // 推出来的也要过"像不像标题"那道筛子 —— 很多 App 拿内部编号当文件名。
    if (info.title.isEmpty()) {
        const QString guess = titleFromUri(request.uri);
        if (looksLikeATitle(guess)) {
            info.title = guess;
            if (!quiet)
                emit logMessage(QStringLiteral("没给标题，从文件名推出「%1」").arg(guess));
        } else if (!guess.isEmpty() && !quiet) {
            emit logMessage(QStringLiteral("文件名「%1」不像标题，跳过").arg(guess));
        }
    }

    // 还是没有，就用类型名顶上。标题空着在 Windows 媒体面板里会显示成"未知"，
    // 比一个中性的类型名还难懂 —— 至少"图片"这两个字说清了现在在放什么。
    //
    // **这里要的是译文，不是日志那种中文**：这个字会直接显示在界面上，
    // 所以走语言文件那一套（mediaKindKey -> 译文）。这也是切语言时得重算
    // 一遍的原因，见 retranslate()。
    if (info.title.isEmpty()) {
        const QString key = mediaKindKey(info.kind);
        if (!key.isEmpty())
            info.title = QCoreApplication::translate("NowPlaying", key.toUtf8().constData());
        if (!info.title.isEmpty() && !quiet)
            emit logMessage(QStringLiteral("没有可用标题，用类型名「%1」顶上").arg(info.title));
    }

    // ── 副标题：按内容类型分工 ──────────────────────────────────────────
    //
    // 音频：**歌手**（没有歌手退到专辑）。音频里没有"副标题"这个概念 ——
    // 所有主流播放器都是这么显示的，Windows 媒体面板的音乐卡片也是独立的
    // Artist / AlbumTitle 两个字段。
    //
    // 视频和图片：**副标题**（"这是什么"的那一句）。那儿不能拿"艺术家"顶 ——
    // 视频里的艺术家是演员/导演，含义完全不同，显示出来更让人困惑。
    //
    // 两个都拿不到就是「未知」。**故意不退到"投送/本地播放"**：那是"打哪儿来的"，
    // 不是"这是什么"，在主界面上写它等于废话（这儿本来就在投送）。Windows
    // 媒体面板那边会另外拼上来源 —— 那块面板是全局的，得分得清是谁在放。
    if (info.kind == MediaKind::Audio)
        info.subtitle = info.artist.isEmpty() ? info.album : info.artist;
    else
        info.subtitle = description;

    if (info.subtitle.isEmpty())
        info.subtitle = QCoreApplication::translate("NowPlaying", "media_unknown");

    return info;
}

void PlaybackController::rebuildNowPlaying()
{
    // 手上什么都没有（刚启动、或者投送已经结束）—— 没东西要重算。
    if (m_state == State::NoMedia || m_current.uri.isEmpty())
        return;

    // 拿同一条请求再算一遍。quiet：这不是"又开始放了一条"，别把标题那条日志
    // 再刷一次。
    m_nowPlaying = buildNowPlaying(m_current, m_nowPlaying.source, /*quiet=*/true);
    emit nowPlayingChanged(m_nowPlaying);
}

void PlaybackController::retranslate()
{
    // 切语言要重算的原因是：兜底那几句是要翻译的（类型名、「未知」）。
    // 文件标签到位时走的也是同一个重算，见构造函数里那段。
    rebuildNowPlaying();
}

void PlaybackController::endSession()
{
    // 「挂断」：相当于在电脑这一端主动结束这次投送。
    //
    // 这里归成 NoMedia 而不是 Stopped，区别很要紧：Stopped 的含义是"媒体还装着，
    // 只是停着"，控制点收到它会认为会话还在、只是没在播 —— 手机上的投屏界面就会
    // 一直挂着。NoMedia 才是"我这儿什么都没有了"。
    if (m_player)
        m_player->unload();            // 真的卸掉 —— 会话都断了，不该再留着上一条

    m_loadWatchdog->stop();            // 手都断了，别再等那条片子了
    m_loadStatus = LoadStatus::Ok;

    m_current = MediaRequest();
    emit mediaChanged(QString(), QString());   // 没内容了，也报一声

    // 文件标签跟着清掉 —— 手上都没内容了，留着上一条的标题没有任何意义。
    m_fileTags.clear();

    // 队列和播放模式一并归零。会话都断了还留着"下一条"没有任何意义，
    // 更要紧的是：下次投送时它会莫名其妙地自动接上。
    m_next = MediaRequest();
    m_previous = MediaRequest();
    m_playMode = PlayMode::Normal;
    emitQueueChanged();

    setState(State::NoMedia);

    // 投送结束了，界面上那块"正在播放"也该清掉。
    const CastState castBefore = castState();
    const bool pictureBefore = showsPicture();
    m_nowPlaying = NowPlaying();
    emit nowPlayingChanged(m_nowPlaying);

    if (castState() != castBefore)
        emit castStateChanged();

    if (showsPicture() != pictureBefore)
        emit showsPictureChanged();

    emit logMessage(QStringLiteral("已在电脑端结束投送"));
}

// ── 画面调节（原样转发）──────────────────────────────────────────────────

QVector<PictureControlInfo> PlaybackController::pictureControls() const
{
    return m_player ? m_player->pictureControls() : QVector<PictureControlInfo>();
}

QVariantList PlaybackController::pictureControlList() const
{
    QVariantList out;
    if (!m_player)
        return out;

    const QVector<PictureControlInfo> controls = m_player->pictureControls();
    out.reserve(controls.size());
    for (const PictureControlInfo &control : controls) {
        QVariantMap item;
        item.insert(QStringLiteral("name"), control.name);
        item.insert(QStringLiteral("min"), control.min);
        item.insert(QStringLiteral("max"), control.max);
        item.insert(QStringLiteral("neutral"), control.neutral);
        out.append(item);
    }
    return out;
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
