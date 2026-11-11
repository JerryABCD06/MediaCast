// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/session/DlnaSession.h"

#include <chrono>
#include <mutex>
#include <utility>

namespace mcast::dlna {
namespace {

template <typename Function>
decltype(auto) outsideLock(
    std::unique_lock<std::recursive_mutex> &lock, Function &&function)
{
    struct Relock {
        std::unique_lock<std::recursive_mutex> &lock;
        ~Relock() { lock.lock(); }
    } relock{lock};
    lock.unlock();
    return std::forward<Function>(function)();
}

bool waitsForOpen(PlaybackCommandType type)
{
    return type == PlaybackCommandType::Play
        || type == PlaybackCommandType::Pause
        || type == PlaybackCommandType::Stop
        || type == PlaybackCommandType::Seek
        || type == PlaybackCommandType::Next
        || type == PlaybackCommandType::Previous;
}

SessionTime clockNow()
{
    return std::chrono::duration_cast<SessionTime>(
        std::chrono::steady_clock::now().time_since_epoch());
}

} // namespace

DlnaSession::DlnaSession(
    PlaybackGateway &playback, DlnaSessionCallbacks callbacks)
    : m_playback(playback)
    , m_callbacks(std::move(callbacks))
{
    m_playbackListener = m_playback.addEventListener(
        [this](const PlaybackEvent &event) {
            handlePlaybackEvent(event);
        });
}

DlnaSession::~DlnaSession()
{
    m_playback.removeEventListener(m_playbackListener);
}

bool DlnaSession::start()
{
    Lock lock(m_mutex);
    if (m_transitionInProgress)
        return false;
    if (m_accepting && m_running)
        return true;

    const bool started = m_callbacks.startNetwork
        && outsideLock(lock, [&] { return m_callbacks.startNetwork(); });
    if (!started) {
        if (m_callbacks.networkStateChanged
            || m_callbacks.log) {
            outsideLock(lock, [&] {
                if (m_callbacks.networkStateChanged)
                    m_callbacks.networkStateChanged(false);
                if (m_callbacks.log)
                    m_callbacks.log("DLNA network failed to start");
            });
        }
        return false;
    }
    m_accepting = true;
    m_running = true;
    if (m_callbacks.networkStateChanged) {
        outsideLock(lock, [&] {
            m_callbacks.networkStateChanged(true);
        });
    }
    return true;
}

void DlnaSession::pauseAccepting(SessionTime now)
{
    Lock lock(m_mutex);
    if (!m_accepting || m_endOperation)
        return;

    const bool openQueued =
        m_openIntentInProgress || m_openInFlight;
    cancelPendingOpen("DLNA receiving paused", lock);
    m_disconnectedByUs = true;
    m_queuePeerDisconnect = true;
    setPeerConnected(false, lock, false);
    const auto playback = outsideLock(lock, [&] {
        return m_playback.snapshot();
    });
    if (playback.state == PlaybackState::NoMedia && !openQueued) {
        cancelPeerGrace();
        finishPause(lock);
        return;
    }
    requestEnd(EndOperationMode::Pause, now, lock);
}

bool DlnaSession::resumeAccepting()
{
    Lock lock(m_mutex);
    if (m_transitionInProgress)
        return false;
    if (m_accepting) {
        if (m_endOperation
            && m_endOperation->mode == EndOperationMode::Pause) {
            if (m_callbacks.log) {
                outsideLock(lock, [&] {
                    m_callbacks.log(
                        "Cannot resume while pause is completing");
                });
            }
            return false;
        }
        return true;
    }

    const bool started = m_callbacks.startNetwork
        && outsideLock(lock, [&] { return m_callbacks.startNetwork(); });
    if (!started) {
        if (m_callbacks.networkStateChanged
            || m_callbacks.log) {
            outsideLock(lock, [&] {
                if (m_callbacks.networkStateChanged)
                    m_callbacks.networkStateChanged(false);
                if (m_callbacks.log)
                    m_callbacks.log("DLNA network failed to resume");
            });
        }
        return false;
    }
    m_accepting = true;
    m_running = true;
    if (m_callbacks.networkStateChanged
        || m_callbacks.log) {
        outsideLock(lock, [&] {
            if (m_callbacks.networkStateChanged)
                m_callbacks.networkStateChanged(true);
            if (m_callbacks.log)
                m_callbacks.log("DLNA receiving resumed");
        });
    }
    return true;
}

void DlnaSession::setSubscriptionCount(
    std::size_t count, SubscriptionChangeReason reason, SessionTime now)
{
    Lock lock(m_mutex);
    m_subscriptionCount = count;
    if (m_endOperation)
        return;
    if (reason == SubscriptionChangeReason::Added
        || reason == SubscriptionChangeReason::Renewed) {
        m_disconnectedByUs = false;
    }

    if (count > 0 && !m_disconnectedByUs) {
        cancelPeerGrace();
        setPeerConnected(true, lock);
    } else if (
        count == 0
        && reason == SubscriptionChangeReason::Unreachable
        && m_peerConnected) {
        startPeerGrace(now, lock);
    } else if (
        count == 0
        && (reason == SubscriptionChangeReason::ExplicitRemoval
            || reason == SubscriptionChangeReason::Expired
            || reason == SubscriptionChangeReason::Count)) {
        cancelPeerGrace();
        setPeerConnected(false, lock);
    }
}

void DlnaSession::peerActivity(SessionTime now)
{
    Lock lock(m_mutex);
    if (m_endOperation)
        return;
    if (m_disconnectedByUs) {
        m_disconnectedByUs = false;
        if (m_subscriptionCount > 0) {
            cancelPeerGrace();
            setPeerConnected(true, lock);
        }
    } else if (m_subscriptionCount > 0) {
        cancelPeerGrace();
        setPeerConnected(true, lock);
    }
    (void)now;
}

void DlnaSession::tick(SessionTime now)
{
    Lock lock(m_mutex);
    if (m_endOperation && now >= m_endOperation->deadline) {
        const auto endToken = m_endOperation->token;
        const auto mode = m_endOperation->mode;
        if (m_callbacks.log) {
            outsideLock(lock, [&] {
                m_callbacks.log(
                    "DLNA session-end confirmation timed out");
            });
        }
        if (!m_endOperation
            || m_endOperation->token != endToken)
            return;
        if (m_endOperation->commandId != 0
            || (!m_openIntentInProgress && !m_openInFlight)) {
            if (mode == EndOperationMode::Pause)
                finishPause(lock);
            else
                finishDisconnect(lock);
            return;
        }
    }
    if (m_openDeadline && now >= *m_openDeadline) {
        m_pendingOpen.reset();
        m_openDeadline.reset();
        m_openInFlight = false;
        m_openCommandId = 0;
        clearDeferredCommands();
        if (m_callbacks.requestFailed) {
            outsideLock(lock, [&] {
                m_callbacks.requestFailed(
                    "Playback did not become ready");
            });
        }
    }
    retryPendingEnd(lock);
    if (!m_peerGraceDeadline || now < *m_peerGraceDeadline)
        return;
    m_peerGraceDeadline.reset();
    setPeerConnected(false, lock);
}

PlaybackCommandSubmission DlnaSession::openMedia(
    const MediaRequest &request, SessionTime now)
{
    Lock lock(m_mutex);
    if (m_endOperation) {
        const auto playback = outsideLock(lock, [&] {
            return m_playback.snapshot();
        });
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation, "DLNA session is ending",
                playback.stateVersion};
    }
    if (request.uri.empty()) {
        PlaybackCommand command;
        command.type = PlaybackCommandType::Open;
        command.media = request;
        return submitLocked(std::move(command), lock);
    }

    const std::uint64_t openToken = ++m_openToken;
    struct IntentGuard {
        bool &value;
        ~IntentGuard() { value = false; }
    } intent{m_openIntentInProgress};
    m_openIntentInProgress = true;
    const auto finishIntent = [&] {
        m_openIntentInProgress = false;
        retryPendingEnd(lock);
    };
    if (m_callbacks.requestUiActivation) {
        outsideLock(lock, [&] {
            m_callbacks.requestUiActivation();
        });
    }
    PlaybackCommand command;
    command.type = PlaybackCommandType::Open;
    command.media = request;
    auto submission = submitLocked(
        std::move(command), lock, true, openToken);
    if (submission.queued) {
        m_pendingOpen.reset();
        m_openInFlight = true;
        m_openCommandId = submission.commandId;
        m_openDeadline = now + kOpenTimeout;
        finishIntent();
        return submission;
    }
    if (submission.status == PlaybackSubmissionStatus::PlayerUnavailable
        || submission.status
            == PlaybackSubmissionStatus::StaleGeneration
        || submission.status
            == PlaybackSubmissionStatus::WaitingForPlayer) {
        clearDeferredCommands();
        m_pendingOpen = request;
        m_openDeadline = now + kOpenTimeout;
        m_openInFlight = true;
        m_openCommandId = 0;
        const auto playback = outsideLock(lock, [&] {
            return m_playback.snapshot();
        });
        finishIntent();
        return {true, PlaybackSubmissionStatus::WaitingForPlayer, 0,
                playback.generation, "waiting for playback readiness",
                playback.stateVersion};
    }
    if (m_callbacks.requestFailed) {
        outsideLock(lock, [&] {
            m_callbacks.requestFailed(submission.error);
        });
    }
    finishIntent();
    return submission;
}

PlaybackCommandSubmission DlnaSession::submit(PlaybackCommand command)
{
    Lock lock(m_mutex);
    return submitLocked(std::move(command), lock);
}

PlaybackCommandSubmission DlnaSession::submitLocked(
    PlaybackCommand command,
    Lock &lock,
    bool allowDefer,
    std::optional<std::uint64_t> requiredOpenToken)
{
    const bool endCommand =
        command.type == PlaybackCommandType::EndSession;
    if (m_endOperation && !endCommand) {
        const auto playback = outsideLock(lock, [&] {
            return m_playback.snapshot();
        });
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation, "DLNA session is ending",
                playback.stateVersion};
    }
    lock.unlock();
    std::unique_lock<std::recursive_mutex> gatewayLock(
        m_gatewaySubmitMutex);
    lock.lock();
    if (m_endOperation && !endCommand) {
        const auto playback = outsideLock(lock, [&] {
            return m_playback.snapshot();
        });
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation, "DLNA session is ending",
                playback.stateVersion};
    }
    if (requiredOpenToken
        && (*requiredOpenToken != m_openToken
            || m_transitionInProgress)) {
        const auto playback = outsideLock(lock, [&] {
            return m_playback.snapshot();
        });
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation, "DLNA open was cancelled",
                playback.stateVersion};
    }
    const auto playback = outsideLock(lock, [&] {
        return m_playback.snapshot();
    });
    if (m_endOperation && !endCommand) {
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation, "DLNA session is ending",
                playback.stateVersion};
    }
    if (requiredOpenToken
        && (*requiredOpenToken != m_openToken
            || m_transitionInProgress)) {
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation, "DLNA open was cancelled",
                playback.stateVersion};
    }
    if (allowDefer && (m_pendingOpen || m_flushingDeferred)
        && waitsForOpen(command.type)) {
        constexpr std::size_t maxDeferred = 8;
        if (m_deferredAfterOpen.size() >= maxDeferred) {
            return {false, PlaybackSubmissionStatus::InvalidCommand, 0,
                    playback.generation,
                    "too many commands are waiting for playback",
                    playback.stateVersion};
        }
        command.expectedGeneration = 0;
        m_deferredAfterOpen.push_back(std::move(command));
        return {true, PlaybackSubmissionStatus::WaitingForPlayer, 0,
                playback.generation,
                "waiting for playback readiness",
                playback.stateVersion};
    }
    if (command.expectedGeneration == 0)
        command.expectedGeneration = playback.generation;
    const auto type = command.type;
    // Keep the cancellation token valid through the actual Gateway write.
    auto submission = m_playback.submit(command);
    if (submission.queued)
        m_pendingCommands.emplace(submission.commandId, type);
    return submission;
}

PlaybackCommandSubmission DlnaSession::endSession(SessionTime now)
{
    Lock lock(m_mutex);
    if (m_endOperation) {
        const auto playback = outsideLock(lock, [&] {
            return m_playback.snapshot();
        });
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation, "session end is already pending",
                playback.stateVersion};
    }
    cancelPendingOpen("DLNA session ended", lock);
    m_disconnectedByUs = true;
    m_queuePeerDisconnect = true;
    cancelPeerGrace();
    setPeerConnected(false, lock, false);
    return requestEnd(EndOperationMode::Disconnect, now, lock);
}

void DlnaSession::handlePlaybackEvent(const PlaybackEvent &event)
{
    Lock lock(m_mutex);
    std::optional<std::string> failure;
    bool notifyState = false;
    if (event.kind == PlaybackEventKind::CommandCompleted) {
        const auto pending = m_pendingCommands.find(event.commandId);
        if (pending != m_pendingCommands.end()) {
            m_pendingCommands.erase(pending);
            if (event.outcome != PlaybackOutcomeStatus::Applied) {
                failure = event.error.empty()
                    ? "Playback command failed" : event.error;
            }
        }
    }
    if (event.kind == PlaybackEventKind::CommandCompleted
        && m_openCommandId != 0
        && event.commandId == m_openCommandId) {
        m_openCommandId = 0;
        m_openDeadline.reset();
        m_openInFlight =
            event.outcome == PlaybackOutcomeStatus::Applied
            && event.snapshot.state == PlaybackState::NoMedia;
        if (event.outcome == PlaybackOutcomeStatus::Applied) {
            if (event.snapshot.state != PlaybackState::NoMedia)
                flushDeferredCommands(lock);
        } else {
            clearDeferredCommands();
        }
    }
    if (event.kind == PlaybackEventKind::CommandCompleted
        && m_endOperation
        && event.commandId == m_endOperation->commandId) {
        m_endOperation->commandCompleted = true;
    }
    if ((event.kind == PlaybackEventKind::Reconnected
         || event.kind == PlaybackEventKind::SnapshotChanged)
        && m_pendingOpen
        && event.snapshot.playerAvailable) {
        tryPendingOpen(lock);
    }
    if ((event.kind == PlaybackEventKind::Reconnected
         || event.kind == PlaybackEventKind::SnapshotChanged)
        && event.snapshot.playerAvailable
        && m_peerSyncPending
        && event.snapshot.peerConnected != m_desiredPeerConnected) {
        queuePeerConnected(m_desiredPeerConnected, lock);
    }
    if (event.kind == PlaybackEventKind::SnapshotChanged
        || event.kind == PlaybackEventKind::Reconnected
        || event.kind == PlaybackEventKind::PlayerLost) {
        if (event.snapshot.state != PlaybackState::NoMedia) {
            m_openDeadline.reset();
            m_openInFlight = false;
            m_openCommandId = 0;
            if (!m_deferredAfterOpen.empty())
                flushDeferredCommands(lock);
        }
        if (event.kind == PlaybackEventKind::PlayerLost)
            clearDeferredCommands();
        if (event.kind == PlaybackEventKind::PlayerLost) {
            m_openDeadline.reset();
            m_openInFlight = false;
            m_openCommandId = 0;
        }
        if (event.kind == PlaybackEventKind::PlayerLost
            && event.snapshot.peerConnected != m_desiredPeerConnected) {
            m_peerSyncPending = true;
        }
        notifyState = true;
        if (m_endOperation
            && event.snapshot.state == PlaybackState::NoMedia) {
            m_endOperation->finalSnapshotSeen = true;
        }
    }

    if (failure && m_callbacks.requestFailed) {
        const std::string message = *failure;
        outsideLock(lock, [&] {
            m_callbacks.requestFailed(message);
        });
    }
    if (notifyState && m_callbacks.stateChanged) {
        outsideLock(lock, [&] {
            m_callbacks.stateChanged();
        });
    }
    retryPendingEnd(lock);
    maybeFinishEnd(lock);
}

bool DlnaSession::transportTransitionPending() const
{
    const std::scoped_lock lock(m_mutex);
    return m_openInFlight || m_pendingOpen.has_value();
}

DlnaSessionSnapshot DlnaSession::snapshot() const
{
    Lock lock(m_mutex);
    DlnaSessionSnapshot result;
    result.accepting = m_accepting;
    result.running = m_running;
    result.peerConnected = m_peerConnected;
    result.disconnectedByUs = m_disconnectedByUs;
    result.subscriptionCount = m_subscriptionCount;
    result.peerGraceActive = m_peerGraceDeadline.has_value();
    if (m_peerGraceDeadline)
        result.peerGraceDeadline = *m_peerGraceDeadline;
    result.playback = outsideLock(lock, [&] {
        return m_playback.snapshot();
    });
    return result;
}

void DlnaSession::setPeerConnected(
    bool connected, Lock &lock, bool queuePlayback)
{
    if (m_peerConnected == connected)
        return;
    m_peerConnected = connected;
    if (queuePlayback)
        queuePeerConnected(connected, lock);
    if (m_callbacks.peerConnectedChanged) {
        outsideLock(lock, [&] {
            m_callbacks.peerConnectedChanged(connected);
        });
    }
}

void DlnaSession::queuePeerConnected(bool connected, Lock &lock)
{
    m_desiredPeerConnected = connected;
    PlaybackCommand command;
    command.type = PlaybackCommandType::SetPeerConnected;
    command.boolValue = connected;
    auto submission =
        submitLocked(std::move(command), lock, false);
    if (submission.queued) {
        m_peerSyncPending = false;
        m_pendingCommands.emplace(
            submission.commandId, PlaybackCommandType::SetPeerConnected);
    } else if (
        submission.status == PlaybackSubmissionStatus::PlayerUnavailable
        || submission.status == PlaybackSubmissionStatus::StaleGeneration
        || submission.status == PlaybackSubmissionStatus::WaitingForPlayer) {
        m_peerSyncPending = true;
    }
}

void DlnaSession::startPeerGrace(SessionTime now, Lock &lock)
{
    if (m_peerGraceDeadline)
        return;
    m_peerGraceDeadline = now + kPeerGrace;
    if (m_callbacks.log) {
        outsideLock(lock, [&] {
            m_callbacks.log(
                "DLNA peer disappeared; waiting out grace period");
        });
    }
}

void DlnaSession::cancelPeerGrace()
{
    m_peerGraceDeadline.reset();
}

void DlnaSession::finishPause(Lock &lock)
{
    if (!m_accepting && !m_running)
        return;
    struct TransitionGuard {
        bool &value;
        ~TransitionGuard() { value = false; }
    } transition{m_transitionInProgress};
    m_transitionInProgress = true;
    m_endOperation.reset();
    m_accepting = false;
    m_running = false;
    m_disconnectedByUs = true;
    cancelPeerGrace();
    if (m_queuePeerDisconnect) {
        m_queuePeerDisconnect = false;
        queuePeerConnected(false, lock);
    }
    if (m_callbacks.announceGoingAway
        || m_callbacks.stopNetwork
        || m_callbacks.networkStateChanged
        || m_callbacks.log) {
        outsideLock(lock, [&] {
            if (m_callbacks.announceGoingAway)
                m_callbacks.announceGoingAway();
            if (m_callbacks.stopNetwork)
                m_callbacks.stopNetwork();
            if (m_callbacks.networkStateChanged)
                m_callbacks.networkStateChanged(false);
            if (m_callbacks.log)
                m_callbacks.log("DLNA receiving paused");
        });
    }
}

void DlnaSession::finishDisconnect(Lock &lock)
{
    struct TransitionGuard {
        bool &value;
        ~TransitionGuard() { value = false; }
    } transition{m_transitionInProgress};
    m_transitionInProgress = true;
    m_endOperation.reset();
    if (m_queuePeerDisconnect) {
        m_queuePeerDisconnect = false;
        queuePeerConnected(false, lock);
    }
    if (m_callbacks.announceGoingAway || m_callbacks.log) {
        outsideLock(lock, [&] {
            if (m_callbacks.announceGoingAway)
                m_callbacks.announceGoingAway();
            if (m_callbacks.log)
                m_callbacks.log("DLNA session ended");
        });
    }
}

PlaybackCommandSubmission DlnaSession::requestEnd(
    EndOperationMode mode, SessionTime now, Lock &lock)
{
    if (m_endOperation) {
        const auto playback = outsideLock(lock, [&] {
            return m_playback.snapshot();
        });
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                playback.generation,
                "session end is already pending",
                playback.stateVersion};
    }
    const std::uint64_t endToken = ++m_endToken;
    m_endOperation = EndOperation{
        mode, endToken, 0, now + SessionTime{5000}, false, false};
    PlaybackCommand command;
    command.type = PlaybackCommandType::EndSession;
    struct SubmitGuard {
        bool &value;
        ~SubmitGuard() { value = false; }
    } submitGuard{m_endSubmitInProgress};
    m_endSubmitInProgress = true;
    const auto submission =
        submitLocked(std::move(command), lock);
    const auto operationStillActive = [&] {
        return m_endOperation
            && m_endOperation->token == endToken;
    };
    if (!submission.queued) {
        if (operationStillActive()) {
            if (m_openIntentInProgress || m_openInFlight)
                return submission;
            if (mode == EndOperationMode::Pause)
                finishPause(lock);
            else
                finishDisconnect(lock);
        }
        return submission;
    }
    if (operationStillActive())
        m_endOperation->commandId = submission.commandId;
    return submission;
}

void DlnaSession::retryPendingEnd(Lock &lock)
{
    if (!m_endOperation || m_endOperation->commandId != 0
        || m_openIntentInProgress || m_openInFlight
        || m_endSubmitInProgress
        || m_transitionInProgress) {
        return;
    }
    const auto mode = m_endOperation->mode;
    const auto endToken = m_endOperation->token;
    PlaybackCommand command;
    command.type = PlaybackCommandType::EndSession;
    const auto submission =
        submitLocked(std::move(command), lock);
    if (!m_endOperation || m_endOperation->token != endToken)
        return;
    if (submission.queued) {
        m_endOperation->commandId = submission.commandId;
        return;
    }
    if (mode == EndOperationMode::Pause)
        finishPause(lock);
    else
        finishDisconnect(lock);
}

void DlnaSession::maybeFinishEnd(Lock &lock)
{
    if (!m_endOperation)
        return;
    if (!m_endOperation->commandCompleted
        || !m_endOperation->finalSnapshotSeen)
        return;
    if (m_endOperation->mode == EndOperationMode::Pause)
        finishPause(lock);
    else
        finishDisconnect(lock);
}

void DlnaSession::cancelPendingOpen(
    const std::string &reason, Lock &lock)
{
    const bool submittedOpen = m_openInFlight && m_openCommandId != 0;
    m_openInFlight = submittedOpen;
    if (!submittedOpen)
        m_openCommandId = 0;
    ++m_openToken;
    m_openDeadline.reset();
    clearDeferredCommands();
    if (!m_pendingOpen)
        return;
    m_pendingOpen.reset();
    if (m_callbacks.requestFailed) {
        outsideLock(lock, [&] {
            m_callbacks.requestFailed(reason);
        });
    }
}

void DlnaSession::tryPendingOpen(Lock &lock)
{
    if (!m_pendingOpen)
        return;
    const std::uint64_t token = m_openToken;
    const MediaRequest pendingMedia = *m_pendingOpen;
    const auto playback = outsideLock(lock, [&] {
        return m_playback.snapshot();
    });
    if (!playback.playerAvailable || !m_pendingOpen
        || token != m_openToken) {
        return;
    }
    PlaybackCommand command;
    command.type = PlaybackCommandType::Open;
    command.media = pendingMedia;
    const auto submission = submitLocked(
        std::move(command), lock, true, token);
    if (submission.queued) {
        ++m_openToken;
        m_pendingOpen.reset();
        m_openDeadline.reset();
        m_openDeadline = clockNow() + kOpenTimeout;
        m_openInFlight = true;
        m_openCommandId = submission.commandId;
        if (m_callbacks.log) {
            outsideLock(lock, [&] {
                m_callbacks.log(
                    "Pending DLNA open submitted after reconnect");
            });
        }
        return;
    }
    if (submission.status == PlaybackSubmissionStatus::InvalidCommand) {
        ++m_openToken;
        m_pendingOpen.reset();
        m_openDeadline.reset();
        m_openInFlight = false;
        m_openCommandId = 0;
        clearDeferredCommands();
        if (m_callbacks.requestFailed) {
            outsideLock(lock, [&] {
                m_callbacks.requestFailed(submission.error);
            });
        }
    }
}

void DlnaSession::flushDeferredCommands(Lock &lock)
{
    if (m_flushingDeferred)
        return;
    struct FlushGuard {
        bool &value;
        ~FlushGuard() { value = false; }
    } guard{m_flushingDeferred};
    m_flushingDeferred = true;
    while (!m_deferredAfterOpen.empty()) {
        auto deferred = std::move(m_deferredAfterOpen);
        m_deferredAfterOpen.clear();
        for (auto &command : deferred) {
            const auto submission = submitLocked(
                std::move(command), lock, false);
            if (!submission.queued
                && m_callbacks.requestFailed) {
                const std::string error = submission.error.empty()
                    ? "Deferred playback command failed"
                    : submission.error;
                outsideLock(lock, [&] {
                    m_callbacks.requestFailed(error);
                });
            }
        }
    }
}

void DlnaSession::clearDeferredCommands()
{
    m_deferredAfterOpen.clear();
}

} // namespace mcast::dlna
