// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/PlaybackController.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

constexpr int kLoadTimeoutSeconds = 10;

const char *stateName(PlaybackController::State state)
{
    switch (state) {
    case PlaybackController::State::NoMedia: return "NO_MEDIA";
    case PlaybackController::State::Stopped: return "STOPPED";
    case PlaybackController::State::Preparing: return "PREPARING";
    case PlaybackController::State::Playing: return "PLAYING";
    case PlaybackController::State::Paused: return "PAUSED";
    }
    return "?";
}

const char *playModeName(PlaybackController::PlayMode mode)
{
    switch (mode) {
    case PlaybackController::PlayMode::Normal: return "NORMAL";
    case PlaybackController::PlayMode::RepeatOne: return "REPEAT_ONE";
    case PlaybackController::PlayMode::RepeatAll: return "REPEAT_ALL";
    case PlaybackController::PlayMode::Direct: return "DIRECT";
    }
    return "?";
}

} // namespace

PlaybackController::PlaybackController(
    MediaPlayer *player, EventLoop &loop)
    : m_player(player)
    , m_loop(loop)
{
    std::string error;
    if (!m_metadataPipeline.loadNearExecutable(&error)) {
        logMessage.emit(
            "元数据管线使用内置规则：" + error);
    }
    connectPlayer();
}

PlaybackController::~PlaybackController()
{
    cancelLoadWatchdog();
}

void PlaybackController::connectPlayer()
{
    if (!m_player)
        return;

    m_playerConnections.reserve(16);
    m_playerConnections.push_back(m_player->ready.connect(
        [this](std::uint64_t generation) {
            if (generation != m_loadGeneration)
                return;
            cancelLoadWatchdog();
            m_loadStatus = LoadStatus::Ok;
            if (m_state == State::Preparing)
                setState(State::Playing);
        }));
    m_playerConnections.push_back(m_player->loadStarted.connect(
        [this](std::uint64_t generation) {
            if (generation == m_loadGeneration)
                scheduleLoadWatchdog();
        }));
    m_playerConnections.push_back(m_player->loadFailed.connect(
        [this](
            std::uint64_t generation,
            const std::string &reason) {
            if (generation != m_loadGeneration)
                return;
            cancelLoadWatchdog();
            logMessage.emit("媒体加载失败：" + reason);
            if (m_current.route != MediaLoadRoute::Native
                && !m_nativeFallbackAttempted) {
                m_nativeFallbackAttempted = true;
                MediaRequest fallback = m_current;
                fallback.route = MediaLoadRoute::Native;
                fallback.imageSequenceFrames.clear();
                logMessage.emit(
                    "兼容播放路线失败，改按原始地址交给 MPV 重试一次");
                startPlaying(fallback, sourceForCurrent(), true);
                return;
            }
            m_loadStatus = LoadStatus::Failed;
            if (m_state == State::Preparing
                || m_state == State::Playing
                || m_state == State::Paused) {
                setState(State::Stopped);
            }
        }));
    m_playerConnections.push_back(m_player->ended.connect([this] {
        if (m_state != State::Playing)
            return;
        if (m_playMode == PlayMode::RepeatOne
            && !m_current.uri.empty()) {
            logMessage.emit("单曲循环，重放这一条");
            startPlaying(m_current, sourceForCurrent());
            return;
        }
        if (hasNext()) {
            logMessage.emit(
                "这一条放完了，自动接上队列里的下一条");
            next();
            return;
        }
        if (m_playMode == PlayMode::RepeatAll
            && !m_current.uri.empty()) {
            logMessage.emit("全部循环：重放当前条目");
            startPlaying(m_current, sourceForCurrent());
            return;
        }
        setState(State::Stopped);
    }));
    m_playerConnections.push_back(
        m_player->pictureControlChanged.connect(
            [this](const std::string &name, int value) {
                pictureControlChanged.emit(name, value);
            }));
    m_playerConnections.push_back(
        m_player->positionChanged.connect(
            [this](double value) {
                positionChanged.emit(value);
            }));
    m_playerConnections.push_back(
        m_player->durationChanged.connect(
            [this](double value) {
                durationChanged.emit(value);
            }));
    m_playerConnections.push_back(
        m_player->volumeChanged.connect(
            [this](int value) {
                volumeChanged.emit(value);
            }));
    m_playerConnections.push_back(
        m_player->muteChanged.connect(
            [this](bool value) {
                muteChanged.emit(value);
            }));
    m_playerConnections.push_back(
        m_player->pausedChanged.connect(
            [this](bool value) {
                pausedChanged.emit(value);
                if (m_state == State::Playing && value) {
                    setState(State::Paused);
                } else if (m_state == State::Paused && !value) {
                    setState(State::Playing);
                }
            }));
    m_playerConnections.push_back(
        m_player->statusChanged.connect(
            [this](const std::string &text) {
                playerStatusChanged.emit(text);
            }));
    m_playerConnections.push_back(
        m_player->logMessage.connect(
            [this](const std::string &text) {
                logMessage.emit(text);
            }));
    m_playerConnections.push_back(
        m_player->metadataChanged.connect(
            [this](
                const std::map<
                    std::string, std::string, std::less<>> &tags,
                const std::string &mediaUri) {
                if (mediaUri.empty()
                    || !sameMediaUri(mediaUri, m_current.uri)
                    || m_fileTags == tags) {
                    return;
                }
                m_fileTags = tags;
                rebuildNowPlaying();
            }));
    m_playerConnections.push_back(
        m_player->artworkChanged.connect(
            [this](
                const std::string &source,
                const std::string &mediaUri) {
                if (mediaUri.empty()
                    || !sameMediaUri(mediaUri, m_current.uri)
                    || m_fileArtworkSource == source) {
                    return;
                }
                m_fileArtworkSource = source;
                logMessage.emit(
                    source.empty()
                        ? "播放器没有提供封面"
                        : "播放器封面：" + source);
                rebuildNowPlaying();
            }));
    m_playerConnections.push_back(
        m_player->mediaKindChanged.connect(
            [this](
                MediaKind kind,
                const std::string &mediaUri) {
                if (kind == MediaKind::Unknown
                    || mediaUri.empty()
                    || !sameMediaUri(mediaUri, m_current.uri)
                    || m_playerKind == kind) {
                    return;
                }
                const CastState castBefore = castState();
                const bool pictureBefore = showsPicture();
                m_playerKind = kind;
                rebuildNowPlaying();
                if (castState() != castBefore)
                    castStateChanged.emit();
                if (showsPicture() != pictureBefore)
                    showsPictureChanged.emit();
            }));
    m_playerConnections.push_back(m_player->lost.connect([this] {
        playerLost.emit();
        endSession();
    }));
}

void PlaybackController::scheduleLoadWatchdog()
{
    cancelLoadWatchdog();
    m_loadWatchdog = m_loop.scheduleAfter(
        std::chrono::seconds(kLoadTimeoutSeconds), [this] {
            m_loadWatchdog = 0;
            if (m_state != State::Preparing)
                return;
            logMessage.emit("等了 10 秒也没放起来，放弃这一条");
            m_loadStatus = LoadStatus::Failed;
            if (m_player)
                m_player->stop();
            setState(State::Stopped);
        });
}

void PlaybackController::cancelLoadWatchdog()
{
    if (m_loadWatchdog != 0) {
        m_loop.cancelTimer(m_loadWatchdog);
        m_loadWatchdog = 0;
    }
}

void PlaybackController::setState(State state)
{
    if (m_state == state)
        return;
    const bool wasIdle = isIdle();
    const bool hadMedia = hasMedia();
    const CastState castBefore = castState();
    const bool pictureBefore = showsPicture();
    m_state = state;
    logMessage.emit("传输状态 -> " + std::string(stateName(state)));
    stateChanged.emit(state);
    if (isIdle() != wasIdle)
        idleChanged.emit();
    if (hasMedia() != hadMedia)
        hasMediaChanged.emit();
    if (castState() != castBefore)
        castStateChanged.emit();
    if (showsPicture() != pictureBefore)
        showsPictureChanged.emit();
}

void PlaybackController::setPeerConnected(bool connected)
{
    if (m_peerConnected == connected) {
        if (!connected && hasMedia()
            && sourceForCurrent() == MediaSource::Cast) {
            endSession();
        }
        return;
    }
    const CastState castBefore = castState();
    m_peerConnected = connected;
    logMessage.emit(
        connected ? "UI peerConnected -> true"
                  : "UI peerConnected -> false");
    clearPlaylist();
    if (!connected && hasMedia()
        && sourceForCurrent() == MediaSource::Cast) {
        endSession();
    }
    peerConnectedChanged.emit();
    if (castState() != castBefore)
        castStateChanged.emit();
}

void PlaybackController::setCastSession(const CastSession &session)
{
    if (m_castSession.connected == session.connected
        && m_castSession.protocol == session.protocol
        && m_castSession.controlPointName
            == session.controlPointName
        && m_castSession.controlPointAddress
            == session.controlPointAddress
        && m_castSession.remoteControlRegistered
            == session.remoteControlRegistered) {
        return;
    }
    m_castSession = session;
    setPeerConnected(session.connected);
    castSessionChanged.emit();
}

PlaybackController::CastState PlaybackController::castState() const
{
    if (!m_peerConnected)
        return CastState::NoViewer;
    if (isIdle())
        return CastState::ViewerIdle;
    switch (m_nowPlaying.kind) {
    case MediaKind::Video: return CastState::ViewerVideo;
    case MediaKind::Audio: return CastState::ViewerAudio;
    case MediaKind::Image: return CastState::ViewerImage;
    case MediaKind::Unknown: break;
    }
    return CastState::ViewerVideo;
}

bool PlaybackController::showsPicture() const
{
    return !isIdle()
        && (m_nowPlaying.kind == MediaKind::Video
            || m_nowPlaying.kind == MediaKind::Image);
}

bool PlaybackController::mediaIsVideo() const
{
    return m_nowPlaying.kind == MediaKind::Video;
}

void PlaybackController::play()
{
    if (!hasMedia())
        return;
    if (m_player)
        m_player->play();
    setState(State::Playing);
}

void PlaybackController::pause()
{
    if (!hasMedia())
        return;
    if (m_player)
        m_player->pause();
    setState(State::Paused);
}

void PlaybackController::togglePlayPause()
{
    if (!hasMedia())
        return;
    if (isPaused() || m_state == State::Stopped)
        play();
    else
        pause();
}

void PlaybackController::stop()
{
    if (m_player)
        m_player->stop();
    if (hasMedia())
        setState(State::Stopped);
}

void PlaybackController::seekTo(double seconds)
{
    if (m_player)
        m_player->seekTo(seconds);
}

void PlaybackController::setVolumePercent(int percent)
{
    if (m_player)
        m_player->setVolumePercent(percent);
}

void PlaybackController::setMuted(bool muted)
{
    if (m_player)
        m_player->setMuted(muted);
}

void PlaybackController::openUri(
    const MediaRequest &request, MediaSource source)
{
    startPlaying(request, source);
}

MediaSource PlaybackController::sourceForCurrent() const
{
    return m_nowPlaying.source;
}

void PlaybackController::emitQueueChanged()
{
    queueChanged.emit(
        hasNext(), hasPrevious(), m_next.uri, m_playMode);
}

void PlaybackController::clearPlaylist()
{
    if (m_next.uri.empty() && m_previous.uri.empty())
        return;
    m_next = {};
    m_previous = {};
    logMessage.emit("播放列表：清空");
    emitQueueChanged();
}

void PlaybackController::setNextUri(const MediaRequest &request)
{
    m_next = request;
    logMessage.emit(
        request.uri.empty()
            ? "队列：清空"
            : "队列：下一条是 " + request.uri);
    emitQueueChanged();
}

void PlaybackController::next()
{
    if (m_next.uri.empty()) {
        logMessage.emit("收到「下一首」，但队列里没有下一条 —— 不动");
        return;
    }
    m_previous = m_current;
    const MediaRequest request = m_next;
    m_next = {};
    emitQueueChanged();
    startPlaying(request, sourceForCurrent());
}

void PlaybackController::previous()
{
    if (m_previous.uri.empty()) {
        logMessage.emit("收到「上一首」，但没有上一条 —— 不动");
        return;
    }
    m_next = m_current;
    const MediaRequest request = m_previous;
    m_previous = {};
    emitQueueChanged();
    startPlaying(request, sourceForCurrent());
}

void PlaybackController::setPlayMode(PlayMode mode)
{
    if (m_playMode == mode)
        return;
    m_playMode = mode;
    logMessage.emit("播放模式 -> " + std::string(playModeName(mode)));
    emitQueueChanged();
}

void PlaybackController::startPlaying(
    const MediaRequest &request, MediaSource source,
    bool nativeFallback)
{
    if (!m_player || request.uri.empty())
        return;
    if (!nativeFallback)
        m_nativeFallbackAttempted = false;

    m_current = request;
    m_fileTags.clear();
    m_fileArtworkSource.clear();
    m_playerKind = MediaKind::Unknown;
    mediaChanged.emit(m_current.uri, m_current.metadata);

    const CastState castBefore = castState();
    const bool pictureBefore = showsPicture();
    m_nowPlaying = buildNowPlaying(request, source);
    logMessage.emit("开始播放 " + request.uri);
    if (!m_nowPlaying.title.empty())
        logMessage.emit("   标题：" + m_nowPlaying.title);
    nowPlayingChanged.emit(m_nowPlaying);
    if (castState() != castBefore)
        castStateChanged.emit();
    if (showsPicture() != pictureBefore)
        showsPictureChanged.emit();

    m_loadStatus = LoadStatus::Ok;
    setState(State::Preparing);
    MediaLoadRequest loadRequest;
    loadRequest.logicalUri = request.uri;
    loadRequest.loadGeneration = ++m_loadGeneration;
    loadRequest.route = request.route;
    loadRequest.imageSequenceFrames = request.imageSequenceFrames;
    m_player->load(loadRequest);
}

std::string PlaybackController::tagValue(
    const std::vector<std::string> &keys) const
{
    for (const std::string &key : keys) {
        for (const auto &[name, value] : m_fileTags) {
            if (lowercaseAscii(name) == lowercaseAscii(key)) {
                const std::string normalized = simplified(value);
                if (!normalized.empty())
                    return normalized;
            }
        }
    }
    return {};
}

NowPlaying PlaybackController::buildNowPlaying(
    const MediaRequest &request, MediaSource source) const
{
    NowPlaying info;
    info.source = source;
    info.kind = m_playerKind != MediaKind::Unknown
        ? m_playerKind : request.kind;
    if (info.kind == MediaKind::Unknown)
        info.kind = mediaKindFromUri(request.uri);

    MediaRequest effective = request;
    effective.kind = info.kind;
    const MetadataPipelineResult resolved =
        m_metadataPipeline.resolve(
            effective, m_fileTags, m_fileArtworkSource);
    info.title = resolved.title;
    info.subtitle = resolved.subtitle;
    info.artist = resolved.artist;
    info.album = resolved.album;
    info.artworkSource = resolved.artworkSource;
    info.lyrics = tagValue(
        {"lyrics", "unsyncedlyrics", "unsynchronisedlyrics",
         "syncedlyrics", "uslt"});
    return info;
}

void PlaybackController::rebuildNowPlaying()
{
    if (m_state == State::NoMedia || m_current.uri.empty())
        return;
    m_nowPlaying = buildNowPlaying(m_current, m_nowPlaying.source);
    nowPlayingChanged.emit(m_nowPlaying);
}

void PlaybackController::endSession()
{
    if (hasMedia() && sourceForCurrent() == MediaSource::Local) {
        logMessage.emit("本地播放不受投送会话结束影响");
        return;
    }
    if (m_player)
        m_player->unload();
    cancelLoadWatchdog();
    m_loadStatus = LoadStatus::Ok;
    m_current = {};
    mediaChanged.emit({}, {});
    m_fileTags.clear();
    m_fileArtworkSource.clear();
    m_playerKind = MediaKind::Unknown;
    m_next = {};
    m_previous = {};
    m_playMode = PlayMode::Normal;
    emitQueueChanged();
    setState(State::NoMedia);

    const CastState castBefore = castState();
    const bool pictureBefore = showsPicture();
    m_nowPlaying = {};
    nowPlayingChanged.emit(m_nowPlaying);
    if (castState() != castBefore)
        castStateChanged.emit();
    if (showsPicture() != pictureBefore)
        showsPictureChanged.emit();
    logMessage.emit("已在电脑端结束投送");
}

std::vector<PictureControlInfo>
PlaybackController::pictureControls() const
{
    return m_player ? m_player->pictureControls()
                    : std::vector<PictureControlInfo>{};
}

bool PlaybackController::setPictureControl(
    const std::string &name, int value)
{
    return m_player ? m_player->setPictureControl(name, value) : false;
}

int PlaybackController::pictureControlValue(
    const std::string &name) const
{
    return m_player ? m_player->pictureControlValue(name) : 0;
}

void PlaybackController::resetPictureControls()
{
    if (m_player)
        m_player->resetPictureControls();
}

double PlaybackController::positionSeconds() const
{
    return m_player ? m_player->positionSeconds() : 0;
}

double PlaybackController::durationSeconds() const
{
    return m_player ? m_player->durationSeconds() : 0;
}

int PlaybackController::volumePercent() const
{
    return m_player ? m_player->volumePercent() : 0;
}

bool PlaybackController::isMuted() const
{
    return m_player ? m_player->isMuted() : false;
}

std::int64_t PlaybackController::mediaSizeBytes() const
{
    return m_player ? m_player->mediaSizeBytes() : 0;
}

} // namespace mcast::avalonia::qtfree

