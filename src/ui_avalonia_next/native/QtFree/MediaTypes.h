// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mcast::avalonia::qtfree {

enum class MediaKind {
    Unknown,
    Video,
    Audio,
    Image,
};

enum class MediaSource {
    Cast,
    Local,
};

enum class MediaLoadRoute {
    Native,
    ImageSequence,
    WrappedTransportStream,
};

struct NowPlaying
{
    MediaSource source = MediaSource::Cast;
    MediaKind kind = MediaKind::Unknown;
    std::string title;
    std::string subtitle;
    std::string artist;
    std::string album;
    std::string lyrics;
    std::string artworkSource;

    bool hasTitle() const { return !title.empty(); }
    bool isEmpty() const
    {
        return title.empty() && artist.empty() && album.empty();
    }
};

struct PictureControlInfo
{
    std::string name;
    int minimum = 0;
    int maximum = 0;
    int neutral = 0;
};

struct MediaLoadFrame
{
    std::string uri;
    double durationSeconds = 0;
};

struct MediaLoadRequest
{
    std::string logicalUri;
    std::uint64_t loadGeneration = 0;
    MediaLoadRoute route = MediaLoadRoute::Native;
    std::vector<MediaLoadFrame> imageSequenceFrames;
};

struct MediaRequest
{
    std::string uri;
    std::string metadata;
    std::string title;
    std::string artist;
    std::string creator;
    std::string album;
    std::string description;
    std::string subtitle;
    std::string longDescription;
    std::string artworkUri;
    MediaKind kind = MediaKind::Unknown;
    MediaLoadRoute route = MediaLoadRoute::Native;
    std::vector<MediaLoadFrame> imageSequenceFrames;
    std::string protocol;
};

std::string lowercaseAscii(std::string_view text);
std::string simplified(std::string_view text);
std::string mediaKindLabel(MediaKind kind);
std::string mediaKindKey(MediaKind kind);
bool looksLikePlaceholder(std::string_view text);
std::string stripMediaExtension(std::string_view text);
MediaKind mediaKindFromUri(std::string_view uri);
std::string titleFromUri(std::string_view uri);
bool looksLikeATitle(std::string_view text);
bool sameMediaUri(std::string_view left, std::string_view right);
bool isHttpArtworkUri(std::string_view uri);
bool isFileArtworkUri(std::string_view uri);
std::string fileUriFromPath(std::string_view path);
std::string resolveUriReference(
    std::string_view base, std::string_view reference);

} // namespace mcast::avalonia::qtfree

