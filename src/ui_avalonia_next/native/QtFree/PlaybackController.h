// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/EventLoop.h"
#include "QtFree/MediaPlayer.h"
#include "QtFree/MetadataPipeline.h"
#include "QtFree/Signal.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mcast::avalonia::qtfree {

struct CastSession
{
    bool connected = false;
    std::string protocol;
    std::string controlPointName;
    std::string controlPointAddress;
    bool remoteControlRegistered = false;
};

class PlaybackController
{
public:
    enum class State {
        NoMedia,
        Stopped,
        Preparing,
        Playing,
        Paused,
    };

    enum class CastState {
        NoViewer,
        ViewerIdle,
        ViewerVideo,
        ViewerAudio,
        ViewerImage,
    };

    enum class PlayMode {
        Normal,
        RepeatOne,
        RepeatAll,
        Direct,
    };

    enum class LoadStatus {
        Ok,
        Failed,
    };

    PlaybackController(MediaPlayer *player, EventLoop &loop);
    ~PlaybackController();

    PlaybackController(const PlaybackController &) = delete;
    PlaybackController &operator=(const PlaybackController &) = delete;

    void openUri(const MediaRequest &request, MediaSource source);
    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void endSession();
    void seekTo(double seconds);
    void setVolumePercent(int percent);
    void setMuted(bool muted);

    void setNextUri(const MediaRequest &request);
    void clearPlaylist();
    void next();
    void previous();
    bool hasNext() const { return !m_next.uri.empty(); }
    bool hasPrevious() const { return !m_previous.uri.empty(); }
    const MediaRequest &currentRequest() const { return m_current; }
    const MediaRequest &nextRequest() const { return m_next; }

    void setPlayMode(PlayMode mode);
    PlayMode playMode() const { return m_playMode; }

    std::vector<PictureControlInfo> pictureControls() const;
    bool setPictureControl(const std::string &name, int value);
    int pictureControlValue(const std::string &name) const;
    void resetPictureControls();

    State state() const { return m_state; }
    bool isIdle() const
    {
        return m_state == State::NoMedia || m_state == State::Stopped;
    }
    bool hasMedia() const { return m_state != State::NoMedia; }
    bool isPaused() const { return m_state == State::Paused; }
    bool peerConnected() const { return m_peerConnected; }
    void setPeerConnected(bool connected);
    void setCastSession(const CastSession &session);
    const CastSession &castSession() const { return m_castSession; }
    CastState castState() const;
    bool showsPicture() const;
    bool mediaIsVideo() const;
    LoadStatus loadStatus() const { return m_loadStatus; }
    const NowPlaying &nowPlaying() const { return m_nowPlaying; }

    const std::string &currentUri() const { return m_current.uri; }
    const std::string &currentMetadata() const
    {
        return m_current.metadata;
    }
    double positionSeconds() const;
    double durationSeconds() const;
    int volumePercent() const;
    bool isMuted() const;
    std::int64_t mediaSizeBytes() const;

    Signal<const std::string &> logMessage;
    Signal<State> stateChanged;
    Signal<> idleChanged;
    Signal<> hasMediaChanged;
    Signal<> peerConnectedChanged;
    Signal<> castSessionChanged;
    Signal<> castStateChanged;
    Signal<> showsPictureChanged;
    Signal<const NowPlaying &> nowPlayingChanged;
    Signal<const std::string &, const std::string &> mediaChanged;
    Signal<bool, bool, const std::string &, PlayMode> queueChanged;
    Signal<const std::string &, int> pictureControlChanged;
    Signal<double> positionChanged;
    Signal<double> durationChanged;
    Signal<int> volumeChanged;
    Signal<bool> muteChanged;
    Signal<bool> pausedChanged;
    Signal<const std::string &> playerStatusChanged;
    Signal<> playerLost;

private:
    void connectPlayer();
    void startPlaying(
        const MediaRequest &request, MediaSource source,
        bool nativeFallback = false);
    MediaSource sourceForCurrent() const;
    void emitQueueChanged();
    void setState(State state);
    void scheduleLoadWatchdog();
    void cancelLoadWatchdog();
    NowPlaying buildNowPlaying(
        const MediaRequest &request, MediaSource source) const;
    void rebuildNowPlaying();
    std::string tagValue(
        const std::vector<std::string> &keys) const;

    MediaPlayer *m_player = nullptr;
    EventLoop &m_loop;
    std::vector<Connection> m_playerConnections;
    std::uint64_t m_loadWatchdog = 0;
    State m_state = State::NoMedia;
    bool m_peerConnected = false;
    CastSession m_castSession;
    LoadStatus m_loadStatus = LoadStatus::Ok;
    MediaKind m_playerKind = MediaKind::Unknown;
    bool m_nativeFallbackAttempted = false;
    std::uint64_t m_loadGeneration = 0;
    MediaRequest m_current;
    MediaRequest m_next;
    MediaRequest m_previous;
    PlayMode m_playMode = PlayMode::Normal;
    NowPlaying m_nowPlaying;
    std::map<std::string, std::string, std::less<>> m_fileTags;
    std::string m_fileArtworkSource;
    MetadataPipeline m_metadataPipeline;
};

} // namespace mcast::avalonia::qtfree

