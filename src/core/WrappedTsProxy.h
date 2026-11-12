// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ImageSequencePlaylist.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class WrappedTsProxy final : public QObject
{
    Q_OBJECT

public:
    explicit WrappedTsProxy(QObject *parent = nullptr);
    ~WrappedTsProxy() override;

    bool start(
        QVector<mcast::core::ImageSequenceSegment> segments,
        QString &error);

    /**
     * Local HTTP URLs suitable for an MPV EDL entry.
     */
    QUrl segmentUrl(qsizetype index) const;
    QUrl playlistUrl() const;

signals:
    void logMessage(const QString &message);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
