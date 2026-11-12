// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "MetadataPipeline.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QUrl>

#include <algorithm>
#include <cmath>

namespace {

QString normalizedText(const QString &value)
{
    return value.simplified();
}

QString strippedWrappers(QString value)
{
    value = value.trimmed();
    while (value.size() >= 2
           && ((value.startsWith(QLatin1Char('<'))
                && value.endsWith(QLatin1Char('>')))
               || (value.startsWith(QLatin1Char('"'))
                   && value.endsWith(QLatin1Char('"')))
               || (value.startsWith(QLatin1Char('\''))
                   && value.endsWith(QLatin1Char('\''))))) {
        value = value.mid(1, value.size() - 2).trimmed();
    }
    return value;
}

QString tagValue(
    const QVariantMap &tags, std::initializer_list<const char *> keys)
{
    for (const char *candidate : keys) {
        const QString wanted = QString::fromLatin1(candidate);
        for (auto it = tags.constBegin(); it != tags.constEnd(); ++it) {
            if (it.key().compare(wanted, Qt::CaseInsensitive) == 0) {
                const QString value = it.value().toString().simplified();
                if (!value.isEmpty())
                    return value;
            }
        }
    }
    return {};
}

bool looksLikeUri(const QString &value)
{
    const QString text = value.trimmed().toLower();
    const bool windowsPath =
        text.size() >= 3
        && text.at(0).isLetter()
        && text.at(1) == QLatin1Char(':')
        && (text.at(2) == QLatin1Char('\\')
            || text.at(2) == QLatin1Char('/'));
    return text.startsWith(QStringLiteral("http://"))
        || text.startsWith(QStringLiteral("https://"))
        || text.startsWith(QStringLiteral("file://"))
        || text.startsWith(QStringLiteral("ftp://"))
        || text.startsWith(QStringLiteral("www."))
        || text.contains(QStringLiteral("://"))
        || text.startsWith(QStringLiteral("\\\\"))
        || text.contains(QStringLiteral("\\\\"))
        || windowsPath;
}

bool stringList(
    const QJsonValue &value, QStringList &result, QString &error,
    const QString &name)
{
    if (!value.isArray()) {
        error = QStringLiteral("%1 must be an array").arg(name);
        return false;
    }
    for (const QJsonValue &item : value.toArray()) {
        if (!item.isString()) {
            error = QStringLiteral("%1 contains a non-string value").arg(name);
            return false;
        }
        if (!item.toString().trimmed().isEmpty())
            result.push_back(item.toString().trimmed());
    }
    return true;
}

bool patterns(
    const QJsonValue &value, QVector<QRegularExpression> &result,
    QString &error)
{
    QStringList texts;
    if (!stringList(value, texts, error, QStringLiteral("reject_patterns")))
        return false;
    for (const QString &text : texts) {
        const QRegularExpression expression(
            text, QRegularExpression::CaseInsensitiveOption);
        if (!expression.isValid()) {
            error = QStringLiteral("invalid regular expression: %1").arg(text);
            return false;
        }
        result.push_back(expression);
    }
    return true;
}

bool parseRule(
    const QJsonObject &root, const QString &name, QStringList &sources,
    QVector<QRegularExpression> &rejectPatterns, QString &error)
{
    const QJsonValue value = root.value(name);
    if (!value.isObject()) {
        error = QStringLiteral("missing object: %1").arg(name);
        return false;
    }
    const QJsonObject rule = value.toObject();
    if (!stringList(
            rule.value(QStringLiteral("sources")), sources, error,
            name + QStringLiteral(".sources"))) {
        return false;
    }
    if (!patterns(
            rule.value(QStringLiteral("reject_patterns")),
            rejectPatterns, error)) {
        return false;
    }
    if (sources.isEmpty()) {
        error = QStringLiteral("%1.sources is empty").arg(name);
        return false;
    }
    return true;
}

bool localArtworkSource(const QString &source)
{
    if (source.isEmpty() || source.size() > 4096)
        return false;
    const QUrl url(source);
    if (!url.isLocalFile())
        return false;
    const QFileInfo file(url.toLocalFile());
    return file.exists() && file.isFile()
        && file.size() > 0 && file.size() <= 16 * 1024 * 1024;
}

bool networkArtworkSource(const QString &source)
{
    if (source.isEmpty() || source.size() > 4096)
        return false;
    const QUrl url(source);
    return !url.host().isEmpty()
        && (url.scheme() == QStringLiteral("http")
            || url.scheme() == QStringLiteral("https"));
}

bool knownSource(const QString &source)
{
    static const QStringList sources = {
        QStringLiteral("protocol.title"),
        QStringLiteral("protocol.artist"),
        QStringLiteral("protocol.creator"),
        QStringLiteral("protocol.album"),
        QStringLiteral("protocol.subtitle"),
        QStringLiteral("protocol.description"),
        QStringLiteral("protocol.long_description"),
        QStringLiteral("protocol.artwork_uri"),
        QStringLiteral("file.title"),
        QStringLiteral("file.artist"),
        QStringLiteral("file.album"),
        QStringLiteral("file.subtitle"),
        QStringLiteral("file.artwork"),
        QStringLiteral("uri.title"),
        QStringLiteral("media_kind"),
        QStringLiteral("unknown"),
    };
    return sources.contains(source);
}

bool validateSources(
    const QStringList &sources, const QString &name, QString &error)
{
    for (const QString &source : sources) {
        if (!knownSource(source)) {
            error = QStringLiteral("%1.sources contains unknown source %2")
                        .arg(name, source);
            return false;
        }
    }
    return true;
}

} // namespace

MetadataPipeline::MetadataPipeline()
{
    m_placeholders = {
        QStringLiteral("unkown"),
        QStringLiteral("unknown"),
        QStringLiteral("none"),
        QStringLiteral("null"),
        QStringLiteral("n/a"),
        QStringLiteral("na"),
        QStringLiteral("-"),
        QStringLiteral("--"),
        QStringLiteral("not available"),
        QStringLiteral("not applicable"),
        QStringLiteral("未知"),
        QStringLiteral("未知艺术家"),
        QStringLiteral("未知歌手"),
        QStringLiteral("未知专辑"),
        QStringLiteral("无"),
    };
    m_mediaExtensions = {
        QStringLiteral("jpg"),  QStringLiteral("jpeg"),
        QStringLiteral("png"),  QStringLiteral("gif"),
        QStringLiteral("bmp"),  QStringLiteral("webp"),
        QStringLiteral("heic"), QStringLiteral("heif"),
        QStringLiteral("tif"),  QStringLiteral("tiff"),
        QStringLiteral("avif"), QStringLiteral("mp3"),
        QStringLiteral("flac"), QStringLiteral("wav"),
        QStringLiteral("m4a"),  QStringLiteral("aac"),
        QStringLiteral("ogg"),  QStringLiteral("opus"),
        QStringLiteral("wma"),  QStringLiteral("ape"),
        QStringLiteral("mp4"),  QStringLiteral("mkv"),
        QStringLiteral("avi"),  QStringLiteral("mov"),
        QStringLiteral("wmv"),  QStringLiteral("flv"),
        QStringLiteral("webm"), QStringLiteral("ts"),
        QStringLiteral("m2ts"), QStringLiteral("mpg"),
        QStringLiteral("mpeg"), QStringLiteral("m4v"),
        QStringLiteral("3gp"),  QStringLiteral("rmvb"),
    };
    m_title.sources = {
        QStringLiteral("protocol.title"),
        QStringLiteral("file.title"),
        QStringLiteral("uri.title"),
        QStringLiteral("media_kind"),
        QStringLiteral("unknown"),
    };
    m_artist.sources = {
        QStringLiteral("protocol.artist"),
        QStringLiteral("protocol.creator"),
        QStringLiteral("file.artist"),
    };
    m_album.sources = {
        QStringLiteral("protocol.album"),
        QStringLiteral("file.album"),
    };
    m_subtitleAudio.sources = {
        QStringLiteral("protocol.artist"),
        QStringLiteral("protocol.creator"),
        QStringLiteral("file.artist"),
        QStringLiteral("protocol.album"),
        QStringLiteral("file.album"),
        QStringLiteral("unknown"),
    };
    m_subtitleVisual.sources = {
        QStringLiteral("protocol.subtitle"),
        QStringLiteral("protocol.description"),
        QStringLiteral("protocol.long_description"),
        QStringLiteral("file.subtitle"),
        QStringLiteral("unknown"),
    };
    m_subtitleVisual.rejectPatterns = {
        QRegularExpression(
            QStringLiteral("^vid:[A-Za-z0-9]+$"),
            QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(
            QStringLiteral("^[A-Za-z0-9_-]{200,}$"),
            QRegularExpression::CaseInsensitiveOption),
        QRegularExpression(
            QStringLiteral(
                "^Bilibili VXCode Swarm Transcoder"
                "(?:\\s+v[\\d.]+)?$"),
            QRegularExpression::CaseInsensitiveOption),
    };
    m_artworkSources = {
        QStringLiteral("protocol.artwork_uri"),
        QStringLiteral("file.artwork"),
    };
    m_titleDigitsPattern =
        QRegularExpression(QStringLiteral("\\d+"));
}

bool MetadataPipeline::loadNearExecutable(QString *error)
{
    return loadFile(
        QCoreApplication::applicationDirPath()
            + QStringLiteral("/config/metadata-pipeline.json"),
        error);
}

bool MetadataPipeline::loadFile(const QString &path, QString *error)
{
    if (error)
        error->clear();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("cannot open %1").arg(path);
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (error) {
            *error = QStringLiteral("%1:%2")
                         .arg(path)
                         .arg(parseError.errorString());
        }
        return false;
    }

    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        if (error)
            *error = QStringLiteral("unsupported pipeline version");
        return false;
    }

    FieldRule title;
    FieldRule artist;
    FieldRule album;
    FieldRule subtitleAudio;
    FieldRule subtitleVisual;
    QString parseMessage;
    const QJsonObject fields =
        root.value(QStringLiteral("text")).toObject();
    const QJsonObject subtitle =
        fields.value(QStringLiteral("subtitle")).toObject();
    if (!parseRule(
            fields, QStringLiteral("title"), title.sources,
            title.rejectPatterns, parseMessage)
        || !parseRule(
            fields, QStringLiteral("artist"), artist.sources,
            artist.rejectPatterns, parseMessage)
        || !parseRule(
            fields, QStringLiteral("album"), album.sources,
            album.rejectPatterns, parseMessage)
        || !parseRule(
            subtitle, QStringLiteral("audio"), subtitleAudio.sources,
            subtitleAudio.rejectPatterns, parseMessage)
        || !parseRule(
            subtitle, QStringLiteral("visual"), subtitleVisual.sources,
            subtitleVisual.rejectPatterns, parseMessage)) {
        if (error)
            *error = path + QStringLiteral(": ") + parseMessage;
        return false;
    }

    const QJsonObject titleOptions =
        fields.value(QStringLiteral("title")).toObject();
    QStringList placeholders;
    QStringList extensions;
    QStringList artwork;
    if (!stringList(
            root.value(QStringLiteral("placeholders")), placeholders,
            parseMessage, QStringLiteral("placeholders"))
        || !stringList(
            titleOptions.value(QStringLiteral("media_extensions")),
            extensions, parseMessage,
            QStringLiteral("text.title.media_extensions"))
        || !stringList(
            root.value(QStringLiteral("artwork")).toObject()
                .value(QStringLiteral("sources")),
            artwork, parseMessage,
            QStringLiteral("artwork.sources"))) {
        if (error)
            *error = path + QStringLiteral(": ") + parseMessage;
        return false;
    }
    if (placeholders.isEmpty() || extensions.isEmpty()
        || artwork.isEmpty()) {
        if (error)
            *error = path + QStringLiteral(": required list is empty");
        return false;
    }
    for (const QString &source : artwork) {
        if (source != QStringLiteral("file.artwork")
            && source != QStringLiteral("protocol.artwork_uri")) {
            if (error) {
                *error = path
                    + QStringLiteral(": unknown artwork source ")
                    + source;
            }
            return false;
        }
    }
    if (!validateSources(
            title.sources, QStringLiteral("text.title"), parseMessage)
        || !validateSources(
            artist.sources, QStringLiteral("text.artist"), parseMessage)
        || !validateSources(
            album.sources, QStringLiteral("text.album"), parseMessage)
        || !validateSources(
            subtitleAudio.sources, QStringLiteral("text.subtitle.audio"),
            parseMessage)
        || !validateSources(
            subtitleVisual.sources, QStringLiteral("text.subtitle.visual"),
            parseMessage)) {
        if (error)
            *error = path + QStringLiteral(": ") + parseMessage;
        return false;
    }

    const int minimumLength =
        titleOptions.value(QStringLiteral("minimum_length")).toInt(-1);
    const int minimumLetters =
        titleOptions.value(QStringLiteral("minimum_letters")).toInt(-1);
    const int maximumDigitRun =
        titleOptions.value(QStringLiteral("maximum_digit_run")).toInt(-1);
    const int maximumDigitRatio = titleOptions
        .value(QStringLiteral("maximum_digit_ratio_percent"))
        .toInt(-1);
    if (minimumLength < 0 || minimumLetters < 0
        || maximumDigitRun < 1 || maximumDigitRatio < 0
        || maximumDigitRatio > 100) {
        if (error)
            *error = path + QStringLiteral(": invalid title heuristic");
        return false;
    }

    m_title = std::move(title);
    m_artist = std::move(artist);
    m_album = std::move(album);
    m_subtitleAudio = std::move(subtitleAudio);
    m_subtitleVisual = std::move(subtitleVisual);
    m_placeholders = placeholders;
    m_mediaExtensions = extensions;
    for (QString &extension : m_mediaExtensions) {
        if (extension.startsWith(QLatin1Char('.')))
            extension.remove(0, 1);
        extension = extension.toLower();
    }
    m_artworkSources = artwork;
    m_titleMinimumLength = minimumLength;
    m_titleMinimumLetters = minimumLetters;
    m_titleMaximumDigitRun = maximumDigitRun;
    m_titleMaximumDigitRatioPercent = maximumDigitRatio;
    m_loadedFromFile = true;
    m_sourcePath = path;
    return true;
}

MetadataPipelineResult MetadataPipeline::resolve(
    const MetadataPipelineInput &input) const
{
    MetadataPipelineResult result;
    const auto firstAccepted = [&](const FieldRule &rule, bool title) {
        for (const QString &source : rule.sources) {
            const QString candidate =
                resolveSource(source, input, result.rejected);
            const QString accepted = acceptCandidate(
                candidate, source, rule, title, result.rejected);
            if (!accepted.isEmpty())
                return accepted;
        }
        return QString();
    };

    result.title = firstAccepted(m_title, true);
    result.artist = firstAccepted(m_artist, false);
    result.album = firstAccepted(m_album, false);
    result.subtitle = firstAccepted(
        input.kind == MediaKind::Audio
            ? m_subtitleAudio : m_subtitleVisual,
        false);
    if (result.title.isEmpty()) {
        result.title =
            QCoreApplication::translate("NowPlaying", "media_unknown");
    }
    if (result.subtitle.isEmpty()) {
        result.subtitle =
            QCoreApplication::translate("NowPlaying", "media_unknown");
    }

    for (const QString &source : m_artworkSources) {
        QString candidate =
            resolveSource(source, input, result.rejected);
        if (!candidate.isEmpty() && QUrl(candidate).isRelative())
            candidate = QUrl(input.uri).resolved(QUrl(candidate)).toString();
        const bool accepted =
            source == QStringLiteral("protocol.artwork_uri")
            ? networkArtworkSource(candidate)
            : localArtworkSource(candidate);
        if (accepted) {
            result.artworkSource = candidate;
            break;
        }
        if (!candidate.isEmpty()) {
            result.rejected.push_back(
                QStringLiteral("%1 rejected artwork source: %2")
                    .arg(source, candidate));
        }
    }
    return result;
}

QString MetadataPipeline::resolveSource(
    const QString &source, const MetadataPipelineInput &input,
    QStringList &) const
{
    if (source == QStringLiteral("protocol.title"))
        return input.protocolTitle;
    if (source == QStringLiteral("protocol.artist"))
        return input.protocolArtist;
    if (source == QStringLiteral("protocol.creator"))
        return input.protocolCreator;
    if (source == QStringLiteral("protocol.album"))
        return input.protocolAlbum;
    if (source == QStringLiteral("protocol.subtitle"))
        return input.protocolSubtitle;
    if (source == QStringLiteral("protocol.description"))
        return input.protocolDescription;
    if (source == QStringLiteral("protocol.long_description"))
        return input.protocolLongDescription;
    if (source == QStringLiteral("protocol.artwork_uri"))
        return input.protocolArtworkUri;
    if (source == QStringLiteral("file.title")) {
        return tagValue(
            input.fileTags, {"title", "TITLE", "name"});
    }
    if (source == QStringLiteral("file.artist")) {
        return tagValue(
            input.fileTags, {"artist", "ARTIST", "album_artist"});
    }
    if (source == QStringLiteral("file.album")) {
        return tagValue(input.fileTags, {"album", "ALBUM"});
    }
    if (source == QStringLiteral("file.subtitle")) {
        return tagValue(
            input.fileTags, {"description", "comment", "synopsis"});
    }
    if (source == QStringLiteral("file.artwork"))
        return input.fileArtworkSource;
    if (source == QStringLiteral("uri.title"))
        return titleFromUri(input.uri);
    if (source == QStringLiteral("media_kind")) {
        const QString key = mediaKindKey(input.kind);
        return key.isEmpty()
            ? QString() : QCoreApplication::translate(
                  "NowPlaying", key.toUtf8().constData());
    }
    if (source == QStringLiteral("unknown")) {
        return QCoreApplication::translate(
            "NowPlaying", "media_unknown");
    }
    return {};
}

QString MetadataPipeline::acceptCandidate(
    const QString &candidate, const QString &field,
    const FieldRule &rule, bool titleRules,
    QStringList &rejected) const
{
    QString value = normalizedText(candidate);
    if (value.isEmpty())
        return {};

    if (field == QStringLiteral("unknown")
        || field == QStringLiteral("media_kind")) {
        return value;
    }
    if (titleRules)
        value = strippedMediaExtension(value);
    if (placeholder(value)) {
        rejected.push_back(
            QStringLiteral("%1 rejected placeholder: %2")
                .arg(field, value));
        return {};
    }
    if (looksLikeUri(value)) {
        rejected.push_back(
            QStringLiteral("%1 rejected URI: %2").arg(field, value));
        return {};
    }
    for (const QRegularExpression &pattern : rule.rejectPatterns) {
        if (pattern.match(value).hasMatch()) {
            rejected.push_back(
                QStringLiteral("%1 rejected pattern %2: %3")
                    .arg(field, pattern.pattern(), value));
            return {};
        }
    }
    if (titleRules && !validTitle(value, m_titleDigitsPattern)) {
        rejected.push_back(
            QStringLiteral("title rejected heuristic: %1").arg(value));
        return {};
    }
    return value;
}

bool MetadataPipeline::placeholder(const QString &value) const
{
    const QString candidate = strippedWrappers(value).toLower();
    return std::any_of(
        m_placeholders.begin(), m_placeholders.end(),
        [&](const QString &entry) {
            return strippedWrappers(entry).toLower() == candidate;
        });
}

bool MetadataPipeline::validTitle(
    const QString &value, const QRegularExpression &digitsPattern) const
{
    if (value.size() < m_titleMinimumLength)
        return false;
    int letters = 0;
    int digits = 0;
    for (const QChar ch : value) {
        if (ch.isLetter())
            ++letters;
        else if (ch.isDigit())
            ++digits;
    }
    if (letters < m_titleMinimumLetters)
        return false;

    int longestRun = 0;
    auto matches = digitsPattern.globalMatch(value);
    while (matches.hasNext())
        longestRun = std::max(
            longestRun,
            static_cast<int>(matches.next().captured().size()));
    if (longestRun > m_titleMaximumDigitRun)
        return false;
    const int meaningful = letters + digits;
    return meaningful > 0
        && digits * 100
            <= meaningful * m_titleMaximumDigitRatioPercent;
}

QString MetadataPipeline::strippedMediaExtension(
    const QString &input) const
{
    QString value = input;
    const int dot = value.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0 || dot + 1 >= value.size())
        return value;
    const QString extension = value.mid(dot + 1).toLower();
    if (m_mediaExtensions.contains(extension))
        value.truncate(dot);
    return value;
}
