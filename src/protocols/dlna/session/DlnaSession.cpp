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

bool DlnaSession::start(bool accepting)
{
    Lock lock(m_mutex);
    if (m_transitionInProgress) {
        if (m_callbacks.log) {
            outsideLock(lock, [&] {
                m_callbacks.log(
                    "DLNA session start rejected while a transition "
                    "is still in progress (accepting="
                    + std::to_string(m_accepting)
                    + ", running=" + std::to_string(m_running)
                    + ", endOperation="
                    + std::to_string(m_endOperation.has_value()) + ")");
            });
        }
        return false;
    }
    if (!accepting) {
        m_accepting = false;
        m_running = false;
        m_disconnectedByUs = true;
        m_reconnectAfterEnd = false;
        m_subscriptionEnded = true;
        m_reconnectClientName.clear();
        m_reconnectClientAddress.clear();
        return true;
    }
    if (m_accepting && m_running)
        return true;

    const bool started = m_callbacks.startNetwork
        && outsideLock(lock, [&] { return m_callbacks.startNetwork(); });
    if (!started) {
        if (m_callbacks.log) {
            outsideLock(lock, [&] {
                m_callbacks.log(
                    "DLNA session start could not start its network");
            });
        }
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
    if (count > 0 && !m_endOperation)
        m_subscriptionEnded = false;
    if (m_endOperation) {
        if (count > 0
            && reason == SubscriptionChangeReason::Added
            && !m_disconnectedByUs) {
            m_reconnectAfterEnd = true;
            m_reconnectClientName = m_clientName;
            m_reconnectClientAddress = m_clientAddress;
        }
        return;
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
        cancelStopIdleGrace();
        if (m_peerConnected) {
            // A zero-subscription transition is a session boundary, not only a
            // presence flag change. End and unload the cast media before the
            // local UI can revive it with the Play button.
            m_subscriptionEnded = true;
            m_queuePeerDisconnect = true;
            setPeerConnected(false, lock, false);
            (void)requestEnd(EndOperationMode::Disconnect, now, lock);
        }
    }
    publishCastSession(lock);
}

void DlnaSession::peerActivity(
    SessionTime now, std::string_view clientAddress)
{
    Lock lock(m_mutex);
    if (m_endOperation)
        return;
    if (m_peerConnected && !clientAddress.empty()
        && !isCurrentOwner(clientAddress)) {
        // A second control point must not keep the current owner's Stop grace
        // alive. Ownership changes only at a validated media boundary.
        return;
    }
    if (m_disconnectedByUs) {
        // A query, Play/Stop or subscription renewal is not a new cast. Keep
        // the explicit local disconnect sticky until SetAVTransportURI.
        return;
    }
    if (m_subscriptionEnded) {
        // Explicit GENA teardown stays closed until a new subscription or
        // media request, even if an old control point keeps polling SOAP.
        return;
    }
    cancelPeerGrace();
    cancelStopIdleGrace();
    setPeerConnected(true, lock);
    (void)now;
}

void DlnaSession::transportStopped(
    SessionTime now, std::string_view clientAddress)
{
    Lock lock(m_mutex);
    if (m_endOperation || !m_peerConnected || m_disconnectedByUs)
        return;
    if (!clientAddress.empty() && !isCurrentOwner(clientAddress))
        return;
    m_stopIdleDeadline = now + kStopIdleGrace;
    m_stopIdleSettling = false;
    if (m_callbacks.log) {
        outsideLock(lock, [&] {
            m_callbacks.log(
                "DLNA control point stopped; ending the session if it "
                "remains idle for "
                + std::to_string(kStopIdleGrace.count()) + " ms");
        });
    }
}

void DlnaSession::observeHttpClient(
    const std::string &name, const std::string &address)
{
    Lock lock(m_mutex);
    // While a session owns the renderer, unrelated discovery/query traffic
    // must not replace its identity. A validated media boundary can claim the
    // renderer for another address.
    if (m_peerConnected && !address.empty()) {
        if (!isCurrentOwner(address))
            return;
        if (address == m_clientAddress && name != m_clientName)
            return;
    }
    if (m_clientName == name && m_clientAddress == address)
        return;
    m_clientName = name;
    m_clientAddress = address;
    if (m_peerConnected)
        publishCastSession(lock);
}

void DlnaSession::publishCastSession(Lock &lock)
{
    playback::CastSessionInfo info;
    info.connected = m_peerConnected;
    info.protocol = m_peerConnected ? "DLNA" : "";
    info.controlPointName = m_clientName;
    info.controlPointAddress = m_clientAddress;
    info.remoteControlRegistered =
        m_peerConnected && m_subscriptionCount > 0;
    if (info.connected == m_lastPublishedCastSession.connected
        && info.protocol == m_lastPublishedCastSession.protocol
        && info.controlPointName
            == m_lastPublishedCastSession.controlPointName
        && info.controlPointAddress
            == m_lastPublishedCastSession.controlPointAddress
        && info.remoteControlRegistered
            == m_lastPublishedCastSession.remoteControlRegistered) {
        return;
    }

    PlaybackCommand command;
    command.type = PlaybackCommandType::SetCastSession;
    command.castSession = info;
    const auto submission =
        submitLocked(std::move(command), lock, false);
    if (submission.queued)
        m_lastPublishedCastSession = std::move(info);
}

void DlnaSession::newMediaRequested(
    SessionTime now, std::string clientName,
    std::string clientAddress)
{
    Lock lock(m_mutex);
    if (m_endOperation)
        return;

    if (!clientName.empty())
        m_clientName = std::move(clientName);
    if (!clientAddress.empty())
        m_clientAddress = std::move(clientAddress);
    cancelStopIdleGrace();

    // A new control point starts a new media epoch. Anything still in flight
    // belongs to the previous owner and must never materialize after this
    // boundary, even if the Gateway reports success late.
    ++m_protocolEpoch;
    ++m_openToken;
    m_pendingOpen.reset();
    m_openDeadline.reset();
    m_openInFlight = false;
    m_openCommandId = 0;
    if (m_openAwaitingReady)
        m_ignoredPlaybackUris.insert(
            m_openAwaitingReady->media.uri);
    for (const auto &[commandId, candidate] : m_openCandidates) {
        (void)commandId;
        m_ignoredPlaybackUris.insert(candidate.media.uri);
    }
    m_openAwaitingReady.reset();
    m_openCandidates.clear();
    m_setNextCandidates.clear();
    m_earlyCommandEvents.clear();
    clearDeferredCommands();
    for (auto it = m_pendingCommands.begin(); it != m_pendingCommands.end();) {
        if (it->second == PlaybackCommandType::Open
            || it->second == PlaybackCommandType::SetNext) {
            m_ignoredCommandIds.insert(it->first);
            it = m_pendingCommands.erase(it);
        } else {
            ++it;
        }
    }
    m_commandCompletions.clear();
    m_awaitedCommands.clear();
    m_commandReady.notify_all();

    m_disconnectedByUs = false;
    m_reconnectAfterEnd = false;
    m_subscriptionEnded = false;
    m_mediaBoundaryPending = true;
    m_ignorePlaybackQueue = true;
    m_protocolNext.reset();
    m_protocolPrevious.reset();
    m_ignoredPlaybackNextUris.clear();
    cancelPeerGrace();
    setPeerConnected(true, lock);
    (void)now;
}

bool DlnaSession::isCurrentOwner(
    std::string_view clientAddress) const noexcept
{
    return clientAddress.empty()
        || m_clientAddress.empty()
        || clientAddress == m_clientAddress;
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
        const std::uint64_t expiredCommandId = m_openCommandId;
        if (expiredCommandId != 0) {
            const auto candidate =
                m_openCandidates.find(expiredCommandId);
            if (candidate != m_openCandidates.end())
                m_ignoredPlaybackUris.insert(
                    candidate->second.media.uri);
            m_ignoredCommandIds.insert(expiredCommandId);
            m_pendingCommands.erase(expiredCommandId);
            m_openCandidates.erase(expiredCommandId);
        }
        if (m_openAwaitingReady) {
            m_ignoredPlaybackUris.insert(
                m_openAwaitingReady->media.uri);
            m_openAwaitingReady.reset();
            m_mediaBoundaryPending = false;
        }
        m_pendingOpen.reset();
        m_openDeadline.reset();
        m_openInFlight = false;
        m_openCommandId = 0;
        m_openAwaitingReady.reset();
        clearDeferredCommands();
        if (m_callbacks.requestFailed) {
            outsideLock(lock, [&] {
                m_callbacks.requestFailed(
                    "Playback did not become ready");
            });
        }
    }
    retryPendingEnd(lock);
    if (m_peerGraceDeadline && now >= *m_peerGraceDeadline) {
        m_peerGraceDeadline.reset();
        cancelStopIdleGrace();
        if (m_peerConnected) {
            // Grace only protects transient network loss. Once it expires,
            // the session is over and the loaded media must be unloaded.
            m_queuePeerDisconnect = true;
            setPeerConnected(false, lock, false);
            (void)requestEnd(EndOperationMode::Disconnect, now, lock);
        }
    }
    if (!m_stopIdleDeadline || now < *m_stopIdleDeadline)
        return;
    if (m_endOperation || !m_peerConnected || m_disconnectedByUs) {
        cancelStopIdleGrace();
        return;
    }
    if (!m_stopIdleSettling) {
        m_stopIdleSettling = true;
        m_stopIdleDeadline = now + kStopBoundarySettle;
        if (m_callbacks.log) {
            outsideLock(lock, [&] {
                m_callbacks.log(
                    "DLNA Stop grace reached its boundary; allowing an "
                    "in-flight SetAVTransportURI to win");
            });
        }
        return;
    }
    m_stopIdleDeadline.reset();
    m_stopIdleSettling = false;
    if (m_callbacks.log) {
        outsideLock(lock, [&] {
            m_callbacks.log(
                "DLNA control point remained idle after Stop; "
                "ending the session");
        });
    }
    m_queuePeerDisconnect = true;
    setPeerConnected(false, lock, false);
    (void)requestEnd(EndOperationMode::Disconnect, now, lock);
}

PlaybackCommandSubmission DlnaSession::openMedia(
    const MediaRequest &request, SessionTime now, bool clearPlaylist)
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
        command.boolValue = clearPlaylist;
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
    command.boolValue = clearPlaylist;
    auto submission = submitLocked(
        std::move(command), lock, true, openToken);
    if (submission.queued) {
        m_pendingOpen.reset();
        m_openInFlight = true;
        m_openCommandId = submission.commandId;
        m_openDeadline = now + kOpenTimeout;
        consumeEarlyCommand(submission.commandId, lock);
        publishCastSession(lock);
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

PlaybackCommandSubmission DlnaSession::submit(
    PlaybackCommand command,
    std::optional<std::uint64_t> expectedOwnerEpoch)
{
    Lock lock(m_mutex);
    return submitLocked(
        std::move(command), lock, true, std::nullopt, nullptr,
        expectedOwnerEpoch);
}

DlnaCommandResult DlnaSession::submitAndWait(
    PlaybackCommand command, SessionTime timeout,
    std::optional<std::uint64_t> expectedOwnerEpoch)
{
    Lock lock(m_mutex);
    const auto baseline = outsideLock(lock, [&] {
        return m_playback.snapshot().stateVersion;
    });
    m_lastObservedStateVersion = baseline;
    std::uint64_t commandId = 0;
    DlnaCommandResult result;
    result.submission = submitLocked(
        std::move(command), lock, true, std::nullopt, &commandId,
        expectedOwnerEpoch);
    if (!result.submission.queued || commandId == 0) {
        if (!result.submission.queued) {
            result.outcome = PlaybackOutcomeStatus::Rejected;
            result.completionError = result.submission.error;
        }
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto completion = m_commandCompletions.find(commandId);
        if (completion != m_commandCompletions.end()) {
            result.outcome = completion->second.first;
            result.completionError = completion->second.second;
            if (*result.outcome != PlaybackOutcomeStatus::Applied
                || m_lastObservedStateVersion > baseline) {
                m_commandCompletions.erase(completion);
                return result;
            }
        }
        m_commandReady.wait_until(lock, deadline);
    }

    result.timedOut = true;
    result.completionError = "playback command timed out";
    const auto pending = m_pendingCommands.find(commandId);
    if (pending != m_pendingCommands.end()) {
        const PlaybackCommandType type = pending->second;
        m_pendingCommands.erase(pending);
        const auto openCandidate = m_openCandidates.find(commandId);
        if (openCandidate != m_openCandidates.end())
            m_ignoredPlaybackUris.insert(
                openCandidate->second.media.uri);
        const auto nextCandidate = m_setNextCandidates.find(commandId);
        if (nextCandidate != m_setNextCandidates.end())
            m_ignoredPlaybackNextUris.insert(
                nextCandidate->second.media.uri);
        m_openCandidates.erase(commandId);
        m_setNextCandidates.erase(commandId);
        m_ignoredCommandIds.insert(commandId);
        if (type == PlaybackCommandType::Open
            && m_openCommandId == commandId) {
            m_openInFlight = false;
            m_openCommandId = 0;
            m_openDeadline.reset();
            m_openAwaitingReady.reset();
        }
    }
    m_awaitedCommands.erase(commandId);
    m_commandCompletions.erase(commandId);
    return result;
}

PlaybackCommandSubmission DlnaSession::submitLocked(
    PlaybackCommand command,
    Lock &lock,
    bool allowDefer,
    std::optional<std::uint64_t> requiredOpenToken,
    std::uint64_t *awaitedCommandId,
    std::optional<std::uint64_t> expectedOwnerEpoch)
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
    if (expectedOwnerEpoch
        && *expectedOwnerEpoch != m_protocolEpoch) {
        return {false, PlaybackSubmissionStatus::StaleGeneration, 0,
                playback.generation, "DLNA owner changed",
                playback.stateVersion};
    }
    if (allowDefer && (m_pendingOpen || m_flushingDeferred)
        && waitsForOpen(command.type)) {
        constexpr std::size_t maxDeferred = 8;
        if (m_deferredAfterOpen.size() >= maxDeferred) {
            return {false, PlaybackSubmissionStatus::Busy, 0,
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
    const MediaRequest protocolMedia = command.media;
    PlaybackCommand presentation = command;
    presentation.media.metadata.clear();
    // Keep the cancellation token valid through the actual Gateway write.
    struct SubmitStateGuard {
        bool &value;
        ~SubmitStateGuard() { value = false; }
    } submitStateGuard{m_gatewaySubmitInProgress};
    m_gatewaySubmitInProgress = true;
    auto submission = m_playback.submit(presentation);
    if (submission.queued) {
        m_pendingCommands.emplace(submission.commandId, type);
        if (type == PlaybackCommandType::Open)
            m_openCandidates[submission.commandId] =
                MediaCandidate{protocolMedia, m_protocolEpoch};
        else if (type == PlaybackCommandType::SetNext)
            m_setNextCandidates[submission.commandId] =
                MediaCandidate{protocolMedia, m_protocolEpoch};
        if (awaitedCommandId) {
            m_awaitedCommands.insert(submission.commandId);
            *awaitedCommandId = submission.commandId;
        }
        if (type != PlaybackCommandType::Open)
            consumeEarlyCommand(submission.commandId, lock);
    }
    return submission;
}

void DlnaSession::consumeEarlyCommand(
    std::uint64_t commandId, Lock &lock)
{
    const auto early = m_earlyCommandEvents.find(commandId);
    if (early == m_earlyCommandEvents.end())
        return;
    const PlaybackEvent event = std::move(early->second);
    m_earlyCommandEvents.erase(early);

    const auto pending = m_pendingCommands.find(commandId);
    if (pending == m_pendingCommands.end())
        return;
    const PlaybackCommandType type = pending->second;
    m_pendingCommands.erase(pending);

    if (m_awaitedCommands.contains(commandId)) {
        m_awaitedCommands.erase(commandId);
        m_commandCompletions.emplace(
            commandId, std::make_pair(event.outcome, event.error));
    }

    if (event.outcome == PlaybackOutcomeStatus::Applied) {
        if (type == PlaybackCommandType::Open) {
            const auto candidate = m_openCandidates.find(commandId);
            if (candidate != m_openCandidates.end()
                && candidate->second.epoch == m_protocolEpoch) {
                m_openAwaitingReady = candidate->second;
            }
        } else if (type == PlaybackCommandType::SetNext) {
            const auto candidate = m_setNextCandidates.find(commandId);
            if (candidate != m_setNextCandidates.end()
                && candidate->second.epoch == m_protocolEpoch) {
                m_ignoredPlaybackNextUris.clear();
                if (candidate->second.media.uri.empty())
                    m_protocolNext.reset();
                else
                    m_protocolNext = candidate->second.media;
                m_ignorePlaybackQueue = false;
            }
        }
    } else if (type == PlaybackCommandType::Open) {
        const auto candidate = m_openCandidates.find(commandId);
        if (candidate != m_openCandidates.end())
            m_ignoredPlaybackUris.insert(
                candidate->second.media.uri);
    } else if (type == PlaybackCommandType::SetNext) {
        const auto candidate = m_setNextCandidates.find(commandId);
        if (candidate != m_setNextCandidates.end())
            m_ignoredPlaybackNextUris.insert(
                candidate->second.media.uri);
    }

    m_openCandidates.erase(commandId);
    m_setNextCandidates.erase(commandId);
    if (m_openCommandId == commandId) {
        m_openCommandId = 0;
        m_openDeadline.reset();
        m_openInFlight =
            event.outcome == PlaybackOutcomeStatus::Applied
            && event.snapshot.state == PlaybackState::NoMedia;
        if (event.outcome != PlaybackOutcomeStatus::Applied)
            clearDeferredCommands();
    }
    syncProtocolPlayback(event.snapshot);
    m_commandReady.notify_all();
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
    m_subscriptionEnded = true;
    m_reconnectAfterEnd = false;
    m_reconnectClientName.clear();
    m_reconnectClientAddress.clear();
    m_queuePeerDisconnect = true;
    cancelPeerGrace();
    cancelStopIdleGrace();
    setPeerConnected(false, lock, false);
    return requestEnd(EndOperationMode::Disconnect, now, lock);
}

void DlnaSession::abandonEndingSession()
{
    Lock lock(m_mutex);
    if (m_transitionInProgress)
        return;
    m_endOperation.reset();
    m_endSubmitInProgress = false;
    m_disconnectedByUs = true;
    m_reconnectAfterEnd = false;
    m_subscriptionEnded = true;
    m_reconnectClientName.clear();
    m_reconnectClientAddress.clear();
    m_queuePeerDisconnect = true;
    cancelPeerGrace();
    cancelStopIdleGrace();
    setPeerConnected(false, lock, false);
    finishDisconnect(lock);
}

void DlnaSession::resetForRestart()
{
    Lock lock(m_mutex);
    m_endOperation.reset();
    m_endSubmitInProgress = false;
    m_transitionInProgress = false;
    m_accepting = false;
    m_running = false;
    m_peerConnected = false;
    m_clientName.clear();
    m_clientAddress.clear();
    m_lastPublishedCastSession = {};
    m_disconnectedByUs = true;
    m_reconnectAfterEnd = false;
    m_subscriptionEnded = true;
    m_reconnectClientName.clear();
    m_reconnectClientAddress.clear();
    m_queuePeerDisconnect = false;
    m_peerSyncPending = false;
    m_desiredPeerConnected = false;
    m_subscriptionCount = 0;
    m_openInFlight = false;
    m_openCommandId = 0;
    m_openIntentInProgress = false;
    m_openDeadline.reset();
    m_pendingOpen.reset();
    m_protocolCurrent.reset();
    m_protocolNext.reset();
    m_protocolPrevious.reset();
    m_openAwaitingReady.reset();
    m_openCandidates.clear();
    m_setNextCandidates.clear();
    m_earlyCommandEvents.clear();
    m_gatewaySubmitInProgress = false;
    m_mediaBoundaryPending = false;
    m_ignoredPlaybackUris.clear();
    m_ignoredPlaybackNextUris.clear();
    m_ignorePlaybackQueue = false;
    m_pendingCommands.clear();
    m_ignoredCommandIds.clear();
    m_awaitedCommands.clear();
    m_commandCompletions.clear();
    ++m_protocolEpoch;
    m_lastObservedStateVersion = 0;
    m_deferredAfterOpen.clear();
    cancelPeerGrace();
    cancelStopIdleGrace();
}

void DlnaSession::handlePlaybackEvent(const PlaybackEvent &event)
{
    Lock lock(m_mutex);
    if (event.kind == PlaybackEventKind::Reconnected)
        m_lastPublishedCastSession = {};
    if (event.snapshot.stateVersion > m_lastObservedStateVersion)
        m_lastObservedStateVersion = event.snapshot.stateVersion;
    std::optional<std::string> failure;
    bool notifyState = false;
    bool ignoredCommand = false;
    if (event.kind == PlaybackEventKind::CommandCompleted) {
        if (event.commandId != 0
            && m_ignoredCommandIds.erase(event.commandId) != 0) {
            m_pendingCommands.erase(event.commandId);
            m_openCandidates.erase(event.commandId);
            m_setNextCandidates.erase(event.commandId);
            ignoredCommand = true;
        }
        if (ignoredCommand) {
            m_commandReady.notify_all();
            return;
        }
        if (m_awaitedCommands.contains(event.commandId)) {
            m_awaitedCommands.erase(event.commandId);
            m_commandCompletions.emplace(
                event.commandId,
                std::make_pair(event.outcome, event.error));
            m_commandReady.notify_all();
        }
        const auto pending = m_pendingCommands.find(event.commandId);
        if (pending == m_pendingCommands.end()
            && event.commandId != 0
            && m_gatewaySubmitInProgress) {
            constexpr std::size_t kMaxEarlyEvents = 64;
            if (m_earlyCommandEvents.size() >= kMaxEarlyEvents)
                m_earlyCommandEvents.erase(m_earlyCommandEvents.begin());
            m_earlyCommandEvents[event.commandId] = event;
        }
        if (pending != m_pendingCommands.end()) {
            const PlaybackCommandType completedType = pending->second;
            m_pendingCommands.erase(pending);
            if (event.outcome == PlaybackOutcomeStatus::Applied) {
                if (completedType == PlaybackCommandType::Open) {
                    const auto candidate =
                        m_openCandidates.find(event.commandId);
                    if (candidate != m_openCandidates.end()
                        && candidate->second.epoch == m_protocolEpoch) {
                        m_openAwaitingReady = candidate->second;
                    }
                } else if (
                    completedType == PlaybackCommandType::SetNext) {
                    const auto candidate =
                        m_setNextCandidates.find(event.commandId);
                    if (candidate != m_setNextCandidates.end()
                        && candidate->second.epoch == m_protocolEpoch) {
                        m_ignoredPlaybackNextUris.clear();
                        if (candidate->second.media.uri.empty())
                            m_protocolNext.reset();
                        else
                            m_protocolNext = candidate->second.media;
                        m_ignorePlaybackQueue = false;
                    }
                }
            }
            if (event.outcome != PlaybackOutcomeStatus::Applied) {
                if (completedType == PlaybackCommandType::Open) {
                    const auto candidate =
                        m_openCandidates.find(event.commandId);
                    if (candidate != m_openCandidates.end())
                        m_ignoredPlaybackUris.insert(
                            candidate->second.media.uri);
                } else if (
                    completedType == PlaybackCommandType::SetNext) {
                    const auto candidate =
                        m_setNextCandidates.find(event.commandId);
                    if (candidate != m_setNextCandidates.end())
                        m_ignoredPlaybackNextUris.insert(
                            candidate->second.media.uri);
                }
                failure = event.error.empty()
                    ? "Playback command failed" : event.error;
            }
        }
        m_openCandidates.erase(event.commandId);
        m_setNextCandidates.erase(event.commandId);
    }
    if (event.kind == PlaybackEventKind::CommandCompleted
        && m_openCommandId != 0
        && event.commandId == m_openCommandId) {
        m_openCommandId = 0;
        if (!(event.outcome == PlaybackOutcomeStatus::Applied
              && event.snapshot.state != PlaybackState::NoMedia)) {
            m_openDeadline.reset();
        }
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
        if (event.snapshot.state == PlaybackState::Playing
            || event.snapshot.state == PlaybackState::Paused
            || event.snapshot.state == PlaybackState::Stopped) {
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
            m_openAwaitingReady.reset();
            m_openInFlight = false;
            m_openCommandId = 0;
            m_openDeadline.reset();
            m_mediaBoundaryPending = false;
            clearDeferredCommands();
        }
    }

    syncProtocolPlayback(event.snapshot);

    if (event.kind == PlaybackEventKind::Reconnected)
        publishCastSession(lock);

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
    m_commandReady.notify_all();
}

void DlnaSession::syncProtocolPlayback(
    const PlaybackSnapshot &playback)
{
    const std::string &currentUri = playback.current.uri;
    if (!currentUri.empty()
        && m_ignoredPlaybackUris.contains(currentUri)
        && (!m_openAwaitingReady
            || m_openAwaitingReady->media.uri != currentUri)) {
        return;
    }
    if (m_mediaBoundaryPending && !m_openAwaitingReady)
        return;
    if (m_openAwaitingReady
        && !currentUri.empty()
        && m_openAwaitingReady->media.uri == currentUri) {
        if (playback.loadStatus == playback::LoadStatus::Failed
            || playback.state == PlaybackState::Stopped) {
            m_ignoredPlaybackUris.insert(currentUri);
            m_mediaBoundaryPending = false;
            m_protocolCurrent.reset();
            m_protocolNext.reset();
            m_protocolPrevious.reset();
            m_openAwaitingReady.reset();
            m_openInFlight = false;
            m_openCommandId = 0;
            m_openDeadline.reset();
            return;
        } else if (
            playback.loadStatus == playback::LoadStatus::Ok
            && (playback.state == PlaybackState::Playing
                || playback.state == PlaybackState::Paused)) {
            m_protocolCurrent = m_openAwaitingReady->media;
            m_ignoredPlaybackUris.erase(currentUri);
            m_mediaBoundaryPending = false;
            m_openAwaitingReady.reset();
            m_protocolNext.reset();
            m_protocolPrevious.reset();
            m_ignorePlaybackQueue = true;
            m_openInFlight = false;
            m_openCommandId = 0;
            m_openDeadline.reset();
            return;
        }
        if (m_openAwaitingReady)
            return;
    }

    if (!currentUri.empty()) {
        if (m_protocolNext
            && m_protocolNext->uri == currentUri) {
            m_protocolPrevious = m_protocolCurrent;
            m_protocolCurrent = std::move(m_protocolNext);
            m_protocolNext.reset();
        } else if (
            m_protocolPrevious
            && m_protocolPrevious->uri == currentUri) {
            m_protocolNext = m_protocolCurrent;
            m_protocolCurrent = std::move(m_protocolPrevious);
            m_protocolPrevious.reset();
        } else if (
            !m_protocolCurrent
            || m_protocolCurrent->uri != currentUri) {
            // Ui-originated playback (SMTC, QML, automatic queue advance).
            // It is not a DLNA playlist entry, so no protocol queue may leak
            // across that boundary.
            m_protocolCurrent = playback.current;
            m_protocolNext.reset();
            m_protocolPrevious.reset();
            m_ignorePlaybackQueue = true;
        }
    }

    if (m_ignorePlaybackQueue)
        return;
    if (playback.next.uri.empty()) {
        if (m_ignoredPlaybackNextUris.empty())
            m_protocolNext.reset();
    } else if (
        m_ignoredPlaybackNextUris.contains(playback.next.uri)) {
        // Keep the previous valid next record when a timed-out/rejected
        // replacement has nevertheless reached the Ui queue.
    } else if (
        !m_protocolNext
        || m_protocolNext->uri != playback.next.uri) {
        m_protocolNext = playback.next;
    }
}

bool DlnaSession::transportTransitionPending() const
{
    const std::scoped_lock lock(m_mutex);
    return m_openInFlight || m_pendingOpen.has_value()
        || m_openAwaitingReady.has_value();
}

std::uint64_t DlnaSession::protocolEpoch() const
{
    const std::scoped_lock lock(m_mutex);
    return m_protocolEpoch;
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
    result.ending = m_endOperation.has_value();
    result.castSession.connected = m_peerConnected;
    result.castSession.protocol =
        m_peerConnected ? "DLNA" : "";
    result.castSession.controlPointName = m_clientName;
    result.castSession.controlPointAddress = m_clientAddress;
    result.castSession.remoteControlRegistered =
        m_peerConnected && m_subscriptionCount > 0;
    result.playback = outsideLock(lock, [&] {
        return m_playback.snapshot();
    });
    result.playback.castSession = result.castSession;
    if (m_protocolCurrent)
        result.playback.current = *m_protocolCurrent;
    else if (m_mediaBoundaryPending
             || m_openAwaitingReady
             || m_ignoredPlaybackUris.contains(
                 result.playback.current.uri))
        result.playback.current = {};
    if (m_protocolNext) {
        result.playback.next = *m_protocolNext;
        result.playback.hasNext = true;
    } else {
        result.playback.next = {};
        result.playback.hasNext = false;
    }
    result.playback.hasPrevious = m_protocolPrevious.has_value();
    return result;
}

void DlnaSession::setPeerConnected(
    bool connected, Lock &lock, bool queuePlayback)
{
    if (m_peerConnected == connected)
        return;
    m_peerConnected = connected;
    if (!connected) {
        m_protocolNext.reset();
        m_protocolPrevious.reset();
        m_ignorePlaybackQueue = true;
    }
    if (m_callbacks.log) {
        outsideLock(lock, [&] {
            m_callbacks.log(
                std::string("DLNA peerConnected -> ")
                + (connected ? "true" : "false"));
        });
    }
    if (queuePlayback)
        queuePeerConnected(connected, lock);
    if (m_callbacks.peerConnectedChanged) {
        outsideLock(lock, [&] {
            m_callbacks.peerConnectedChanged(connected);
        });
    }
    // Publish only after observers have seen the connection transition. An
    // in-flight Open may still hold the Gateway submission lock while awaiting
    // its snapshot; delaying this event must not make Pause wait behind it.
    publishCastSession(lock);
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

void DlnaSession::cancelStopIdleGrace()
{
    m_stopIdleDeadline.reset();
    m_stopIdleSettling = false;
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
    m_reconnectAfterEnd = false;
    m_subscriptionEnded = true;
    m_reconnectClientName.clear();
    m_reconnectClientAddress.clear();
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
    const bool mediaCleared =
        m_endOperation && m_endOperation->finalSnapshotSeen;
    const bool reconnectAfterEnd =
        m_reconnectAfterEnd && !m_disconnectedByUs
        && m_subscriptionCount > 0 && mediaCleared;
    std::string reconnectClientName = std::move(m_reconnectClientName);
    std::string reconnectClientAddress =
        std::move(m_reconnectClientAddress);
    m_reconnectAfterEnd = false;
    m_reconnectClientName.clear();
    m_reconnectClientAddress.clear();
    m_endOperation.reset();
    m_clientName.clear();
    m_clientAddress.clear();
    publishCastSession(lock);
    m_protocolCurrent.reset();
    m_protocolNext.reset();
    m_protocolPrevious.reset();
    m_openAwaitingReady.reset();
    m_openCandidates.clear();
    m_setNextCandidates.clear();
    m_earlyCommandEvents.clear();
    m_ignoredPlaybackUris.clear();
    m_ignoredPlaybackNextUris.clear();
    m_mediaBoundaryPending = false;
    m_openInFlight = false;
    m_openCommandId = 0;
    m_openDeadline.reset();
    m_ignorePlaybackQueue = false;
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
    if (reconnectAfterEnd) {
        cancelPeerGrace();
        m_clientName = std::move(reconnectClientName);
        m_clientAddress = std::move(reconnectClientAddress);
        m_subscriptionEnded = false;
        setPeerConnected(true, lock);
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
    m_openAwaitingReady.reset();
    if (!submittedOpen && m_openCommandId != 0) {
        const auto candidate = m_openCandidates.find(m_openCommandId);
        if (candidate != m_openCandidates.end())
            m_ignoredPlaybackUris.insert(
                candidate->second.media.uri);
        m_openCandidates.erase(m_openCommandId);
        m_pendingCommands.erase(m_openCommandId);
        m_ignoredCommandIds.insert(m_openCommandId);
    }
    if (!submittedOpen)
        m_mediaBoundaryPending = false;
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
        consumeEarlyCommand(submission.commandId, lock);
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
