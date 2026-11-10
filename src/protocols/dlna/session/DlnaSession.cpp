// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/session/DlnaSession.h"

#include <utility>

namespace mcast::dlna {

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
    if (m_accepting && m_running)
        return true;
    if (!m_callbacks.startNetwork || !m_callbacks.startNetwork()) {
        if (m_callbacks.networkStateChanged)
            m_callbacks.networkStateChanged(false);
        if (m_callbacks.log)
            m_callbacks.log("DLNA network failed to start");
        return false;
    }
    m_accepting = true;
    m_running = true;
    if (m_callbacks.networkStateChanged)
        m_callbacks.networkStateChanged(true);
    return true;
}

void DlnaSession::pauseAccepting(SessionTime now)
{
    if (!m_accepting || m_endOperation)
        return;

    cancelPendingOpen("DLNA receiving paused");
    m_disconnectedByUs = true;
    m_queuePeerDisconnect = true;
    setPeerConnected(false, false);
    if (m_playback.snapshot().state == PlaybackState::NoMedia) {
        cancelPeerGrace();
        finishPause();
        return;
    }
    requestEnd(EndOperationMode::Pause, now);
}

bool DlnaSession::resumeAccepting()
{
    if (m_accepting)
    {
        if (m_endOperation
            && m_endOperation->mode == EndOperationMode::Pause) {
            if (m_callbacks.log)
                m_callbacks.log("Cannot resume while pause is completing");
            return false;
        }
        return true;
    }
    if (!m_callbacks.startNetwork || !m_callbacks.startNetwork()) {
        if (m_callbacks.networkStateChanged)
            m_callbacks.networkStateChanged(false);
        if (m_callbacks.log)
            m_callbacks.log("DLNA network failed to resume");
        return false;
    }
    m_accepting = true;
    m_running = true;
    if (m_callbacks.networkStateChanged)
        m_callbacks.networkStateChanged(true);
    if (m_callbacks.log)
        m_callbacks.log("DLNA receiving resumed");
    return true;
}

void DlnaSession::setSubscriptionCount(
    std::size_t count, SubscriptionChangeReason reason, SessionTime now)
{
    m_subscriptionCount = count;
    if (m_endOperation)
        return;
    if (reason == SubscriptionChangeReason::Added
        || reason == SubscriptionChangeReason::Renewed) {
        m_disconnectedByUs = false;
    }

    if (count > 0 && !m_disconnectedByUs) {
        cancelPeerGrace();
        setPeerConnected(true);
    } else if (
        count == 0
        && reason == SubscriptionChangeReason::Unreachable
        && m_peerConnected) {
        startPeerGrace(now);
    } else if (
        count == 0
        && (reason == SubscriptionChangeReason::ExplicitRemoval
            || reason == SubscriptionChangeReason::Expired
            || reason == SubscriptionChangeReason::Count)) {
        cancelPeerGrace();
        setPeerConnected(false);
    }
}

void DlnaSession::peerActivity(SessionTime now)
{
    if (m_endOperation)
        return;
    if (m_disconnectedByUs) {
        m_disconnectedByUs = false;
        if (m_subscriptionCount > 0) {
            cancelPeerGrace();
            setPeerConnected(true);
        }
    } else if (m_subscriptionCount > 0) {
        cancelPeerGrace();
        setPeerConnected(true);
    }
    (void)now;
}

void DlnaSession::tick(SessionTime now)
{
    if (m_endOperation && now >= m_endOperation->deadline) {
        if (m_callbacks.log)
            m_callbacks.log("DLNA session-end confirmation timed out");
        if (m_endOperation->mode == EndOperationMode::Pause)
            finishPause();
        else
            finishDisconnect();
        return;
    }
    if (m_openDeadline && now >= *m_openDeadline) {
        m_pendingOpen.reset();
        m_openDeadline.reset();
        if (m_callbacks.requestFailed)
            m_callbacks.requestFailed("Playback did not become ready");
    }
    if (!m_peerGraceDeadline || now < *m_peerGraceDeadline)
        return;
    m_peerGraceDeadline.reset();
    setPeerConnected(false);
}

PlaybackCommandSubmission DlnaSession::openMedia(
    const MediaRequest &request, SessionTime now)
{
    if (m_endOperation) {
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                m_playback.snapshot().generation,
                "DLNA session is ending",
                m_playback.snapshot().stateVersion};
    }
    if (request.uri.empty()) {
        PlaybackCommand command;
        command.type = PlaybackCommandType::Open;
        command.media = request;
        return submit(std::move(command));
    }
    if (m_callbacks.requestUiActivation)
        m_callbacks.requestUiActivation();
    PlaybackCommand command;
    command.type = PlaybackCommandType::Open;
    command.media = request;
    auto submission = submit(command);
    if (submission.queued)
        return submission;
    if (submission.status == PlaybackSubmissionStatus::PlayerUnavailable
        || submission.status
            == PlaybackSubmissionStatus::StaleGeneration) {
        m_pendingOpen = request;
        m_openDeadline = now + kOpenTimeout;
        return {true, PlaybackSubmissionStatus::WaitingForPlayer, 0,
                m_playback.snapshot().generation,
                "waiting for playback readiness",
                m_playback.snapshot().stateVersion};
    }
    if (m_callbacks.requestFailed)
        m_callbacks.requestFailed(submission.error);
    return submission;
}

PlaybackCommandSubmission DlnaSession::submit(PlaybackCommand command)
{
    if (m_endOperation
        && command.type != PlaybackCommandType::EndSession) {
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                m_playback.snapshot().generation,
                "DLNA session is ending",
                m_playback.snapshot().stateVersion};
    }
    if (command.expectedGeneration == 0)
        command.expectedGeneration = m_playback.snapshot().generation;
    return m_playback.submit(command);
}

PlaybackCommandSubmission DlnaSession::endSession(SessionTime now)
{
    if (m_endOperation) {
        return {false, PlaybackSubmissionStatus::SessionEnding, 0,
                m_playback.snapshot().generation,
                "session end is already pending",
                m_playback.snapshot().stateVersion};
    }
    cancelPendingOpen("DLNA session ended");
    m_disconnectedByUs = true;
    m_queuePeerDisconnect = true;
    cancelPeerGrace();
    setPeerConnected(false, false);
    return requestEnd(EndOperationMode::Disconnect, now);
}

void DlnaSession::handlePlaybackEvent(const PlaybackEvent &event)
{
    if (event.kind == PlaybackEventKind::CommandCompleted
        && m_endOperation
        && event.commandId == m_endOperation->commandId) {
        m_endOperation->commandCompleted = true;
    }
    if ((event.kind == PlaybackEventKind::Reconnected
         || event.kind == PlaybackEventKind::SnapshotChanged)
        && m_pendingOpen
        && event.snapshot.playerAvailable) {
        tryPendingOpen();
    }
    if (event.kind == PlaybackEventKind::SnapshotChanged
        || event.kind == PlaybackEventKind::Reconnected
        || event.kind == PlaybackEventKind::PlayerLost) {
        if (m_callbacks.stateChanged)
            m_callbacks.stateChanged();
        if (m_endOperation
            && event.snapshot.state == PlaybackState::NoMedia) {
            m_endOperation->finalSnapshotSeen = true;
            maybeFinishEnd();
        }
    }
}

DlnaSessionSnapshot DlnaSession::snapshot() const
{
    DlnaSessionSnapshot result;
    result.accepting = m_accepting;
    result.running = m_running;
    result.peerConnected = m_peerConnected;
    result.disconnectedByUs = m_disconnectedByUs;
    result.subscriptionCount = m_subscriptionCount;
    result.peerGraceActive = m_peerGraceDeadline.has_value();
    if (m_peerGraceDeadline)
        result.peerGraceDeadline = *m_peerGraceDeadline;
    result.playback = m_playback.snapshot();
    return result;
}

void DlnaSession::setPeerConnected(bool connected, bool queuePlayback)
{
    if (m_peerConnected == connected)
        return;
    m_peerConnected = connected;
    if (queuePlayback)
        queuePeerConnected(connected);
    if (m_callbacks.peerConnectedChanged)
        m_callbacks.peerConnectedChanged(connected);
}

void DlnaSession::queuePeerConnected(bool connected)
{
    PlaybackCommand command;
    command.type = PlaybackCommandType::SetPeerConnected;
    command.boolValue = connected;
    command.expectedGeneration = m_playback.snapshot().generation;
    m_playback.submit(std::move(command));
}

void DlnaSession::startPeerGrace(SessionTime now)
{
    if (m_peerGraceDeadline)
        return;
    m_peerGraceDeadline = now + kPeerGrace;
    if (m_callbacks.log)
        m_callbacks.log("DLNA peer disappeared; waiting out grace period");
}

void DlnaSession::cancelPeerGrace()
{
    m_peerGraceDeadline.reset();
}

void DlnaSession::finishPause()
{
    if (!m_accepting && !m_running)
        return;
    m_endOperation.reset();
    m_accepting = false;
    m_running = false;
    m_disconnectedByUs = true;
    cancelPeerGrace();
    if (m_queuePeerDisconnect) {
        m_queuePeerDisconnect = false;
        queuePeerConnected(false);
    }
    if (m_callbacks.announceGoingAway)
        m_callbacks.announceGoingAway();
    if (m_callbacks.stopNetwork)
        m_callbacks.stopNetwork();
    if (m_callbacks.networkStateChanged)
        m_callbacks.networkStateChanged(false);
    if (m_callbacks.log)
        m_callbacks.log("DLNA receiving paused");
}

void DlnaSession::finishDisconnect()
{
    m_endOperation.reset();
    if (m_queuePeerDisconnect) {
        m_queuePeerDisconnect = false;
        queuePeerConnected(false);
    }
    if (m_callbacks.announceGoingAway)
        m_callbacks.announceGoingAway();
    if (m_callbacks.log)
        m_callbacks.log("DLNA session ended");
}

PlaybackCommandSubmission DlnaSession::requestEnd(
    EndOperationMode mode, SessionTime now)
{
    PlaybackCommand command;
    command.type = PlaybackCommandType::EndSession;
    command.expectedGeneration = m_playback.snapshot().generation;
    const auto submission = m_playback.submit(std::move(command));
    if (!submission.queued) {
        if (mode == EndOperationMode::Pause)
            finishPause();
        else
            finishDisconnect();
        return submission;
    }
    m_endOperation = EndOperation{
        mode, submission.commandId, now + SessionTime{5000}, false, false};
    return submission;
}

void DlnaSession::maybeFinishEnd()
{
    if (!m_endOperation)
        return;
    if (!m_endOperation->commandCompleted
        || !m_endOperation->finalSnapshotSeen)
        return;
    if (m_endOperation->mode == EndOperationMode::Pause)
        finishPause();
    else
        finishDisconnect();
}

void DlnaSession::cancelPendingOpen(const std::string &reason)
{
    if (!m_pendingOpen)
        return;
    m_pendingOpen.reset();
    m_openDeadline.reset();
    if (m_callbacks.requestFailed)
        m_callbacks.requestFailed(reason);
}

void DlnaSession::tryPendingOpen()
{
    if (!m_pendingOpen || !m_playback.snapshot().playerAvailable)
        return;
    PlaybackCommand command;
    command.type = PlaybackCommandType::Open;
    command.media = *m_pendingOpen;
    const auto submission = submit(std::move(command));
    if (submission.queued) {
        m_pendingOpen.reset();
        m_openDeadline.reset();
        if (m_callbacks.log)
            m_callbacks.log("Pending DLNA open submitted after reconnect");
        return;
    }
    if (submission.status == PlaybackSubmissionStatus::InvalidCommand) {
        m_pendingOpen.reset();
        m_openDeadline.reset();
        if (m_callbacks.requestFailed)
            m_callbacks.requestFailed(submission.error);
    }
}

} // namespace mcast::dlna
