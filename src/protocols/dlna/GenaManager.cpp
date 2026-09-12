#include "GenaManager.h"

#include "UpnpXml.h"

#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QNetworkProxy>
#include <QSharedPointer>

#include <cmath>

namespace {

const int kTimeoutSeconds = 1800;
const QString kTimeoutHeader = QStringLiteral("Second-1800");

QByteArray httpResponse(int code, const QString &reason, const QString &extraHeaders)
{
    QString head = QStringLiteral("HTTP/1.1 %1 %2\r\n").arg(code).arg(reason);
    head += extraHeaders;
    head += QStringLiteral("Content-Length: 0\r\nConnection: close\r\n\r\n");
    return head.toUtf8();
}

/**
 * 从 CALLBACK 头里取出第一个回调地址。
 *
 * 格式形如 <http://192.168.1.5:49152/notify>。规范允许一次给好几个（空格分开），
 * 我们只用第一个 —— 一台设备只需要一个通知入口。
 */
QString firstCallback(const QString &header)
{
    const int open = header.indexOf(QLatin1Char('<'));
    if (open < 0)
        return header.trimmed();

    const int close = header.indexOf(QLatin1Char('>'), open + 1);
    if (close < 0)
        return header.mid(open + 1).trimmed();

    return header.mid(open + 1, close - open - 1).trimmed();
}

/** 把 LastChange 文档包进 properset 外壳里（内容是转义后嵌进去的）。 */
QByteArray propertysetWithLastChange(const QString &lastChange)
{
    const QString xml = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
        "<e:property><LastChange>%1</LastChange></e:property>"
        "</e:propertyset>").arg(UpnpXml::escapeText(lastChange));
    return xml.toUtf8();
}

} // namespace

GenaManager::GenaManager(QObject *parent)
    : QObject(parent)
{
}

void GenaManager::prune()
{
    const QDateTime now = QDateTime::currentDateTime();
    for (auto it = m_subscriptions.begin(); it != m_subscriptions.end();) {
        if (it->expiresAt < now)
            it = m_subscriptions.erase(it);
        else
            ++it;
    }
}

QByteArray GenaManager::handleSubscribe(const QString &service,
                                        const QMultiMap<QString, QString> &headers)
{
    prune();

    // 带 SID 而不带 CALLBACK，是"续订"而不是新订阅。
    //
    // 这里如果错当成新订阅、另发一个 SID，控制点手里那个号就作废了：它会一直收不到
    // 通知，而它自己完全不知道。这种"静默失效"是最难查的一类问题。
    const QString existingSid = headers.value(QStringLiteral("sid"));
    if (!existingSid.isEmpty()) {
        auto it = m_subscriptions.find(existingSid);
        if (it == m_subscriptions.end()) {
            emit logMessage(QStringLiteral("续订了一个不认识的 SID，回 412"));
            return httpResponse(412, QStringLiteral("Precondition Failed"), QString());
        }

        it->expiresAt = QDateTime::currentDateTime().addSecs(kTimeoutSeconds);
        emit logMessage(QStringLiteral("事件订阅续订：%1").arg(existingSid));
        return httpResponse(200, QStringLiteral("OK"),
                            QStringLiteral("SID: %1\r\nTIMEOUT: %2\r\n")
                                .arg(existingSid, kTimeoutHeader));
    }

    const QString callback = firstCallback(headers.value(QStringLiteral("callback")));
    const QString notificationType = headers.value(QStringLiteral("nt"));

    if (callback.isEmpty() ||
        notificationType.compare(QLatin1String("upnp:event"), Qt::CaseInsensitive) != 0) {
        emit logMessage(QStringLiteral("SUBSCRIBE 缺 CALLBACK 或 NT，回 412"));
        return httpResponse(412, QStringLiteral("Precondition Failed"), QString());
    }

    const QString sid = QStringLiteral("uuid:") + QUuid::createUuid().toString(QUuid::WithoutBraces);

    Subscription sub;
    sub.service = service;
    sub.callbackUrl = callback;
    sub.expiresAt = QDateTime::currentDateTime().addSecs(kTimeoutSeconds);
    // **先把"在途"占上。** 首条事件要等应答发出去之后才发（原因见下），
    // 这中间万一有状态变化要推，它只会记一个 resend —— 不会抢在首条前面
    // 出去把 SEQ 顺序打乱。
    sub.busy = true;
    m_subscriptions.insert(sid, sub);

    emit logMessage(QStringLiteral("事件订阅 %1  ->  %2").arg(service, callback));
    notifyCountIfChanged();

    // ── 首条事件不能跟订阅应答抢跑 ────────────────────────────────────────
    //
    // 少了首条事件，严格的控制点会认为设备根本不提供事件功能；但**发早了
    // 一样要命**。原来这里就是立刻发，于是应答和"SEQ=0 的事件"几乎同时出去。
    // 控制点得先从应答里把 SID 记下来，才认得那条事件 —— 抢输了的控制点会把
    // SEQ=0 丢掉，而它的期待值就停在 0；我们下一条发的是 SEQ=1，一对不上又丢……
    // **从此每一条都被丢掉**，手机上的播放/暂停再也不变（进度条没事，那是它
    // 自己轮询 GetPositionInfo 问的）。
    //
    // 所以延后一小会儿再发：让应答先到、控制点先把 SID 记好。
    QTimer::singleShot(100, this, [this, sid, service] {
        auto it = m_subscriptions.find(sid);
        if (it == m_subscriptions.end())
            return;   // 这中间已经退订了

        it->busy = false;
        sendEvent(sid, eventBodyFor(service));   // 这一条拿到 SEQ=0
    });

    return httpResponse(200, QStringLiteral("OK"),
                        QStringLiteral("SID: %1\r\nTIMEOUT: %2\r\n").arg(sid, kTimeoutHeader));
}

QByteArray GenaManager::handleUnsubscribe(const QMultiMap<QString, QString> &headers)
{
    const QString sid = headers.value(QStringLiteral("sid"));

    if (sid.isEmpty() || !m_subscriptions.contains(sid)) {
        emit logMessage(QStringLiteral("UNSUBSCRIBE 带着不认识的 SID，回 412"));
        return httpResponse(412, QStringLiteral("Precondition Failed"), QString());
    }

    m_subscriptions.remove(sid);
    emit logMessage(QStringLiteral("事件订阅取消：%1").arg(sid));
    notifyCountIfChanged();
    return httpResponse(200, QStringLiteral("OK"), QString());
}

void GenaManager::pushTransportState(const QString &state)
{
    if (m_lastTransportState == state)
        return;

    m_lastTransportState = state;
    pushToService(QStringLiteral("AVTransport"));
}

void GenaManager::setMediaDuration(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        seconds = 0.0;

    const qint64 total = static_cast<qint64>(seconds);
    const QString text = QStringLiteral("%1:%2:%3")
                             .arg(total / 3600)
                             .arg((total % 3600) / 60, 2, 10, QLatin1Char('0'))
                             .arg(total % 60, 2, 10, QLatin1Char('0'));

    if (text == m_mediaDuration)
        return;

    m_mediaDuration = text;

    // 时长变了通常意味着换了内容 —— 顺手推一条，让控制点把进度条重新标定。
    pushToService(QStringLiteral("AVTransport"));
}

void GenaManager::pushRendering(int volume, bool muted)
{
    if (m_lastVolume == volume && m_lastMuted == muted)
        return;

    m_lastVolume = volume;
    m_lastMuted = muted;
    pushToService(QStringLiteral("RenderingControl"));
}

void GenaManager::pushPictureControls(int brightness, int contrast, int sharpness)
{
    if (m_brightness == brightness && m_contrast == contrast && m_sharpness == sharpness)
        return;

    m_brightness = brightness;
    m_contrast = contrast;
    m_sharpness = sharpness;
    pushToService(QStringLiteral("RenderingControl"));
}

void GenaManager::pushQueueState(bool hasNext, bool hasPrevious,
                                 const QString &nextUri, const QString &playMode)
{
    // 没变就不推。订阅者那边每收到一条 NOTIFY 都要处理一次，白发是浪费。
    if (m_hasNext == hasNext && m_hasPrevious == hasPrevious
        && m_nextUri == nextUri && m_playMode == playMode)
        return;

    m_hasNext = hasNext;
    m_hasPrevious = hasPrevious;
    m_nextUri = nextUri;
    m_playMode = playMode;
    pushToService(QStringLiteral("AVTransport"));
}

void GenaManager::pushMediaState(const QString &uri, const QString &metadata)
{
    const bool hasMedia = !uri.isEmpty();
    if (m_mediaUri == uri && m_mediaMetadata == metadata && m_hasMedia == hasMedia)
        return;

    m_mediaUri = uri;
    m_mediaMetadata = metadata;
    m_hasMedia = hasMedia;
    pushToService(QStringLiteral("AVTransport"));
}

void GenaManager::pushToService(const QString &service)
{
    prune();
    notifyCountIfChanged();   // prune 可能清掉了过期的订阅
    if (m_subscriptions.isEmpty())
        return;

    const QByteArray body = eventBodyFor(service);

    // 先把 key 拷一份出来：sendEvent 在对方地址无效时会删订阅，
    // 直接在哈希上边遍历边删是要出事的。
    const QList<QString> sids = m_subscriptions.keys();
    for (const QString &sid : sids) {
        auto it = m_subscriptions.find(sid);
        if (it != m_subscriptions.end() && it->service == service)
            sendEvent(sid, body);
    }
}

QByteArray GenaManager::eventBodyFor(const QString &service) const
{
    if (service == QLatin1String("RenderingControl")) {
        // 画面调节那三项没有 channel（它们不分声道），音量/静音有 —— 这是规范的写法，
        // 少了 channel 属性的控制点会当成非法事件。
        const QString lastChange = QStringLiteral(
            "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/RCS/\">"
            "<InstanceID val=\"0\">"
            "<Volume channel=\"Master\" val=\"%1\"/>"
            "<Mute channel=\"Master\" val=\"%2\"/>"
            "<Brightness val=\"%3\"/>"
            "<Contrast val=\"%4\"/>"
            "<Sharpness val=\"%5\"/>"
            "</InstanceID></Event>")
            .arg(m_lastVolume)
            .arg(m_lastMuted ? 1 : 0)
            .arg(m_brightness)
            .arg(m_contrast)
            .arg(m_sharpness);
        return propertysetWithLastChange(lastChange);
    }

    if (service == QLatin1String("ConnectionManager")) {
        // ConnectionManager 不走 LastChange：它的能力清单是直接当属性报的。
        const QString xml = QStringLiteral(
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
            "<e:propertyset xmlns:e=\"urn:schemas-upnp-org:event-1-0\">"
            "<e:property><SourceProtocolInfo></SourceProtocolInfo></e:property>"
            "<e:property><SinkProtocolInfo>%1</SinkProtocolInfo></e:property>"
            "</e:propertyset>").arg(UpnpXml::sinkProtocolInfo());
        return xml.toUtf8();
    }

    // ── AVTransport 的 LastChange ────────────────────────────────────────
    //
    // **这一段是照 Macast 抄的。** Macast 是成熟的 DLNA 渲染器，手机跟它配合是
    // 好的；我们原来在事件里塞的是一份"全量快照"，其中包含 AVTransportURI、
    // CurrentTrackURI，以及**两整坨 DIDL 元数据**（转义过两遍的长 XML）。
    //
    // 少发那两坨不只是省字节：事件是丢给控制点**解析**的，里面塞的结构化内容
    // 越多，它解析出岔子、然后整条事件被丢掉的机会就越大 —— 而且这种失败完全
    // 静默（HTTP 层照样回 200，日志里只能看到"对方回了 200 OK"）。
    // 手机上"收到了却不改播放/暂停按钮"就是这么一类毛病。
    //
    // Macast 在事件里只发几个标量：TransportState / TransportStatus /
    // CurrentMediaDuration / CurrentTrackDuration / CurrentTrack / NumberOfTracks。
    // 我们在它那个集合上加回了 CurrentTransportActions（控制点靠它决定哪些按钮
    // 能用，Play 和 Pause 必须互斥，见 UpnpXml::transportActionsFor）。
    const QString lastChange = QStringLiteral(
        "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
        "<InstanceID val=\"0\">"
        "<TransportState val=\"%1\"/>"
        "<TransportStatus val=\"OK\"/>"
        "<CurrentMediaDuration val=\"%2\"/>"
        "<CurrentTrackDuration val=\"%3\"/>"
        "<CurrentTrack val=\"%4\"/>"
        "<NumberOfTracks val=\"%5\"/>"
        "<CurrentPlayMode val=\"%6\"/>"
        "<CurrentTransportActions val=\"%7\"/>"
        "</InstanceID></Event>")
        // 一次填完，不要一个一个链式 arg —— 链式的话，某个参数里只要含 "%1"
        // 这种字样，下一轮就会把内容替换进去。
        .arg(m_lastTransportState,
             m_mediaDuration,
             m_mediaDuration,
             m_hasMedia ? QStringLiteral("1") : QStringLiteral("0"),
             m_hasMedia ? QStringLiteral("1") : QStringLiteral("0"),
             m_playMode,
             UpnpXml::transportActionsFor(m_lastTransportState, m_hasNext, m_hasPrevious));
    return propertysetWithLastChange(lastChange);
}

void GenaManager::sendEvent(const QString &sid, const QByteArray &body)
{
    auto it = m_subscriptions.find(sid);
    if (it == m_subscriptions.end())
        return;

    // ── 同一个订阅同一时刻只允许一条 NOTIFY 在路上 ────────────────────────
    //
    // 这不是为了省资源，是为了**顺序**。GENA 要求订阅者按 SEQ 严格递增处理：
    // 它等 5 却先收到 6 就必须丢掉，而且丢掉之后期待值还是 5 —— 后面每一条
    // 都对不上，全被丢。表现就是手机上的播放/暂停状态从此冻住不再变。
    //
    // 我们原来是每变一次状态就新开一条 TCP 连接，两条挨着发谁先到说不准，
    // 实测真的乱过（SEQ=6 比 SEQ=5 先到）。所以这里改成：在途的时候只做个
    // 记号，等那条结束再按**那时候**的最新状态补一条。
    if (it->busy) {
        it->resend = true;
        return;
    }
    it->busy = true;

    const QUrl url(it->callbackUrl);
    if (!url.isValid() || url.host().isEmpty()) {
        emit logMessage(QStringLiteral("回调地址无效，丢掉这个订阅：%1").arg(it->callbackUrl));
        m_subscriptions.erase(it);
        return;
    }

    const int port = url.port(80);
    const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    const int sequence = it->sequence++;
    // 后面那些 lambda 是异步跑的，那时 it 早就不能用了，所以先拷出来。
    const QString service = it->service;

    const QByteArray head = QStringLiteral(
        "NOTIFY %1 HTTP/1.1\r\n"
        "HOST: %2:%3\r\n"
        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
        "NT: upnp:event\r\n"
        "NTS: upnp:propchange\r\n"
        "SID: %4\r\n"
        "SEQ: %5\r\n"
        "CONTENT-LENGTH: %6\r\n"
        // SERVER 是 UPnP 规范里"建议带上"的一项，真实渲染器都带（Macast 也带）。
        // TIMEOUT 也一样 —— 有些控制点会顺手拿它，不发它是不完整的。
        "SERVER: %7\r\n"
        "TIMEOUT: Second-1800\r\n"
        "Connection: close\r\n\r\n")
        .arg(path, url.host())
        .arg(port)
        .arg(sid)
        .arg(sequence)
        .arg(body.size())
        .arg(QStringLiteral("Windows/10.0 UPnP/1.0 MediaCast/0.1"))
        .toUtf8();

    // 用裸 socket，不用高层的 HTTP 客户端：NOTIFY 是个很少见的动词，
    // 高层的那些要么不支持，要么直接判定为非法方法。
    auto *socket = new QTcpSocket(this);

    // **必须显式绕过代理。**
    //
    // Qt 在连接时如果应用代理是"默认"类型，会去问操作系统的代理配置。而系统上
    // 只要装了带"系统代理"模式的工具（这台机器上是 FlClash），Qt 就可能拿到一个
    // 它不能用于裸 TCP 的代理类型，然后连接直接失败，错误是那句很难懂的
    // "The proxy type is invalid for this operation"。
    //
    // 这个错误在本机测试时永远看不到 —— 回调地址是 127.0.0.1 时 Qt 不套代理，
    // 一换成真实设备的局域网地址就必现。实测日志里二十多条事件推送全是这个错误。
    //
    // 而且从道理上讲，DLNA 的事件推送目标永远在同一个局域网里，经过代理本身就是错的。
    socket->setProxy(QNetworkProxy::NoProxy);

    // **只在连接真的建立之后才记一条"推送事件"。**
    //
    // 早先这行打在连接之前，看着像"发出去了"，其实是"打算发" —— 排查
    // "手机上收不到"的时候，满屏的"推送事件"会把真相盖住（真相是底下那行
    // "连接被拒绝"）。日志要能一眼看出哪条到了、哪条没到。
    //
    // 而且**写完不是就完事了**：NOTIFY 是个请求，规范要求对方回一条 200。
    // 早先我们写完就断，于是"手机到底认没认这条事件"根本看不出来。现在等
    // 对方的应答，日志里那条"对方回了「HTTP/1.1 200 OK」"才是真凭据。
    // （对方不回也不算错 —— 有的控制点就是闷头收 —— 五秒等不到就记一笔收工。）
    auto replied = QSharedPointer<bool>::create(false);

    connect(socket, &QTcpSocket::connected, socket,
            [socket, head, body] {
        socket->write(head);
        socket->write(body);
        socket->flush();
    });

    connect(socket, &QTcpSocket::readyRead, socket,
            [this, socket, replied, url, port, sequence, service, sid] {
        const QByteArray reply = socket->readAll();
        *replied = true;
        noteSendOk(sid);

        // 只取第一行（"HTTP/1.1 200 OK"）。正文不重要，本机测试时对方回的
        // 东西也五花八门。
        const int eol = reply.indexOf("\r\n");
        const QString status =
            QString::fromLatin1(reply.left(eol < 0 ? reply.size() : eol)).trimmed();

        emit logMessage(QStringLiteral("推送事件 %1 -> %2:%3  SEQ=%4  对方回了「%5」")
                            .arg(service)
                            .arg(url.host())
                            .arg(port)
                            .arg(sequence)
                            .arg(status));
        socket->disconnectFromHost();
        finishSend(sid);
    });

    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);

    // 失败也要说清楚**发去哪**失败了。只有"连接被拒绝"的话，多订阅一多就
    // 分不清是哪个设备/哪个端口的问题。
    connect(socket, &QTcpSocket::errorOccurred, socket,
            [this, socket, url, port, service, sid](QAbstractSocket::SocketError) {
        emit logMessage(QStringLiteral("事件推送失败 %1 -> %2:%3：%4")
                            .arg(service)
                            .arg(url.host())
                            .arg(port)
                            .arg(socket->errorString()));
        noteSendFailed(sid);
        finishSend(sid);
        socket->deleteLater();
    });

    // 连不上、或者连上了对方一直不回话，都不能无限等下去。
    auto *guard = new QTimer(socket);
    guard->setSingleShot(true);
    connect(guard, &QTimer::timeout, socket,
            [this, socket, replied, url, port, sequence, service, sid] {
        if (*replied)
            return;   // 已经在断开的路上了

        if (socket->state() == QAbstractSocket::ConnectedState) {
            emit logMessage(QStringLiteral("推送事件 %1 -> %2:%3  SEQ=%4  "
                                           "写完了，但对方一直没回话")
                                .arg(service)
                                .arg(url.host())
                                .arg(port)
                                .arg(sequence));
        } else if (socket->state() != QAbstractSocket::UnconnectedState) {
            emit logMessage(QStringLiteral("推送事件 %1 -> %2:%3  SEQ=%4  "
                                           "五秒还没连上，放弃")
                                .arg(service)
                                .arg(url.host())
                                .arg(port)
                                .arg(sequence));
            noteSendFailed(sid);
        }

        finishSend(sid);
        socket->abort();
        socket->deleteLater();
    });
    guard->start(5000);

    socket->connectToHost(url.host(), static_cast<quint16>(port));
}

void GenaManager::noteSendOk(const QString &sid)
{
    auto it = m_subscriptions.find(sid);
    if (it != m_subscriptions.end())
        it->failures = 0;
}

void GenaManager::noteSendFailed(const QString &sid)
{
    auto it = m_subscriptions.find(sid);
    if (it == m_subscriptions.end())
        return;

    // 挨个儿失败几次就判定这个回调地址已经没人听了。控制点的回调服务器本来
    // 就是会消失的（App 退出、换端口重新订阅），留着它只会让之后每一次状态
    // 变化都白开一条连接，并且把日志淹掉 —— 排查的时候，满屏的"连接被拒绝"
    // 会把真正有用的那几行盖住。
    if (++it->failures < 5)
        return;

    emit logMessage(QStringLiteral("连着 %1 次推不出去，丢掉这个订阅：%2")
                        .arg(it->failures)
                        .arg(it->callbackUrl));
    m_subscriptions.erase(it);
    notifyCountIfChanged();
}

void GenaManager::notifyCountIfChanged()
{
    const int now = m_subscriptions.size();
    if (now == m_lastReportedCount)
        return;

    m_lastReportedCount = now;
    emit subscriptionCountChanged(now);
}

void GenaManager::finishSend(const QString &sid)
{
    auto it = m_subscriptions.find(sid);
    if (it == m_subscriptions.end())
        return;

    it->busy = false;

    if (!it->resend)
        return;

    it->resend = false;

    // 攒着的那次**重新取一遍当前状态**，而不是把当初那条原样再发一遍。
    // 中间可能又变了好几回（播放→暂停→播放），发最新的那一个才是对的，
    // 中间态没人关心。
    sendEvent(sid, eventBodyFor(it->service));
}
