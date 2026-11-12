// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "core/NowPlaying.h"

#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

struct MetadataPipelineInput
{
    QString uri;
    MediaKind kind = MediaKind::Unknown;

    // Unfiltered neutral fields supplied by the network protocol adapter.
    QString protocolTitle;
    QString protocolArtist;
    QString protocolCreator;
    QString protocolAlbum;
    QString protocolSubtitle;
    QString protocolDescription;
    QString protocolLongDescription;
    QString protocolArtworkUri;

    // Locally observed file metadata.
    QVariantMap fileTags;
    QString fileArtworkSource;
};

struct MetadataPipelineResult
{
    QString title;
    QString subtitle;
    QString artist;
    QString album;
    QString artworkSource;
    QStringList rejected;
};

// Ui-side presentation policy. Protocol adapters pass raw neutral fields in;
// this class applies one data-driven fallback/filter chain and produces the
// single NowPlaying value consumed by both QML and WindowsMediaControls.
class MetadataPipeline
{
public:
    MetadataPipeline();

    bool loadFile(const QString &path, QString *error = nullptr);
    bool loadNearExecutable(QString *error = nullptr);
    bool loadedFromFile() const { return m_loadedFromFile; }
    QString sourcePath() const { return m_sourcePath; }

    MetadataPipelineResult resolve(
        const MetadataPipelineInput &input) const;

private:
    struct FieldRule
    {
        QStringList sources;
        QVector<QRegularExpression> rejectPatterns;
    };

    QString resolveSource(
        const QString &source, const MetadataPipelineInput &input,
        QStringList &rejected) const;
    QString acceptCandidate(
        const QString &candidate, const QString &field,
        const FieldRule &rule, bool titleRules,
        QStringList &rejected) const;
    bool placeholder(const QString &value) const;
    bool validTitle(
        const QString &value, const QRegularExpression &digitsPattern) const;
    QString strippedMediaExtension(const QString &value) const;

    FieldRule m_title;
    FieldRule m_artist;
    FieldRule m_album;
    FieldRule m_subtitleAudio;
    FieldRule m_subtitleVisual;
    QStringList m_artworkSources;
    QStringList m_placeholders;
    QStringList m_mediaExtensions;
    QRegularExpression m_titleDigitsPattern;
    int m_titleMinimumLength = 2;
    int m_titleMinimumLetters = 1;
    int m_titleMaximumDigitRun = 4;
    int m_titleMaximumDigitRatioPercent = 40;
    bool m_loadedFromFile = false;
    QString m_sourcePath;
};
