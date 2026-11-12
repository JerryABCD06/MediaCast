// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/playback/DlnaPlaybackPlanner.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlwapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace mcast::dlna::planning {
namespace {

constexpr std::size_t kMaxPlaylistBytes = 8 * 1024 * 1024;
constexpr std::size_t kMaxProbeBytes = 256 * 1024;
constexpr std::size_t kMaxSegments = 1024;

std::wstring widen(const std::string &value)
{
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1)
        return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

std::string narrow(const wchar_t *value, int length = -1)
{
    if (!value)
        return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, length, result.data(), size,
        nullptr, nullptr);
    if (length < 0)
        result.resize(static_cast<std::size_t>(size - 1));
    return result;
}

std::string trim(std::string value)
{
    const auto isSpace = [](unsigned char ch) {
        return std::isspace(ch) != 0;
    };
    value.erase(
        value.begin(),
        std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(
        std::find_if_not(value.rbegin(), value.rend(), isSpace).base(),
        value.end());
    return value;
}

std::string lower(std::string value)
{
    std::transform(
        value.begin(), value.end(), value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return value;
}

std::string withoutQuery(std::string url)
{
    const std::size_t cut = url.find_first_of("?#");
    if (cut != std::string::npos)
        url.resize(cut);
    return url;
}

std::string extensionOf(const std::string &url)
{
    const std::string path = withoutQuery(url);
    const std::size_t slash = path.find_last_of('/');
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos
        || (slash != std::string::npos && dot < slash)) {
        return {};
    }
    return lower(path.substr(dot + 1));
}

bool isImageUrl(const std::string &url)
{
    static const char *const extensions[] = {
        "png", "jpg", "jpeg", "webp", "gif", "bmp",
        "avif", "heic", "heif", "tif", "tiff",
    };
    const std::string extension = extensionOf(url);
    return std::any_of(
        std::begin(extensions), std::end(extensions),
        [&](const char *candidate) {
            return extension == candidate;
        });
}

bool isHlsUrl(const std::string &url)
{
    return extensionOf(url) == "m3u8";
}

bool isAbsoluteUrl(const std::string &url)
{
    const std::string prefix = lower(url.substr(0, 8));
    return prefix.starts_with("http://")
        || prefix.starts_with("https://");
}

std::string resolveUrl(
    const std::string &base, const std::string &reference)
{
    if (reference.empty())
        return {};
    if (isAbsoluteUrl(reference))
        return reference;

    const std::wstring baseWide = widen(base);
    const std::wstring referenceWide = widen(reference);
    if (baseWide.empty() || referenceWide.empty())
        return {};
    DWORD length = 0;
    UrlCombineW(
        baseWide.c_str(), referenceWide.c_str(),
        nullptr, &length, 0);
    if (length == 0)
        return {};
    std::wstring combined(static_cast<std::size_t>(length), L'\0');
    if (UrlCombineW(
            baseWide.c_str(), referenceWide.c_str(),
            combined.data(), &length, 0) != S_OK) {
        return {};
    }
    if (!combined.empty() && combined.back() == L'\0')
        combined.pop_back();
    return narrow(combined.c_str(), static_cast<int>(combined.size()));
}

struct PlaylistSegment {
    std::string url;
    double durationSeconds = 0.0;
};

struct ParsedPlaylist {
    std::vector<PlaylistSegment> segments;
    std::string error;
};

ParsedPlaylist parsePlaylist(
    const std::vector<std::uint8_t> &bytes,
    const std::string &playlistUrl)
{
    ParsedPlaylist result;
    const std::string content(bytes.begin(), bytes.end());
    std::istringstream stream(content);
    std::string rawLine;
    if (!std::getline(stream, rawLine)
        || !trim(rawLine).starts_with("#EXTM3U")) {
        result.error = "not an HLS playlist";
        return result;
    }

    double pendingDuration = 2.0;
    while (std::getline(stream, rawLine)) {
        const std::string line = trim(rawLine);
        if (line.empty())
            continue;
        if (line.starts_with("#EXT-X-STREAM-INF")) {
            result.error = "HLS master playlist";
            result.segments.clear();
            return result;
        }
        if (line.starts_with("#EXTINF:")) {
            std::string value = line.substr(8);
            const std::size_t comma = value.find(',');
            if (comma != std::string::npos)
                value.resize(comma);
            try {
                const double parsed = std::stod(value);
                if (parsed > 0.0)
                    pendingDuration = parsed;
            } catch (...) {
            }
            continue;
        }
        if (line.front() == '#')
            continue;

        const std::string resolved = resolveUrl(playlistUrl, line);
        if (resolved.empty() || !isImageUrl(resolved)) {
            result.error = "HLS segment is not an image";
            result.segments.clear();
            return result;
        }
        if (result.segments.size() >= kMaxSegments) {
            result.error = "HLS playlist has too many image segments";
            result.segments.clear();
            return result;
        }
        result.segments.push_back({
            resolved,
            std::clamp(pendingDuration, 0.05, 3600.0),
        });
        pendingDuration = 2.0;
    }
    if (result.segments.empty())
        result.error = "HLS playlist has no media segments";
    return result;
}

std::optional<std::size_t> wrappedTransportStreamOffset(
    const std::vector<std::uint8_t> &bytes)
{
    static constexpr std::uint8_t signature[] = {
        0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
    };
    if (bytes.size() < sizeof(signature)
        || !std::equal(
            std::begin(signature), std::end(signature),
            bytes.begin())) {
        return std::nullopt;
    }

    std::size_t offset = sizeof(signature);
    for (int chunks = 0; chunks < 128 && offset + 12 <= bytes.size();
         ++chunks) {
        const std::uint32_t length =
            (static_cast<std::uint32_t>(bytes[offset]) << 24)
            | (static_cast<std::uint32_t>(bytes[offset + 1]) << 16)
            | (static_cast<std::uint32_t>(bytes[offset + 2]) << 8)
            | static_cast<std::uint32_t>(bytes[offset + 3]);
        const std::size_t next =
            offset + 12 + static_cast<std::size_t>(length);
        if (next < offset || next > bytes.size())
            return std::nullopt;
        if (std::equal(
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8),
                "IEND")) {
            constexpr std::size_t kTsPacketSize = 188;
            if (next + 2 * kTsPacketSize >= bytes.size())
                return std::nullopt;
            for (int packet = 0; packet < 3; ++packet) {
                if (bytes[next + packet * kTsPacketSize] != 0x47)
                    return std::nullopt;
            }
            return next;
        }
        offset = next;
    }
    return std::nullopt;
}

bool looksLikeImage(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.size() >= 8
        && bytes[0] == 0x89 && bytes[1] == 'P'
        && bytes[2] == 'N' && bytes[3] == 'G'
        && bytes[4] == '\r' && bytes[5] == '\n'
        && bytes[6] == 0x1a && bytes[7] == '\n') {
        return true;
    }
    if (bytes.size() >= 3
        && bytes[0] == 0xff && bytes[1] == 0xd8
        && bytes[2] == 0xff) {
        return true;
    }
    if (bytes.size() >= 6) {
        const std::string prefix(
            reinterpret_cast<const char *>(bytes.data()),
            std::min<std::size_t>(12, bytes.size()));
        if (prefix.starts_with("GIF87a")
            || prefix.starts_with("GIF89a")
            || prefix.starts_with("BM")) {
            return true;
        }
        if (prefix.size() >= 12
            && prefix.starts_with("RIFF")
            && prefix.substr(8, 4) == "WEBP") {
            return true;
        }
    }
    return false;
}

bool winHttpGet(
    const std::string &url,
    const std::optional<std::string> &range,
    std::size_t maxBytes,
    PlanningHttpResponse &response,
    std::string &error)
{
    response = {};
    const std::wstring wideUrl = widen(url);
    if (wideUrl.empty()) {
        error = "invalid media URL";
        return false;
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts)
        || !parts.lpszHostName) {
        error = "cannot parse media URL";
        return false;
    }
    const std::wstring host(
        parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target;
    if (parts.lpszUrlPath) {
        target.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    }
    if (target.empty())
        target = L"/";
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength)
        target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HINTERNET session = WinHttpOpen(
        L"MediaCast/1.0",
        WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        error = "cannot initialize WinHTTP";
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 10000);
    HINTERNET connection = WinHttpConnect(
        session, host.c_str(), parts.nPort, 0);
    HINTERNET request = connection
        ? WinHttpOpenRequest(
              connection, L"GET", target.c_str(), nullptr,
              WINHTTP_NO_REFERER,
              WINHTTP_DEFAULT_ACCEPT_TYPES,
              parts.nScheme == INTERNET_SCHEME_HTTPS
                  ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;
    if (request) {
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(
            request, WINHTTP_OPTION_REDIRECT_POLICY,
            &redirectPolicy, sizeof(redirectPolicy));
    }

    std::wstring rangeHeader;
    if (request && range) {
        rangeHeader = widen("Range: " + *range + "\r\n");
    }
    bool bodyReadFailed = false;
    const bool sent = request
        && WinHttpSendRequest(
            request,
            rangeHeader.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                : rangeHeader.c_str(),
            rangeHeader.empty()
                ? 0 : static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        && WinHttpReceiveResponse(request, nullptr);
    if (sent) {
        DWORD status = 0;
        DWORD statusSize = sizeof(status);
        WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
            WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(status);

        DWORD urlLength = 0;
        WinHttpQueryOption(
            request, WINHTTP_OPTION_URL, nullptr, &urlLength);
        if (urlLength > sizeof(wchar_t)) {
            std::wstring finalUrl(urlLength / sizeof(wchar_t), L'\0');
            if (WinHttpQueryOption(
                    request, WINHTTP_OPTION_URL,
                    finalUrl.data(), &urlLength)) {
                while (!finalUrl.empty()
                       && finalUrl.back() == L'\0') {
                    finalUrl.pop_back();
                }
                response.finalUrl = narrow(
                    finalUrl.c_str(),
                    static_cast<int>(finalUrl.size()));
            }
        }
        if (response.finalUrl.empty())
            response.finalUrl = url;

        while (response.body.size() < maxBytes) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available)) {
                bodyReadFailed = true;
                error = "cannot query HTTP response body";
                break;
            }
            if (available == 0)
                break;
            const std::size_t wanted = std::min<std::size_t>(
                available, maxBytes - response.body.size());
            const std::size_t oldSize = response.body.size();
            response.body.resize(oldSize + wanted);
            DWORD read = 0;
            if (!WinHttpReadData(
                    request, response.body.data() + oldSize,
                    static_cast<DWORD>(wanted), &read)) {
                response.body.resize(oldSize);
                bodyReadFailed = true;
                error = "cannot read HTTP response body";
                break;
            }
            response.body.resize(oldSize + read);
            if (read == 0)
                break;
        }
    } else {
        error = "HTTP request failed (Win32 "
            + std::to_string(GetLastError()) + ")";
    }

    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return sent && !bodyReadFailed;
}

} // namespace

DlnaPlaybackPlanner::DlnaPlaybackPlanner(
    Log log, PlanningHttpGet httpGet)
    : m_log(std::move(log))
    , m_httpGet(httpGet ? std::move(httpGet) : PlanningHttpGet(winHttpGet))
{
}

::mcast::playback::MediaRequest DlnaPlaybackPlanner::plan(
    const ::mcast::playback::MediaRequest &request) const
{
    ::mcast::playback::MediaRequest result = request;
    if (result.route != ::mcast::playback::MediaRoute::Native
        || !result.imageSequenceFrames.empty()
        || !isHlsUrl(result.uri)) {
        return result;
    }

    PlanningHttpResponse playlist;
    std::string error;
    if (!get(result.uri, std::nullopt, kMaxPlaylistBytes, playlist, error)
        || playlist.status != 200) {
        log(
            "HLS playlist probe failed: "
            + (error.empty()
                   ? "HTTP " + std::to_string(playlist.status)
                   : error));
        return result;
    }

    const ParsedPlaylist parsed =
        parsePlaylist(playlist.body, playlist.finalUrl);
    if (!parsed.error.empty()) {
        log("HLS compatibility probe: " + parsed.error);
        return result;
    }

    PlanningHttpResponse firstSegment;
    error.clear();
    const bool probeSucceeded = get(
        parsed.segments.front().url,
        std::string("bytes=0-262143"),
        kMaxProbeBytes, firstSegment, error);
    if (!probeSucceeded
        || (firstSegment.status != 200 && firstSegment.status != 206)
        || !looksLikeImage(firstSegment.body)) {
        log(
            "HLS image probe failed: "
            + (error.empty()
                   ? "unexpected first-segment payload"
                   : error));
        return result;
    }
    const bool wrapped =
        wrappedTransportStreamOffset(firstSegment.body).has_value();

    result.kind = ::mcast::playback::MediaKind::Video;
    result.route = wrapped
        ? ::mcast::playback::MediaRoute::WrappedTransportStream
        : ::mcast::playback::MediaRoute::ImageSequence;
    result.imageSequenceFrames.clear();
    result.imageSequenceFrames.reserve(parsed.segments.size());
    for (const PlaylistSegment &segment : parsed.segments) {
        result.imageSequenceFrames.push_back({
            segment.url,
            segment.durationSeconds,
        });
    }
    log(
        std::string("HLS compatibility plan: ")
        + (wrapped ? "wrapped transport stream" : "image sequence")
        + ", segments="
        + std::to_string(result.imageSequenceFrames.size()));
    return result;
}

bool DlnaPlaybackPlanner::get(
    const std::string &url,
    const std::optional<std::string> &range,
    std::size_t maxBytes,
    PlanningHttpResponse &response,
    std::string &error) const
{
    if (!m_httpGet) {
        error = "HTTP transport is unavailable";
        return false;
    }
    return m_httpGet(url, range, maxBytes, response, error);
}

void DlnaPlaybackPlanner::log(const std::string &message) const
{
    if (!m_log)
        return;
    try {
        m_log(message);
    } catch (...) {
    }
}

} // namespace mcast::dlna::planning
