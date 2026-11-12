// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "core/ImageSequencePlaylist.h"
#include "core/MpvCore.h"
#include "core/WrappedTsProxy.h"

#include <mpv/client.h>

#include <QCoreApplication>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <chrono>
#include <cstdlib>
#include <iostream>

namespace {

class HeadlessMpvCore final : public MpvCore
{
protected:
    void applyStartupOptions(mpv_handle *mpv) override
    {
        mpv_set_option_string(mpv, "vo", "null");
        mpv_set_option_string(mpv, "ao", "null");
    }
};

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(data) == data.size();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QString wrappedSample =
        qEnvironmentVariable("MCAST_WRAPPED_TS_SAMPLE");
    if (!wrappedSample.isEmpty()) {
        HeadlessMpvCore player;
        bool ready = false;
        bool failed = false;
        QObject::connect(
            &player, &MpvCore::ready, &app,
            [&](std::uint64_t) { ready = true; });
        QObject::connect(
            &player, &MediaPlayer::logMessage, &app,
            [&](const QString &message) {
                std::cerr << "mpv: " << message.toStdString() << '\n';
                if (message.contains(QStringLiteral("播放失败")))
                    failed = true;
            });
        check(player.start(), "wrapped TS probe starts MPV");
        const QString property =
            qEnvironmentVariable("MCAST_WRAPPED_OPTION_NAME");
        const QString value =
            qEnvironmentVariable("MCAST_WRAPPED_OPTION_VALUE");
        if (!property.isEmpty()) {
            const int result = mpv_set_property_string(
                player.handle(), property.toUtf8().constData(),
                value.toUtf8().constData());
            std::cerr << "option " << property.toStdString() << '='
                      << value.toStdString() << " result=" << result
                      << '\n';
        }
        MediaLoadRequest request;
        request.logicalUri =
            QUrl(wrappedSample).scheme().startsWith(
                QStringLiteral("http"))
            ? wrappedSample
            : QUrl::fromLocalFile(wrappedSample).toString();
        player.setRendererAttached(true);
        player.load(request);
        const auto wrappedDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(25);
        while (!ready && !failed
               && std::chrono::steady_clock::now() < wrappedDeadline) {
            app.processEvents();
            QThread::msleep(20);
        }
        const auto wrappedSettleDeadline =
            std::chrono::steady_clock::now()
            + std::chrono::milliseconds(1200);
        while (!failed
               && std::chrono::steady_clock::now()
                   < wrappedSettleDeadline) {
            app.processEvents();
            QThread::msleep(20);
        }
        std::cerr << "wrapped ready=" << ready
                  << " failed=" << failed
                  << " position=" << player.positionSeconds()
                  << " duration=" << player.durationSeconds()
                  << " info="
                  << QJsonDocument::fromVariant(player.mediaInfo())
                         .toJson(QJsonDocument::Compact)
                         .toStdString()
                  << '\n';
        player.shutdown();
        return ready && !failed ? 0 : 1;
    }

    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory is created");

    const QByteArray png = QByteArray::fromBase64(
        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lE"
        "QVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=");
    QByteArray wrappedTs = png;
    for (int packet = 0; packet < 3; ++packet) {
        QByteArray tsPacket(188, '\0');
        tsPacket[0] = '\x47';
        wrappedTs.append(tsPacket);
    }
    QTcpServer server;
    int imageRequests = 0;
    check(
        server.listen(QHostAddress::LocalHost, 0),
        "local image server starts");
    const QString baseUrl = QStringLiteral("http://127.0.0.1:")
        + QString::number(server.serverPort());
    const QByteArray playlist =
        QStringLiteral(
            "#EXTM3U\n"
            "#EXT-X-VERSION:3\n"
            "#EXTINF:1.0,\n"
            "%1/frame-1.png\n"
            "#EXTINF:1.0,\n"
            "%2/frame-2.png\n"
            "#EXT-X-ENDLIST\n")
            .arg(baseUrl, baseUrl)
            .toUtf8();

    QObject::connect(
        &server, &QTcpServer::newConnection, &app, [&] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                QObject::connect(
                    socket, &QTcpSocket::readyRead, socket,
                    [socket, &png, &wrappedTs, &playlist, &imageRequests] {
                        if (!socket->canReadLine())
                            return;
                        const QByteArray request = socket->readAll();
                        if (!request.startsWith("GET /"))
                            return;
                        const bool isPlaylist =
                            request.startsWith("GET /movie.m3u8 ");
                        const bool isWrapped =
                            request.startsWith("GET /wrapped.png ");
                        const bool isProbe =
                            request.toLower().contains(
                                "range: bytes=0-4095");
                        if (!isPlaylist && !isProbe)
                            ++imageRequests;
                        const QByteArray body =
                            isPlaylist
                            ? playlist
                            : (isWrapped ? wrappedTs : png);
                        const QByteArray contentType =
                            isPlaylist
                            ? QByteArrayLiteral(
                                  "application/vnd.apple.mpegurl")
                            : QByteArrayLiteral("image/png");
                        socket->write(
                            QByteArrayLiteral(
                                "HTTP/1.1 200 OK\r\n"
                                "Content-Type: ")
                            + contentType
                            + QByteArrayLiteral(
                                "\r\nConnection: close\r\nContent-Length: ")
                            + QByteArray::number(body.size())
                            + QByteArrayLiteral("\r\n\r\n")
                            + body);
                        socket->disconnectFromHost();
                    });
            }
        });

    HeadlessMpvCore player;
    bool ready = false;
    QString failure;
    QObject::connect(
        &player, &MpvCore::ready, &app,
        [&](std::uint64_t) { ready = true; });
    QObject::connect(
        &player, &MediaPlayer::lost, &app,
        [&] { failure = QStringLiteral("MPV was lost"); });
    QObject::connect(
        &player, &MediaPlayer::logMessage, &app,
        [&](const QString &message) {
            std::cerr << "mpv: " << message.toStdString() << '\n';
            if (message.contains(QStringLiteral("播放失败")))
                failure = message;
        });
    check(player.start(), "headless MPV starts");
    player.setRendererAttached(true);
    MediaLoadRequest request;
    request.logicalUri = baseUrl + QStringLiteral("/movie.m3u8");
    request.route = MediaLoadRoute::ImageSequence;
    request.imageSequenceFrames = {
        {baseUrl + QStringLiteral("/frame-1.png"), 1.0},
        {baseUrl + QStringLiteral("/frame-2.png"), 1.0},
    };
    player.load(request);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!ready && failure.isEmpty()
           && std::chrono::steady_clock::now() < deadline) {
        app.processEvents();
        QThread::msleep(20);
    }
    check(failure.isEmpty(), "MPV opens the image-sequence list");
    check(ready, "the image-sequence list reaches FILE_LOADED");

    const auto settleDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(750);
    while (std::chrono::steady_clock::now() < settleDeadline) {
        app.processEvents();
        QThread::msleep(20);
    }
    const QVariantMap info = player.mediaInfo();
    std::cerr << "duration="
              << info.value(QStringLiteral("duration_seconds"))
                     .toDouble()
              << " kind="
              << info.value(QStringLiteral("kind"))
                     .toString()
                     .toStdString()
              << " position=" << player.positionSeconds()
              << " playerDuration=" << player.durationSeconds()
              << " requests=" << imageRequests
              << '\n';
    const QString kind =
        info.value(QStringLiteral("kind")).toString();
    check(
        kind == QStringLiteral("video"),
        "an image sequence remains a video playback route");
    check(
        imageRequests == 2,
        "the compatibility layer feeds both remote images to MPV");
    check(
        player.positionSeconds() > 0.25,
        "the virtual image timeline advances");
    check(
        info.value(QStringLiteral("duration_seconds")).toDouble() > 1.5,
        "the virtual image timeline exposes a usable duration");

    WrappedTsProxy proxy;
    QString proxyError;
    check(
        proxy.start(
            {{QUrl(baseUrl + QStringLiteral("/wrapped.png")), 1.0}},
            proxyError),
        "wrapped transport-stream proxy starts");
    QNetworkAccessManager network;
    QNetworkReply *reply = network.get(
        QNetworkRequest(proxy.segmentUrl(0)));
    bool proxyFinished = false;
    QObject::connect(
        reply, &QNetworkReply::finished, &app,
        [&] { proxyFinished = true; });
    const auto proxyDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!proxyFinished
           && std::chrono::steady_clock::now() < proxyDeadline) {
        app.processEvents();
        QThread::msleep(10);
    }
    const QByteArray proxied = reply->readAll();
    check(
        proxyFinished
            && reply->error() == QNetworkReply::NoError
            && proxied.size() == wrappedTs.size() - png.size()
            && proxied.startsWith('\x47'),
        "proxy strips the PNG prefix from transport-stream data");
    reply->deleteLater();
    QNetworkReply *playlistReply = network.get(
        QNetworkRequest(proxy.playlistUrl()));
    bool playlistFinished = false;
    QObject::connect(
        playlistReply, &QNetworkReply::finished, &app,
        [&] { playlistFinished = true; });
    const auto playlistDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!playlistFinished
           && std::chrono::steady_clock::now() < playlistDeadline) {
        app.processEvents();
        QThread::msleep(10);
    }
    const QByteArray proxyPlaylist = playlistReply->readAll();
    check(
        playlistFinished
            && playlistReply->error() == QNetworkReply::NoError
            && proxyPlaylist.startsWith("#EXTM3U\n")
            && proxyPlaylist.contains("#EXTINF:1.000000,")
            && proxyPlaylist.contains("/segment/0"),
        "proxy serves a standard local HLS playlist");
    playlistReply->deleteLater();

    QTcpServer hangingOrigin;
    int hangingRequests = 0;
    check(
        hangingOrigin.listen(QHostAddress::LocalHost, 0),
        "hanging origin server starts");
    QObject::connect(
        &hangingOrigin, &QTcpServer::newConnection, &app,
        [&] {
            while (hangingOrigin.hasPendingConnections()) {
                QTcpSocket *socket =
                    hangingOrigin.nextPendingConnection();
                QObject::connect(
                    socket, &QTcpSocket::readyRead, socket,
                    [&] { ++hangingRequests; });
            }
        });
    {
        WrappedTsProxy transient;
        QString transientError;
        check(
            transient.start(
                {{
                    QUrl(QStringLiteral(
                        "http://127.0.0.1:%1/slow.png")
                        .arg(hangingOrigin.serverPort())),
                    1.0,
                }},
                transientError),
            "transient proxy starts");
        QNetworkAccessManager transientNetwork;
        QNetworkReply *transientReply = transientNetwork.get(
            QNetworkRequest(transient.segmentUrl(0)));
        const auto hangingDeadline =
            std::chrono::steady_clock::now()
            + std::chrono::seconds(3);
        while (hangingRequests == 0
               && std::chrono::steady_clock::now() < hangingDeadline) {
            app.processEvents();
            QThread::msleep(10);
        }
        check(hangingRequests > 0,
              "transient proxy has an in-flight origin request");
        transientReply->deleteLater();
    }
    app.processEvents();
    player.shutdown();
    return 0;
}
