// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackGateway.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>

namespace mcast::dlna {

using playback::MediaRequest;
using playback::PlaybackCommand;
using playback::PlaybackCommandType;
using playback::PlaybackCommandSubmission;
using playback::PlaybackEvent;
using playback::PlaybackEventKind;
using playback::PlaybackGateway;
using playback::PlaybackSnapshot;
using playback::PlaybackState;
using playback::PlaybackSubmissionStatus;

using SessionTime = std::chrono::milliseconds;

struct DlnaSessionSnapshot {
    bool accepting = false;
    bool running = false;
    bool peerConnected = false;
    bool disconnectedByUs = false;
    std::size_t subscriptionCount = 0;
    bool peerGraceActive = false;
    SessionTime peerGraceDeadline{0};
    PlaybackSnapshot playback;
};

enum class SubscriptionChangeReason {
    Added,
    Renewed,
    Count,
    ExplicitRemoval,
    Expired,
    Unreachable,
};

struct DlnaSessionCallbacks {
    std::function<void()> requestUiActivation;
    std::function<bool()> startNetwork;
    std::function<void()> stopNetwork;
    std::function<void()> announceGoingAway;
    std::function<void(bool)> networkStateChanged;
    std::function<void(bool)> peerConnectedChanged;
    std::function<void()> stateChanged;
    std::function<void(const std::string &)> requestFailed;
    std::function<void(const std::string &)> log;
};

class DlnaSession
{
public:
    static constexpr SessionTime kPeerGrace{60000};
    static constexpr SessionTime kOpenTimeout{10000};

    DlnaSession(PlaybackGateway &playback, DlnaSessionCallbacks callbacks);
    ~DlnaSession();

    DlnaSession(const DlnaSession &) = delete;
    DlnaSession &operator=(const DlnaSession &) = delete;

    bool start();
    void pauseAccepting(SessionTime now);
    bool resumeAccepting();

    void setSubscriptionCount(
        std::size_t count, SubscriptionChangeReason reason, SessionTime now);
    void peerActivity(SessionTime now);
    void tick(SessionTime now);

    PlaybackCommandSubmission openMedia(
        const MediaRequest &request, SessionTime now);
    PlaybackCommandSubmission submit(PlaybackCommand command);
    PlaybackCommandSubmission endSession(SessionTime now);

    void handlePlaybackEvent(const PlaybackEvent &event);
    DlnaSessionSnapshot snapshot() const;

private:
    enum class EndOperationMode {
        Pause,
        Disconnect,
    };

    struct EndOperation {
        EndOperationMode mode = EndOperationMode::Disconnect;
        std::uint64_t commandId = 0;
        SessionTime deadline{0};
        bool commandCompleted = false;
        bool finalSnapshotSeen = false;
    };

    void setPeerConnected(bool connected, bool queuePlayback = true);
    void queuePeerConnected(bool connected);
    void startPeerGrace(SessionTime now);
    void cancelPeerGrace();
    PlaybackCommandSubmission requestEnd(
        EndOperationMode mode, SessionTime now);
    void maybeFinishEnd();
    void finishPause();
    void finishDisconnect();
    void cancelPendingOpen(const std::string &reason);
    void tryPendingOpen();

    PlaybackGateway &m_playback;
    DlnaSessionCallbacks m_callbacks;
    bool m_accepting = false;
    bool m_running = false;
    bool m_peerConnected = false;
    bool m_queuePeerDisconnect = false;
    bool m_disconnectedByUs = false;
    std::size_t m_subscriptionCount = 0;
    std::optional<SessionTime> m_peerGraceDeadline;
    std::uint64_t m_playbackListener = 0;
    std::optional<EndOperation> m_endOperation;
    std::optional<MediaRequest> m_pendingOpen;
    std::optional<SessionTime> m_openDeadline;
};

} // namespace mcast::dlna
