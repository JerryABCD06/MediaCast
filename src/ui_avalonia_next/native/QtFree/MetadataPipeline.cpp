// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/MetadataPipeline.h"

#include "QtFree/Json.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

constexpr std::uintmax_t kMaxLocalArtworkBytes = 16u * 1024u * 1024u;

std::string lower(std::string_view value)
{
    return lowercaseAscii(value);
}

std::string fileTag(
    const std::map<std::string, std::string, std::less<>> &tags,
    std::string_view wanted)
{
    for (const auto &[key, value] : tags) {
        if (lower(key) == lower(wanted))
            return simplified(value);
    }
    return {};
}

bool readStringArray(
    const JsonValue *value, std::vector<std::string> &target,
    bool requireNonEmpty, std::string *error, const char *name)
{
    if (!value)
        return true;
    if (!value->isArray()) {
        if (error)
            *error = std::string(name) + " must be an array";
        return false;
    }
    std::vector<std::string> parsed;
    for (const auto &item : value->items()) {
        if (!item.isString()) {
            if (error)
                *error = std::string(name) + " must contain strings";
            return false;
        }
        parsed.push_back(item.asString());
    }
    if (requireNonEmpty && parsed.empty()) {
        if (error)
            *error = std::string(name) + " cannot be empty";
        return false;
    }
    target = std::move(parsed);
    return true;
}

bool readRegexArray(
    const JsonValue *value, std::vector<std::regex> &target,
    std::string *error, const char *name)
{
    std::vector<std::string> patterns;
    if (!readStringArray(
            value, patterns, false, error, name)) {
        return false;
    }
    std::vector<std::regex> compiled;
    compiled.reserve(patterns.size());
    for (const std::string &pattern : patterns) {
        try {
            compiled.emplace_back(pattern);
        } catch (const std::regex_error &) {
            if (error)
                *error = std::string(name) + " contains invalid regex";
            return false;
        }
    }
    target = std::move(compiled);
    return true;
}

std::filesystem::path executableDirectory()
{
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size())
        return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
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

bool uriHasScheme(std::string_view value)
{
    const auto colon = value.find(':');
    if (colon == std::string_view::npos || colon == 0)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(value.front())))
        return false;
    for (std::size_t index = 1; index < colon; ++index) {
        const auto ch = static_cast<unsigned char>(value[index]);
        if (!std::isalnum(ch) && ch != '+' && ch != '-' && ch != '.')
            return false;
    }
    return true;
}

std::string localPathFromArtworkUri(std::string_view value)
{
    std::string path(value);
    if (isFileArtworkUri(value)) {
        const auto schemeEnd = path.find("://");
        path = schemeEnd == std::string::npos
            ? path.substr(5)
            : path.substr(schemeEnd + 3);
        path = percentDecode(path);
        if (path.size() >= 3 && path.front() == '/'
            && std::isalpha(static_cast<unsigned char>(path[1]))
            && path[2] == ':') {
            path.erase(path.begin());
        }
    }
    return path;
}

bool validLocalArtwork(std::string_view value)
{
    if (value.empty())
        return false;
    const auto path = std::filesystem::u8path(
        localPathFromArtworkUri(value));
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;
    const auto size = std::filesystem::file_size(path, error);
    return !error && size <= kMaxLocalArtworkBytes;
}

std::size_t utf8ScalarCount(std::string_view value)
{
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < value.size();) {
        const auto ch = static_cast<unsigned char>(value[offset]);
        std::size_t length = 1;
        if ((ch & 0xe0) == 0xc0)
            length = 2;
        else if ((ch & 0xf0) == 0xe0)
            length = 3;
        else if ((ch & 0xf8) == 0xf0)
            length = 4;
        if (offset + length > value.size())
            length = 1;
        offset += length;
        ++count;
    }
    return count;
}

} // namespace

MetadataPipeline::MetadataPipeline()
{
    useBuiltInRules();
}

void MetadataPipeline::useBuiltInRules()
{
    m_placeholders = {
        "unkown", "unknown", "none", "null", "n/a", "na",
        "-", "--", "not available", "not applicable", "未知",
        "未知艺术家", "未知歌手", "未知专辑", "无",
    };
    m_mediaExtensions = {
        "jpg", "jpeg", "png", "gif", "bmp", "webp", "heic",
        "heif", "tif", "tiff", "avif", "mp3", "flac", "wav",
        "m4a", "aac", "ogg", "opus", "wma", "ape", "mp4",
        "mkv", "avi", "mov", "wmv", "flv", "webm", "ts",
        "m2ts", "mpg", "mpeg", "m4v", "3gp", "rmvb",
    };
    m_title.sources = {
        "protocol.title", "file.title", "uri.title",
        "media_kind", "unknown"};
    m_title.rejectPatterns = {std::regex("^\\d{5,}$")};
    m_artist.sources = {
        "protocol.artist", "protocol.creator", "file.artist"};
    m_album.sources = {"protocol.album", "file.album"};
    m_subtitleAudio.sources = {
        "protocol.artist", "protocol.creator", "file.artist",
        "protocol.album", "file.album", "unknown"};
    m_subtitleVisual.sources = {
        "protocol.subtitle", "protocol.description",
        "protocol.long_description", "file.subtitle", "unknown"};
    m_subtitleVisual.rejectPatterns = {
        std::regex("^vid:[A-Za-z0-9]+$"),
        std::regex("^[A-Za-z0-9_-]{200,}$"),
        std::regex(
            "^Bilibili VXCode Swarm Transcoder(?:\\s+v[\\d.]+)?$"),
    };
    m_artworkSources = {"protocol.artwork_uri", "file.artwork"};
    m_titleMinimumScalars = 2;
    m_titleMinimumLetters = 1;
    m_titleMaximumDigitRun = 4;
    m_titleMaximumDigitRatioPercent = 40;
}

bool MetadataPipeline::loadFile(
    const std::string &path, std::string *error)
{
    std::ifstream stream(
        std::filesystem::u8path(path), std::ios::binary);
    if (!stream) {
        if (error)
            *error = "cannot open metadata pipeline configuration";
        return false;
    }
    std::ostringstream text;
    text << stream.rdbuf();

    const MetadataPipeline previousPolicy = *this;
    useBuiltInRules();
    if (!applyConfig(text.str(), error)) {
        *this = previousPolicy;
        return false;
    }
    m_loadedFromFile = true;
    m_sourcePath = path;
    return true;
}

bool MetadataPipeline::loadNearExecutable(std::string *error)
{
    const auto directory = executableDirectory();
    if (directory.empty()) {
        if (error)
            *error = "cannot locate Ui executable";
        return false;
    }
    return loadFile(
        (directory / "config" / "metadata-pipeline.json").string(),
        error);
}

bool MetadataPipeline::applyConfig(
    std::string_view json, std::string *error)
{
    JsonError parseError;
    const auto root = JsonValue::parse(json, &parseError);
    if (!root || !root->isObject()) {
        if (error) {
            *error = root
                ? "metadata pipeline root is not an object"
                : "invalid metadata pipeline JSON: " + parseError.message;
        }
        return false;
    }

    if (!readStringArray(
            root->find("placeholders"), m_placeholders, false,
            error, "placeholders")) {
        return false;
    }

    if (const JsonValue *text = root->find("text");
        text && text->isObject()) {
        if (const JsonValue *title = text->find("title");
            title && title->isObject()) {
            if (!readStringArray(
                    title->find("sources"), m_title.sources, true,
                    error, "text.title.sources")
                || !readRegexArray(
                    title->find("reject_patterns"),
                    m_title.rejectPatterns, error,
                    "text.title.reject_patterns")
                || !readStringArray(
                    title->find("media_extensions"),
                    m_mediaExtensions, true, error,
                    "text.title.media_extensions")) {
                return false;
            }
            m_titleMinimumScalars = std::max(
                1, title->at("minimum_length").asInt(
                       m_titleMinimumScalars));
            m_titleMinimumLetters = std::max(
                0, title->at("minimum_letters").asInt(
                       m_titleMinimumLetters));
            m_titleMaximumDigitRun = std::max(
                0, title->at("maximum_digit_run").asInt(
                       m_titleMaximumDigitRun));
            m_titleMaximumDigitRatioPercent = std::clamp(
                title->at("maximum_digit_ratio_percent").asInt(
                    m_titleMaximumDigitRatioPercent),
                0, 100);
        }
        if (const JsonValue *artist = text->find("artist");
            artist && artist->isObject()) {
            if (!readStringArray(
                    artist->find("sources"), m_artist.sources, true,
                    error, "text.artist.sources")
                || !readRegexArray(
                    artist->find("reject_patterns"),
                    m_artist.rejectPatterns, error,
                    "text.artist.reject_patterns")) {
                return false;
            }
        }
        if (const JsonValue *album = text->find("album");
            album && album->isObject()) {
            if (!readStringArray(
                    album->find("sources"), m_album.sources, true,
                    error, "text.album.sources")
                || !readRegexArray(
                    album->find("reject_patterns"),
                    m_album.rejectPatterns, error,
                    "text.album.reject_patterns")) {
                return false;
            }
        }
        if (const JsonValue *subtitle = text->find("subtitle");
            subtitle && subtitle->isObject()) {
            if (const JsonValue *audio = subtitle->find("audio");
                audio && audio->isObject()) {
                if (!readStringArray(
                        audio->find("sources"),
                        m_subtitleAudio.sources, true, error,
                        "text.subtitle.audio.sources")
                    || !readRegexArray(
                        audio->find("reject_patterns"),
                        m_subtitleAudio.rejectPatterns, error,
                        "text.subtitle.audio.reject_patterns")) {
                    return false;
                }
            }
            if (const JsonValue *visual = subtitle->find("visual");
                visual && visual->isObject()) {
                if (!readStringArray(
                        visual->find("sources"),
                        m_subtitleVisual.sources, true, error,
                        "text.subtitle.visual.sources")
                    || !readRegexArray(
                        visual->find("reject_patterns"),
                        m_subtitleVisual.rejectPatterns, error,
                        "text.subtitle.visual.reject_patterns")) {
                    return false;
                }
            }
        }
    }

    if (const JsonValue *artwork = root->find("artwork");
        artwork && artwork->isObject()) {
        if (!readStringArray(
                artwork->find("sources"), m_artworkSources, true,
                error, "artwork.sources")) {
            return false;
        }
    }
    return true;
}

bool MetadataPipeline::placeholder(std::string_view value) const
{
    std::string candidate = simplified(value);
    while (candidate.size() >= 2
           && ((candidate.front() == '<' && candidate.back() == '>')
               || (candidate.front() == '"'
                   && candidate.back() == '"'))) {
        candidate = simplified(
            std::string_view(candidate).substr(
                1, candidate.size() - 2));
    }
    const std::string lowered = lower(candidate);
    return std::find(
               m_placeholders.begin(), m_placeholders.end(), lowered)
        != m_placeholders.end();
}

bool MetadataPipeline::validTitle(std::string_view value) const
{
    if (utf8ScalarCount(value)
        < static_cast<std::size_t>(m_titleMinimumScalars)) {
        return false;
    }
    int letters = 0;
    int digits = 0;
    int run = 0;
    int longest = 0;
    for (const unsigned char ch : value) {
        if (ch >= 0x80
            || std::isalpha(static_cast<unsigned char>(ch)) != 0) {
            ++letters;
            run = 0;
        } else if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
            ++digits;
            longest = std::max(longest, ++run);
        } else {
            run = 0;
        }
    }
    if (letters < m_titleMinimumLetters
        || longest > m_titleMaximumDigitRun) {
        return false;
    }
    return digits * 100
        <= (letters + digits) * m_titleMaximumDigitRatioPercent;
}

std::string MetadataPipeline::acceptCandidate(
    const std::string &text, const std::string &field,
    const FieldRule &rule, bool titleRules,
    std::vector<std::string> &rejected) const
{
    std::string candidate = simplified(text);
    if (candidate.empty() || placeholder(candidate))
        return {};
    if (titleRules) {
        const auto dot = candidate.find_last_of('.');
        if (dot != std::string::npos && dot > 0) {
            const std::string extension =
                lower(candidate.substr(dot + 1));
            if (std::find(
                    m_mediaExtensions.begin(), m_mediaExtensions.end(),
                    extension)
                != m_mediaExtensions.end()) {
                candidate.erase(dot);
            }
        }
        if (!validTitle(candidate)) {
            rejected.push_back(field + "=" + candidate);
            return {};
        }
    }
    for (const auto &pattern : rule.rejectPatterns) {
        if (std::regex_match(candidate, pattern)) {
            rejected.push_back(field + "=" + candidate);
            return {};
        }
    }
    return candidate;
}

std::string MetadataPipeline::resolveSource(
    const std::string &source, const MediaRequest &request,
    const std::map<
        std::string, std::string, std::less<>> &fileTags,
    std::string_view fileArtworkSource) const
{
    if (source == "protocol.title") return request.title;
    if (source == "protocol.artist") return request.artist;
    if (source == "protocol.creator") return request.creator;
    if (source == "protocol.album") return request.album;
    if (source == "protocol.subtitle") return request.subtitle;
    if (source == "protocol.description") return request.description;
    if (source == "protocol.long_description")
        return request.longDescription;
    if (source == "protocol.artwork_uri") return request.artworkUri;
    if (source == "file.title") return fileTag(fileTags, "title");
    if (source == "file.artist") return fileTag(fileTags, "artist");
    if (source == "file.album") return fileTag(fileTags, "album");
    if (source == "file.subtitle")
        return fileTag(fileTags, "subtitle");
    if (source == "file.artwork")
        return std::string(fileArtworkSource);
    if (source == "uri.title") return titleFromUri(request.uri);
    if (source == "media_kind") {
        return mediaKindLabel(
            request.kind == MediaKind::Unknown
                ? mediaKindFromUri(request.uri) : request.kind);
    }
    if (source == "unknown") return "未知";
    return {};
}

MetadataPipelineResult MetadataPipeline::resolve(
    const MediaRequest &request,
    const std::map<
        std::string, std::string, std::less<>> &fileTags,
    std::string_view fileArtworkSource) const
{
    MetadataPipelineResult result;
    const MediaKind kind =
        request.kind == MediaKind::Unknown
        ? mediaKindFromUri(request.uri) : request.kind;
    auto resolveRule = [&](const FieldRule &rule, bool titleRules) {
        for (const std::string &source : rule.sources) {
            const std::string accepted = acceptCandidate(
                resolveSource(source, request, fileTags, fileArtworkSource),
                source, rule, titleRules, result.rejected);
            if (!accepted.empty())
                return accepted;
        }
        return std::string{};
    };

    result.title = resolveRule(m_title, true);
    result.artist = resolveRule(m_artist, false);
    result.album = resolveRule(m_album, false);
    result.subtitle = resolveRule(
        kind == MediaKind::Audio
            ? m_subtitleAudio : m_subtitleVisual,
        false);

    for (const std::string &source : m_artworkSources) {
        std::string value = resolveSource(
            source, request, fileTags, fileArtworkSource);
        if (value.empty() || placeholder(value))
            continue;

        const bool protocolArtwork = source == "protocol.artwork_uri";
        if (!uriHasScheme(value) && !value.starts_with("//"))
            value = resolveUriReference(request.uri, value);

        if (protocolArtwork) {
            if (!isHttpArtworkUri(value))
                continue;
            result.artworkSource = value;
            break;
        }

        if (validLocalArtwork(value)) {
            result.artworkSource =
                isFileArtworkUri(value)
                ? value
                : fileUriFromPath(value);
            break;
        }
    }

    if (result.title.empty())
        result.title = "未知";
    if (result.subtitle.empty())
        result.subtitle = "未知";
    return result;
}

} // namespace mcast::avalonia::qtfree

