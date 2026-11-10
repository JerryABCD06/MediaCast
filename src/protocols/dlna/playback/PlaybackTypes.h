// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mcast::dlna {

enum class PlaybackState {
    NoMedia,
    Stopped,
    Preparing,
    Playing,
    Paused,
};

enum class LoadStatus {
    Ok,
    Failed,
};

enum class PlayMode {
    Normal,
    RepeatOne,
    RepeatAll,
    Direct,
};

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

struct MediaRequest {
    std::string uri;
    std::string metadata;
    std::string title;
    std::string artist;
    std::string album;
    std::string description;
    std::string protocol;
    MediaKind kind = MediaKind::Unknown;
    MediaSource source = MediaSource::Cast;
    std::optional<std::uint64_t> mediaSizeBytes;
};

struct PictureControl {
    std::string name;
    int minimum = 0;
    int maximum = 0;
    int neutral = 0;
    int value = 0;
};

struct PlaybackSnapshot {
    PlaybackState state = PlaybackState::NoMedia;
    LoadStatus loadStatus = LoadStatus::Ok;
    MediaRequest current;
    MediaRequest next;
    PlayMode playMode = PlayMode::Normal;
    bool hasNext = false;
    bool hasPrevious = false;
    bool peerConnected = false;
    bool playerAvailable = true;
    double positionSeconds = 0.0;
    double durationSeconds = 0.0;
    int volumePercent = 100;
    bool muted = false;
    std::vector<PictureControl> pictureControls;
    std::uint64_t generation = 1;
    std::uint64_t stateVersion = 1;
};

enum class PlaybackCommandType {
    Open,
    Play,
    Pause,
    Stop,
    Seek,
    Next,
    Previous,
    EndSession,
    SetNext,
    SetPlayMode,
    SetVolume,
    SetMuted,
    SetPictureControl,
    ResetPictureControls,
    SetPeerConnected,
};

struct PlaybackCommand {
    PlaybackCommandType type = PlaybackCommandType::Play;
    std::uint64_t expectedGeneration = 0;
    MediaRequest media;
    PlayMode playMode = PlayMode::Normal;
    std::string text;
    int intValue = 0;
    double doubleValue = 0.0;
    bool boolValue = false;
};

enum class PlaybackSubmissionStatus {
    Queued,
    InvalidCommand,
    PlayerUnavailable,
    StaleGeneration,
};

struct PlaybackCommandSubmission {
    bool queued = false;
    PlaybackSubmissionStatus status = PlaybackSubmissionStatus::InvalidCommand;
    std::uint64_t commandId = 0;
    std::uint64_t generation = 0;
    std::string error;
    std::uint64_t stateVersion = 0;
};

enum class PlaybackOutcomeStatus {
    Applied,
    Rejected,
    TimedOut,
    StaleGeneration,
};

enum class PlaybackEventKind {
    CommandCompleted,
    SnapshotChanged,
    PlayerLost,
    Reconnected,
};

struct PlaybackEvent {
    PlaybackEventKind kind = PlaybackEventKind::SnapshotChanged;
    std::uint64_t commandId = 0;
    PlaybackOutcomeStatus outcome = PlaybackOutcomeStatus::Applied;
    std::string error;
    PlaybackSnapshot snapshot;
};

} // namespace mcast::dlna
