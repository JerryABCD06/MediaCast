// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/claim/DlnaClaimRuntime.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace mcast::dlna::claim {
namespace {

DlnaHttpCompletion completion(
    std::uint64_t requestId, bool success,
    cast::ClaimReason reason)
{
    return DlnaHttpCompletion{requestId, success, reason};
}

void addCompletions(
    DlnaClaimRuntimeActions &actions,
    const std::vector<std::uint64_t> &requests, bool success,
    cast::ClaimReason reason)
{
    for (std::uint64_t requestId : requests)
        actions.http.push_back(completion(requestId, success, reason));
}

} // namespace

DlnaClaimRuntime::DlnaClaimRuntime(DlnaClaimAdapter &adapter)
    : m_adapter(adapter)
{
}

DlnaClaimRuntimeActions DlnaClaimRuntime::beginSetUri(
    std::uint64_t requestId, cast::PeerIdentity peer,
    playback::MediaRequest media, cast::CastTime now)
{
    DlnaClaimRuntimeActions invalid;
    if (requestId == 0 || requestTracked(requestId)) {
        invalid.http.push_back(completion(
            requestId, false, cast::ClaimReason::InvalidClaim));
        return invalid;
    }

    const cast::PeerIdentity requestPeer = peer;
    DlnaClaimOutcome outcome = m_adapter.beginMedia(
        std::move(peer), std::move(media), now);

    DlnaClaimRuntimeActions actions;
    if (outcome.invalidatedPending) {
        completePending(
            actions, outcome.invalidatedPending->decision);
    }

    if (outcome.disposition == DlnaClaimDisposition::Execute) {
        if (outcome.reason == cast::ClaimReason::DuplicateClaim) {
            actions.http.push_back(completion(
                requestId, true, outcome.reason));
            return actions;
        }
        const cast::ClaimReason reason = outcome.reason;
        DlnaClaimRuntimeActions executed =
            beginExecute(std::move(outcome), now);
        if (m_operation) {
            m_operation->setRequests.push_back(requestId);
        } else {
            actions.http.push_back(completion(
                requestId, false, reason));
        }
        std::move(
            executed.http.begin(), executed.http.end(),
            std::back_inserter(actions.http));
        actions.confirmation = std::move(executed.confirmation);
        actions.media = std::move(executed.media);
        actions.playNow = executed.playNow;
        return actions;
    }

    if (outcome.disposition == DlnaClaimDisposition::Pending) {
        const cast::ClaimKey key{
            cast::ProtocolKind::Dlna, outcome.transactionId};
        if (!m_pending || m_pending->key.id != key.id)
            m_pending = PendingHttp{key, requestPeer, {}, {}};
        m_pending->setRequests.push_back(requestId);
        actions.confirmation = std::move(outcome.confirmation);
        return actions;
    }

    actions.http.push_back(completion(
        requestId, false, outcome.reason));
    return actions;
}

DlnaClaimRuntimeActions DlnaClaimRuntime::attachPlay(
    std::uint64_t requestId, cast::PeerIdentity peer,
    cast::CastTime now)
{
    if (requestId == 0 || requestTracked(requestId)) {
        DlnaClaimRuntimeActions invalid;
        invalid.http.push_back(completion(
            requestId, false, cast::ClaimReason::InvalidClaim));
        return invalid;
    }

    DlnaClaimRuntimeActions actions;
    const cast::PeerIdentity requestPeer = peer;
    DlnaClaimOutcome outcome =
        m_adapter.requestPlay(std::move(peer), now);
    if (outcome.invalidatedPending) {
        completePending(
            actions, outcome.invalidatedPending->decision);
    }

    if (outcome.disposition == DlnaClaimDisposition::Pending) {
        if (!m_pending)
            m_pending = PendingHttp{
                {cast::ProtocolKind::Dlna, outcome.transactionId},
                requestPeer,
                {},
                {},
            };
        if (requestId != 0)
            m_pending->playRequests.push_back(requestId);
        return actions;
    }

    if (outcome.disposition == DlnaClaimDisposition::Execute) {
        if (m_operation
            && outcome.ownerSessionId
            && m_operation->sessionId == *outcome.ownerSessionId) {
            m_operation->playRequested = true;
            if (requestId != 0)
                m_operation->playRequests.push_back(requestId);
            return actions;
        }
        if (requestId != 0) {
            actions.http.push_back(completion(
                requestId, true, cast::ClaimReason::None));
        }
        actions.playNow = true;
        return actions;
    }

    if (requestId != 0) {
        actions.http.push_back(completion(
            requestId, false, outcome.reason));
    }
    return actions;
}

DlnaClaimRuntimeActions DlnaClaimRuntime::requestStop(
    std::uint64_t requestId, cast::PeerIdentity peer,
    cast::CastTime now)
{
    DlnaClaimRuntimeActions actions;
    if (requestId == 0 || requestTracked(requestId)) {
        actions.http.push_back(completion(
            requestId, false, cast::ClaimReason::InvalidClaim));
        return actions;
    }

    if (m_operation && m_adapter.ownsPeer(peer)) {
        actions.http.push_back(completion(
            requestId, false, cast::ClaimReason::SessionTransitionPending));
        return actions;
    }

    DlnaClaimOutcome outcome =
        m_adapter.requestStop(std::move(peer), now);
    if (outcome.invalidatedPending) {
        completePending(
            actions, outcome.invalidatedPending->decision);
    }
    if (outcome.disposition == DlnaClaimDisposition::Execute) {
        actions.stopNow = true;
        actions.http.push_back(completion(
            requestId, true, cast::ClaimReason::None));
    } else if (outcome.disposition == DlnaClaimDisposition::Ignored) {
        actions.http.push_back(completion(
            requestId, true, outcome.reason));
    } else {
        actions.http.push_back(completion(
            requestId, false, outcome.reason));
    }
    return actions;
}

DlnaClaimDisposition DlnaClaimRuntime::requestControl(
    cast::PeerIdentity peer, cast::CastTime now)
{
    static_cast<void>(now);
    if (m_adapter.ownsPeer(peer)) {
        if (m_operation)
            return DlnaClaimDisposition::Pending;
        return DlnaClaimDisposition::Execute;
    }
    if (m_pending
        && m_pending->peer.id == peer.id
        && m_pending->peer.address == peer.address)
        return DlnaClaimDisposition::Pending;
    return DlnaClaimDisposition::Rejected;
}

bool DlnaClaimRuntime::ownsPeer(
    const cast::PeerIdentity &peer) const
{
    return m_adapter.ownsPeer(peer);
}

DlnaClaimRuntimeActions DlnaClaimRuntime::confirmationDecision(
    cast::ClaimKey key, bool accepted, cast::CastTime now)
{
    DlnaClaimRuntimeActions actions;
    DlnaClaimResolution resolution =
        m_adapter.resolveConfirmation(key, accepted, now);
    if (resolution.invalidatedPending) {
        completePending(
            actions, resolution.invalidatedPending->decision);
    }

    if (resolution.executeMedia) {
        DlnaClaimRuntimeActions resolved =
            beginResolution(std::move(resolution), now);
        std::move(
            resolved.http.begin(), resolved.http.end(),
            std::back_inserter(actions.http));
        actions.media = std::move(resolved.media);
        return actions;
    }
    if (!accepted || resolution.decision.status
            != cast::ClaimStatus::Accepted) {
        completePending(actions, resolution.decision);
    }
    return actions;
}

DlnaClaimRuntimeActions DlnaClaimRuntime::tick(cast::CastTime now)
{
    DlnaClaimRuntimeActions actions;
    if (m_operation && now >= m_operation->deadline) {
        Operation operation = std::move(*m_operation);
        m_operation.reset();
        static_cast<void>(m_adapter.sessionEnded(
            operation.sessionId, cast::EndReason::Failed));
        completeOperation(
            actions, operation, false,
            cast::ClaimReason::SessionTransitionPending);
    }
    DlnaClaimTick ticked = m_adapter.tick(now);
    if (ticked.pendingInvalidation) {
        completePending(
            actions, ticked.pendingInvalidation->decision);
    }
    return actions;
}

DlnaClaimRuntimeActions DlnaClaimRuntime::mediaReady(
    std::uint64_t operationId, bool success,
    cast::CastTime now)
{
    DlnaClaimRuntimeActions actions;
    if (!m_operation || m_operation->id != operationId
        || operationId == 0) {
        actions.staleOperation = true;
        return actions;
    }

    Operation operation = std::move(*m_operation);
    m_operation.reset();
    if (success) {
        if (now >= operation.deadline) {
            static_cast<void>(m_adapter.sessionEnded(
                operation.sessionId, cast::EndReason::Failed));
            completeOperation(
                actions, operation, false,
                cast::ClaimReason::SessionTransitionPending);
            return actions;
        }
        if ((operation.newSession
             && !m_adapter.sessionReady(operation.sessionId))
            || !m_adapter.ownsOperation(
                operation.sessionId, operation.epoch,
                operation.mediaGeneration)) {
            static_cast<void>(m_adapter.sessionEnded(
                operation.sessionId, cast::EndReason::Failed));
            completeOperation(
                actions, operation, false,
                cast::ClaimReason::InvalidClaim);
            return actions;
        }
        completeOperation(
            actions, operation, true, cast::ClaimReason::None);
        actions.playNow = operation.playRequested;
    } else {
        static_cast<void>(m_adapter.sessionEnded(
            operation.sessionId, cast::EndReason::Failed));
        completeOperation(
            actions, operation, false, cast::ClaimReason::InvalidClaim);
    }
    static_cast<void>(now);
    return actions;
}

DlnaClaimRuntimeActions DlnaClaimRuntime::controlLost(
    cast::CastTime now)
{
    DlnaClaimRuntimeActions actions;
    DlnaClaimResolution cancelled = m_adapter.cancelAllPending(now);
    if (m_pending) {
        completePending(
            actions, m_pending->key.id
                ? cast::ClaimDecision{
                      cast::ClaimStatus::Rejected,
                      cast::ClaimReason::ConfirmationRejected,
                      cast::ProtocolKind::Dlna,
                      m_pending->key.id,
                      m_adapter.snapshot().revision,
                  }
                : cancelled.decision);
    } else if (cancelled.decision.claimId != 0) {
        completePending(actions, cancelled.decision);
    }

    if (m_operation) {
        Operation operation = std::move(*m_operation);
        m_operation.reset();
        static_cast<void>(m_adapter.sessionEnded(
            operation.sessionId, cast::EndReason::ProtocolLost));
        completeOperation(
            actions, operation, false,
            cast::ClaimReason::InvalidClaim);
    }
    const auto &snapshot = m_adapter.snapshot();
    if (snapshot.owner
        && snapshot.owner->protocol == cast::ProtocolKind::Dlna) {
        static_cast<void>(m_adapter.sessionEnded(
            snapshot.owner->id, cast::EndReason::ProtocolLost));
    }
    return actions;
}

std::optional<std::uint64_t>
DlnaClaimRuntime::operationId() const noexcept
{
    return m_operation
        ? std::optional<std::uint64_t>(m_operation->id)
        : std::nullopt;
}

std::optional<cast::ClaimKey>
DlnaClaimRuntime::pendingKey() const noexcept
{
    return m_pending
        ? std::optional<cast::ClaimKey>(m_pending->key)
        : std::nullopt;
}

DlnaClaimRuntimeActions DlnaClaimRuntime::beginExecute(
    DlnaClaimOutcome &&outcome, cast::CastTime now)
{
    DlnaClaimRuntimeActions actions;
    if (!outcome.media || !outcome.ownerSessionId) {
        return actions;
    }

    replaceOperation(actions, cast::ClaimDecision{
        cast::ClaimStatus::Rejected,
        cast::ClaimReason::InvalidClaim,
        cast::ProtocolKind::Dlna,
        outcome.transactionId,
        m_adapter.snapshot().revision,
    });

    const bool newSession =
        outcome.claimedSessionId.has_value()
        || m_adapter.sessionNeedsReady(*outcome.ownerSessionId);
    m_operation = Operation{
        nextOperationId(),
        *outcome.ownerSessionId,
        outcome.ownerEpoch,
        outcome.mediaGeneration,
        newSession,
        false,
        now + kOpenTimeout,
        *outcome.media,
        {},
        {},
    };
    actions.media = DlnaMediaExecution{
        m_operation->id,
        m_operation->sessionId,
        m_operation->epoch,
        m_operation->mediaGeneration,
        m_operation->newSession,
        false,
        m_operation->media,
    };
    return actions;
}

DlnaClaimRuntimeActions DlnaClaimRuntime::beginResolution(
    DlnaClaimResolution &&resolution, cast::CastTime now)
{
    DlnaClaimRuntimeActions actions;
    if (!resolution.media || !resolution.ownerSessionId) {
        completePending(actions, resolution.decision);
        return actions;
    }

    PendingHttp pending = m_pending
        ? std::move(*m_pending)
        : PendingHttp{};
    m_pending.reset();
    replaceOperation(actions, resolution.decision);
    const bool newSession =
        !resolution.decision.duplicate
        || m_adapter.sessionNeedsReady(
            *resolution.ownerSessionId);
    m_operation = Operation{
        nextOperationId(),
        *resolution.ownerSessionId,
        resolution.ownerEpoch,
        resolution.mediaGeneration,
        newSession,
        !pending.playRequests.empty(),
        now + kOpenTimeout,
        *resolution.media,
        std::move(pending.setRequests),
        std::move(pending.playRequests),
    };
    actions.media = DlnaMediaExecution{
        m_operation->id,
        m_operation->sessionId,
        m_operation->epoch,
        m_operation->mediaGeneration,
        m_operation->newSession,
        m_operation->playRequested,
        m_operation->media,
    };
    return actions;
}

void DlnaClaimRuntime::completePending(
    DlnaClaimRuntimeActions &actions,
    const cast::ClaimDecision &decision)
{
    if (!m_pending)
        return;
    if (decision.claimId != 0
        && m_pending->key.id != decision.claimId) {
        return;
    }
    PendingHttp pending = std::move(*m_pending);
    m_pending.reset();
    addCompletions(
        actions, pending.setRequests, false, decision.reason);
    addCompletions(
        actions, pending.playRequests, false, decision.reason);
}

void DlnaClaimRuntime::completeOperation(
    DlnaClaimRuntimeActions &actions,
    const Operation &operation, bool success,
    cast::ClaimReason reason)
{
    addCompletions(
        actions, operation.setRequests, success, reason);
    addCompletions(
        actions, operation.playRequests, success, reason);
}

void DlnaClaimRuntime::replaceOperation(
    DlnaClaimRuntimeActions &actions,
    const cast::ClaimDecision &decision)
{
    if (!m_operation)
        return;
    Operation operation = std::move(*m_operation);
    m_operation.reset();
    completeOperation(
        actions, operation, false, decision.reason);
}

bool DlnaClaimRuntime::requestTracked(
    std::uint64_t requestId) const
{
    if (requestId == 0)
        return false;
    const auto contains = [requestId](
                              const std::vector<std::uint64_t> &ids) {
        return std::find(ids.begin(), ids.end(), requestId)
            != ids.end();
    };
    if (m_pending
        && (contains(m_pending->setRequests)
            || contains(m_pending->playRequests))) {
        return true;
    }
    return m_operation
        && (contains(m_operation->setRequests)
            || contains(m_operation->playRequests));
}

std::uint64_t DlnaClaimRuntime::nextOperationId()
{
    if (m_nextOperationId == 0)
        m_nextOperationId = 1;
    return m_nextOperationId++;
}

} // namespace mcast::dlna::claim
