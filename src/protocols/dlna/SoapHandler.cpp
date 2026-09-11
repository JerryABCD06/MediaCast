#include "SoapHandler.h"

#include "core/MediaPlayer.h"
#include "core/NowPlaying.h"
#include "UpnpXml.h"

#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <cmath>

namespace {

// ── 几个 XML 小工具 ────────────────────────────────────────────────────────

/** 取 <tag>...</tag> 中间的内容。取不到就返回空串。 */
QString tagValue(const QString &xml, const QString &tag)
{
    const QRegularExpression re(
        QStringLiteral("<%1[^>]*>([^<]*)</%1>").arg(QRegularExpression::escape(tag)));
    const QRegularExpressionMatch match = re.match(xml);
    return match.hasMatch() ? match.captured(1).trimmed() : QString();
}

/** 拼一个正常的 SOAP 响应。 */
QString soapOk(const QString &service, const QString &action, const QString &inner = QString())
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
               "<s:Body><u:%1Response xmlns:u=\"urn:schemas-upnp-org:service:%2:1\">%3"
               "</u:%1Response></s:Body>\n</s:Envelope>\n")
        .arg(action, service, inner);
}

// UPnP 标准错误码。用对了控制点才知道是"这个动作我没实现"还是"参数不对"。
constexpr int kInvalidAction = 401;
constexpr int kInvalidArgs   = 402;
constexpr int kActionFailed  = 501;
constexpr int kTransitionNotAvailable = 701;
// 701 在 RenderingControl 里的含义是"这个名字不认识"（同一个号，两套服务各叫各的）。
constexpr int kInvalidName = 701;
constexpr int kSeekModeNotSupported   = 710;
constexpr int kIllegalSeekTarget      = 711;
constexpr int kPlaySpeedNotSupported  = 717;
constexpr int kInvalidInstanceId      = 718;
constexpr int kInvalidConnectionReference = 706;

/** 加载超过这么久还没放起来，就认为这条取不到了。 */
constexpr int kLoadTimeoutSeconds = 10;

// DLNA 那几项画面调节统一是 0~100，而且 **50 才是"没调过"** —— 这是 UPnP 的惯例，
// 不是我们定的。我们后端那边是 -100~100、0 是中性。
//   往后端送： (dlna - 50) * 2
//   从后端读：  ours / 2 + 50
constexpr int kDlnaNeutral = 50;

QString soapFault(int code, const QString &text)
{
    return QStringLiteral(
               "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
               "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
               "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\n"
               "<s:Body><s:Fault>\n"
               "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>\n"
               "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">\n"
               "<errorCode>%1</errorCode><errorString>%2</errorString>\n"
               "</UPnPError></detail>\n"
               "</s:Fault></s:Body>\n</s:Envelope>\n")
        .arg(code)
        .arg(text);
}

/** UPnP 的时间格式是 H:MM:SS。 */
QString upnpTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        seconds = 0.0;

    const qint64 total = static_cast<qint64>(seconds);
    return QStringLiteral("%1:%2:%3")
        .arg(total / 3600)
        .arg((total % 3600) / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

/**
 * 解析 UPnP 的时间或位置。
 *
 * 这里不按字段个数分支，而是从左往右累加：每遇到一段就"乘 60 再加上"。
 * 这样 "0:01:30"、"01:30"、"90" 三种写法用同一段代码就都对。
 */
double parseUpnpTime(const QString &text)
{
    double seconds = 0.0;
    const QStringList parts = text.split(QLatin1Char(':'));
    for (const QString &part : parts) {
        bool ok = false;
        const double value = part.toDouble(&ok);
        seconds = seconds * 60.0 + (ok ? value : 0.0);
    }
    return seconds;
}

/**
 * SOAPACTION 头缺失时，从请求体里把动作名抠出来。
 *
 * Body 里的第一个元素就是动作名。控制点的写法不统一：有的带 u: 命名空间前缀，
 * 有的裸写。所以前缀要当可选项处理。
 */
QString actionFromBody(const QString &body)
{
    const QRegularExpression re(QStringLiteral("<(?:[A-Za-z0-9_]+:)?([A-Za-z][A-Za-z0-9_]*)[^>]*>"));
    QRegularExpressionMatchIterator it = re.globalMatch(body);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (name == QLatin1String("Envelope") || name == QLatin1String("Body"))
            continue;
        return name;
    }
    return QString();
}

/**
 * 控制点有没有填一个占位的"作者/专辑"。
 *
 * 这个不是洁癖。实测 vivo 相册投图片时，DIDL 里固定带一条
 * upnp:artist = "unkown"（它自己拼错了）。我们照单全收地显示出去，任务栏那个面板
 * 上就挂着一条"unkown"，看着像程序坏了 —— 而且那是**它的**拼写错误，替它背锅没道理。
 *
 * 认不出来就返回 false，也就是照常显示；宁可多显示一条可疑的值，也不要把真的
 * 歌手名字误伤掉，所以这张表只收最不可能撞车的几个。
 */
bool looksLikePlaceholder(const QString &text)
{
    static const QStringList junk = {
        QStringLiteral("unknown"), QStringLiteral("unkown"),
        QStringLiteral("none"),    QStringLiteral("null"),
        QStringLiteral("n/a"),     QStringLiteral("na"),
        QStringLiteral("-"),       QStringLiteral("--"),
        QStringLiteral("未知"),    QStringLiteral("未知艺术家"),
        QStringLiteral("未知歌手"), QStringLiteral("未知专辑"),
        QStringLiteral("无"),
    };
    // 占位值还有好几种包装，都得认：
    //
    //   unknown            直白型
    //   <unknown>          用尖括号包起来（BubbleUPnP 就是这个）
    //   &lt;unknown&gt;  连尖括号一起转义了 —— 元数据里的实体只解一层，
    //                      到我们手上就是这副样子（日志里实测到的）
    //
    // 所以：先按原样比一遍，再解一层转义比一遍，比之前把外层的括号/引号剥掉。
    const QStringList candidates = { text.trimmed(), UpnpXml::unescapeText(text.trimmed()) };
    for (QString candidate : candidates) {
        while (candidate.size() >= 2
               && ((candidate.startsWith(QLatin1Char('<')) && candidate.endsWith(QLatin1Char('>')))
                   || (candidate.startsWith(QLatin1Char('"'))
                       && candidate.endsWith(QLatin1Char('"'))))) {
            candidate = candidate.mid(1, candidate.size() - 2).trimmed();
        }
        if (junk.contains(candidate.toLower()))
            return true;
    }

    return false;
}

} // namespace

SoapHandler::SoapHandler(MediaPlayer *player, QObject *parent)
    : QObject(parent)
    , m_player(player)
{
    if (!m_player)
        return;

    // 加载看门狗：见头文件里那段说明，为什么非有它不可。
    m_loadWatchdog = new QTimer(this);
    m_loadWatchdog->setSingleShot(true);
    connect(m_loadWatchdog, &QTimer::timeout, this, [this] {
        if (m_transportState != QLatin1String("TRANSITIONING"))
            return;

        emit logMessage(QStringLiteral("等了 %1 秒也没放起来，放弃这一条 —— 十有八九是"
                                       "那个地址已经取不到了（控制点的临时媒体服务器"
                                       "往往只服务它当前认的那一条）")
                            .arg(kLoadTimeoutSeconds));

        // 播不出来要如实说。只把状态归成 STOPPED 的话，控制点会以为是我们自己停的。
        m_transportStatus = QStringLiteral("ERROR_OCCURRED");
        if (m_player)
            m_player->stop();   // 把那个挂着的请求丢掉
        setTransportState(QStringLiteral("STOPPED"));
    });

    // 播放器说"文件好了"，才把状态从"正在准备"翻成"正在播放"。
    // 这是 SetAVTransportURI 之后一直悬着的那一步。
    connect(m_player, &MediaPlayer::ready, this, [this] {
        m_loadWatchdog->stop();
        m_transportStatus = QStringLiteral("OK");
        setTransportState(QStringLiteral("PLAYING"));
    });

    // 一条内容播完之后干什么 —— 队列三件套里的"自动接上"就在这儿。
    connect(m_player, &MediaPlayer::ended, this, [this] {
        // 只在"正在播放"时才动手。加载新片子时，旧片子的结束事件也会来一次，
        // 那时候状态是 TRANSITIONING：既不能被打回去，也不该触发"接下一首"。
        if (m_transportState != QLatin1String("PLAYING"))
            return;

        // 单曲循环：重放这一条，队列原样留着。
        if (m_playMode == QLatin1String("REPEAT_ONE") && !m_currentUri.isEmpty()) {
            emit logMessage(QStringLiteral("单曲循环，重放这一条"));
            startPlaying(m_currentUri, m_currentMetadata, senderNameForCurrent());
            return;
        }

        // 排了下一首就自动接上 —— 这才是 SetNextAVTransportURI 的意义所在。
        if (hasNext()) {
            emit logMessage(QStringLiteral("这一条放完了，自动接上队列里的下一条"));
            next();
            return;
        }

        // 全部循环，可手上就这一条：那就从头再来。
        if (m_playMode == QLatin1String("REPEAT_ALL") && !m_currentUri.isEmpty()) {
            emit logMessage(QStringLiteral("全部循环：队列里只有这一条，重放"));
            startPlaying(m_currentUri, m_currentMetadata, senderNameForCurrent());
            return;
        }

        // 没有下一条，也没有循环 —— 停在这儿。
        setTransportState(QStringLiteral("STOPPED"));
    });

    // 画面值一变就报给订阅者。这里连的是**播放器**的信号，所以不管是界面拖的、
    // 控制点设的、还是「恢复出厂设置」一口气全改的，都会走到 —— 只连 SOAP 那条路
    // 的话，电脑上拖一下亮度，手机那边就不会知道。
    connect(m_player, &MediaPlayer::pictureControlChanged, this,
            [this](const QString &, int) {
        emit pictureControlsChanged(dlnaPictureValue("brightness"),
                                    dlnaPictureValue("contrast"),
                                    dlnaPictureValue("sharpen"));
    });
}

void SoapHandler::setTransportState(const QString &state)
{
    if (m_transportState == state)
        return;
    m_transportState = state;
    emit logMessage(QStringLiteral("传输状态 -> %1").arg(state));
    emit transportStateChanged(state);
}

void SoapHandler::stopTransport()
{
    if (m_player)
        m_player->stop();

    // 地址**不清**：媒体还装着，只是停了。控制点随时可以再按播放。
    setTransportState(QStringLiteral("STOPPED"));
}

void SoapHandler::play()
{
    if (m_player)
        m_player->play();
    setTransportState(QStringLiteral("PLAYING"));
}

void SoapHandler::pause()
{
    if (m_player)
        m_player->pause();
    setTransportState(QStringLiteral("PAUSED_PLAYBACK"));
}

void SoapHandler::openUri(const QString &uri, const QString &metadata)
{
    // 换内容之前先记一笔历史，这样「上一首」退得回去。
    //
    // 队列（Next）**不动**：控制点排歌单就是"SetAVTransportURI 设当前、
    // SetNextAVTransportURI 设下一条"，在这里清掉等于把它的意图抹了。
    pushCurrentIntoHistory();
    startPlaying(uri, metadata, QStringLiteral("DLNA 投送"));
}

void SoapHandler::openLocalUri(const QString &uri)
{
    // 本地播放没有 DIDL 元数据，标题只能从文件名推。
    pushCurrentIntoHistory();
    startPlaying(uri, QString(), QStringLiteral("本地播放"));
}

void SoapHandler::pushCurrentIntoHistory()
{
    if (m_currentUri.isEmpty())
        return;   // 还没放过东西，"刚才那条"不存在

    m_previousUri = m_currentUri;
    m_previousMetadata = m_currentMetadata;
    emitQueueChanged();
}

QString SoapHandler::senderNameForCurrent() const
{
    return m_nowPlaying.senderName.isEmpty() ? QStringLiteral("DLNA 投送")
                                             : m_nowPlaying.senderName;
}

void SoapHandler::emitQueueChanged()
{
    emit queueChanged(hasNext(), hasPrevious(), m_nextUri, m_playMode);
}

int SoapHandler::dlnaPictureValue(const char *control) const
{
    if (!m_player)
        return kDlnaNeutral;

    return m_player->pictureControlValue(QString::fromLatin1(control)) / 2 + kDlnaNeutral;
}

void SoapHandler::setNextUri(const QString &uri, const QString &metadata)
{
    m_nextUri = uri;
    m_nextMetadata = metadata;

    emit logMessage(uri.isEmpty() ? QStringLiteral("队列：清空")
                                  : QStringLiteral("队列：下一条是 %1").arg(uri));
    emitQueueChanged();
}

void SoapHandler::next()
{
    if (m_nextUri.isEmpty()) {
        emit logMessage(QStringLiteral("收到「下一首」，但队列里没有下一条 —— 不动"));
        return;
    }

    // 现在这条退到"上一条"的位置，这样按「上一首」还回得来。
    m_previousUri = m_currentUri;
    m_previousMetadata = m_currentMetadata;

    const QString uri = m_nextUri;
    const QString metadata = m_nextMetadata;
    m_nextUri.clear();
    m_nextMetadata.clear();

    emitQueueChanged();
    startPlaying(uri, metadata, senderNameForCurrent());
}

void SoapHandler::previous()
{
    if (m_previousUri.isEmpty()) {
        emit logMessage(QStringLiteral("收到「上一首」，但没有上一条 —— 不动"));
        return;
    }

    // 当前这条退回队列，这样按「下一首」还回得来。
    m_nextUri = m_currentUri;
    m_nextMetadata = m_currentMetadata;

    const QString uri = m_previousUri;
    const QString metadata = m_previousMetadata;
    m_previousUri.clear();
    m_previousMetadata.clear();

    emitQueueChanged();
    startPlaying(uri, metadata, senderNameForCurrent());
}

bool SoapHandler::setPlayMode(const QString &mode)
{
    static const QStringList supported = {
        QStringLiteral("NORMAL"),
        QStringLiteral("REPEAT_ONE"),
        QStringLiteral("REPEAT_ALL"),
        QStringLiteral("DIRECT_1"),
    };

    if (!supported.contains(mode)) {
        // SHUFFLE 会落到这儿。我们手上只有"上一条/当前/下一条"三个位置，没有一份
        // 列表可以打乱；收下它却照顺序放，等于骗控制点，不如直接说不支持。
        emit logMessage(QStringLiteral("控制点要的播放模式「%1」我们做不到").arg(mode));
        return false;
    }

    if (m_playMode != mode) {
        m_playMode = mode;
        emit logMessage(QStringLiteral("播放模式 -> %1").arg(mode));
    }
    return true;
}

void SoapHandler::startPlaying(const QString &uri, const QString &metadata, const QString &senderName)
{
    if (!m_player || uri.isEmpty())
        return;

    m_currentUri = uri;
    m_currentMetadata = metadata;

    // 换了内容就报一声。控制点靠这个知道渲染器现在装的是哪一条 ——
    // 少了它，我们这边的「上一首/下一首」在手机看来就像没发生过。
    emit mediaChanged(m_currentUri, m_currentMetadata);

    NowPlaying info;
    info.senderName = senderName;

    // ── 类型 ────────────────────────────────────────────────────────────
    // 优先信控制点声明的 upnp:class，它没有才看扩展名。
    //
    // 这不只是显示问题：Windows 媒体面板按类型决定卡片是音乐样式还是视频样式，
    // 一律报成音乐的话，投过来的图片在面板里显示得驴唇不对马嘴。
    const QString upnpClass = tagValue(metadata, QStringLiteral("upnp:class"));
    if (upnpClass.contains(QLatin1String("imageItem")))
        info.kind = MediaKind::Image;
    else if (upnpClass.contains(QLatin1String("audioItem")))
        info.kind = MediaKind::Audio;
    else if (upnpClass.contains(QLatin1String("videoItem")))
        info.kind = MediaKind::Video;
    else
        info.kind = mediaKindFromUri(uri);

    // ── 标题三层取值 ────────────────────────────────────────────────────
    //   一、控制点给的 DIDL 元数据（最准）—— 但它给的可能压根不是标题
    //   二、从媒体地址的文件名推 —— 同样要过"像不像标题"那道筛子
    //   三、还是空就用类型名顶上
    info.title = tagValue(metadata, QStringLiteral("dc:title"));

    // 第一层也要筛，这一条是被实际数据逼出来的：
    //
    // vivo 相册投图片时，dc:title 里塞的就是文件名本身 ——
    // "Screenshot_20260905_232916.jpg"，连扩展名都带着。那不是标题，是文件名搬运。
    // 而本地放同一个文件时我们推出的是"图片"。同一张图两个名字，说不通。
    //
    // 于是：带已知媒体扩展名的标题先去掉扩展名，再过一遍同一道筛子。过不了就当它
    // 没给标题，往下走第二层、第三层。
    if (!info.title.isEmpty()) {
        const QString cleaned = stripMediaExtension(info.title);
        if (looksLikeATitle(cleaned)) {
            info.title = cleaned;
        } else {
            emit logMessage(QStringLiteral("控制点给的标题「%1」不像标题，跳过").arg(info.title));
            info.title.clear();
        }
    }

    info.artist = tagValue(metadata, QStringLiteral("upnp:artist"));
    if (info.artist.isEmpty())
        info.artist = tagValue(metadata, QStringLiteral("dc:creator"));
    if (looksLikePlaceholder(info.artist)) {
        emit logMessage(QStringLiteral("控制点给的作者是占位符「%1」，忽略").arg(info.artist));
        info.artist.clear();
    }

    info.album = tagValue(metadata, QStringLiteral("upnp:album"));
    if (looksLikePlaceholder(info.album))
        info.album.clear();

    if (info.title.isEmpty()) {
        const QString guess = titleFromUri(uri);
        if (looksLikeATitle(guess)) {
            info.title = guess;
            emit logMessage(QStringLiteral("控制点没给标题，从文件名推出「%1」").arg(guess));
        } else if (!guess.isEmpty()) {
            // 很多 App 拿内部编号当文件名，那种推出来还不如不显示。
            emit logMessage(QStringLiteral("文件名「%1」不像标题，跳过").arg(guess));
        }
    }

    if (info.title.isEmpty()) {
        // 第三层。标题空着在 Windows 媒体面板里会显示成"未知"，比一个中性的类型名
        // 还难懂 —— 至少"图片"这两个字说清了现在在放什么。
        info.title = mediaKindLabel(info.kind);
        if (!info.title.isEmpty())
            emit logMessage(QStringLiteral("没有可用标题，用类型名「%1」顶上").arg(info.title));
    }

    emit logMessage(QStringLiteral("开始播放 %1").arg(uri));
    emit logMessage(QStringLiteral("   来源：%1    类型：%2")
                        .arg(senderName,
                             mediaKindLabel(info.kind).isEmpty() ? QStringLiteral("未知")
                                                                 : mediaKindLabel(info.kind)));
    if (info.hasTitle())
        emit logMessage(QStringLiteral("   标题：%1").arg(info.title));

    m_nowPlaying = info;
    emit nowPlayingChanged(info);

    // 先报 TRANSITIONING，等 mpv 说文件好了再翻成 PLAYING —— 和 SOAP 那条路完全一样。
    setTransportState(QStringLiteral("TRANSITIONING"));

    // 这条如果一直放不起来，看门狗会把它收掉。
    m_transportStatus = QStringLiteral("OK");
    m_loadWatchdog->start(kLoadTimeoutSeconds * 1000);
    m_player->load(uri);
}

void SoapHandler::endSession()
{
    // 电脑端的"断开投屏"。
    //
    // 这里报的是 NO_MEDIA_PRESENT 而不是 STOPPED，区别很要紧：STOPPED 的含义是
    // "媒体还装着，只是停着"，控制点收到它会认为会话还在、只是没在播 —— 手机上的
    // 投屏界面就会一直挂着。NO_MEDIA_PRESENT 才是"我这儿什么都没有了"。
    if (m_player)
        m_player->stop();              // 播放器回到空闲，下次投送不用重启

    m_loadWatchdog->stop();            // 手都断了，别再等那条片子了
    m_transportStatus = QStringLiteral("OK");

    m_currentUri.clear();
    m_currentMetadata.clear();
    emit mediaChanged(QString(), QString());   // 没内容了，也报一声

    // 队列和播放模式一并归零。会话都断了还留着"下一条"没有任何意义，
    // 更要紧的是：下次投送时它会莫名其妙地自动接上。
    m_nextUri.clear();
    m_nextMetadata.clear();
    m_previousUri.clear();
    m_previousMetadata.clear();
    m_playMode = QStringLiteral("NORMAL");
    emitQueueChanged();

    setTransportState(QStringLiteral("NO_MEDIA_PRESENT"));

    // 投送结束了，界面上那块"正在播放"也该清掉。
    m_nowPlaying = NowPlaying();
    emit nowPlayingChanged(m_nowPlaying);

    emit logMessage(QStringLiteral("已在电脑端结束投送"));
}

QString SoapHandler::handle(const QString &service, const QString &action, const QString &body)
{
    const QString act = action.isEmpty() ? actionFromBody(body) : action;

    if (act.isEmpty()) {
        emit logMessage(QStringLiteral("SOAP: 认不出动作名，回 401"));
        return soapFault(kInvalidAction, QStringLiteral("Invalid Action"));
    }

    // InstanceID 只管一道，所有动作都跑不掉 —— AVTransport 和 RenderingControl 都带它。
    //
    // 我们只有 0 号实例（就一个播放器）。规范里请求别的实例号应当回 718，而不是
    // 装作没看见 —— 装作没看见的话，控制点会以为它那"第二个播放器"真的存在，
    // 接下来一直往那儿发指令，两边就这么错下去了。
    const QString instance = tagValue(body, QStringLiteral("InstanceID"));
    if (!instance.isEmpty()) {
        bool ok = false;
        const int id = instance.toInt(&ok);
        if (!ok || id != 0) {
            emit logMessage(QStringLiteral("InstanceID「%1」不是 0：我们只有一个播放器，回 718")
                                .arg(instance));
            return soapFault(kInvalidInstanceId, QStringLiteral("Invalid InstanceID"));
        }
    }

    if (service == QLatin1String("AVTransport"))
        return handleAvTransport(act, body);
    if (service == QLatin1String("RenderingControl"))
        return handleRenderingControl(act, body);
    if (service == QLatin1String("ConnectionManager"))
        return handleConnectionManager(act, body);

    emit logMessage(QStringLiteral("SOAP: 不认识的服务 %1").arg(service));
    return soapFault(kInvalidAction, QStringLiteral("Invalid Action"));
}

QString SoapHandler::handleAvTransport(const QString &action, const QString &body)
{
    if (!m_player)
        return soapFault(kActionFailed, QStringLiteral("Action Failed"));

    if (action == QLatin1String("SetAVTransportURI")) {
        // DIDL-Lite 文档是**转义后**塞在 CurrentURIMetaData 里的，所以必须先解转义
        // 再去里面找 dc:title。不解这一步的话，标签在原文里长这样：
        // &lt;dc:title&gt;，任何标签匹配都找不到东西。
        const QString rawMeta = tagValue(body, QStringLiteral("CurrentURIMetaData"));
        const QString meta = rawMeta.isEmpty() ? QString() : UpnpXml::unescapeText(rawMeta);
        const QString uri = UpnpXml::unescapeText(tagValue(body, QStringLiteral("CurrentURI")));

        if (uri.isEmpty()) {
            emit logMessage(QStringLiteral("SetAVTransportURI 没带地址，回 402"));
            return soapFault(kInvalidArgs, QStringLiteral("Invalid Args"));
        }

        // 标题和元数据的处理都收在 openUri 里 —— 电脑端的播放按钮也走那条路，
        // 两边就不会各写一份、各漏一处。
        openUri(uri, meta);
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("SetNextAVTransportURI")) {
        // 和 SetAVTransportURI 一样，元数据也是转义后塞进来的，先解转义。
        const QString rawMeta = tagValue(body, QStringLiteral("NextURIMetaData"));
        const QString meta = rawMeta.isEmpty() ? QString() : UpnpXml::unescapeText(rawMeta);
        const QString uri = UpnpXml::unescapeText(tagValue(body, QStringLiteral("NextURI")));

        // 空地址是合法的，含义是"把队列清掉"——规范里就是这么规定的。
        setNextUri(uri, meta);
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    // 手上什么都没有的时候，播放/暂停/停止/跳转都不成立 —— 规范里这就是 701
    // （Transition not available）。
    //
    // 不拦的话会出一件很别扭的事：一个走错路的 Play（或者投送结束之后控制点补发的
    // Stop）会把状态从"我这儿什么都没有"改成"正在放/停着"，而播放器其实空着。
    // 控制点那边于是以为设备上有片子，界面上挂着一堆能按的按钮，按下去都没反应。
    if (m_transportState == QLatin1String("NO_MEDIA_PRESENT")
        && (action == QLatin1String("Play") || action == QLatin1String("Pause")
            || action == QLatin1String("Stop") || action == QLatin1String("Seek")
            || action == QLatin1String("Next") || action == QLatin1String("Previous"))) {
        emit logMessage(QStringLiteral("手上没内容，%1 不成立，回 701").arg(action));
        return soapFault(kTransitionNotAvailable, QStringLiteral("Transition not available"));
    }

    if (action == QLatin1String("Play")) {
        // 倍速只支持 1 倍。控制点要别的倍速时如实拒掉 —— 收下却按原速放，
        // 用户会以为倍速键坏了，还找不到原因。
        // 写法各家不一："1"、"1.0"、"1/1" 都算 1 倍速。
        const QString speed = tagValue(body, QStringLiteral("Speed")).trimmed();
        if (!speed.isEmpty() && speed != QLatin1String("1")
            && speed != QLatin1String("1.0") && speed != QLatin1String("1/1")) {
            emit logMessage(QStringLiteral("控制点要 %1 倍速，我们只放 1 倍速").arg(speed));
            return soapFault(kPlaySpeedNotSupported, QStringLiteral("Play speed not supported"));
        }
        play();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Next")) {
        next();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Previous")) {
        previous();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("SetPlayMode")) {
        const QString mode = tagValue(body, QStringLiteral("NewPlayMode")).trimmed().toUpper();
        if (!setPlayMode(mode)) {
            // 701 = Transition not available。告诉控制点"这个模式我做不到"，
            // 比收下来然后按普通模式放要诚实。
            return soapFault(kTransitionNotAvailable,
                             QStringLiteral("Transition not available"));
        }
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("GetTransportSettings")) {
        const QString inner =
            QStringLiteral("<PlayMode>%1</PlayMode>").arg(m_playMode) +
            // 我们不录音，时长报 0 —— 规范里 NOT_IMPLEMENTED 用在这儿是常见写法。
            QStringLiteral("<RecMediaDuration>00:00:00</RecMediaDuration>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("GetDeviceCapabilities")) {
        const QString inner =
            // 只能从网络拿内容，别的存储介质都没有。
            QStringLiteral("<PlayMedia>NETWORK</PlayMedia>") +
            QStringLiteral("<RecMedia>NOT_IMPLEMENTED</RecMedia>") +
            QStringLiteral("<RecQualityModes>NOT_IMPLEMENTED</RecQualityModes>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("Pause")) {
        pause();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Stop")) {
        // 控制点自己点的"停止"，和电脑上的「停止」按钮是同一件事。
        stopTransport();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Seek")) {
        const QString unit = tagValue(body, QStringLiteral("Unit")).trimmed().toUpper();
        const QString target = tagValue(body, QStringLiteral("Target")).trimmed();

        // 没写单位就按 REL_TIME 算 —— 它是最常用的那个。
        if (unit.isEmpty() || unit == QLatin1String("REL_TIME")) {
            const double seconds = parseUpnpTime(target);
            emit logMessage(QStringLiteral("Seek 到 %1 秒").arg(seconds));
            m_player->seekTo(seconds);
            return soapOk(QStringLiteral("AVTransport"), action);
        }

        if (unit == QLatin1String("TRACK_NR")) {
            // 我们只有一条轨道。按规范，跳到"第 1 条"就是回到开头 —— 这不是敷衍，
            // 单轨道设备本来就该这么答。
            bool ok = false;
            const int track = target.toInt(&ok);
            if (ok && track == 1) {
                emit logMessage(QStringLiteral("Seek 到第 1 条（只有一条，回到开头）"));
                m_player->seekTo(0.0);
                return soapOk(QStringLiteral("AVTransport"), action);
            }
            emit logMessage(QStringLiteral("要跳到第「%1」条，可我们只有一条").arg(target));
            return soapFault(kIllegalSeekTarget, QStringLiteral("Illegal seek target"));
        }

        if (unit == QLatin1String("X_DLNA_REL_BYTE")) {
            // DLNA 加的单位：控制点给的是**字节位置**。我们手上只有时间轴，所以按
            // "总字节数 + 总时长"按比例换算。定码率的片子足够准；变码率的会偏一些，
            // 但我们确实没法知道"第 N 个字节"对应哪一帧 —— 这是能做到的最好程度。
            bool ok = false;
            const qint64 targetByte = target.toLongLong(&ok);
            const qint64 totalBytes = m_player->mediaSizeBytes();
            const double totalSeconds = m_player->durationSeconds();

            if (!ok || targetByte < 0 || totalBytes <= 0 || totalSeconds <= 0.0) {
                // 算不出来就如实说跳不了。装作跳了的话，控制点那边的进度条会停在一个
                // 根本没到过的位置上。
                emit logMessage(QStringLiteral("按字节跳转到 %1 算不出来（总字节 %2、总时长 %3）")
                                    .arg(target).arg(totalBytes).arg(totalSeconds));
                return soapFault(kIllegalSeekTarget, QStringLiteral("Illegal seek target"));
            }

            const double seconds = totalSeconds * (static_cast<double>(targetByte) / totalBytes);
            emit logMessage(QStringLiteral("按字节跳转 %1/%2 -> %3 秒")
                                .arg(targetByte).arg(totalBytes).arg(seconds, 0, 'f', 1));
            m_player->seekTo(seconds);
            return soapOk(QStringLiteral("AVTransport"), action);
        }

        emit logMessage(QStringLiteral("不支持的 Seek 单位「%1」").arg(unit));
        return soapFault(kSeekModeNotSupported, QStringLiteral("Seek mode not supported"));
    }

    if (action == QLatin1String("GetTransportInfo")) {
        const QString inner =
            QStringLiteral("<CurrentTransportState>%1</CurrentTransportState>").arg(m_transportState) +
            QStringLiteral("<CurrentTransportStatus>%1</CurrentTransportStatus>").arg(m_transportStatus) +
            QStringLiteral("<CurrentSpeed>1</CurrentSpeed>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("GetPositionInfo")) {
        const QString pos = upnpTime(m_player->positionSeconds());
        const QString inner =
            QStringLiteral("<Track>1</Track>") +
            QStringLiteral("<TrackDuration>%1</TrackDuration>").arg(upnpTime(m_player->durationSeconds())) +
            QStringLiteral("<TrackMetaData>%1</TrackMetaData>").arg(UpnpXml::escapeText(m_currentMetadata)) +
            QStringLiteral("<TrackURI>%1</TrackURI>").arg(UpnpXml::escapeText(m_currentUri)) +
            QStringLiteral("<RelTime>%1</RelTime>").arg(pos) +
            QStringLiteral("<AbsTime>%1</AbsTime>").arg(pos) +
            // 2147483647 是 UPnP 里"这个值不适用"的约定写法。
            QStringLiteral("<RelCount>2147483647</RelCount>") +
            QStringLiteral("<AbsCount>2147483647</AbsCount>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("GetMediaInfo")) {
        // 没有媒体时轨道数要报 0。报 1 会让控制点以为片子还装着。
        const bool hasMedia = (m_transportState != QLatin1String("NO_MEDIA_PRESENT"));
        const QString inner =
            QStringLiteral("<NrTracks>%1</NrTracks>").arg(hasMedia ? 1 : 0) +
            QStringLiteral("<MediaDuration>%1</MediaDuration>").arg(upnpTime(m_player->durationSeconds())) +
            QStringLiteral("<CurrentURI>%1</CurrentURI>").arg(UpnpXml::escapeText(m_currentUri)) +
            QStringLiteral("<CurrentURIMetaData>%1</CurrentURIMetaData>").arg(UpnpXml::escapeText(m_currentMetadata)) +
            // 队列里排的那条要如实报出去。以前这里是写死的空串 ——
            // 控制点排了歌单，回头问"下一条是什么"，我们答"没有"，它就以为排队失败了。
            QStringLiteral("<NextURI>%1</NextURI>").arg(UpnpXml::escapeText(m_nextUri)) +
            QStringLiteral("<NextURIMetaData>%1</NextURIMetaData>").arg(UpnpXml::escapeText(m_nextMetadata)) +
            QStringLiteral("<PlayMedium>NETWORK</PlayMedium>") +
            QStringLiteral("<RecordMedium>NOT_IMPLEMENTED</RecordMedium>") +
            QStringLiteral("<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("GetCurrentTransportActions")) {
        // 随状态变化：没有媒体时报空，控制点就知道这台设备现在没东西可操作。
        const QString inner = QStringLiteral("<Actions>%1</Actions>")
                                  .arg(UpnpXml::transportActionsFor(m_transportState,
                                                                    hasNext(), hasPrevious()));
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    emit logMessage(QStringLiteral("AVTransport 里没有实现的动作：%1").arg(action));
    return soapFault(kInvalidAction, QStringLiteral("Invalid Action"));
}

QString SoapHandler::handleRenderingControl(const QString &action, const QString &body)
{
    if (!m_player)
        return soapFault(kActionFailed, QStringLiteral("Action Failed"));

    if (action == QLatin1String("SetVolume")) {
        bool ok = false;
        const int level = tagValue(body, QStringLiteral("DesiredVolume")).toInt(&ok);
        if (!ok) {
            emit logMessage(QStringLiteral("SetVolume 没带音量值，回 402"));
            return soapFault(kInvalidArgs, QStringLiteral("Invalid Args"));
        }
        emit logMessage(QStringLiteral("SetVolume %1").arg(level));
        m_player->setVolumePercent(level);
        return soapOk(QStringLiteral("RenderingControl"), action);
    }

    if (action == QLatin1String("GetVolume")) {
        const QString inner =
            QStringLiteral("<CurrentVolume>%1</CurrentVolume>").arg(m_player->volumePercent());
        return soapOk(QStringLiteral("RenderingControl"), action, inner);
    }

    if (action == QLatin1String("SetMute")) {
        // UPnP 的布尔值各家写法不一：有发 1/0 的，也有发 true/false 的。
        const QString raw = tagValue(body, QStringLiteral("DesiredMute")).toLower();
        const bool mute = (raw == QLatin1String("1") || raw == QLatin1String("true") ||
                           raw == QLatin1String("yes"));
        emit logMessage(QStringLiteral("SetMute %1").arg(mute ? 1 : 0));
        m_player->setMuted(mute);
        return soapOk(QStringLiteral("RenderingControl"), action);
    }

    if (action == QLatin1String("GetMute")) {
        const QString inner =
            QStringLiteral("<CurrentMute>%1</CurrentMute>").arg(m_player->isMuted() ? 1 : 0);
        return soapOk(QStringLiteral("RenderingControl"), action, inner);
    }

    // ── 画面调节 ────────────────────────────────────────────────────────
    //
    // DLNA 那边的标准名字 ↔ 我们后端里的短名。只列**真有对应物**的三项：
    // ColorTemperature / HorizontalKeystone / VerticalKeystone 在 DLNA 里也是标准
    // 名字，但 mpv 没有对应属性，SCPD 里就没声明它们，控制点也不会来问。
    //
    // 用一张表转着办，是因为这几个动作除了名字以外一模一样 —— 六段几乎相同的
    // 代码，改一处忘一处是迟早的事。
    struct PictureAction {
        const char *dlna;      // DLNA 里的变量名，动作名是 Get/Set + 它
        const char *control;   // 我们后端里的短名
    };
    static const PictureAction pictureActions[] = {
        { "Brightness", "brightness" },
        { "Contrast",   "contrast"   },
        { "Sharpness",  "sharpen"    },
    };

    // DLNA 这几项统一是 0~100，而且**50 表示"没调过"** —— 这是 UPnP 的惯例，
    // 不是我们定的。我们后端那边是 -100~100、0 是中性。这道换算必须写对：
    // 写漏了的话，控制点发一个 50（它的"原样"）会把画面直接调到最亮那一头。
    for (const PictureAction &item : pictureActions) {
        const QString base = QString::fromLatin1(item.dlna);
        const QString control = QString::fromLatin1(item.control);

        if (action == QStringLiteral("Get") + base) {
            const QString inner = QStringLiteral("<Current%1>%2</Current%1>")
                                      .arg(base)
                                      .arg(dlnaPictureValue(item.control));
            return soapOk(QStringLiteral("RenderingControl"), action, inner);
        }

        if (action == QStringLiteral("Set") + base) {
            bool ok = false;
            const int value = tagValue(body, QStringLiteral("Desired%1").arg(base)).toInt(&ok);
            if (!ok) {
                emit logMessage(QStringLiteral("%1 没带数值，回 402").arg(action));
                return soapFault(kInvalidArgs, QStringLiteral("Invalid Args"));
            }
            emit logMessage(QStringLiteral("%1 %2").arg(action).arg(value));
            m_player->setPictureControl(control, (value - kDlnaNeutral) * 2);
            return soapOk(QStringLiteral("RenderingControl"), action, QString());
        }
    }

    if (action == QLatin1String("ListPresets")) {
        // 只有一个预设。规范要求至少要报出 FactoryDefaults。
        const QString inner =
            QStringLiteral("<CurrentPresetNameList>FactoryDefaults</CurrentPresetNameList>");
        return soapOk(QStringLiteral("RenderingControl"), action, inner);
    }

    if (action == QLatin1String("SelectPreset")) {
        const QString name = tagValue(body, QStringLiteral("PresetName")).trimmed();
        if (name.compare(QLatin1String("FactoryDefaults"), Qt::CaseInsensitive) != 0) {
            // 不认识的名字就说不认识。装作接受了，控制点那边的界面会一直显示
            // 一个根本不存在的预设名。
            emit logMessage(QStringLiteral("不认识的预设「%1」，回 701").arg(name));
            return soapFault(kInvalidName, QStringLiteral("Invalid Name"));
        }

        // 只复位画面调节，不动音量 —— 音量是用户当下要的效果，顺手改掉会让人莫名其妙。
        emit logMessage(QStringLiteral("SelectPreset(FactoryDefaults)：画面调节复位"));
        m_player->resetPictureControls();
        return soapOk(QStringLiteral("RenderingControl"), action, QString());
    }

    emit logMessage(QStringLiteral("RenderingControl 里没有实现的动作：%1").arg(action));
    return soapFault(kInvalidAction, QStringLiteral("Invalid Action"));
}

QString SoapHandler::handleConnectionManager(const QString &action, const QString &body)
{
    if (action == QLatin1String("GetProtocolInfo")) {
        // Source 留空：我们是渲染器，只接收不提供，没有"能拿出去的"东西。
        const QString inner =
            QStringLiteral("<Source></Source><Sink>%1</Sink>").arg(UpnpXml::sinkProtocolInfo());
        emit logMessage(QStringLiteral("GetProtocolInfo —— 已报出 %1 种可接收格式")
                            .arg(UpnpXml::sinkProtocolInfo().count(QLatin1Char(',')) + 1));
        return soapOk(QStringLiteral("ConnectionManager"), action, inner);
    }

    // 我们只有一条连接，而且它不是"建"出来的 —— 控制点直接 SetAVTransportURI 就开播了，
    // 那条连接就是 0 号。有内容装着的时候它在，投送结束之后它就不在了。
    const bool hasConnection = (m_transportState != QLatin1String("NO_MEDIA_PRESENT"));

    if (action == QLatin1String("GetCurrentConnectionIDs")) {
        const QString inner = QStringLiteral("<ConnectionIDs>%1</ConnectionIDs>")
                                  .arg(hasConnection ? QStringLiteral("0") : QString());
        return soapOk(QStringLiteral("ConnectionManager"), action, inner);
    }

    if (action == QLatin1String("GetCurrentConnectionInfo")) {
        bool ok = false;
        const int id = tagValue(body, QStringLiteral("ConnectionID")).toInt(&ok);
        if (!ok || id != 0 || !hasConnection) {
            // 706 = Invalid connection reference。装作有这条连接的话，控制点会一直
            // 拿着一条假的连接号往下走，后面每一步都对不上。
            emit logMessage(QStringLiteral("问的是连接 %1，我们没有这条，回 706").arg(id));
            return soapFault(kInvalidConnectionReference,
                             QStringLiteral("Invalid connection reference"));
        }

        const QString inner =
            // 没有挂 RenderingControl 子服务连接，规范里"不适用"写 -1。
            QStringLiteral("<RcsID>-1</RcsID>") +
            // AVTransport 我们有，就是 0 号。
            QStringLiteral("<AVTransportID>0</AVTransportID>") +
            // 通配的协议信息。我们确实没有去解析内容的真实类型，所以不编一个具体的出来
            // —— 通配表示"不挑"，控制点拿它判断能不能维持连接时不会被挡住。
            QStringLiteral("<ProtocolInfo>http-get:*:*:*</ProtocolInfo>") +
            // 控制点那边的连接管理器地址：我们没记，报空。
            QStringLiteral("<PeerConnectionManager></PeerConnectionManager>") +
            QStringLiteral("<PeerConnectionID>-1</PeerConnectionID>") +
            // 我们是接收方。
            QStringLiteral("<Direction>Input</Direction>") +
            QStringLiteral("<Status>OK</Status>");
        return soapOk(QStringLiteral("ConnectionManager"), action, inner);
    }

    emit logMessage(QStringLiteral("ConnectionManager 里没有实现的动作：%1").arg(action));
    return soapFault(kInvalidAction, QStringLiteral("Invalid Action"));
}
