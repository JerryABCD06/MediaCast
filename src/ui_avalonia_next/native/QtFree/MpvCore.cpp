// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/MpvCore.h"

#include "QtFree/Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <map>
#include <sstream>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

struct PictureControlSpec
{
    const char *name;
    const char *mpvProperty;
    int minimum;
    int maximum;
    int neutral;
    double scale;
};

constexpr PictureControlSpec kPictureControls[] = {
    {"brightness", "brightness", -100, 100, 0, 1.0},
    {"contrast", "contrast", -100, 100, 0, 1.0},
    {"saturation", "saturation", -100, 100, 0, 1.0},
    {"gamma", "gamma", -100, 100, 0, 1.0},
    {"hue", "hue", -100, 100, 0, 1.0},
    {"sharpen", nullptr, -100, 100, 0, 1.0},
    {"zoom", "video-zoom", -20, 20, 0, 1.0},
    {"pan-x", "video-pan-x", -100, 100, 0, 0.01},
    {"pan-y", "video-pan-y", -100, 100, 0, 0.01},
    {"rotate", "video-rotate", 0, 359, 0, 1.0},
};

enum PropertyId {
    IdTimePos = 1,
    IdDuration = 2,
    IdPause = 3,
    IdVolume = 4,
    IdMute = 5,
    IdEof = 6,
    IdMetadata = 7,
    IdTrackList = 8,
};

const PictureControlSpec *findPictureSpec(std::string_view name)
{
    for (const auto &spec : kPictureControls) {
        if (name == spec.name)
            return &spec;
    }
    return nullptr;
}

std::string mpvError(int code)
{
    return mpv_error_string(code);
}

const mpv_node *mapValue(const mpv_node &map, const char *key)
{
    if (map.format != MPV_FORMAT_NODE_MAP || !map.u.list)
        return nullptr;
    for (int index = 0; index < map.u.list->num; ++index) {
        if (map.u.list->keys[index]
            && std::strcmp(map.u.list->keys[index], key) == 0) {
            return &map.u.list->values[index];
        }
    }
    return nullptr;
}

std::optional<std::string> stringValue(
    const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    if (!value || value->format != MPV_FORMAT_STRING
        || !value->u.string) {
        return std::nullopt;
    }
    return std::string(value->u.string);
}

std::optional<std::int64_t> intValue(
    const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    return value && value->format == MPV_FORMAT_INT64
        ? std::optional<std::int64_t>(value->u.int64)
        : std::nullopt;
}

std::optional<double> doubleValue(
    const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    if (!value)
        return std::nullopt;
    if (value->format == MPV_FORMAT_DOUBLE)
        return value->u.double_;
    if (value->format == MPV_FORMAT_INT64)
        return static_cast<double>(value->u.int64);
    return std::nullopt;
}

bool flagValue(const mpv_node &map, const char *key)
{
    const mpv_node *value = mapValue(map, key);
    return value && value->format == MPV_FORMAT_FLAG
        && value->u.flag != 0;
}

std::map<std::string, std::string, std::less<>> metadataFromNode(
    const mpv_node *node)
{
    std::map<std::string, std::string, std::less<>> values;
    if (!node || node->format != MPV_FORMAT_NODE_MAP || !node->u.list)
        return values;
    for (int index = 0; index < node->u.list->num; ++index) {
        const mpv_node &value = node->u.list->values[index];
        if (node->u.list->keys[index]
            && value.format == MPV_FORMAT_STRING
            && value.u.string) {
            values.emplace(node->u.list->keys[index], value.u.string);
        }
    }
    return values;
}

std::string currentMediaUri(mpv_handle *mpv)
{
    if (!mpv)
        return {};
    char *value = mpv_get_property_string(mpv, "path");
    if (!value)
        return {};
    std::string uri(value);
    mpv_free(value);
    return uri;
}

std::string utf8Path(const std::filesystem::path &path)
{
    const auto bytes = path.u8string();
    return std::string(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::string embeddedAlbumArt(
    mpv_handle *mpv, std::string_view mediaUri)
{
    std::error_code error;
    const auto directory =
        std::filesystem::temp_directory_path(error)
        / "MediaCast" / "artwork";
    std::filesystem::create_directories(directory, error);
    if (error || !std::filesystem::is_directory(directory, error)
        || error) {
        return {};
    }

    std::uint64_t hash = 1469598103934665603ull;
    for (const unsigned char ch : mediaUri) {
        hash ^= ch;
        hash *= 1099511628211ull;
    }
    std::ostringstream name;
    name << "cover-" << std::hex << std::setw(16)
         << std::setfill('0') << hash << ".png";
    const auto path = directory / name.str();
    const std::string pathText = utf8Path(path);
    const char *args[] = {
        "screenshot-to-file", pathText.c_str(), "video", nullptr};
    mpv_node result{};
    const int status = mpv_command_ret(mpv, args, &result);
    mpv_free_node_contents(&result);
    if (status < 0)
        return {};

    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > 16ull * 1024ull * 1024ull) {
        std::filesystem::remove(path, error);
        return {};
    }
    const std::string current = path.filename().string();
    for (std::filesystem::directory_iterator iterator(
             directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error)
            && iterator->path().filename().string() != current
            && iterator->path().filename().string().starts_with(
                "cover-")) {
            std::filesystem::remove(iterator->path(), error);
        }
    }
    return fileUriFromPath(pathText);
}

std::string albumArt(mpv_handle *mpv, std::string_view mediaUri)
{
    if (!mpv)
        return {};
    mpv_node root{};
    if (mpv_get_property(mpv, "track-list", MPV_FORMAT_NODE, &root) < 0)
        return {};
    struct Guard {
        mpv_node *node;
        ~Guard() { mpv_free_node_contents(node); }
    } guard{&root};
    if (root.format != MPV_FORMAT_NODE_ARRAY || !root.u.list)
        return {};
    for (int index = 0; index < root.u.list->num; ++index) {
        const mpv_node &track = root.u.list->values[index];
        if (!flagValue(track, "albumart"))
            continue;
        if (const auto external =
                stringValue(track, "external-filename")) {
            return fileUriFromPath(*external);
        }
        return embeddedAlbumArt(mpv, mediaUri);
    }
    return {};
}

struct MediaSummary
{
    JsonValue info = JsonValue::object();
    MediaKind kind = MediaKind::Unknown;
};

MediaSummary mediaSummary(mpv_handle *mpv, double durationSeconds)
{
    MediaSummary summary;
    if (!mpv)
        return summary;
    mpv_node tracks{};
    if (mpv_get_property(
            mpv, "track-list", MPV_FORMAT_NODE, &tracks) < 0
        || tracks.format != MPV_FORMAT_NODE_ARRAY
        || !tracks.u.list) {
        return summary;
    }
    struct Guard {
        mpv_node *node;
        ~Guard() { mpv_free_node_contents(node); }
    } guard{&tracks};

    mpv_node videoParams{};
    const bool hasVideoParams =
        mpv_get_property(
            mpv, "video-params", MPV_FORMAT_NODE, &videoParams) >= 0
        && videoParams.format == MPV_FORMAT_NODE_MAP;
    Guard videoGuard{&videoParams};
    mpv_node audioParams{};
    const bool hasAudioParams =
        mpv_get_property(
            mpv, "audio-params", MPV_FORMAT_NODE, &audioParams) >= 0
        && audioParams.format == MPV_FORMAT_NODE_MAP;
    Guard audioGuard{&audioParams};

    JsonValue video = JsonValue::object();
    JsonValue audio = JsonValue::object();
    bool image = false;
    bool haveVideo = false;
    bool haveAudio = false;
    for (int index = 0; index < tracks.u.list->num; ++index) {
        const mpv_node &track = tracks.u.list->values[index];
        const std::string type =
            stringValue(track, "type").value_or("");
        if (type == "video" && !flagValue(track, "albumart")
            && !haveVideo) {
            haveVideo = true;
            image = flagValue(track, "image");
            video.set(
                "codec",
                JsonValue(stringValue(track, "codec").value_or("")));
            video.set(
                "profile",
                JsonValue(
                    stringValue(track, "codec-profile")
                        .value_or(
                            stringValue(track, "profile").value_or(""))));
            if (const auto width = intValue(track, "demux-w"))
                video.set(
                    "width", JsonValue(static_cast<double>(*width)));
            if (const auto height = intValue(track, "demux-h"))
                video.set(
                    "height", JsonValue(static_cast<double>(*height)));
            if (const auto fps = doubleValue(track, "demux-fps"))
                video.set("fps", JsonValue(*fps));
            if (const auto bitrate = intValue(track, "demux-bitrate"))
                video.set(
                    "bitrate",
                    JsonValue(static_cast<double>(*bitrate)));
            if (hasVideoParams) {
                if (const auto width = intValue(videoParams, "w"))
                    video.set(
                        "width",
                        JsonValue(static_cast<double>(*width)));
                if (const auto height = intValue(videoParams, "h"))
                    video.set(
                        "height",
                        JsonValue(static_cast<double>(*height)));
                const std::string pixelFormat =
                    stringValue(videoParams, "pixelformat").value_or("");
                if (!pixelFormat.empty())
                    video.set("pixel_format", JsonValue(pixelFormat));
                if (const auto aspect =
                        doubleValue(videoParams, "aspect")) {
                    video.set("aspect", JsonValue(*aspect));
                }
            }
        } else if (type == "audio" && !haveAudio) {
            haveAudio = true;
            audio.set(
                "codec",
                JsonValue(stringValue(track, "codec").value_or("")));
            audio.set(
                "profile",
                JsonValue(
                    stringValue(track, "codec-profile")
                        .value_or(
                            stringValue(track, "profile").value_or(""))));
            if (const auto rate = intValue(track, "demux-samplerate"))
                audio.set(
                    "sample_rate",
                    JsonValue(static_cast<double>(*rate)));
            if (const auto channels =
                    intValue(track, "demux-channel-count")) {
                audio.set(
                    "channels",
                    JsonValue(static_cast<double>(*channels)));
            }
            const std::string layout =
                stringValue(track, "demux-channels").value_or("");
            if (!layout.empty())
                audio.set("channel_layout", JsonValue(layout));
            if (const auto bitrate = intValue(track, "demux-bitrate"))
                audio.set(
                    "bitrate",
                    JsonValue(static_cast<double>(*bitrate)));
            if (hasAudioParams) {
                if (const auto rate =
                        intValue(audioParams, "samplerate")) {
                    audio.set(
                        "sample_rate",
                        JsonValue(static_cast<double>(*rate)));
                }
                if (const auto channels =
                        intValue(audioParams, "channel-count")) {
                    audio.set(
                        "channels",
                        JsonValue(static_cast<double>(*channels)));
                }
                const std::string layout =
                    stringValue(audioParams, "channels").value_or("");
                if (!layout.empty())
                    audio.set("channel_layout", JsonValue(layout));
                const std::string sampleFormat =
                    stringValue(audioParams, "format").value_or("");
                if (!sampleFormat.empty())
                    audio.set("sample_format", JsonValue(sampleFormat));
            }
        }
    }

    std::int64_t videoBitrate = 0;
    if (mpv_get_property(
            mpv, "video-bitrate", MPV_FORMAT_INT64, &videoBitrate)
            >= 0
        && videoBitrate > 0) {
        video.set(
            "bitrate", JsonValue(static_cast<double>(videoBitrate)));
    }
    std::int64_t audioBitrate = 0;
    if (mpv_get_property(
            mpv, "audio-bitrate", MPV_FORMAT_INT64, &audioBitrate)
            >= 0
        && audioBitrate > 0) {
        audio.set(
            "bitrate", JsonValue(static_cast<double>(audioBitrate)));
    }
    char *format = mpv_get_property_string(mpv, "file-format");
    if (format) {
        summary.info.set("container", JsonValue(simplified(format)));
        mpv_free(format);
    }
    summary.info.set("duration_seconds", JsonValue(durationSeconds));
    if (haveVideo) {
        summary.info.set("video", video);
        summary.kind = image ? MediaKind::Image : MediaKind::Video;
    }
    if (haveAudio) {
        summary.info.set("audio", audio);
        if (summary.kind == MediaKind::Unknown)
            summary.kind = MediaKind::Audio;
    }
    if (summary.kind != MediaKind::Unknown)
        summary.info.set("kind", JsonValue(mediaKindKey(summary.kind)));
    return summary;
}

} // namespace

MpvCore::MpvCore(EventLoop &loop)
    : m_loop(loop)
    , m_imageSequence(std::make_unique<ImageSequenceStrategy>(loop))
{
    m_imageSequenceConnections.push_back(
        m_imageSequence->positionChanged.connect([this](double seconds) {
            m_positionSeconds = seconds;
            positionChanged.emit(seconds);
        }));
    m_imageSequenceConnections.push_back(
        m_imageSequence->durationChanged.connect([this](double seconds) {
            m_durationSeconds = seconds;
            publishMediaSummary(m_mediaEpoch.load());
            durationChanged.emit(seconds);
        }));
    m_imageSequenceConnections.push_back(
        m_imageSequence->ended.connect([this] { ended.emit(); }));
    m_imageSequenceConnections.push_back(m_imageSequence->loadFailed.connect(
        [this](const std::string &reason) {
            m_loadFailed = true;
            m_imageSequenceActive = false;
            logMessage.emit("图片序列加载失败：" + reason);
            loadFailed.emit(m_pendingLoadGeneration.load(), reason);
        }));
    m_imageSequenceConnections.push_back(m_imageSequence->logMessage.connect(
        [this](const std::string &message) {
            logMessage.emit(message);
        }));
}

MpvCore::~MpvCore()
{
    shutdown();
}

bool MpvCore::start()
{
    if (m_mpv)
        return true;
    m_stopRequested = false;
    m_mpv = mpv_create();
    if (!m_mpv) {
        statusChanged.emit("libmpv 初始化失败：mpv_create 返回空");
        return false;
    }
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "image-display-duration", "inf");
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "cursor-autohide", "always");
    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "vo", "libmpv");
    mpv_set_option_string(m_mpv, "gpu-api", "opengl");
    const int result = mpv_initialize(m_mpv);
    if (result < 0) {
        statusChanged.emit(
            "libmpv 初始化失败：" + mpvError(result));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }
    mpv_request_log_messages(m_mpv, "warn");
    mpv_observe_property(
        m_mpv, IdTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(
        m_mpv, IdDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, IdEof, "eof-reached", MPV_FORMAT_FLAG);
    mpv_observe_property(
        m_mpv, IdVolume, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdMute, "mute", MPV_FORMAT_FLAG);
    mpv_observe_property(
        m_mpv, IdMetadata, "metadata", MPV_FORMAT_NODE);
    mpv_observe_property(
        m_mpv, IdTrackList, "track-list", MPV_FORMAT_NODE);
    mpv_set_property_string(m_mpv, "title", "MCast.Ui.Avalonia");
    m_eventThread = std::jthread(
        [this](std::stop_token stop) { eventLoop(stop); });
    statusChanged.emit("libmpv 已就绪");
    runningChanged.emit(true);
    return true;
}

void MpvCore::shutdown()
{
    m_stopRequested = true;
    if (m_mpv)
        mpv_wakeup(m_mpv);
    if (m_eventThread.joinable())
        m_eventThread.join();
    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        runningChanged.emit(false);
    }
    if (m_imageSequence)
        m_imageSequence->reset();
    m_imageSequenceActive = false;
    m_wrappedTsProxy.reset();
    m_pendingLoad.reset();
    m_lastLoadRequest.reset();
}

void MpvCore::setRendererAttached(bool attached)
{
    m_rendererAttached = attached;
    if (!attached)
        return;
    rendererReady.emit();
    std::optional<MediaLoadRequest> request = std::move(m_pendingLoad);
    m_pendingLoad.reset();
    if ((!request || request->logicalUri.empty())
        && m_loadFailed.load() && m_lastLoadRequest) {
        request = m_lastLoadRequest;
        m_loadFailed = false;
    }
    if (!request || request->logicalUri.empty())
        return;
    logMessage.emit("渲染窗口已就绪，把攒着的那条放出去");
    activateLoad(*request);
}

void MpvCore::load(const MediaLoadRequest &request)
{
    if (!m_mpv || request.logicalUri.empty())
        return;
    {
        const std::scoped_lock lock(m_uriMutex);
        m_lastRequestedUri = request.logicalUri;
    }
    m_lastLoadRequest = request;
    m_pendingLoadGeneration = request.loadGeneration;
    m_loadFailed = false;
    if (!m_rendererAttached) {
        m_pendingLoad = request;
        logMessage.emit(
            "渲染窗口还没准备好，这条先攒着，等窗口起来再放");
        return;
    }
    activateLoad(request);
}

void MpvCore::activateLoad(const MediaLoadRequest &request)
{
    if (request.route == MediaLoadRoute::ImageSequence) {
        resetLoadState(request.logicalUri);
        loadImageSequencePlan(request);
        return;
    }
    if (request.route == MediaLoadRoute::WrappedTransportStream) {
        resetLoadState(request.logicalUri);
        m_imageSequenceActive = false;
        m_imageSequence->reset();
        m_wrappedTsProxy = std::make_unique<WrappedTsProxy>();
        std::string proxyError;
        if (!m_wrappedTsProxy->start(
                request.imageSequenceFrames, proxyError)) {
            const std::string reason =
                "无法启动本地媒体代理：" + proxyError;
            m_loadFailed = true;
            m_wrappedTsProxy.reset();
            logMessage.emit(reason);
            loadFailed.emit(
                m_pendingLoadGeneration.load(), reason);
            return;
        }
        logMessage.emit(
            "检测到 PNG 包装的 MPEG-TS，已恢复 "
            + std::to_string(request.imageSequenceFrames.size())
            + " 段视频");
        loadMpvFile(
            m_wrappedTsProxy->playlistUrl(), request.logicalUri);
        return;
    }
    if (request.route != MediaLoadRoute::Native) {
        const std::string reason =
            "compatibility playback route is not available";
        m_loadFailed = true;
        logMessage.emit(reason);
        post([this, reason] {
            loadFailed.emit(m_pendingLoadGeneration.load(), reason);
        });
        return;
    }
    m_imageSequenceActive = false;
    m_imageSequence->reset();
    m_wrappedTsProxy.reset();
    resetLoadState(request.logicalUri);
    loadMpvFile(request.logicalUri, request.logicalUri);
}

void MpvCore::resetLoadState(const std::string &logicalUri)
{
    m_mediaSummaryReady = false;
    ++m_mediaEpoch;
    m_playOnLoad = true;
    m_loadFailed = false;
    m_eofReached = false;
    m_positionSeconds = 0;
    m_durationSeconds = 0;
    {
        const std::scoped_lock lock(m_uriMutex);
        m_lastRequestedUri = logicalUri;
        m_activeMpvUri.clear();
        m_eventMediaUri.clear();
    }
    clearMediaSummary();
}

void MpvCore::loadNow(const std::string &uri)
{
    m_imageSequenceActive = false;
    m_imageSequence->reset();
    m_wrappedTsProxy.reset();
    resetLoadState(uri);
    loadMpvFile(uri, uri);
}

bool MpvCore::loadImageSequencePlan(
    const MediaLoadRequest &request)
{
    m_imageSequence->reset();
    m_imageSequenceActive = false;
    m_wrappedTsProxy.reset();
    if (request.imageSequenceFrames.empty()) {
        const std::string reason = "图片序列计划没有帧";
        m_loadFailed = true;
        logMessage.emit(reason);
        loadFailed.emit(m_pendingLoadGeneration.load(), reason);
        return false;
    }
    logMessage.emit(
        "收到协议指定的图片序列计划："
        + std::to_string(request.imageSequenceFrames.size()) + " 帧");
    return loadMfImageSequence(
        request.imageSequenceFrames, request.logicalUri);
}

bool MpvCore::loadMfImageSequence(
    const std::vector<MediaLoadFrame> &segments,
    const std::string &logicalUri)
{
    ++m_imageSequenceLoadGeneration;
    m_imageSequenceActive = true;
    const bool started = m_imageSequence->start(
        segments,
        [this, logicalUri](
            const std::string &mfUri,
            const std::string &,
            double fps, bool play) {
            char fpsText[64]{};
            std::snprintf(fpsText, sizeof(fpsText), "%.6f", fps);
            mpv_set_property_string(m_mpv, "mf-fps", fpsText);
            m_playOnLoad = play;
            loadMpvFile(mfUri, logicalUri, true);
            return true;
        });
    m_imageSequenceActive = started;
    if (!started)
        m_loadFailed = true;
    return started;
}

void MpvCore::loadMpvFile(
    const std::string &mpvUri,
    const std::string &logicalUri,
    bool emitStarted)
{
    m_currentLoadCommandEpoch.fetch_add(1);
    m_commandMediaEpoch = m_mediaEpoch.load();
    {
        const std::scoped_lock lock(m_uriMutex);
        m_lastRequestedUri = logicalUri;
        m_activeMpvUri = mpvUri;
        m_eventMediaUri = logicalUri;
    }
    const char *command[] = {
        "loadfile", mpvUri.c_str(), "replace", nullptr};
    mpv_command(m_mpv, command);
    if (emitStarted)
        loadStarted.emit(m_pendingLoadGeneration.load());
}

std::string MpvCore::logicalMediaUri(const std::string &mpvUri) const
{
    const std::scoped_lock lock(m_uriMutex);
    const std::string effective =
        mpvUri.empty() ? m_activeMpvUri : mpvUri;
    if (!m_activeMpvUri.empty()
        && effective == m_activeMpvUri
        && !m_lastRequestedUri.empty()) {
        return m_lastRequestedUri;
    }
    return mpvUri;
}

bool MpvCore::mediaEventsCurrent() const
{
    return m_eventLoadCommandEpoch.load()
            == m_currentLoadCommandEpoch.load()
        && m_eventMediaEpoch.load() == m_mediaEpoch.load();
}

void MpvCore::publishMediaSummary(std::uint64_t expectedEpoch)
{
    if (!m_mpv || expectedEpoch != m_mediaEpoch.load()
        || !m_mediaSummaryReady.load() || !mediaEventsCurrent()) {
        return;
    }
    const MediaSummary summary =
        mediaSummary(m_mpv, m_durationSeconds.load());
    {
        const std::scoped_lock lock(m_mediaInfoMutex);
        m_mediaInfoJson = summary.info.dump();
    }
    mediaInfoChanged.emit();

    std::string mediaUri = logicalMediaUri(currentMediaUri(m_mpv));
    {
        const std::scoped_lock lock(m_uriMutex);
        if (mediaUri.empty())
            mediaUri = m_eventMediaUri;
        if (mediaUri.empty())
            mediaUri = m_lastRequestedUri;
    }
    if (summary.kind != MediaKind::Unknown && !mediaUri.empty())
        mediaKindChanged.emit(summary.kind, mediaUri);
}

void MpvCore::clearMediaSummary()
{
    {
        const std::scoped_lock lock(m_mediaInfoMutex);
        m_mediaInfoJson = "{}";
    }
    mediaInfoChanged.emit();
}

std::string MpvCore::mediaInfoJson() const
{
    const std::scoped_lock lock(m_mediaInfoMutex);
    return m_mediaInfoJson;
}

void MpvCore::eventLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        mpv_event *event = mpv_wait_event(m_mpv, 0.25);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            if (m_stopRequested.load()) {
                finishPacketCorruptBurst();
                break;
            }
            if (m_packetCorruptBurst
                && std::chrono::steady_clock::now()
                        - m_packetCorruptLast
                    >= std::chrono::seconds(1)) {
                finishPacketCorruptBurst();
            }
            continue;
        }

        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *property =
                static_cast<mpv_event_property *>(event->data);
            if (!property)
                break;
            switch (event->reply_userdata) {
            case IdTimePos: {
                if (m_imageSequenceActive.load())
                    break;
                const double value = property->data
                    ? *static_cast<double *>(property->data) : 0;
                if (!std::isfinite(value) || std::abs(value) > 1e9) {
                    logMessage.emit("忽略异常的 MPV 播放位置");
                    break;
                }
                m_positionSeconds = value;
                const std::uint64_t epoch = m_mediaEpoch.load();
                post([this, value, epoch] {
                    if (epoch == m_mediaEpoch.load())
                        positionChanged.emit(value);
                });
                break;
            }
            case IdDuration: {
                if (m_imageSequenceActive.load())
                    break;
                const double value = property->data
                    ? *static_cast<double *>(property->data) : 0;
                m_durationSeconds = value;
                const std::uint64_t epoch = m_mediaEpoch.load();
                post([this, value, epoch] {
                    if (epoch != m_mediaEpoch.load())
                        return;
                    publishMediaSummary(epoch);
                    durationChanged.emit(value);
                });
                break;
            }
            case IdPause: {
                const bool paused = property->data
                    && *static_cast<int *>(property->data) != 0;
                logMessage.emit(
                    "MPV pause property -> "
                    + std::string(paused ? "true" : "false"));
                post([this, paused] { pausedChanged.emit(paused); });
                break;
            }
            case IdEof: {
                if (m_imageSequenceActive.load())
                    break;
                const bool eof = property->data
                    && *static_cast<int *>(property->data) != 0;
                const bool was = m_eofReached.exchange(eof);
                if (eof && !was) {
                    const std::uint64_t epoch = m_mediaEpoch.load();
                    post([this, epoch] {
                        if (epoch == m_mediaEpoch.load()
                            && mediaEventsCurrent()) {
                            ended.emit();
                        }
                    });
                }
                break;
            }
            case IdVolume: {
                const int value = property->data
                    ? static_cast<int>(std::lround(
                          *static_cast<double *>(property->data)))
                    : 0;
                m_volumePercent = value;
                post([this, value] { volumeChanged.emit(value); });
                break;
            }
            case IdMute: {
                const bool value = property->data
                    && *static_cast<int *>(property->data) != 0;
                m_muted = value;
                post([this, value] { muteChanged.emit(value); });
                break;
            }
            case IdMetadata: {
                auto tags = metadataFromNode(
                    static_cast<mpv_node *>(property->data));
                std::string uri;
                {
                    const std::scoped_lock lock(m_uriMutex);
                    uri = m_eventMediaUri;
                }
                const std::uint64_t epoch = m_mediaEpoch.load();
                post([this, tags = std::move(tags), uri, epoch]() mutable {
                    if (epoch == m_mediaEpoch.load()
                        && mediaEventsCurrent()) {
                        metadataChanged.emit(tags, uri);
                    }
                });
                break;
            }
            case IdTrackList: {
                const std::uint64_t epoch = m_mediaEpoch.load();
                post([this, epoch] { publishMediaSummary(epoch); });
                break;
            }
            default:
                break;
            }
            break;
        }
        case MPV_EVENT_FILE_LOADED: {
            if (!mediaEventsCurrent())
                break;
            m_mediaSummaryReady = true;
            const std::uint64_t summaryEpoch = m_eventMediaEpoch.load();
            if (m_playOnLoad.load()) {
                int flag = 0;
                mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
            }
            m_loadFailed = false;
            const std::uint64_t generation = m_activeLoadGeneration.load();
            post([this, summaryEpoch, generation] {
                publishMediaSummary(summaryEpoch);
                ready.emit(generation);
            });
            if (m_imageSequenceActive.load()) {
                const std::uint64_t imageGeneration =
                    m_imageSequenceLoadGeneration.load();
                post([this, imageGeneration] {
                    if (m_imageSequenceActive.load()
                        && imageGeneration
                            == m_imageSequenceLoadGeneration.load()) {
                        m_imageSequence->markLoaded();
                    }
                });
            }
            std::string loaded = logicalMediaUri(currentMediaUri(m_mpv));
            {
                const std::scoped_lock lock(m_uriMutex);
                if (!loaded.empty())
                    m_eventMediaUri = loaded;
                else
                    loaded = m_eventMediaUri;
                if (loaded.empty())
                    loaded = m_lastRequestedUri;
            }
            const std::string artwork = albumArt(m_mpv, loaded);
            post([this, artwork, loaded] {
                artworkChanged.emit(artwork, loaded);
            });
            break;
        }
        case MPV_EVENT_START_FILE: {
            finishPacketCorruptBurst();
            m_eventLoadCommandEpoch = m_currentLoadCommandEpoch.load();
            m_eventMediaEpoch = m_commandMediaEpoch.load();
            m_activeLoadGeneration = m_pendingLoadGeneration.load();
            const std::string starting =
                logicalMediaUri(currentMediaUri(m_mpv));
            {
                const std::scoped_lock lock(m_uriMutex);
                m_eventMediaUri = starting;
            }
            logMessage.emit("开始加载媒体 ...");
            break;
        }
        case MPV_EVENT_END_FILE: {
            if (!mediaEventsCurrent())
                break;
            finishPacketCorruptBurst();
            auto *endFile =
                static_cast<mpv_event_end_file *>(event->data);
            if (endFile && endFile->error < 0) {
                m_loadFailed = true;
                const std::string reason =
                    "播放失败：" + mpvError(endFile->error);
                const std::uint64_t generation =
                    m_activeLoadGeneration.load();
                post([this, generation, reason] {
                    if (m_imageSequenceActive.exchange(false))
                        m_imageSequence->reset();
                    logMessage.emit(reason);
                    loadFailed.emit(generation, reason);
                });
                break;
            }
            if (m_imageSequenceActive.load())
                break;
            post([this] { ended.emit(); });
            break;
        }
        case MPV_EVENT_LOG_MESSAGE: {
            auto *message =
                static_cast<mpv_event_log_message *>(event->data);
            if (!message)
                break;
            const std::string text = simplified(message->text);
            if (lowercaseAscii(text).find("packet corrupt")
                != std::string::npos) {
                if (!m_packetCorruptBurst) {
                    m_packetCorruptBurst = true;
                    m_packetCorruptCount = 0;
                    logMessage.emit("mpv: Packet corrupt 连续错误开始");
                }
                ++m_packetCorruptCount;
                m_packetCorruptLast = std::chrono::steady_clock::now();
            } else {
                logMessage.emit("mpv: " + text);
            }
            break;
        }
        case MPV_EVENT_SHUTDOWN:
            finishPacketCorruptBurst();
            post([this] { lost.emit(); });
            break;
        default:
            break;
        }
        if (m_stopRequested.load())
            break;
    }
}

void MpvCore::finishPacketCorruptBurst()
{
    if (!m_packetCorruptBurst)
        return;
    logMessage.emit(
        "mpv: Packet corrupt 连续错误结束，共 "
        + std::to_string(m_packetCorruptCount) + " 条");
    m_packetCorruptBurst = false;
    m_packetCorruptCount = 0;
}

void MpvCore::play()
{
    if (!m_mpv)
        return;
    logMessage.emit("MPV play command");
    if (m_imageSequenceActive.load())
        m_imageSequence->play();
    if (!m_imageSequenceActive.load() && m_eofReached) {
        const char *seek[] = {"seek", "0", "absolute", nullptr};
        mpv_command(m_mpv, seek);
        m_eofReached = false;
    }
    m_playOnLoad = true;
    int flag = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvCore::pause()
{
    if (!m_mpv)
        return;
    logMessage.emit("MPV pause command");
    if (m_imageSequenceActive.load())
        m_imageSequence->pause();
    m_playOnLoad = false;
    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvCore::stop()
{
    if (!m_mpv)
        return;
    if (m_imageSequenceActive.load())
        m_imageSequence->stop();
    m_playOnLoad = false;
    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
    const char *seek[] = {"seek", "0", "absolute", nullptr};
    mpv_command(m_mpv, seek);
}

void MpvCore::unload()
{
    if (!m_mpv)
        return;
    m_playOnLoad = false;
    m_wrappedTsProxy.reset();
    m_imageSequence->reset();
    m_imageSequenceActive = false;
    m_mediaSummaryReady = false;
    ++m_mediaEpoch;
    m_pendingLoad.reset();
    m_lastLoadRequest.reset();
    {
        const std::scoped_lock lock(m_uriMutex);
        m_lastRequestedUri.clear();
        m_activeMpvUri.clear();
        m_eventMediaUri.clear();
    }
    {
        const std::scoped_lock lock(m_mediaInfoMutex);
        m_mediaInfoJson = "{}";
    }
    mediaInfoChanged.emit();
    const char *command[] = {"stop", nullptr};
    mpv_command(m_mpv, command);
}

void MpvCore::seekTo(double seconds)
{
    if (!m_mpv)
        return;
    if (m_imageSequenceActive.load()) {
        m_imageSequence->seek(seconds);
        return;
    }
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.3f", seconds);
    const char *command[] = {"seek", buffer, "absolute", nullptr};
    mpv_command(m_mpv, command);
}

void MpvCore::setVolumePercent(int percent)
{
    if (!m_mpv)
        return;
    double value = std::clamp(percent, 0, 100);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &value);
}

void MpvCore::setMuted(bool muted)
{
    if (!m_mpv)
        return;
    int value = muted ? 1 : 0;
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &value);
}

std::int64_t MpvCore::mediaSizeBytes() const
{
    if (!m_mpv)
        return 0;
    std::int64_t value = 0;
    return mpv_get_property(
               m_mpv, "file-size", MPV_FORMAT_INT64, &value)
            >= 0
        && value > 0
        ? value : 0;
}

std::vector<PictureControlInfo> MpvCore::pictureControls() const
{
    std::vector<PictureControlInfo> controls;
    controls.reserve(std::size(kPictureControls));
    for (const auto &spec : kPictureControls) {
        controls.push_back(
            {spec.name, spec.minimum, spec.maximum, spec.neutral});
    }
    return controls;
}

bool MpvCore::setPictureControl(
    const std::string &name, int value)
{
    if (!m_mpv)
        return false;
    const PictureControlSpec *spec = findPictureSpec(name);
    if (!spec)
        return false;
    value = std::clamp(value, spec->minimum, spec->maximum);
    if (!spec->mpvProperty)
        return applySharpen(value);

    double raw = value * spec->scale;
    int result = mpv_set_property(
        m_mpv, spec->mpvProperty, MPV_FORMAT_DOUBLE, &raw);
    if (result < 0) {
        std::int64_t integer =
            static_cast<std::int64_t>(std::llround(raw));
        result = mpv_set_property(
            m_mpv, spec->mpvProperty, MPV_FORMAT_INT64, &integer);
    }
    if (result < 0)
        return false;
    pictureControlChanged.emit(name, value);
    return true;
}

int MpvCore::pictureControlValue(const std::string &name) const
{
    if (!m_mpv)
        return 0;
    const PictureControlSpec *spec = findPictureSpec(name);
    if (!spec)
        return 0;
    if (!spec->mpvProperty)
        return m_sharpen;
    double raw = 0;
    if (mpv_get_property(
            m_mpv, spec->mpvProperty, MPV_FORMAT_DOUBLE, &raw) < 0) {
        std::int64_t integer = 0;
        if (mpv_get_property(
                m_mpv, spec->mpvProperty,
                MPV_FORMAT_INT64, &integer)
            < 0) {
            return spec->neutral;
        }
        raw = static_cast<double>(integer);
    }
    return static_cast<int>(std::lround(raw / spec->scale));
}

bool MpvCore::applySharpen(int value)
{
    if (value == 0) {
        if (mpv_set_property_string(m_mpv, "vf", "") < 0)
            return false;
    } else {
        const double amount =
            value > 0 ? 0.5 + value * 0.025 : value * 0.02;
        char buffer[128]{};
        std::snprintf(
            buffer, sizeof(buffer),
            "unsharp=luma_msize_x=7:luma_msize_y=7:luma_amount=%.2f",
            amount);
        if (mpv_set_property_string(m_mpv, "vf", buffer) < 0)
            return false;
    }
    m_sharpen = value;
    pictureControlChanged.emit("sharpen", value);
    return true;
}

} // namespace mcast::avalonia::qtfree
