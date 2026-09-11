#include "HttpServer.h"

#include "GenaManager.h"
#include "SoapHandler.h"
#include "UpnpXml.h"

#include <QHostAddress>
#include <QMultiMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {

/**
 * 还没实现的操作统一回一个 SOAP Fault，而不是 404。
 *
 * 差别不是形式上的：控制点收到 404 会认为"这台设备是坏的"，而收到 501
 * （Action Failed）只会认为"这个操作失败了"。前者会让设备从列表里消失，
 * 后者只是让那个按钮不好使 —— 在我们还没做完 SOAP 的阶段，这个区别很要紧。
 */
QByteArray soapFault(int errorCode, const QString &errorString)
{
    return QStringLiteral(
        "<?xml version=\"1.0\"?>\r\n"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
        "<s:Body><s:Fault>\r\n"
        "<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring>\r\n"
        "<detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">\r\n"
        "<errorCode>%1</errorCode><errorString>%2</errorString>\r\n"
        "</UPnPError></detail>\r\n"
        "</s:Fault></s:Body></s:Envelope>\r\n")
        .arg(errorCode)
        .arg(errorString)
        .toUtf8();
}

} // namespace

HttpServer::HttpServer(QObject *parent)
    : QObject(parent)
{
}

HttpServer::~HttpServer()
{
    stop();
}

bool HttpServer::start(quint16 port, const QString &friendlyName, const QString &udn)
{
    if (m_running)
        return true;

    m_friendlyName = friendlyName;
    m_udn = udn;

    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::Any, port)) {
        emit statusChanged(QStringLiteral("HTTP 服务未启动：端口 %1 绑定失败 —— %2")
                               .arg(port).arg(m_server->errorString()));
        delete m_server;
        m_server = nullptr;
        return false;
    }

    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::onNewConnection);

    m_running = true;
    emit statusChanged(QStringLiteral("HTTP: 监听 0.0.0.0:%1").arg(port));
    return true;
}

void HttpServer::stop()
{
    if (!m_running)
        return;

    if (m_server) {
        m_server->close();
        delete m_server;
        m_server = nullptr;
    }
    m_buffers.clear();

    m_running = false;
    emit statusChanged(QStringLiteral("HTTP: 已停止"));
}

void HttpServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, this, &HttpServer::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &HttpServer::onDisconnected);

        // 兜底：对方把连接开起来却迟迟不把请求发完（比如声明了 Content-Length
        // 却不接着发请求体），这条连接会一直挂着占着缓冲区。
        // 十秒还没凑齐一个完整请求就直接掐掉。
        auto *guard = new QTimer(socket);
        guard->setSingleShot(true);
        connect(guard, &QTimer::timeout, socket, [this, socket] {
            if (m_buffers.contains(socket)) {
                emit logMessage(QStringLiteral("有连接 10 秒没把请求发完，已断开"));
                m_buffers.remove(socket);
                socket->abort();
            }
        });
        guard->start(10 * 1000);
    }
}

void HttpServer::onReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    QByteArray &buffer = m_buffers[socket];

    // TCP 是流不是包：请求头和请求体可能分几批到达，头本身也可能只到一半。
    buffer += socket->readAll();

    if (buffer.size() > 64 * 1024) {
        // 我们只服务几个短请求。堆到 64KB 还没见空行，对面八成不是在说 HTTP。
        socket->abort();
        return;
    }

    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;   // 请求头还没收完

    // 只看"头到了"是不够的 —— 请求体是另外算的。
    //
    // 这里踩过一次坑：控制点发来的 SetAVTransportURI，媒体地址装在**请求体**里。
    // 一看到头就动手的话，体往往还在路上，于是我们拿到一个"没带地址"的空请求，
    // 回一个"参数不对"的错误。本机测试时头和体凑巧在同一次读取里到达，完全看不出
    // 问题；换到手机就必现。
    int contentLength = 0;
    const QList<QByteArray> headerLines = buffer.left(headerEnd).split('\n');
    for (const QByteArray &line : headerLines) {
        if (line.toLower().startsWith("content-length:")) {
            contentLength = line.mid(line.indexOf(':') + 1).trimmed().toInt();
            break;
        }
    }

    if (buffer.size() < headerEnd + 4 + contentLength)
        return;   // 请求体还没收完，接着等

    respond(socket, buffer);
    m_buffers.remove(socket);
}

void HttpServer::onDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket)
        return;

    m_buffers.remove(socket);
    socket->deleteLater();
}

void HttpServer::respond(QTcpSocket *socket, const QByteArray &request)
{
    // 头和体之间空一行分开。我们只服务短请求，一次读进来就够。
    const int headerEnd = request.indexOf("\r\n\r\n");
    const QByteArray headerBytes = request.left(headerEnd);
    const QByteArray bodyBytes = request.mid(headerEnd + 4);

    // 请求头是 ASCII，用 Latin-1 读出来不会因为中文而解坏。
    const QStringList headerLines = QString::fromLatin1(headerBytes).split(QStringLiteral("\r\n"));
    const QStringList parts = headerLines.value(0).split(QLatin1Char(' '));

    const QString method = parts.value(0);
    // 顺手去掉 "?query" 部分 —— 我们的路径不带查询串，但控制点可能加。
    const QString path = parts.value(1).section(QLatin1Char('?'), 0, 0);

    // 头统一用**小写键**存进多值表。两个理由：HTTP 的头名不区分大小写，各家的写法
    // 五花八门；而 CALLBACK 这类头还可能出现多次，用普通 map 会把后面的悄悄丢掉。
    QMultiMap<QString, QString> headers;
    for (int i = 1; i < headerLines.size(); ++i) {
        const QString &line = headerLines.at(i);
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }

    // SOAPACTION 头里带着要调用的动作名，形如
    //   "urn:schemas-upnp-org:service:AVTransport:1#Play"
    // 引号可有可无，动作名在 # 后面。
    QString soapAction = headers.value(QStringLiteral("soapaction"));
    soapAction.remove(QLatin1Char('"'));
    soapAction = soapAction.section(QLatin1Char('#'), -1);

    int status = 200;
    QByteArray payload;
    QByteArray contentType = "text/xml; charset=\"utf-8\"";

    if (method == QLatin1String("POST") && path.endsWith(QLatin1String("/control")) && m_soap) {
        // "/AVTransport/control" 的第二段就是服务名。
        const QString service = path.section(QLatin1Char('/'), 1, 1);
        const QString response = m_soap->handle(service, soapAction, QString::fromUtf8(bodyBytes));

        // SOAP 的规矩：即使业务出错也是 HTTP 500 + Body 里一个 Fault。
        const bool isFault = response.contains(QLatin1String("<s:Fault>"));
        status = isFault ? 500 : 200;
        payload = response.toUtf8();
        emit logMessage(QStringLiteral("SOAP %1#%2  请求体 %3 字节 -> %4")
                            .arg(service, soapAction)
                            .arg(bodyBytes.size())
                            .arg(isFault ? QStringLiteral("失败") : QStringLiteral("成功")));

        // 失败时把请求体前一段也打出来。没有这个，下次再出问题只能靠猜。
        if (isFault)
            emit logMessage(QStringLiteral("   请求体片段：%1")
                                .arg(QString::fromUtf8(bodyBytes).left(300)));
    } else if ((method == QLatin1String("SUBSCRIBE") || method == QLatin1String("UNSUBSCRIBE"))
               && m_gena) {
        // "/AVTransport/event" 的第二段是服务名。
        const QString service = path.section(QLatin1Char('/'), 1, 1);

        // 事件订阅的应答格式和 SOAP 完全不是一回事，由 GenaManager 直接给整块应答头，
        // 这里原样写出去就行。
        const QByteArray reply = (method == QLatin1String("SUBSCRIBE"))
            ? m_gena->handleSubscribe(service, headers)
            : m_gena->handleUnsubscribe(headers);

        socket->write(reply);
        socket->flush();
        socket->disconnectFromHost();
        m_buffers.remove(socket);
        return;
    } else if (method != QLatin1String("GET")) {
        status = 500;
        payload = soapFault(501, QStringLiteral("Action Failed"));
        emit logMessage(QStringLiteral("HTTP %1 %2 —— 不支持的方法").arg(method, path));
    } else if (path == QLatin1String("/description.xml")) {
        payload = UpnpXml::deviceDescription(m_friendlyName, m_udn).toUtf8();
        emit logMessage(QStringLiteral("HTTP GET %1 —— 已送出设备描述").arg(path));
    } else if (path == QLatin1String("/AVTransport/scpd")) {
        payload = UpnpXml::avTransportScpd().toUtf8();
        emit logMessage(QStringLiteral("HTTP GET %1").arg(path));
    } else if (path == QLatin1String("/RenderingControl/scpd")) {
        payload = UpnpXml::renderingControlScpd().toUtf8();
        emit logMessage(QStringLiteral("HTTP GET %1").arg(path));
    } else if (path == QLatin1String("/ConnectionManager/scpd")) {
        payload = UpnpXml::connectionManagerScpd().toUtf8();
        emit logMessage(QStringLiteral("HTTP GET %1").arg(path));
    } else {
        status = 404;
        payload = QByteArrayLiteral("Not Found");
        contentType = "text/plain; charset=\"utf-8\"";
        emit logMessage(QStringLiteral("HTTP %1 %2 —— 404").arg(method, path));
    }

    QByteArray head;
    head += QStringLiteral("HTTP/1.1 %1 %2\r\n")
                .arg(status)
                .arg(status == 200 ? QStringLiteral("OK")
                     : status == 404 ? QStringLiteral("Not Found")
                                     : QStringLiteral("Internal Server Error"))
                .toUtf8();
    head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + QByteArray::number(payload.size()) + "\r\n";
    head += "Connection: close\r\n";
    head += "\r\n";

    socket->write(head);
    socket->write(payload);
    socket->flush();
    socket->disconnectFromHost();
}
