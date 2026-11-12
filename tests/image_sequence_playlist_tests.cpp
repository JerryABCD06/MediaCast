// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "core/ImageSequencePlaylist.h"

#include <QCoreApplication>
#include <QUrl>

#include <cstdlib>
#include <iostream>

using namespace mcast::core;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testImagePlaylist()
{
    const QString playlist = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXTINF:1.250,\n"
        "frames/001.png?token=a,b\n"
        "#EXTINF:2.5,\n"
        "https://cdn.example/frames/002.webp?auth=x\n"
        "#EXT-X-ENDLIST\n");
    const QUrl base(QStringLiteral(
        "https://media.example/path/movie.m3u8?auth_key=1"));
    const auto parsed = parseImageSequencePlaylist(playlist, base);
    check(parsed.isValid(), "image playlist is recognized");
    check(parsed.segments.size() == 2,
          "image playlist keeps all media segments");
    check(
        parsed.segments[0].uri
            == QUrl(QStringLiteral(
                "https://media.example/path/frames/001.png?token=a,b")),
        "relative image URLs resolve against the playlist");
    check(parsed.segments[0].durationSeconds == 1.25,
          "EXTINF duration reaches the first image");
    check(parsed.segments[1].durationSeconds == 2.5,
          "EXTINF duration reaches the second image");
}

void testRegularAndMasterPlaylistsAreRejected()
{
    const QUrl base(QStringLiteral("https://media.example/live.m3u8"));
    const auto regular = parseImageSequencePlaylist(
        QStringLiteral(
            "#EXTM3U\n"
            "#EXTINF:6,\n"
            "segment-1.ts\n"),
        base);
    check(!regular.isValid(), "ordinary MPEG-TS HLS is not intercepted");

    const auto master = parseImageSequencePlaylist(
        QStringLiteral(
            "#EXTM3U\n"
            "#EXT-X-STREAM-INF:BANDWIDTH=1000000\n"
            "high.m3u8\n"),
        base);
    check(!master.isValid(), "master playlists fall back to MPV");
}

void testMfImageList()
{
    const QVector<ImageSequenceSegment> segments = {
        {
            QUrl(QStringLiteral(
                "https://cdn.example/a,b;c.png?auth=x!y")),
            1.5,
        },
        {
            QUrl(QStringLiteral(
                "https://cdn.example/second.png")),
            3.0,
        },
    };
    const QByteArray list = buildMfImageList(segments);
    check(
        list == QByteArrayLiteral(
                    "https://cdn.example/a,b;c.png?auth=x!y\n"
                    "https://cdn.example/second.png\n"),
        "mf:// list keeps the original image URLs");
    check(
        buildMfImageList(segments, 1)
            == QByteArrayLiteral("https://cdn.example/second.png\n"),
        "mf:// list can start at a seek target");
}

void testPlaylistUrlDetection()
{
    check(
        isHlsPlaylistUrl(QUrl(QStringLiteral(
            "https://example.test/movie.m3u8?auth=1"))),
        "query strings do not hide an m3u8 suffix");
    check(
        !isHlsPlaylistUrl(QUrl(QStringLiteral(
            "https://example.test/movie.mp4"))),
        "ordinary media URLs are not probed");
}

void testWrappedTransportStreamDetection()
{
    QByteArray png = QByteArrayLiteral("\x89PNG\r\n\x1a\n");
    const auto appendChunk = [&](const QByteArray &type,
                                 const QByteArray &payload) {
        const quint32 length =
            static_cast<quint32>(payload.size());
        png.append(static_cast<char>((length >> 24) & 0xff));
        png.append(static_cast<char>((length >> 16) & 0xff));
        png.append(static_cast<char>((length >> 8) & 0xff));
        png.append(static_cast<char>(length & 0xff));
        png.append(type);
        png.append(payload);
        png.append(QByteArray(4, '\0'));
    };
    appendChunk(QByteArrayLiteral("IHDR"), QByteArray(13, '\0'));
    appendChunk(QByteArrayLiteral("IDAT"), QByteArray(9, 'x'));
    appendChunk(QByteArrayLiteral("IEND"), {});
    const qsizetype tsOffset = png.size();
    for (int packet = 0; packet < 3; ++packet) {
        QByteArray tsPacket(188, '\0');
        tsPacket[0] = '\x47';
        png.append(tsPacket);
    }
    const auto detected = wrappedTransportStreamOffset(png);
    check(
        detected && *detected == tsOffset,
        "PNG-wrapped MPEG-TS payload offset is detected");

    QByteArray imageOnly =
        QByteArrayLiteral("\x89PNG\r\n\x1a\n");
    check(
        !wrappedTransportStreamOffset(imageOnly).has_value(),
        "ordinary PNG data is not mistaken for wrapped MPEG-TS");

    const QVector<ImageSequenceSegment> segments = {
        {QUrl(QStringLiteral("https://cdn.example/one.png")), 8.5},
        {QUrl(QStringLiteral("https://cdn.example/two.png")), 2.25},
    };
    const QByteArray playlist =
        buildSliceEdl(segments, {447, 383});
    check(
        playlist.startsWith("# mpv EDL v0\n!no_chapters\n"),
        "wrapped transport stream uses an MPV EDL timeline");
    check(
        playlist.contains(
            "%39%slice://447@https://cdn.example/one.png"),
        "wrapped EDL uses the per-segment payload offset");
    check(
        playlist.contains(",start=0,length=2.250000"),
        "wrapped EDL preserves segment durations");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testImagePlaylist();
    testRegularAndMasterPlaylistsAreRejected();
    testMfImageList();
    testPlaylistUrlDetection();
    testWrappedTransportStreamDetection();
    return 0;
}
