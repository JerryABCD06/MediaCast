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
    // 注意：这里的 hasNext 是 QRegularExpressionMatchIterator 的，
    // 跟队列那件事没关系 —— 别被同名骗了。
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

// ── 构造：把控制器的中性信号翻成 DLNA 的词 ───────────────────────────────
//
// 这一层自己没有任何状态 —— 所有状态都在控制器里。这里只是"接线 + 翻译"。

SoapHandler::SoapHandler(PlaybackController *controller, QObject *parent)
    : QObject(parent)
    , m_ctl(controller)
{
    if (!m_ctl)
        return;

    // 注意：**不要**把控制器的 logMessage 转发成本类的 logMessage。
    //
    // DlnaRenderer 那边已经直接连了控制器的 logMessage；这里再转发一道，同一句
    // 话就会从两条路到达它，日志里每行都出现两遍。这个 bug 是实测时看出来的 ——
    // 每行时间戳一模一样地重复。
    //
    // 所以分工是：控制器自己的日志由 DlnaRenderer 直接接；这个类只发"它自己"
    // 的日志（比如"控制点给的标题不像标题"）。

    // 状态：中性枚举翻成 DLNA 的词再往外报。
    connect(m_ctl, &PlaybackController::stateChanged, this,
            [this](PlaybackController::State) {
        emit transportStateChanged(transportState());
    });

    connect(m_ctl, &PlaybackController::nowPlayingChanged,
            this, &SoapHandler::nowPlayingChanged);
    connect(m_ctl, &PlaybackController::mediaChanged,
            this, &SoapHandler::mediaChanged);

    // 队列：控制器报的是中性枚举，这里翻成 DLNA 的词。
    connect(m_ctl, &PlaybackController::queueChanged, this,
            [this](bool hasNext, bool hasPrevious, const QString &nextUri,
                   PlaybackController::PlayMode mode) {
        emit queueChanged(hasNext, hasPrevious, nextUri, dlnaPlayModeName(mode));
    });

    // 画面值：控制器报的是后端原始值，这里翻成 DLNA 的 0~100。
    //
    // 三个值一起报，是因为 DLNA RenderingControl 的事件本来就是一份 LastChange
    // 全量推 —— 只报变了的那一个，控制点那边的另外两个就永远补不齐。
    connect(m_ctl, &PlaybackController::pictureControlChanged, this,
            [this](const QString &, int) {
        emit pictureControlsChanged(dlnaPictureValue("brightness"),
                                    dlnaPictureValue("contrast"),
                                    dlnaPictureValue("sharpen"));
    });
}

QString SoapHandler::transportState() const
{
    return m_ctl ? dlnaStateName(m_ctl->state()) : QStringLiteral("NO_MEDIA_PRESENT");
}

NowPlaying SoapHandler::nowPlaying() const
{
    return m_ctl ? m_ctl->nowPlaying() : NowPlaying();
}

// ── 词汇表翻译 ───────────────────────────────────────────────────────────
//
// 中性 → DLNA。这几个字符串是 DLNA 规范定义好的，控制点按它们判断状态，
// 所以一个字都不能改。别处（控制器里）一律用中性的话。

QString SoapHandler::dlnaStateName(PlaybackController::State state)
{
    switch (state) {
    case PlaybackController::State::NoMedia:   return QStringLiteral("NO_MEDIA_PRESENT");
    case PlaybackController::State::Stopped:   return QStringLiteral("STOPPED");
    case PlaybackController::State::Preparing: return QStringLiteral("TRANSITIONING");
    case PlaybackController::State::Playing:   return QStringLiteral("PLAYING");
    case PlaybackController::State::Paused:    return QStringLiteral("PAUSED_PLAYBACK");
    }
    return QStringLiteral("NO_MEDIA_PRESENT");
}

QString SoapHandler::dlnaPlayModeName(PlaybackController::PlayMode mode)
{
    switch (mode) {
    case PlaybackController::PlayMode::Normal:    return QStringLiteral("NORMAL");
    case PlaybackController::PlayMode::RepeatOne: return QStringLiteral("REPEAT_ONE");
    case PlaybackController::PlayMode::RepeatAll: return QStringLiteral("REPEAT_ALL");
    case PlaybackController::PlayMode::Direct:    return QStringLiteral("DIRECT_1");
    }
    return QStringLiteral("NORMAL");
}

QString SoapHandler::dlnaLoadStatusName(PlaybackController::LoadStatus status)
{
    // 只有两个值。Failed 是加载看门狗放弃时置上的 —— 播不出来要如实说，
    // 光把状态归成"停了"会让控制点以为是我们自己停的。
    return status == PlaybackController::LoadStatus::Failed
               ? QStringLiteral("ERROR_OCCURRED")
               : QStringLiteral("OK");
}

bool SoapHandler::parsePlayMode(const QString &text, PlaybackController::PlayMode &out)
{
    // 认不出来就返回 false，调用方回一个 SOAP 错误。
    //
    // SHUFFLE 会落到这儿。我们手上只有"上一条/当前/下一条"三个位置，没有一份
    // 列表可以打乱；收下它却照顺序放，等于骗控制点，不如直接说不支持。
    if (text == QLatin1String("NORMAL"))          { out = PlaybackController::PlayMode::Normal;    return true; }
    if (text == QLatin1String("REPEAT_ONE"))      { out = PlaybackController::PlayMode::RepeatOne; return true; }
    if (text == QLatin1String("REPEAT_ALL"))      { out = PlaybackController::PlayMode::RepeatAll; return true; }
    if (text == QLatin1String("DIRECT_1"))        { out = PlaybackController::PlayMode::Direct;    return true; }
    return false;
}

int SoapHandler::dlnaPictureValue(const char *control) const
{
    if (!m_ctl)
        return kDlnaNeutral;

    // 后端是 -100~100，DLNA 是 0~100 且 50 才是"原样"。
    return m_ctl->pictureControlValue(QString::fromLatin1(control)) / 2 + kDlnaNeutral;
}

// ── DIDL-Lite → MediaRequest ─────────────────────────────────────────────

PlaybackController::MediaRequest SoapHandler::mediaRequestFromSoap(const QString &uri,
                                                                  const QString &metadata)
{
    PlaybackController::MediaRequest request;
    request.uri = uri;
    request.metadata = metadata;

    // ── 类型 ────────────────────────────────────────────────────────────
    // 优先信控制点声明的 upnp:class，它没有控制器会看扩展名。
    const QString upnpClass = tagValue(metadata, QStringLiteral("upnp:class"));
    if (upnpClass.contains(QLatin1String("imageItem")))
        request.kind = MediaKind::Image;
    else if (upnpClass.contains(QLatin1String("audioItem")))
        request.kind = MediaKind::Audio;
    else if (upnpClass.contains(QLatin1String("videoItem")))
        request.kind = MediaKind::Video;

    // ── 标题 ────────────────────────────────────────────────────────────
    // 控制点给的 dc:title 要先过"像不像标题"的筛子。
    //
    // 这一条是被实际数据逼出来的：vivo 相册投图片时，dc:title 里塞的就是文件名
    // 本身 —— "Screenshot_20260905_232916.jpg"，连扩展名都带着。那不是标题，
    // 是文件名搬运。而本地放同一个文件时控制器推出的是"图片"，同一张图两个名字
    // 说不通。所以：带已知媒体扩展名的先去掉扩展名，再过一遍筛子；过不了就当它
    // 没给标题，交给控制器往后退。
    QString title = tagValue(metadata, QStringLiteral("dc:title"));
    if (!title.isEmpty()) {
        const QString cleaned = stripMediaExtension(title);
        if (looksLikeATitle(cleaned)) {
            request.title = cleaned;
        } else {
            emit logMessage(QStringLiteral("控制点给的标题「%1」不像标题，跳过").arg(title));
        }
    }

    // ── 作者 / 专辑 ─────────────────────────────────────────────────────
    request.artist = tagValue(metadata, QStringLiteral("upnp:artist"));
    if (request.artist.isEmpty())
        request.artist = tagValue(metadata, QStringLiteral("dc:creator"));
    if (looksLikePlaceholder(request.artist)) {
        emit logMessage(QStringLiteral("控制点给的作者是占位符「%1」，忽略").arg(request.artist));
        request.artist.clear();
    }

    request.album = tagValue(metadata, QStringLiteral("upnp:album"));
    if (looksLikePlaceholder(request.album))
        request.album.clear();

    return request;
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
    if (!m_ctl)
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

        // DIDL 的解码在这一层（那是 UPnP 的东西），解完交给控制器 ——
        // 电脑端的播放按钮走同一个控制器入口，两边就不会各写一份、各漏一处。
        m_ctl->openUri(mediaRequestFromSoap(uri, meta), MediaSource::Cast);
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("SetNextAVTransportURI")) {
        // 和 SetAVTransportURI 一样，元数据也是转义后塞进来的，先解转义。
        const QString rawMeta = tagValue(body, QStringLiteral("NextURIMetaData"));
        const QString meta = rawMeta.isEmpty() ? QString() : UpnpXml::unescapeText(rawMeta);
        const QString uri = UpnpXml::unescapeText(tagValue(body, QStringLiteral("NextURI")));

        // 空地址是合法的，含义是"把队列清掉"——规范里就是这么规定的。
        // （清空时 mediaRequestFromSoap 收的是空地址，控制器那边 uri 空就是清空。）
        m_ctl->setNextUri(mediaRequestFromSoap(uri, meta));
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    // 手上什么都没有的时候，播放/暂停/停止/跳转都不成立 —— 规范里这就是 701
    // （Transition not available）。
    //
    // 不拦的话会出一件很别扭的事：一个走错路的 Play（或者投送结束之后控制点补发的
    // Stop）会把状态从"我这儿什么都没有"改成"正在放/停着"，而播放器其实空着。
    // 控制点那边于是以为设备上有片子，界面上挂着一堆能按的按钮，按下去都没反应。
    if (m_ctl->state() == PlaybackController::State::NoMedia
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
        m_ctl->play();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Next")) {
        m_ctl->next();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Previous")) {
        m_ctl->previous();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("SetPlayMode")) {
        const QString modeText = tagValue(body, QStringLiteral("NewPlayMode")).trimmed().toUpper();
        PlaybackController::PlayMode mode = PlaybackController::PlayMode::Normal;
        // "哪些播放模式存在"是 DLNA 自己的事 —— 比如 SHUFFLE 规范里有、
        // 但我们手上只有三格队列打乱不了，所以在这一层就回绝，压根不调控制器。
        if (!parsePlayMode(modeText, mode)) {
            // 701 = Transition not available。告诉控制点"这个模式我做不到"，
            // 比收下来然后按普通模式放要诚实。
            emit logMessage(QStringLiteral("控制点要的播放模式「%1」我们做不到").arg(modeText));
            return soapFault(kTransitionNotAvailable,
                             QStringLiteral("Transition not available"));
        }
        m_ctl->setPlayMode(mode);
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("GetTransportSettings")) {
        const QString inner =
            QStringLiteral("<PlayMode>%1</PlayMode>").arg(dlnaPlayModeName(m_ctl->playMode())) +
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
        m_ctl->pause();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Stop")) {
        // 控制点自己点的"停止"，和电脑上的「停止」按钮是同一件事。
        m_ctl->stop();
        return soapOk(QStringLiteral("AVTransport"), action);
    }

    if (action == QLatin1String("Seek")) {
        const QString unit = tagValue(body, QStringLiteral("Unit")).trimmed().toUpper();
        const QString target = tagValue(body, QStringLiteral("Target")).trimmed();

        // 没写单位就按 REL_TIME 算 —— 它是最常用的那个。
        if (unit.isEmpty() || unit == QLatin1String("REL_TIME")) {
            const double seconds = parseUpnpTime(target);
            emit logMessage(QStringLiteral("Seek 到 %1 秒").arg(seconds));
            m_ctl->seekTo(seconds);
            return soapOk(QStringLiteral("AVTransport"), action);
        }

        if (unit == QLatin1String("TRACK_NR")) {
            // 我们只有一条轨道。按规范，跳到"第 1 条"就是回到开头 —— 这不是敷衍，
            // 单轨道设备本来就该这么答。
            bool ok = false;
            const int track = target.toInt(&ok);
            if (ok && track == 1) {
                emit logMessage(QStringLiteral("Seek 到第 1 条（只有一条，回到开头）"));
                m_ctl->seekTo(0.0);
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
            const qint64 totalBytes = m_ctl->mediaSizeBytes();
            const double totalSeconds = m_ctl->durationSeconds();

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
            m_ctl->seekTo(seconds);
            return soapOk(QStringLiteral("AVTransport"), action);
        }

        emit logMessage(QStringLiteral("不支持的 Seek 单位「%1」").arg(unit));
        return soapFault(kSeekModeNotSupported, QStringLiteral("Seek mode not supported"));
    }

    if (action == QLatin1String("GetTransportInfo")) {
        const QString inner =
            QStringLiteral("<CurrentTransportState>%1</CurrentTransportState>").arg(transportState()) +
            QStringLiteral("<CurrentTransportStatus>%1</CurrentTransportStatus>").arg(dlnaLoadStatusName(m_ctl->loadStatus())) +
            QStringLiteral("<CurrentSpeed>1</CurrentSpeed>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("GetPositionInfo")) {
        const QString pos = upnpTime(m_ctl->positionSeconds());
        const QString inner =
            QStringLiteral("<Track>1</Track>") +
            QStringLiteral("<TrackDuration>%1</TrackDuration>").arg(upnpTime(m_ctl->durationSeconds())) +
            QStringLiteral("<TrackMetaData>%1</TrackMetaData>").arg(UpnpXml::escapeText(m_ctl->currentMetadata())) +
            QStringLiteral("<TrackURI>%1</TrackURI>").arg(UpnpXml::escapeText(m_ctl->currentUri())) +
            QStringLiteral("<RelTime>%1</RelTime>").arg(pos) +
            QStringLiteral("<AbsTime>%1</AbsTime>").arg(pos) +
            // 2147483647 是 UPnP 里"这个值不适用"的约定写法。
            QStringLiteral("<RelCount>2147483647</RelCount>") +
            QStringLiteral("<AbsCount>2147483647</AbsCount>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("GetMediaInfo")) {
        // 没有媒体时轨道数要报 0。报 1 会让控制点以为片子还装着。
        const bool hasMedia = (m_ctl->state() != PlaybackController::State::NoMedia);
        const QString inner =
            QStringLiteral("<NrTracks>%1</NrTracks>").arg(hasMedia ? 1 : 0) +
            QStringLiteral("<MediaDuration>%1</MediaDuration>").arg(upnpTime(m_ctl->durationSeconds())) +
            QStringLiteral("<CurrentURI>%1</CurrentURI>").arg(UpnpXml::escapeText(m_ctl->currentUri())) +
            QStringLiteral("<CurrentURIMetaData>%1</CurrentURIMetaData>").arg(UpnpXml::escapeText(m_ctl->currentMetadata())) +
            // 队列里排的那条要如实报出去。以前这里是写死的空串 ——
            // 控制点排了歌单，回头问"下一条是什么"，我们答"没有"，它就以为排队失败了。
            QStringLiteral("<NextURI>%1</NextURI>").arg(UpnpXml::escapeText(m_ctl->nextUri())) +
            QStringLiteral("<NextURIMetaData>%1</NextURIMetaData>").arg(UpnpXml::escapeText(m_ctl->nextMetadata())) +
            QStringLiteral("<PlayMedium>NETWORK</PlayMedium>") +
            QStringLiteral("<RecordMedium>NOT_IMPLEMENTED</RecordMedium>") +
            QStringLiteral("<WriteStatus>NOT_IMPLEMENTED</WriteStatus>");
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    if (action == QLatin1String("GetCurrentTransportActions")) {
        // 随状态变化：没有媒体时报空，控制点就知道这台设备现在没东西可操作。
        const QString inner = QStringLiteral("<Actions>%1</Actions>")
                                  .arg(UpnpXml::transportActionsFor(transportState(),
                                                                    m_ctl->hasNext(), m_ctl->hasPrevious()));
        return soapOk(QStringLiteral("AVTransport"), action, inner);
    }

    emit logMessage(QStringLiteral("AVTransport 里没有实现的动作：%1").arg(action));
    return soapFault(kInvalidAction, QStringLiteral("Invalid Action"));
}

QString SoapHandler::handleRenderingControl(const QString &action, const QString &body)
{
    if (!m_ctl)
        return soapFault(kActionFailed, QStringLiteral("Action Failed"));

    if (action == QLatin1String("SetVolume")) {
        bool ok = false;
        const int level = tagValue(body, QStringLiteral("DesiredVolume")).toInt(&ok);
        if (!ok) {
            emit logMessage(QStringLiteral("SetVolume 没带音量值，回 402"));
            return soapFault(kInvalidArgs, QStringLiteral("Invalid Args"));
        }
        emit logMessage(QStringLiteral("SetVolume %1").arg(level));
        m_ctl->setVolumePercent(level);
        return soapOk(QStringLiteral("RenderingControl"), action);
    }

    if (action == QLatin1String("GetVolume")) {
        const QString inner =
            QStringLiteral("<CurrentVolume>%1</CurrentVolume>").arg(m_ctl->volumePercent());
        return soapOk(QStringLiteral("RenderingControl"), action, inner);
    }

    if (action == QLatin1String("SetMute")) {
        // UPnP 的布尔值各家写法不一：有发 1/0 的，也有发 true/false 的。
        const QString raw = tagValue(body, QStringLiteral("DesiredMute")).toLower();
        const bool mute = (raw == QLatin1String("1") || raw == QLatin1String("true") ||
                           raw == QLatin1String("yes"));
        emit logMessage(QStringLiteral("SetMute %1").arg(mute ? 1 : 0));
        m_ctl->setMuted(mute);
        return soapOk(QStringLiteral("RenderingControl"), action);
    }

    if (action == QLatin1String("GetMute")) {
        const QString inner =
            QStringLiteral("<CurrentMute>%1</CurrentMute>").arg(m_ctl->isMuted() ? 1 : 0);
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
            m_ctl->setPictureControl(control, (value - kDlnaNeutral) * 2);
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
        m_ctl->resetPictureControls();
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
    const bool hasConnection = (m_ctl->state() != PlaybackController::State::NoMedia);

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
