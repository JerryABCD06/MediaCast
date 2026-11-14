// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/MediaTypes.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

constexpr std::array<std::string_view, 11> kImageExtensions{
    "jpg", "jpeg", "png", "gif", "bmp", "webp",
    "heic", "heif", "tif", "tiff", "avif",
};

constexpr std::array<std::string_view, 9> kAudioExtensions{
    "mp3", "flac", "wav", "m4a", "aac",
    "ogg", "opus", "wma", "ape",
};

constexpr std::array<std::string_view, 14> kVideoExtensions{
    "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm",
    "ts", "m2ts", "mpg", "mpeg", "m4v", "3gp", "rmvb",
};

template <std::size_t Size>
bool contains(
    const std::array<std::string_view, Size> &values,
    std::string_view value)
{
    return std::find(values.begin(), values.end(), value) != values.end();
}

bool knownExtension(std::string_view extension)
{
    return contains(kImageExtensions, extension)
        || contains(kAudioExtensions, extension)
        || contains(kVideoExtensions, extension);
}

int hexValue(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

std::string percentDecode(std::string_view value)
{
    std::string output;
    output.reserve(value.size());
    for (std::size_t offset = 0; offset < value.size(); ++offset) {
        if (value[offset] == '%' && offset + 2 < value.size()) {
            const int high = hexValue(value[offset + 1]);
            const int low = hexValue(value[offset + 2]);
            if (high >= 0 && low >= 0) {
                output.push_back(
                    static_cast<char>((high << 4) | low));
                offset += 2;
                continue;
            }
        }
        output.push_back(value[offset]);
    }
    return output;
}

std::string pathWithoutQuery(std::string_view uri)
{
    std::string path(uri);
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto query = path.find('?');
    if (query != std::string::npos)
        path.erase(query);
    return path;
}

std::string extensionOf(std::string_view path)
{
    const auto slash = path.find_last_of("/\\");
    const auto dot = path.find_last_of('.');
    if (dot == std::string_view::npos
        || (slash != std::string_view::npos && dot < slash)
        || dot + 1 >= path.size()) {
        return {};
    }
    return lowercaseAscii(path.substr(dot + 1));
}

std::string lastPathSegment(std::string_view uri)
{
    std::string path(uri);
    std::replace(path.begin(), path.end(), '\\', '/');
    const auto slash = path.find_last_of('/');
    std::string name =
        slash == std::string::npos ? path : path.substr(slash + 1);
    const auto query = name.find('?');
    if (query != std::string::npos)
        name.erase(query);
    return percentDecode(name);
}

std::pair<std::uint32_t, std::size_t> nextUtf8Scalar(
    std::string_view text, std::size_t offset)
{
    const auto first = static_cast<unsigned char>(text[offset]);
    if (first < 0x80)
        return {first, 1};

    int length = 1;
    std::uint32_t scalar = first;
    if ((first & 0xe0) == 0xc0) {
        length = 2;
        scalar = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        length = 3;
        scalar = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        length = 4;
        scalar = first & 0x07;
    } else {
        return {first, 1};
    }
    if (offset + static_cast<std::size_t>(length) > text.size())
        return {first, 1};
    for (int index = 1; index < length; ++index) {
        const auto next =
            static_cast<unsigned char>(text[offset + index]);
        if ((next & 0xc0) != 0x80)
            return {first, 1};
        scalar = (scalar << 6) | (next & 0x3f);
    }
    return {scalar, static_cast<std::size_t>(length)};
}

bool unreservedPercentByte(unsigned char value)
{
    return std::isalnum(value) != 0
        || value == '-' || value == '.' || value == '_'
        || value == '~';
}

std::string normalizePercentEncoding(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size());
    for (std::size_t offset = 0; offset < value.size(); ++offset) {
        if (value[offset] == '%' && offset + 2 < value.size()) {
            const int high = hexValue(value[offset + 1]);
            const int low = hexValue(value[offset + 2]);
            if (high >= 0 && low >= 0) {
                const auto decoded =
                    static_cast<unsigned char>((high << 4) | low);
                if (unreservedPercentByte(decoded)) {
                    output.push_back(static_cast<char>(decoded));
                } else {
                    output.push_back('%');
                    output.push_back(hex[high]);
                    output.push_back(hex[low]);
                }
                offset += 2;
                continue;
            }
        }
        output.push_back(value[offset]);
    }
    return output;
}

bool defaultPort(std::string_view scheme, std::string_view port)
{
    return (scheme == "http" && port == "80")
        || (scheme == "https" && port == "443")
        || (scheme == "ftp" && port == "21");
}

std::string normalizedUri(std::string_view raw)
{
    const auto fragmentAt = raw.find('#');
    const std::string_view main = fragmentAt == std::string_view::npos
        ? raw : raw.substr(0, fragmentAt);
    const std::string_view fragment =
        fragmentAt == std::string_view::npos
        ? std::string_view{}
        : raw.substr(fragmentAt + 1);

    std::string output;
    std::string_view rest = main;
    const auto colon = main.find(':');
    if (colon != std::string_view::npos) {
        const std::string scheme =
            lowercaseAscii(main.substr(0, colon));
        bool validScheme = !scheme.empty()
            && std::isalpha(
                   static_cast<unsigned char>(scheme.front()))
                != 0;
        for (const char ch : scheme) {
            if (!std::isalnum(static_cast<unsigned char>(ch))
                && ch != '+' && ch != '-' && ch != '.') {
                validScheme = false;
                break;
            }
        }
        if (validScheme) {
            output += scheme;
            output.push_back(':');
            rest = main.substr(colon + 1);
            if (rest.starts_with("//")) {
                output += "//";
                rest.remove_prefix(2);
                const auto authorityEnd = rest.find_first_of("/?");
                const std::string_view authority =
                    rest.substr(0, authorityEnd);
                std::string_view userInfo;
                std::string_view hostPort = authority;
                const auto at = authority.rfind('@');
                if (at != std::string_view::npos) {
                    userInfo = authority.substr(0, at + 1);
                    hostPort = authority.substr(at + 1);
                }
                output += normalizePercentEncoding(userInfo);

                std::string_view host = hostPort;
                std::string_view port;
                if (hostPort.starts_with("[")
                    && hostPort.find(']') != std::string_view::npos) {
                    const auto close = hostPort.find(']');
                    host = hostPort.substr(0, close + 1);
                    if (close + 1 < hostPort.size()
                        && hostPort[close + 1] == ':') {
                        port = hostPort.substr(close + 2);
                    }
                } else {
                    const auto portAt = hostPort.rfind(':');
                    if (portAt != std::string_view::npos
                        && hostPort.find(':') == portAt) {
                        host = hostPort.substr(0, portAt);
                        port = hostPort.substr(portAt + 1);
                    }
                }
                output += lowercaseAscii(host);
                if (!port.empty() && !defaultPort(scheme, port)) {
                    output.push_back(':');
                    output += port;
                }
                rest = authorityEnd == std::string_view::npos
                    ? std::string_view{}
                    : rest.substr(authorityEnd);
            }
        }
    }

    output += normalizePercentEncoding(rest);
    if (fragmentAt != std::string_view::npos) {
        output.push_back('#');
        output += normalizePercentEncoding(fragment);
    }
    return output;
}

bool hasScheme(std::string_view uri)
{
    const auto colon = uri.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(uri.front())))
        return false;
    for (std::size_t index = 1; index < colon; ++index) {
        const auto ch = static_cast<unsigned char>(uri[index]);
        if (!std::isalnum(ch) && ch != '+' && ch != '-' && ch != '.')
            return false;
    }
    return true;
}

std::string percentEncodePath(std::string_view path)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string output;
    output.reserve(path.size());
    for (const unsigned char ch : path) {
        if (std::isalnum(ch) != 0
            || ch == '-' || ch == '.' || ch == '_'
            || ch == '~' || ch == '/' || ch == ':') {
            output.push_back(static_cast<char>(ch));
        } else {
            output.push_back('%');
            output.push_back(hex[ch >> 4]);
            output.push_back(hex[ch & 0x0f]);
        }
    }
    return output;
}

} // namespace

std::string lowercaseAscii(std::string_view text)
{
    std::string output(text);
    for (char &ch : output) {
        const auto value = static_cast<unsigned char>(ch);
        if (value >= 'A' && value <= 'Z')
            ch = static_cast<char>(value - 'A' + 'a');
    }
    return output;
}

std::string simplified(std::string_view text)
{
    std::string output;
    output.reserve(text.size());
    bool pendingSpace = false;
    for (const unsigned char ch : text) {
        if (std::isspace(ch) != 0) {
            pendingSpace = !output.empty();
            continue;
        }
        if (pendingSpace) {
            output.push_back(' ');
            pendingSpace = false;
        }
        output.push_back(static_cast<char>(ch));
    }
    return output;
}

std::string titleFromUri(std::string_view uri)
{
    if (uri.empty())
        return {};
    std::string name = lastPathSegment(uri);
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name.erase(dot);
    std::replace(name.begin(), name.end(), '_', ' ');
    std::replace(name.begin(), name.end(), '-', ' ');
    return simplified(name);
}

bool looksLikeATitle(std::string_view text)
{
    const std::string candidate = simplified(text);
    int scalars = 0;
    for (std::size_t offset = 0; offset < candidate.size();) {
        const auto [scalar, length] = nextUtf8Scalar(candidate, offset);
        (void)scalar;
        ++scalars;
        offset += length;
    }
    if (scalars < 2)
        return false;

    int letters = 0;
    int digits = 0;
    int currentRun = 0;
    int longestRun = 0;
    for (std::size_t offset = 0; offset < candidate.size();) {
        const auto [scalar, length] = nextUtf8Scalar(candidate, offset);
        const auto ch = static_cast<unsigned char>(scalar);
        if (scalar >= 0x80
            || (scalar <= 0x7f && std::isalpha(ch) != 0)) {
            ++letters;
            currentRun = 0;
        } else if (scalar <= 0x7f && std::isdigit(ch) != 0) {
            ++digits;
            longestRun = std::max(longestRun, ++currentRun);
        } else {
            currentRun = 0;
        }
        offset += length;
    }
    return letters > 0 && longestRun < 5
        && digits * 10 <= (letters + digits) * 4;
}

std::string mediaKindLabel(MediaKind kind)
{
    switch (kind) {
    case MediaKind::Video: return "视频";
    case MediaKind::Audio: return "音频";
    case MediaKind::Image: return "图片";
    case MediaKind::Unknown: return {};
    }
    return {};
}

std::string mediaKindKey(MediaKind kind)
{
    switch (kind) {
    case MediaKind::Video: return "media_kind_video";
    case MediaKind::Audio: return "media_kind_audio";
    case MediaKind::Image: return "media_kind_image";
    case MediaKind::Unknown: return {};
    }
    return {};
}

bool looksLikePlaceholder(std::string_view text)
{
    static constexpr std::array<std::string_view, 13> placeholders{
        "unknown", "unkown", "none", "null", "n/a", "na",
        "-", "--", "未知", "未知艺术家", "未知歌手",
        "未知专辑", "无",
    };

    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end
           && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    while (end > begin
           && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }

    std::string candidate(text.substr(begin, end - begin));
    while (candidate.size() >= 2
           && ((candidate.front() == '<' && candidate.back() == '>')
               || (candidate.front() == '"'
                   && candidate.back() == '"'))) {
        candidate = simplified(
            std::string_view(candidate).substr(1, candidate.size() - 2));
    }
    const std::string lowered = lowercaseAscii(candidate);
    return std::find(
               placeholders.begin(), placeholders.end(), lowered)
        != placeholders.end();
}

std::string stripMediaExtension(std::string_view text)
{
    const auto dot = text.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0)
        return std::string(text);
    return knownExtension(lowercaseAscii(text.substr(dot + 1)))
        ? std::string(text.substr(0, dot))
        : std::string(text);
}

MediaKind mediaKindFromUri(std::string_view uri)
{
    if (uri.empty())
        return MediaKind::Unknown;
    const std::string path = pathWithoutQuery(uri);
    const std::string extension = extensionOf(path);
    if (extension.empty())
        return MediaKind::Video;
    if (contains(kImageExtensions, extension))
        return MediaKind::Image;
    if (contains(kAudioExtensions, extension))
        return MediaKind::Audio;
    return MediaKind::Video;
}

bool sameMediaUri(std::string_view left, std::string_view right)
{
    return left == right || normalizedUri(left) == normalizedUri(right);
}

bool isHttpArtworkUri(std::string_view uri)
{
    const std::string lower = lowercaseAscii(uri);
    return lower.starts_with("http://") || lower.starts_with("https://");
}

bool isFileArtworkUri(std::string_view uri)
{
    return lowercaseAscii(uri).starts_with("file:");
}

std::string fileUriFromPath(std::string_view path)
{
    if (path.empty())
        return {};
    if (isFileArtworkUri(path))
        return std::string(path);

    std::string normalized(path);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    if (normalized.starts_with("//"))
        return "file:" + percentEncodePath(normalized);
    if (normalized.front() != '/')
        normalized.insert(normalized.begin(), '/');
    return "file://" + percentEncodePath(normalized);
}

std::string resolveUriReference(
    std::string_view base, std::string_view reference)
{
    if (reference.empty())
        return {};
    if (hasScheme(reference) || reference.starts_with("//"))
        return std::string(reference);
    if (reference.front() == '/') {
        const auto scheme = base.find("://");
        if (scheme != std::string_view::npos) {
            const auto path = base.find('/', scheme + 3);
            return std::string(
                       base.substr(
                           0, path == std::string_view::npos
                                  ? base.size() : path))
                + std::string(reference);
        }
        return std::string(reference);
    }
    const auto cut = base.find_last_of("/\\");
    if (cut == std::string_view::npos)
        return std::string(reference);
    return std::string(base.substr(0, cut + 1))
        + std::string(reference);
}

} // namespace mcast::avalonia::qtfree

