// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ImageSequencePlaylist.h"

#include <QFileInfo>
#include <QLatin1Char>
#include <QStringList>

#include <algorithm>
namespace mcast::core {
namespace {

bool isImageUri(const QUrl &uri)
{
    static const QStringList extensions = {
        QStringLiteral("png"),
        QStringLiteral("jpg"),
        QStringLiteral("jpeg"),
        QStringLiteral("webp"),
        QStringLiteral("gif"),
        QStringLiteral("bmp"),
        QStringLiteral("avif"),
        QStringLiteral("heic"),
        QStringLiteral("heif"),
        QStringLiteral("tif"),
        QStringLiteral("tiff"),
    };
    const QString suffix =
        QFileInfo(uri.path()).suffix().toLower();
    return extensions.contains(suffix);
}

QByteArray edlValue(const QByteArray &value)
{
    QByteArray escaped;
    escaped.reserve(value.size() + 16);
    escaped.append('%');
    escaped.append(QByteArray::number(value.size()));
    escaped.append('%');
    escaped.append(value);
    return escaped;
}

} // namespace

bool isHlsPlaylistUrl(const QUrl &url)
{
    return url.isValid()
        && QFileInfo(url.path()).suffix().compare(
               QStringLiteral("m3u8"), Qt::CaseInsensitive)
            == 0;
}

ImageSequencePlaylist parseImageSequencePlaylist(
    QStringView content, const QUrl &playlistUrl)
{
    ImageSequencePlaylist result;
    const QStringList lines = content.toString().split(
        QLatin1Char('\n'));
    if (lines.isEmpty()
        || !lines.front().trimmed().startsWith(
            QStringLiteral("#EXTM3U"))) {
        result.error = QStringLiteral("not an HLS playlist");
        return result;
    }

    double pendingDuration = 2.0;
    bool sawSegment = false;
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        if (line.startsWith(QStringLiteral("#EXT-X-STREAM-INF"))) {
            result.error = QStringLiteral("HLS master playlist");
            result.segments.clear();
            return result;
        }
        if (line.startsWith(QStringLiteral("#EXTINF:"))) {
            QString value = line.mid(8);
            const qsizetype comma = value.indexOf(QLatin1Char(','));
            if (comma >= 0)
                value.truncate(comma);
            bool ok = false;
            const double parsed = value.toDouble(&ok);
            if (ok && parsed > 0.0)
                pendingDuration = parsed;
            continue;
        }
        if (line.startsWith(QLatin1Char('#')))
            continue;

        sawSegment = true;
        const QUrl segmentUrl = playlistUrl.resolved(
            QUrl::fromEncoded(line.toUtf8()));
        if (segmentUrl.isValid() && isImageUri(segmentUrl)) {
            result.segments.append(
                {segmentUrl, std::clamp(pendingDuration, 0.05, 3600.0)});
        } else {
            result.error =
                QStringLiteral("HLS segment is not an image");
            result.segments.clear();
            return result;
        }
        pendingDuration = 2.0;
    }

    if (!sawSegment)
        result.error = QStringLiteral("HLS playlist has no media segments");
    return result;
}

QByteArray buildMfImageList(
    const QVector<ImageSequenceSegment> &segments,
    qsizetype firstSegment)
{
    QByteArray list;
    const qsizetype first =
        std::clamp<qsizetype>(firstSegment, 0, segments.size());
    for (qsizetype index = first; index < segments.size(); ++index) {
        list.append(segments[index].uri.toString(
            QUrl::FullyEncoded).toUtf8());
        list.append('\n');
    }
    return list;
}

std::optional<qint64> wrappedTransportStreamOffset(
    QByteArrayView bytes)
{
    static const QByteArray signature =
        QByteArrayLiteral("\x89PNG\r\n\x1a\n");
    if (bytes.size() < 8
        || bytes.first(8) != QByteArrayView(signature)) {
        return std::nullopt;
    }

    qsizetype offset = 8;
    for (int chunks = 0; chunks < 128 && offset + 12 <= bytes.size();
         ++chunks) {
        const auto byteAt = [&](qsizetype index) {
            return static_cast<quint8>(bytes[index]);
        };
        const quint32 length =
            (static_cast<quint32>(byteAt(offset)) << 24)
            | (static_cast<quint32>(byteAt(offset + 1)) << 16)
            | (static_cast<quint32>(byteAt(offset + 2)) << 8)
            | static_cast<quint32>(byteAt(offset + 3));
        const qsizetype next =
            offset + 12 + static_cast<qsizetype>(length);
        if (next < offset || next > bytes.size())
            return std::nullopt;
        const QByteArrayView type = bytes.sliced(offset + 4, 4);
        if (type == QByteArrayView("IEND", 4)) {
            constexpr qsizetype kTsPacketSize = 188;
            if (next + 2 * kTsPacketSize >= bytes.size())
                return std::nullopt;
            for (qsizetype packet = 0; packet < 3; ++packet) {
                if (static_cast<quint8>(
                        bytes[next + packet * kTsPacketSize])
                    != 0x47) {
                    return std::nullopt;
                }
            }
            return next;
        }
        offset = next;
    }
    return std::nullopt;
}

QByteArray buildSliceEdl(
    const QVector<ImageSequenceSegment> &segments,
    const QVector<qint64> &payloadOffsets)
{
    if (segments.size() != payloadOffsets.size()
        || segments.isEmpty()) {
        return {};
    }
    QByteArray edl = QByteArrayLiteral(
        "# mpv EDL v0\n!no_chapters\n");
    for (qsizetype index = 0; index < segments.size(); ++index) {
        if (payloadOffsets[index] < 0)
            return {};
        QByteArray source = QByteArrayLiteral("slice://");
        source.append(QByteArray::number(payloadOffsets[index]));
        source.append('@');
        source.append(segments[index].uri.toString(
            QUrl::FullyEncoded).toUtf8());
        edl.append("file=");
        edl.append(edlValue(source));
        edl.append(",start=0,length=");
        edl.append(QByteArray::number(
            std::clamp(segments[index].durationSeconds, 0.05, 3600.0),
            'f', 6));
        edl.append('\n');
    }
    return edl;
}

} // namespace mcast::core
