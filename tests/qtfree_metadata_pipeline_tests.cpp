// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/MetadataPipeline.h"

#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path tempDirectory()
{
    const auto root = std::filesystem::temp_directory_path()
        / ("mcast-qtfree-metadata-"
           + std::to_string(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    check(!error, "temporary directory created");
    return root;
}

void testFallbackAndTypo()
{
    mcast::avalonia::qtfree::MetadataPipeline pipeline;
    mcast::avalonia::qtfree::MediaRequest request;
    request.uri =
        "http://example.test/Screenshot_20260905_232916.jpg";
    request.kind = mcast::avalonia::qtfree::MediaKind::Image;
    request.title = "unkown";
    request.artist = "<UNKOWN>";
    request.creator = "Creator";
    request.subtitle = "unkown";
    request.description = "Useful subtitle";
    request.artworkUri = "/cover.jpg";

    const auto result = pipeline.resolve(
        request, {{"title", "Spring"}}, {});
    check(result.title == "Spring",
          "typo is rejected and file title wins");
    check(result.artist == "Creator",
          "bad artist falls through to creator");
    check(result.subtitle == "Useful subtitle",
          "bad subtitle falls through to description");
    check(result.artworkSource == "http://example.test/cover.jpg",
          "relative protocol artwork resolves as HTTP");
}

void testTextPriorityAndArtwork()
{
    const auto root = tempDirectory();
    const auto artwork = root / "cover.png";
    {
        std::ofstream stream(artwork, std::ios::binary);
        stream << "png";
    }

    mcast::avalonia::qtfree::MetadataPipeline pipeline;
    mcast::avalonia::qtfree::MediaRequest request;
    request.uri = "http://example.test/fallback.mp4";
    request.kind = mcast::avalonia::qtfree::MediaKind::Video;
    request.title = "Protocol Movie";
    request.artist = "Protocol Artist";
    request.album = "Protocol Album";
    request.subtitle = "Protocol Subtitle";
    request.artworkUri = "https://example.test/network.jpg";
    const std::map<std::string, std::string, std::less<>> tags{
        {"title", "File Title"},
        {"artist", "File Artist"},
    };

    auto result = pipeline.resolve(
        request, tags,
        mcast::avalonia::qtfree::fileUriFromPath(artwork.string()));
    check(result.title == "Protocol Movie",
          "protocol title has priority");
    check(result.artist == "Protocol Artist",
          "protocol artist has priority");
    check(result.subtitle == "Protocol Subtitle",
          "protocol subtitle has priority");
    check(result.artworkSource == request.artworkUri,
          "protocol artwork has priority");

    request.artworkUri.clear();
    result = pipeline.resolve(
        request, tags,
        mcast::avalonia::qtfree::fileUriFromPath(artwork.string()));
    check(
        result.artworkSource
            == mcast::avalonia::qtfree::fileUriFromPath(
                artwork.string()),
        "local file artwork fills missing protocol artwork");

    request.artworkUri =
        mcast::avalonia::qtfree::fileUriFromPath(artwork.string());
    result = pipeline.resolve(request, tags, {});
    check(result.artworkSource.empty(),
          "protocol artwork cannot reference a local file");

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void testUriFallbackAndNoise()
{
    mcast::avalonia::qtfree::MetadataPipeline pipeline;
    mcast::avalonia::qtfree::MediaRequest request;
    request.uri =
        "http://example.test/Cold-Enough_For_Snow.mkv";
    request.kind = mcast::avalonia::qtfree::MediaKind::Video;
    auto result = pipeline.resolve(request, {}, {});
    check(result.title == "Cold Enough For Snow",
          "URI basename is text fallback");

    const char *noise[] = {
        "vid:AbC123",
        "Bilibili VXCode Swarm Transcoder v1.2.3",
    };
    for (const char *candidate : noise) {
        request.subtitle = candidate;
        request.description = "Useful fallback";
        result = pipeline.resolve(request, {}, {});
        check(result.subtitle == "Useful fallback",
              "known visual subtitles fall through");
    }

    request.subtitle =
        "Bilibili VXCode Swarm Transcoder - Behind the Scenes";
    result = pipeline.resolve(request, {}, {});
    check(result.subtitle == request.subtitle,
          "similar real subtitles are preserved");

    request.kind = mcast::avalonia::qtfree::MediaKind::Audio;
    request.subtitle.clear();
    request.description.clear();
    request.artist = "vid:Artist";
    result = pipeline.resolve(request, {}, {});
    check(result.subtitle == "vid:Artist",
          "visual noise filters do not damage audio text");
}

void testExternalPolicyValidation()
{
    const auto root = tempDirectory();
    const auto path =
        root / "metadata-pipeline.json";
    {
        std::ofstream stream(path, std::ios::binary);
        stream << R"({
          "placeholders": ["mystery"],
          "text": {
            "title": {
              "sources": ["protocol.title", "unknown"],
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
        })";
    }

    mcast::avalonia::qtfree::MetadataPipeline pipeline;
    std::string error;
    check(pipeline.loadFile(path.string(), &error),
          error.c_str());
    mcast::avalonia::qtfree::MediaRequest request;
    request.uri = "http://example.test/demo.foo";
    request.title = "mystery";
    auto result = pipeline.resolve(request, {}, {});
    check(result.title == "未知",
          "external placeholders replace built-in rules");

    {
        std::ofstream stream(
            path, std::ios::binary | std::ios::trunc);
        stream << R"({
          "placeholders": ["unknown"],
          "text": {
            "title": {"sources": [1]},
            "artist": {"sources": ["unknown"]},
            "album": {"sources": ["unknown"]},
            "subtitle": {
              "audio": {"sources": ["unknown"]},
              "visual": {"sources": ["unknown"]}
            }
          },
          "artwork": {"sources": ["protocol.artwork_uri"]}
        })";
    }
    check(!pipeline.loadFile(path.string(), &error),
          "non-string sources reject whole policy");
    result = pipeline.resolve(request, {}, {});
    check(result.title == "未知",
          "rejected reload keeps previous valid policy");

    std::error_code removeError;
    std::filesystem::remove_all(root, removeError);
}

} // namespace

int main()
{
    testFallbackAndTypo();
    testTextPriorityAndArtwork();
    testUriFallbackAndNoise();
    testExternalPolicyValidation();
    std::cout << "Qt-free metadata pipeline tests passed\n";
    return 0;
}
