// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackGateway.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mcast::dlna {

using playback::MediaRequest;
using playback::PlaybackCommand;
using playback::PlaybackCommandType;
using playback::PlaybackCommandSubmission;
using playback::PlaybackEvent;
using playback::PlaybackEventKind;
using playback::PlaybackGateway;
using playback::PlaybackOutcomeStatus;
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
    bool ending = false;
    playback::CastSessionInfo castSession;
    PlaybackSnapshot playback;
};

struct DlnaCommandResult {
    PlaybackCommandSubmission submission;
    std::optional<PlaybackOutcomeStatus> outcome;
    std::string completionError;
    bool timedOut = false;
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
    // Control points commonly use AVTransport Stop as their de-facto leave
    // signal. Give a normal Stop -> SetAVTransportURI switch enough time to
    // arrive before treating the control point as gone.
    static constexpr SessionTime kStopIdleGrace{8000};
    static constexpr SessionTime kStopBoundarySettle{250};
    // Includes Protocol-side compatibility planning before the Ui command can
    // even be submitted. Ui still has its own 10-second load watchdog after
    // MPV reports that loading actually started.
    static constexpr SessionTime kOpenTimeout{20000};

    DlnaSession(PlaybackGateway &playback, DlnaSessionCallbacks callbacks);
    ~DlnaSession();

    DlnaSession(const DlnaSession &) = delete;
    DlnaSession &operator=(const DlnaSession &) = delete;

    bool start(bool accepting = true);
    void pauseAccepting(SessionTime now);
    bool resumeAccepting();

    void setSubscriptionCount(
        std::size_t count, SubscriptionChangeReason reason, SessionTime now);
    void peerActivity(
        SessionTime now, std::string_view clientAddress = {});
    void transportStopped(
        SessionTime now, std::string_view clientAddress = {});
    void observeHttpClient(
        const std::string &name, const std::string &address);
    void newMediaRequested(
        SessionTime now, std::string clientName = {},
        std::string clientAddress = {});
    void tick(SessionTime now);

    PlaybackCommandSubmission openMedia(
        const MediaRequest &request, SessionTime now,
        bool clearPlaylist = false);
    PlaybackCommandSubmission submit(
        PlaybackCommand command,
        std::optional<std::uint64_t> expectedOwnerEpoch =
            std::nullopt);
    DlnaCommandResult submitAndWait(
        PlaybackCommand command,
        SessionTime timeout = SessionTime{5000},
        std::optional<std::uint64_t> expectedOwnerEpoch =
            std::nullopt);
    PlaybackCommandSubmission endSession(SessionTime now);
    void abandonEndingSession();
    void resetForRestart();

    void handlePlaybackEvent(const PlaybackEvent &event);
    bool transportTransitionPending() const;
    std::uint64_t protocolEpoch() const;
    DlnaSessionSnapshot snapshot() const;

private:
    using Lock = std::unique_lock<std::recursive_mutex>;

    enum class EndOperationMode {
        Pause,
        Disconnect,
    };

    struct EndOperation {
        EndOperationMode mode = EndOperationMode::Disconnect;
        std::uint64_t token = 0;
        std::uint64_t commandId = 0;
        SessionTime deadline{0};
        bool commandCompleted = false;
        bool finalSnapshotSeen = false;
    };

    struct MediaCandidate {
        MediaRequest media;
        std::uint64_t epoch = 0;
    };

    void setPeerConnected(
        bool connected, Lock &lock, bool queuePlayback = true);
    void queuePeerConnected(bool connected, Lock &lock);
    void startPeerGrace(SessionTime now, Lock &lock);
    void cancelPeerGrace();
    void cancelStopIdleGrace();
    PlaybackCommandSubmission submitLocked(
        PlaybackCommand command,
        Lock &lock,
        bool allowDefer = true,
        std::optional<std::uint64_t> requiredOpenToken = std::nullopt,
        std::uint64_t *awaitedCommandId = nullptr,
        std::optional<std::uint64_t> expectedOwnerEpoch =
            std::nullopt);
    void flushDeferredCommands(Lock &lock);
    void clearDeferredCommands();
    PlaybackCommandSubmission requestEnd(
        EndOperationMode mode, SessionTime now, Lock &lock);
    void maybeFinishEnd(Lock &lock);
    void retryPendingEnd(Lock &lock);
    void finishPause(Lock &lock);
    void finishDisconnect(Lock &lock);
    void cancelPendingOpen(const std::string &reason, Lock &lock);
    void tryPendingOpen(Lock &lock);
    void syncProtocolPlayback(const PlaybackSnapshot &playback);
    void consumeEarlyCommand(std::uint64_t commandId, Lock &lock);
    void publishCastSession(Lock &lock);
    bool isCurrentOwner(std::string_view clientAddress) const noexcept;

    PlaybackGateway &m_playback;
    mutable std::recursive_mutex m_mutex;
    std::recursive_mutex m_gatewaySubmitMutex;
    DlnaSessionCallbacks m_callbacks;
    bool m_accepting = false;
    bool m_running = false;
    bool m_peerConnected = false;
    bool m_queuePeerDisconnect = false;
    bool m_disconnectedByUs = false;
    bool m_reconnectAfterEnd = false;
    bool m_subscriptionEnded = false;
    std::string m_reconnectClientName;
    std::string m_reconnectClientAddress;
    std::string m_clientName;
    std::string m_clientAddress;
    playback::CastSessionInfo m_lastPublishedCastSession;
    std::size_t m_subscriptionCount = 0;
    std::optional<SessionTime> m_peerGraceDeadline;
    std::optional<SessionTime> m_stopIdleDeadline;
    bool m_stopIdleSettling = false;
    std::uint64_t m_playbackListener = 0;
    std::optional<EndOperation> m_endOperation;
    std::uint64_t m_endToken = 0;
    std::optional<MediaRequest> m_pendingOpen;
    std::optional<SessionTime> m_openDeadline;
    std::optional<MediaRequest> m_protocolCurrent;
    std::optional<MediaRequest> m_protocolNext;
    std::optional<MediaRequest> m_protocolPrevious;
    std::optional<MediaCandidate> m_openAwaitingReady;
    std::map<std::uint64_t, MediaCandidate> m_openCandidates;
    std::map<std::uint64_t, MediaCandidate> m_setNextCandidates;
    std::map<std::uint64_t, PlaybackEvent> m_earlyCommandEvents;
    std::unordered_set<std::uint64_t> m_ignoredCommandIds;
    std::unordered_set<std::string> m_ignoredPlaybackUris;
    std::unordered_set<std::string> m_ignoredPlaybackNextUris;
    std::uint64_t m_protocolEpoch = 0;
    bool m_gatewaySubmitInProgress = false;
    bool m_mediaBoundaryPending = false;
    bool m_ignorePlaybackQueue = false;
    std::map<std::uint64_t, PlaybackCommandType> m_pendingCommands;
    std::unordered_set<std::uint64_t> m_awaitedCommands;
    std::map<std::uint64_t, std::pair<PlaybackOutcomeStatus, std::string>>
        m_commandCompletions;
    std::condition_variable_any m_commandReady;
    std::uint64_t m_lastObservedStateVersion = 0;
    std::vector<PlaybackCommand> m_deferredAfterOpen;
    std::uint64_t m_openToken = 0;
    std::uint64_t m_openCommandId = 0;
    bool m_openInFlight = false;
    bool m_openIntentInProgress = false;
    bool m_flushingDeferred = false;
    bool m_endSubmitInProgress = false;
    bool m_transitionInProgress = false;
    bool m_desiredPeerConnected = false;
    bool m_peerSyncPending = false;
};

} // namespace mcast::dlna
