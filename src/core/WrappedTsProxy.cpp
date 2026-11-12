// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "WrappedTsProxy.h"

#include <QCache>
#include <QHash>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <cmath>

class WrappedTsProxy::Impl
{
public:
    explicit Impl(WrappedTsProxy *owner)
        : q(owner)
    {
        cache.setMaxCost(64 * 1024);
    }

    static std::optional<QPair<qint64, qint64>> parseRange(
        const QByteArray &headers)
    {
        const QByteArray lower = headers.toLower();
        const qsizetype position = lower.indexOf("\r\nrange: bytes=");
        if (position < 0)
            return std::nullopt;
        const qsizetype startPos = position + 15;
        const qsizetype endPos = headers.indexOf("\r\n", startPos);
        const QByteArray value = headers.mid(
            startPos, endPos < 0 ? -1 : endPos - startPos);
        const qsizetype dash = value.indexOf('-');
        if (dash <= 0)
            return std::nullopt;
        bool startOk = false;
        bool endOk = false;
        const qint64 start = value.left(dash).toLongLong(&startOk);
        const qint64 end = value.mid(dash + 1).toLongLong(&endOk);
        if (!startOk)
            return std::nullopt;
        return QPair<qint64, qint64>{
            start, endOk ? end : -1,
        };
    }

    void writeResponse(
        QTcpSocket *socket, const QByteArray &data,
        const std::optional<QPair<qint64, qint64>> &range,
        bool headOnly, const QByteArray &contentType)
    {
        if (!socket || socket->state() == QAbstractSocket::UnconnectedState)
            return;
        qint64 start = 0;
        qint64 end = data.isEmpty()
            ? -1 : static_cast<qint64>(data.size()) - 1;
        int status = 200;
        QByteArray contentRange;
        if (range && !data.isEmpty()) {
            start = std::clamp<qint64>(
                range->first, 0, data.size() - 1);
            end = range->second >= 0
                ? std::clamp<qint64>(
                      range->second, start, data.size() - 1)
                : static_cast<qint64>(data.size()) - 1;
            status = 206;
            contentRange = "Content-Range: bytes "
                + QByteArray::number(start) + "-"
                + QByteArray::number(end) + "/"
                + QByteArray::number(data.size()) + "\r\n";
        }
        const qint64 length = data.isEmpty() ? 0 : end - start + 1;
        QByteArray response =
            "HTTP/1.1 " + QByteArray::number(status) + " "
            + (status == 206 ? "Partial Content" : "OK")
            + "\r\nContent-Type: " + contentType
            + "\r\nAccept-Ranges: bytes\r\n" + contentRange
            + "Content-Length: " + QByteArray::number(length)
            + "\r\nConnection: close\r\n\r\n";
        socket->write(response);
        if (!headOnly && length > 0) {
            socket->write(
                data.constData() + start,
                static_cast<qint64>(length));
        }
        socket->disconnectFromHost();
    }

    void fail(QTcpSocket *socket, int status, const char *reason)
    {
        if (!socket)
            return;
        emit q->logMessage(
            QStringLiteral("本地分片代理错误 %1：%2")
                .arg(status)
                .arg(QString::fromUtf8(reason)));
        const QByteArray body(reason);
        socket->write(
            "HTTP/1.1 " + QByteArray::number(status) + " "
            + (status == 404 ? "Not Found" : "Unsupported Media Type")
            + "\r\nContent-Type: text/plain\r\nContent-Length: "
            + QByteArray::number(body.size())
            + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

    void serve(const QPointer<QTcpSocket> &socketGuard, qsizetype index,
               std::optional<QPair<qint64, qint64>> range, bool headOnly)
    {
        QTcpSocket *socket = socketGuard.data();
        if (!socket) {
            m_headers.remove(socket);
            m_payloads.remove(socket);
            return;
        }
        if (index < 0 || index >= segments.size()) {
            fail(socket, 404, "unknown segment");
            return;
        }
        if (QByteArray *cached = cache.object(static_cast<int>(index))) {
            writeResponse(
                socket, *cached, range, headOnly,
                QByteArrayLiteral("video/mp2t"));
            return;
        }

        QNetworkRequest headerRequest(segments[index].uri);
        headerRequest.setAttribute(
            QNetworkRequest::RedirectPolicyAttribute,
            QNetworkRequest::NoLessSafeRedirectPolicy);
        headerRequest.setRawHeader("Range", "bytes=0-262143");
        headerRequest.setTransferTimeout(20000);
        QNetworkReply *headerReply = network.get(headerRequest);
        m_headers.insert(socket, headerReply);
        QObject::connect(
            headerReply, &QNetworkReply::finished, q,
            [this, socketGuard, index, range, headOnly, headerReply] {
                QTcpSocket *socket = socketGuard.data();
                m_headers.remove(socket);
                headerReply->deleteLater();
                if (!socket)
                    return;
                const int status = headerReply->attribute(
                    QNetworkRequest::HttpStatusCodeAttribute).toInt();
                const QByteArray headerBody = headerReply->readAll();
                const auto offset =
                    headerReply->error() == QNetworkReply::NoError
                        && (status == 200 || status == 206)
                    ? mcast::core::wrappedTransportStreamOffset(headerBody)
                    : std::nullopt;
                if (!offset) {
                    fail(socket, 415, "not PNG-wrapped MPEG-TS");
                    return;
                }
                if (status == 200 && headerBody.size() > *offset) {
                    QByteArray data = headerBody.mid(*offset);
                    cache.insert(
                        static_cast<int>(index),
                        new QByteArray(data),
                        qMax(1, data.size() / 1024));
                    writeResponse(
                        socket, data, range, headOnly,
                        QByteArrayLiteral("video/mp2t"));
                    return;
                }

                QNetworkRequest payloadRequest(segments[index].uri);
                payloadRequest.setAttribute(
                    QNetworkRequest::RedirectPolicyAttribute,
                    QNetworkRequest::NoLessSafeRedirectPolicy);
                payloadRequest.setRawHeader(
                    "Range",
                    QByteArrayLiteral("bytes=")
                        + QByteArray::number(*offset) + "-");
                payloadRequest.setTransferTimeout(20000);
                QNetworkReply *payloadReply = network.get(payloadRequest);
                m_payloads.insert(socket, payloadReply);
                QObject::connect(
                    payloadReply, &QNetworkReply::finished, q,
                    [this, socketGuard, index, range, headOnly,
                     payloadReply, offset] {
                        QTcpSocket *socket = socketGuard.data();
                        m_payloads.remove(socket);
                        payloadReply->deleteLater();
                        if (!socket)
                            return;
                        const int status = payloadReply->attribute(
                            QNetworkRequest::HttpStatusCodeAttribute)
                                              .toInt();
                        QByteArray data = payloadReply->readAll();
                        if (payloadReply->error()
                                != QNetworkReply::NoError
                            || (status != 200 && status != 206)) {
                            fail(socket, 502, "remote segment failed");
                            return;
                        }
                        if (status == 200
                            && data.size() > *offset) {
                            data.remove(0, *offset);
                        }
                        cache.insert(
                            static_cast<int>(index),
                            new QByteArray(data),
                            qMax(1, data.size() / 1024));
                        writeResponse(
                            socket, data, range, headOnly,
                            QByteArrayLiteral("video/mp2t"));
                    });
            });
    }

    WrappedTsProxy *q;
    QTcpServer server;
    QNetworkAccessManager network;
    QVector<mcast::core::ImageSequenceSegment> segments;
    QCache<int, QByteArray> cache{64 * 1024};
    QHash<QTcpSocket *, QByteArray> buffers;
    QHash<QTcpSocket *, QNetworkReply *> m_headers;
    QHash<QTcpSocket *, QNetworkReply *> m_payloads;
    QString token;
    QUrl baseUrl;
};

WrappedTsProxy::WrappedTsProxy(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<Impl>(this))
{
    connect(
        &m_impl->server, &QTcpServer::newConnection, this, [this] {
            while (m_impl->server.hasPendingConnections()) {
                QTcpSocket *socket =
                    m_impl->server.nextPendingConnection();
                m_impl->buffers.insert(socket, {});
                connect(
                    socket, &QTcpSocket::readyRead, this,
                    [this, guard = QPointer<QTcpSocket>(socket)] {
                        QTcpSocket *socket = guard.data();
                        if (!socket || !m_impl->buffers.contains(socket))
                            return;
                        QByteArray &buffer = m_impl->buffers[socket];
                        buffer.append(socket->readAll());
                        if (!buffer.contains("\r\n\r\n"))
                            return;
                        const QByteArray request = buffer;
                        m_impl->buffers.remove(socket);
                        const qsizetype firstSpace =
                            request.indexOf(' ');
                        const qsizetype secondSpace =
                            request.indexOf(' ', firstSpace + 1);
                        if (firstSpace < 0 || secondSpace < 0) {
                            m_impl->fail(socket, 404, "bad request");
                            return;
                        }
                        const QByteArray method =
                            request.left(firstSpace);
                        const QByteArray path = request.mid(
                            firstSpace + 1,
                            secondSpace - firstSpace - 1);
                        const QByteArray playlistPath =
                            "/" + m_impl->token.toUtf8()
                            + "/playlist.m3u8";
                        if (path == playlistPath) {
                            double maximumDuration = 0.0;
                            for (const auto &segment : m_impl->segments) {
                                maximumDuration = std::max(
                                    maximumDuration,
                                    std::clamp(
                                        segment.durationSeconds,
                                        0.05, 3600.0));
                            }
                            QByteArray playlist = QByteArrayLiteral(
                                "#EXTM3U\n"
                                "#EXT-X-VERSION:3\n"
                                "#EXT-X-MEDIA-SEQUENCE:0\n"
                                "#EXT-X-PLAYLIST-TYPE:VOD\n"
                                "#EXT-X-INDEPENDENT-SEGMENTS\n");
                            playlist.append("#EXT-X-TARGETDURATION:");
                            playlist.append(QByteArray::number(
                                static_cast<int>(
                                    std::ceil(maximumDuration))));
                            playlist.append('\n');
                            for (qsizetype index = 0;
                                 index < m_impl->segments.size();
                                 ++index) {
                                playlist.append("#EXTINF:");
                                playlist.append(QByteArray::number(
                                    std::clamp(
                                        m_impl->segments[index]
                                            .durationSeconds,
                                        0.05, 3600.0),
                                    'f', 6));
                                playlist.append(",\n");
                                playlist.append(
                                    QUrl(
                                        m_impl->baseUrl.toString(
                                            QUrl::FullyEncoded)
                                        + QString::number(index))
                                        .toString(QUrl::FullyEncoded)
                                        .toUtf8());
                                playlist.append('\n');
                            }
                            playlist.append("#EXT-X-ENDLIST\n");
                            m_impl->writeResponse(
                                socket, playlist, std::nullopt,
                                method == "HEAD",
                                QByteArrayLiteral(
                                    "application/vnd.apple.mpegurl"));
                            return;
                        }
                        const QByteArray prefix =
                            "/" + m_impl->token.toUtf8()
                            + "/segment/";
                        if (!path.startsWith(prefix)) {
                            m_impl->fail(socket, 404, "unknown path");
                            return;
                        }
                        bool ok = false;
                        const qsizetype index = path.mid(
                            prefix.size()).toLongLong(&ok);
                        if (!ok) {
                            m_impl->fail(socket, 404, "bad segment id");
                            return;
                        }
                        m_impl->serve(
                            guard, index,
                            Impl::parseRange(request),
                            method == "HEAD");
                    });
                connect(
                    socket, &QTcpSocket::disconnected, this,
                    [this, socket] {
                        m_impl->buffers.remove(socket);
                        if (QNetworkReply *reply =
                                m_impl->m_headers.take(socket)) {
                            reply->abort();
                        }
                        if (QNetworkReply *reply =
                                m_impl->m_payloads.take(socket)) {
                            reply->abort();
                        }
                        socket->deleteLater();
                    });
            }
        });
}

WrappedTsProxy::~WrappedTsProxy()
{
    m_impl->server.close();

    QSet<QNetworkReply *> replies;
    for (QNetworkReply *reply : m_impl->m_headers)
        replies.insert(reply);
    for (QNetworkReply *reply : m_impl->m_payloads)
        replies.insert(reply);
    for (QNetworkReply *reply : replies) {
        QObject::disconnect(reply, nullptr, this, nullptr);
        reply->abort();
    }

    QSet<QTcpSocket *> sockets;
    for (QTcpSocket *socket :
         m_impl->server.findChildren<QTcpSocket *>()) {
        sockets.insert(socket);
    }
    for (QTcpSocket *socket : m_impl->buffers.keys())
        sockets.insert(socket);
    for (QTcpSocket *socket : m_impl->m_headers.keys())
        sockets.insert(socket);
    for (QTcpSocket *socket : m_impl->m_payloads.keys())
        sockets.insert(socket);
    for (QTcpSocket *socket : sockets) {
        QObject::disconnect(socket, nullptr, this, nullptr);
        socket->abort();
    }
    m_impl->m_headers.clear();
    m_impl->m_payloads.clear();
    m_impl->buffers.clear();
}

bool WrappedTsProxy::start(
    QVector<mcast::core::ImageSequenceSegment> segments,
    QString &error)
{
    if (segments.isEmpty()) {
        error = QStringLiteral("image sequence is empty");
        return false;
    }
    if (!m_impl->server.listen(QHostAddress::LocalHost, 0)) {
        error = m_impl->server.errorString();
        return false;
    }
    m_impl->segments = std::move(segments);
    m_impl->token =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_impl->baseUrl = QUrl(
        QStringLiteral("http://127.0.0.1:%1/%2/segment/")
            .arg(m_impl->server.serverPort())
            .arg(m_impl->token));
    return true;
}

QUrl WrappedTsProxy::segmentUrl(qsizetype index) const
{
    return QUrl(
        m_impl->baseUrl.toString(QUrl::FullyEncoded)
        + QString::number(index));
}

QUrl WrappedTsProxy::playlistUrl() const
{
    return QUrl(
        QStringLiteral("http://127.0.0.1:%1/%2/playlist.m3u8")
            .arg(m_impl->server.serverPort())
            .arg(m_impl->token));
}
