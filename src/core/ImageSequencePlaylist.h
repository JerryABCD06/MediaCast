// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringView>
#include <QUrl>
#include <QVector>

#include <optional>

namespace mcast::core {

struct ImageSequenceSegment
{
    QUrl uri;
    double durationSeconds = 0.0;
};

struct ImageSequencePlaylist
{
    QVector<ImageSequenceSegment> segments;
    QString error;

    bool isValid() const { return !segments.isEmpty(); }
};

bool isHlsPlaylistUrl(const QUrl &url);

ImageSequencePlaylist parseImageSequencePlaylist(
    QStringView content, const QUrl &playlistUrl);

QByteArray buildMfImageList(
    const QVector<ImageSequenceSegment> &segments,
    qsizetype firstSegment = 0);

std::optional<qint64> wrappedTransportStreamOffset(
    QByteArrayView bytes);

QByteArray buildSliceEdl(
    const QVector<ImageSequenceSegment> &segments,
    const QVector<qint64> &payloadOffsets);

} // namespace mcast::core
