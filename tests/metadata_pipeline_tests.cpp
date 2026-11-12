// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "core/MetadataPipeline.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void testFallbackAndTypo()
{
    MetadataPipeline pipeline;
    MetadataPipelineInput input;
    input.uri =
        "http://example.test/Screenshot_20260905_232916.jpg";
    input.kind = MediaKind::Image;
    input.protocolTitle = "unkown";
    input.protocolArtist = "<UNKOWN>";
    input.protocolCreator = "Creator";
    input.protocolSubtitle = "unkown";
    input.protocolDescription = "Useful subtitle";
    input.fileTags.insert(QStringLiteral("title"), QStringLiteral("Spring"));
    input.protocolArtworkUri =
        QStringLiteral("/cover.jpg");

    const auto result = pipeline.resolve(input);
    check(result.title == QStringLiteral("Spring"),
          "vivo typo unkown is rejected and file title wins");
    check(result.artist == QStringLiteral("Creator"),
          "bad artist candidate falls through to creator");
    check(result.subtitle == QStringLiteral("Useful subtitle"),
          "bad subtitle candidate falls through to description");
    check(result.artworkSource
              == QStringLiteral("http://example.test/cover.jpg"),
          "relative artwork resolves against the media URI");
}

void testTextPriorityAndFileArtwork()
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "temporary directory");
    const QString artworkPath =
        temporary.filePath(QStringLiteral("cover.png"));
    QFile artwork(artworkPath);
    check(artwork.open(QIODevice::WriteOnly), "create artwork placeholder");
    artwork.write("image");
    artwork.close();

    MetadataPipeline pipeline;
    MetadataPipelineInput input;
    input.uri = QStringLiteral("http://example.test/fallback.mp4");
    input.kind = MediaKind::Video;
    input.protocolTitle = QStringLiteral("Protocol Movie");
    input.protocolArtist = QStringLiteral("Protocol Artist");
    input.protocolAlbum = QStringLiteral("Protocol Album");
    input.protocolSubtitle = QStringLiteral("Protocol Subtitle");
    input.fileTags.insert(QStringLiteral("title"), QStringLiteral("File Title"));
    input.fileTags.insert(QStringLiteral("artist"), QStringLiteral("File Artist"));
    input.fileArtworkSource =
        QUrl::fromLocalFile(artworkPath).toString();
    input.protocolArtworkUri =
        QStringLiteral("https://example.test/network.jpg");

    const auto result = pipeline.resolve(input);
    check(result.title == QStringLiteral("Protocol Movie"),
          "valid protocol title has priority");
    check(result.artist == QStringLiteral("Protocol Artist"),
          "valid protocol artist has priority");
    check(result.subtitle == QStringLiteral("Protocol Subtitle"),
          "valid protocol subtitle has priority");
    check(result.artworkSource == input.protocolArtworkUri,
          "protocol artwork has priority over MPV/local artwork");

    input.protocolArtworkUri.clear();
    const auto fileFallback = pipeline.resolve(input);
    check(fileFallback.artworkSource == input.fileArtworkSource,
          "MPV/local artwork fills in when protocol artwork is absent");

    input.fileArtworkSource.clear();
    const auto noArtwork = pipeline.resolve(input);
    check(noArtwork.artworkSource.isEmpty(),
          "artwork stays empty when neither source has a cover");
}

void testUriAndKindFallback()
{
    MetadataPipeline pipeline;
    MetadataPipelineInput input;
    input.uri =
        QStringLiteral("http://example.test/Cold-Enough_For_Snow.mkv");
    input.kind = MediaKind::Video;
    auto result = pipeline.resolve(input);
    check(result.title == QStringLiteral("Cold Enough For Snow"),
          "URI basename is the third text fallback");

    input.uri = QStringLiteral("http://example.test/1000140373");
    result = pipeline.resolve(input);
    check(result.title == QStringLiteral("media_kind_video")
              || result.title == QStringLiteral("视频"),
          "invalid URI title falls back to localized media kind");
}

void testProtocolArtworkRejectsLocalFiles()
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "artwork trust temporary directory");
    const QString path =
        temporary.filePath(QStringLiteral("private.png"));
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "create local artwork");
    file.write("image");
    file.close();

    MetadataPipeline pipeline;
    MetadataPipelineInput input;
    input.uri = QStringLiteral("http://example.test/movie.mp4");
    input.kind = MediaKind::Video;
    input.protocolArtworkUri = QUrl::fromLocalFile(path).toString();

    const auto result = pipeline.resolve(input);
    check(result.artworkSource.isEmpty(),
          "protocol artwork cannot reference a local file");
}

void testExternalJson()
{
    QTemporaryDir temporary;
    check(temporary.isValid(), "external JSON temporary directory");
    const QString path =
        temporary.filePath(QStringLiteral("metadata-pipeline.json"));
    QFile file(path);
    check(file.open(QIODevice::WriteOnly), "write external pipeline");
    file.write(R"({
      "version": 1,
      "placeholders": ["mystery"],
      "text": {
        "title": {
          "sources": ["protocol.title", "file.title", "unknown"],
          "minimum_length": 1,
          "minimum_letters": 1,
          "maximum_digit_run": 8,
          "maximum_digit_ratio_percent": 90,
          "reject_patterns": [],
          "media_extensions": ["foo"]
        },
        "artist": {"sources": ["protocol.artist"], "reject_patterns": []},
        "album": {"sources": ["protocol.album"], "reject_patterns": []},
        "subtitle": {
          "audio": {"sources": ["unknown"], "reject_patterns": []},
          "visual": {"sources": ["unknown"], "reject_patterns": []}
        }
      },
      "artwork": {"sources": ["protocol.artwork_uri"]}
    })");
    file.close();

    MetadataPipeline pipeline;
    QString error;
    check(pipeline.loadFile(path, &error),
          "external metadata pipeline loads");
    MetadataPipelineInput input;
    input.uri = QStringLiteral("http://example.test/fallback.foo");
    input.kind = MediaKind::Video;
    input.protocolTitle = QStringLiteral("mystery");
    input.fileTags.insert(QStringLiteral("title"), QStringLiteral("demo.foo"));
    const auto result = pipeline.resolve(input);
    check(result.title == QStringLiteral("demo"),
          "external rules drive placeholder and extension filtering");

    QFile nonString(path);
    check(nonString.open(QIODevice::WriteOnly | QIODevice::Truncate),
          "rewrite external pipeline with bad source type");
    nonString.write(R"({
      "version": 1,
      "placeholders": ["unknown"],
      "text": {
        "title": {
          "sources": [1],
          "minimum_length": 1,
          "minimum_letters": 1,
          "maximum_digit_run": 8,
          "maximum_digit_ratio_percent": 90,
          "reject_patterns": [],
          "media_extensions": ["foo"]
        },
        "artist": {"sources": ["unknown"], "reject_patterns": []},
        "album": {"sources": ["unknown"], "reject_patterns": []},
        "subtitle": {
          "audio": {"sources": ["unknown"], "reject_patterns": []},
          "visual": {"sources": ["unknown"], "reject_patterns": []}
        }
      },
      "artwork": {"sources": ["protocol.artwork_uri"]}
    })");
    nonString.close();
    check(!pipeline.loadFile(path, &error),
          "non-string source entries reject the whole policy");
    const auto retained = pipeline.resolve(input);
    check(retained.title == QStringLiteral("demo"),
          "a rejected reload keeps the previous valid policy");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    testFallbackAndTypo();
    testTextPriorityAndFileArtwork();
    testUriAndKindFallback();
    testProtocolArtworkRejectsLocalFiles();
    testExternalJson();
    std::cout << "Metadata pipeline tests passed\n";
    return 0;
}
