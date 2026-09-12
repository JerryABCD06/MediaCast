#include "GenaManager.h"

#include "UpnpXml.h"

#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QNetworkProxy>
#include <QSharedPointer>

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
    m_subscriptions.insert(sid, sub);

    emit logMessage(QStringLiteral("事件订阅 %1  ->  %2").arg(service, callback));

    // 立刻推一条当前状态。少了这一步，严格的控制点会认为设备不提供事件功能。
    sendEvent(sid, eventBodyFor(service));

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
    return httpResponse(200, QStringLiteral("OK"), QString());
}

void GenaManager::pushTransportState(const QString &state)
{
    if (m_lastTransportState == state)
        return;

    m_lastTransportState = state;
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

    // 其余的按 AVTransport 处理。
    const QString lastChange = QStringLiteral(
        "<Event xmlns=\"urn:schemas-upnp-org:metadata-1-0/AVT/\">"
        "<InstanceID val=\"0\">"
        "<TransportState val=\"%1\"/>"
        "<CurrentTransportActions val=\"%2\"/>"
        "<CurrentPlayMode val=\"%3\"/>"
        "<NumberOfTracks val=\"%4\"/>"
        "<AVTransportURI val=\"%5\"/>"
        "<AVTransportURIMetaData val=\"%6\"/>"
        "<CurrentTrackURI val=\"%7\"/>"
        "<CurrentTrackMetaData val=\"%8\"/>"
        "<NextAVTransportURI val=\"%9\"/>"
        "</InstanceID></Event>")
        // 九项一次填完，不要一个一个链式 arg —— 链式的话，某个参数里只要含 "%1"
        // 这种字样，下一轮就会把内容替换进去。地址里带百分号编码（%E5%8E%9F）很常见。
        .arg(m_lastTransportState,
             UpnpXml::transportActionsFor(m_lastTransportState, m_hasNext, m_hasPrevious),
             m_playMode,
             m_hasMedia ? QStringLiteral("1") : QStringLiteral("0"),
             UpnpXml::escapeText(m_mediaUri),
             UpnpXml::escapeText(m_mediaMetadata),
             UpnpXml::escapeText(m_mediaUri),
             UpnpXml::escapeText(m_mediaMetadata),
             UpnpXml::escapeText(m_nextUri));
    return propertysetWithLastChange(lastChange);
}

void GenaManager::sendEvent(const QString &sid, const QByteArray &body)
{
    auto it = m_subscriptions.find(sid);
    if (it == m_subscriptions.end())
        return;

    const QUrl url(it->callbackUrl);
    if (!url.isValid() || url.host().isEmpty()) {
        emit logMessage(QStringLiteral("回调地址无效，丢掉这个订阅：%1").arg(it->callbackUrl));
        m_subscriptions.erase(it);
        return;
    }

    const int port = url.port(80);
    const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    const int sequence = it->sequence++;

    const QByteArray head = QStringLiteral(
        "NOTIFY %1 HTTP/1.1\r\n"
        "HOST: %2:%3\r\n"
        "CONTENT-TYPE: text/xml; charset=\"utf-8\"\r\n"
        "NT: upnp:event\r\n"
        "NTS: upnp:propchange\r\n"
        "SID: %4\r\n"
        "SEQ: %5\r\n"
        "CONTENT-LENGTH: %6\r\n"
        "Connection: close\r\n\r\n")
        .arg(path, url.host())
        .arg(port)
        .arg(sid)
        .arg(sequence)
        .arg(body.size())
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
            [this, socket, replied, url, port, sequence] {
        const QByteArray reply = socket->readAll();
        *replied = true;

        // 只取第一行（"HTTP/1.1 200 OK"）。正文不重要，本机测试时对方回的
        // 东西也五花八门。
        const int eol = reply.indexOf("\r\n");
        const QString status =
            QString::fromLatin1(reply.left(eol < 0 ? reply.size() : eol)).trimmed();

        emit logMessage(QStringLiteral("推送事件 -> %1:%2  SEQ=%3  对方回了「%4」")
                            .arg(url.host())
                            .arg(port)
                            .arg(sequence)
                            .arg(status));
        socket->disconnectFromHost();
    });

    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);

    // 失败也要说清楚**发去哪**失败了。只有"连接被拒绝"的话，多订阅一多就
    // 分不清是哪个设备/哪个端口的问题。
    connect(socket, &QTcpSocket::errorOccurred, socket,
            [this, socket, url, port](QAbstractSocket::SocketError) {
        emit logMessage(QStringLiteral("事件推送失败 -> %1:%2：%3")
                            .arg(url.host())
                            .arg(port)
                            .arg(socket->errorString()));
        socket->deleteLater();
    });

    // 连不上、或者连上了对方一直不回话，都不能无限等下去。
    auto *guard = new QTimer(socket);
    guard->setSingleShot(true);
    connect(guard, &QTimer::timeout, socket,
            [this, socket, replied, url, port, sequence] {
        if (*replied)
            return;   // 已经在断开的路上了

        if (socket->state() == QAbstractSocket::ConnectedState) {
            emit logMessage(QStringLiteral("推送事件 -> %1:%2  SEQ=%3  "
                                           "写完了，但对方一直没回话")
                                .arg(url.host())
                                .arg(port)
                                .arg(sequence));
        } else if (socket->state() != QAbstractSocket::UnconnectedState) {
            emit logMessage(QStringLiteral("推送事件 -> %1:%2  SEQ=%3  "
                                           "五秒还没连上，放弃")
                                .arg(url.host())
                                .arg(port)
                                .arg(sequence));
        }

        socket->abort();
        socket->deleteLater();
    });
    guard->start(5000);

    socket->connectToHost(url.host(), static_cast<quint16>(port));
}
