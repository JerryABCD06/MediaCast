// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "cast/CastArbiter.h"

#include <algorithm>
#include <utility>

namespace mcast::cast {
namespace {

constexpr std::size_t kRecentDecisionLimit = 32;

bool isInTransition(ArbitrationPhase phase)
{
    return phase == ArbitrationPhase::Preparing
        || phase == ArbitrationPhase::Handoff;
}

bool hasPendingClaim(ArbitrationPhase phase)
{
    return phase == ArbitrationPhase::ActiveWithPendingClaim
        || phase == ArbitrationPhase::IdleWithPendingClaim;
}

bool sameClaim(const ClaimKey &key, ProtocolKind protocol, std::uint64_t id)
{
    return key.protocol == protocol && key.id == id;
}

ClaimDecision decision(
    ClaimStatus status,
    ClaimReason reason,
    ProtocolKind protocol,
    std::uint64_t claimId,
    const ArbitrationSnapshot &snapshot,
    std::optional<Session> displaced = std::nullopt)
{
    return {
        status,
        reason,
        protocol,
        claimId,
        snapshot.revision,
        snapshot.owner,
        std::move(displaced),
        false,
    };
}

ClaimKey keyOf(const ClaimRequest &request)
{
    return {request.protocol, request.claimId};
}

} // namespace

CastArbiter::CastArbiter(ArbitrationPolicy policy)
    : m_policy(normalized(policy))
{
}

ArbitrationPolicy CastArbiter::normalized(ArbitrationPolicy policy)
{
    if (policy.confirmationTimeout <= CastTime::zero())
        policy.confirmationTimeout = CastTime{10000};
    if (policy.retirementTimeout <= CastTime::zero())
        policy.retirementTimeout = CastTime{5000};
    policy.maxRetiringSessions =
        std::max<std::size_t>(1, policy.maxRetiringSessions);
    return policy;
}

void CastArbiter::setPolicy(ArbitrationPolicy policy)
{
    // A policy change only affects future claims. A claim already waiting for
    // confirmation keeps the deadline and meaning it was accepted with.
    m_policy = normalized(policy);
}

Session CastArbiter::makeSession(
    const ClaimRequest &request, CastTime now)
{
    (void)now;
    ++m_snapshot.lastEpoch;
    return {
        m_nextSessionId++,
        m_snapshot.lastEpoch,
        request.claimId,
        request.protocol,
        request.peer,
        request.mediaTitle,
    };
}

std::optional<ClaimDecision> CastArbiter::cachedDecision(ClaimKey key) const
{
    const auto it = std::find_if(
        m_recentDecisions.begin(),
        m_recentDecisions.end(),
        [&](const CachedDecision &cached) {
            return sameClaim(key, cached.key.protocol, cached.key.id);
        });
    if (it == m_recentDecisions.end())
        return std::nullopt;
    ClaimDecision duplicate = it->decision;
    duplicate.duplicate = true;
    return duplicate;
}

ClaimDecision CastArbiter::rememberDecision(ClaimDecision result)
{
    if (result.claimId != 0
        && result.protocol != ProtocolKind::None) {
        const ClaimKey key{result.protocol, result.claimId};
        const auto existing = std::find_if(
            m_recentDecisions.begin(),
            m_recentDecisions.end(),
            [&](const CachedDecision &cached) {
                return sameClaim(
                    key, cached.key.protocol, cached.key.id);
            });
        if (existing != m_recentDecisions.end()) {
            existing->decision = result;
            return result;
        }

        if (m_recentDecisions.size() >= kRecentDecisionLimit)
            m_recentDecisions.erase(m_recentDecisions.begin());
        m_recentDecisions.push_back({key, result});
    }
    return result;
}

ClaimDecision CastArbiter::busy(
    ClaimReason reason, const ClaimRequest &request) const
{
    return decision(
        ClaimStatus::Busy,
        reason,
        request.protocol,
        request.claimId,
        m_snapshot);
}

ClaimDecision CastArbiter::acceptFirstClaim(
    const ClaimRequest &request, CastTime now)
{
    ++m_snapshot.revision;
    m_snapshot.owner = makeSession(request, now);
    m_snapshot.phase = ArbitrationPhase::Preparing;
    m_snapshot.pendingClaim.reset();
    m_snapshot.lastFailure.reset();
    return decision(
        ClaimStatus::Accepted,
        ClaimReason::FirstClaim,
        request.protocol,
        request.claimId,
        m_snapshot);
}

ClaimDecision CastArbiter::beginHandoff(
    const ClaimRequest &request, CastTime now)
{
    std::optional<Session> displaced;
    if (m_snapshot.owner) {
        displaced = *m_snapshot.owner;
        m_snapshot.retiringSessions.push_back(
            {*displaced, EndReason::Preempted,
             now + m_policy.retirementTimeout});
        while (m_snapshot.retiringSessions.size()
               > m_policy.maxRetiringSessions) {
            m_snapshot.retiringSessions.erase(
                m_snapshot.retiringSessions.begin());
        }
    }

    ++m_snapshot.revision;
    m_snapshot.owner = makeSession(request, now);
    m_snapshot.phase = ArbitrationPhase::Handoff;
    m_snapshot.pendingClaim.reset();
    m_snapshot.lastFailure.reset();
    return decision(
        ClaimStatus::Accepted,
        ClaimReason::Preempted,
        request.protocol,
        request.claimId,
        m_snapshot,
        displaced);
}

ClaimDecision CastArbiter::claim(
    ClaimRequest request, CastTime now)
{
    if (request.claimId == 0
        || request.protocol == ProtocolKind::None) {
        return rememberDecision(decision(
            ClaimStatus::Rejected,
            ClaimReason::InvalidClaim,
            request.protocol,
            request.claimId,
            m_snapshot));
    }

    const ClaimKey key = keyOf(request);
    if (m_snapshot.pendingClaim
        && now >= m_snapshot.pendingClaim->deadline) {
        (void)expirePending(now);
    }
    if (m_snapshot.owner
        && sameClaim(
            key,
            m_snapshot.owner->protocol,
            m_snapshot.owner->claimId)) {
        ClaimDecision duplicate = decision(
            ClaimStatus::Accepted,
            ClaimReason::DuplicateClaim,
            request.protocol,
            request.claimId,
            m_snapshot);
        duplicate.duplicate = true;
        return duplicate;
    }
    if (m_snapshot.pendingClaim
        && sameClaim(
            key,
            m_snapshot.pendingClaim->protocol,
            m_snapshot.pendingClaim->claimId)) {
        ClaimDecision duplicate = decision(
            ClaimStatus::AwaitingConfirmation,
            ClaimReason::DuplicateClaim,
            request.protocol,
            request.claimId,
            m_snapshot);
        duplicate.duplicate = true;
        return duplicate;
    }
    if (const auto cached = cachedDecision(key))
        return *cached;

    if (hasPendingClaim(m_snapshot.phase)) {
        return rememberDecision(
            busy(ClaimReason::PendingClaimExists, request));
    }
    if (isInTransition(m_snapshot.phase)) {
        return rememberDecision(
            busy(ClaimReason::SessionTransitionPending, request));
    }
    if (!m_snapshot.owner) {
        return rememberDecision(acceptFirstClaim(request, now));
    }
    if (!m_policy.allowPreemption) {
        return rememberDecision(
            busy(ClaimReason::ActiveClaimProtected, request));
    }

    if (m_policy.confirmPreemption) {
        ++m_snapshot.revision;
        m_snapshot.phase = ArbitrationPhase::ActiveWithPendingClaim;
        m_snapshot.pendingClaim = PendingClaim{
            request.claimId,
            request.protocol,
            request.peer,
            request.mediaTitle,
            now + m_policy.confirmationTimeout,
        };
        return rememberDecision(decision(
            ClaimStatus::AwaitingConfirmation,
            ClaimReason::ConfirmationRequired,
            request.protocol,
            request.claimId,
            m_snapshot));
    }

    return rememberDecision(beginHandoff(request, now));
}

ClaimDecision CastArbiter::confirmPending(
    ClaimKey key, bool accepted, CastTime now)
{
    if (!m_snapshot.pendingClaim
        || !sameClaim(
            key,
            m_snapshot.pendingClaim->protocol,
            m_snapshot.pendingClaim->claimId)) {
        if (m_snapshot.owner
            && sameClaim(
                key,
                m_snapshot.owner->protocol,
                m_snapshot.owner->claimId)) {
            ClaimDecision duplicate = decision(
                ClaimStatus::Accepted,
                ClaimReason::DuplicateClaim,
                key.protocol,
                key.id,
                m_snapshot);
            duplicate.duplicate = true;
            return duplicate;
        }
        if (const auto previous = cachedDecision(key);
            previous
            && (previous->reason == ClaimReason::ConfirmationAccepted
                || previous->reason == ClaimReason::ConfirmationRejected
                || previous->reason == ClaimReason::ConfirmationTimedOut
                || previous->reason == ClaimReason::DuplicateClaim
                    && (previous->status == ClaimStatus::Accepted
                        || previous->status == ClaimStatus::Rejected))) {
            return *previous;
        }
        return decision(
            ClaimStatus::NotFound,
            ClaimReason::UnknownClaim,
            key.protocol,
            key.id,
            m_snapshot);
    }

    if (now >= m_snapshot.pendingClaim->deadline)
        return expirePending(now);

    const PendingClaim pending = *m_snapshot.pendingClaim;
    m_snapshot.pendingClaim.reset();
    if (!accepted) {
        ++m_snapshot.revision;
        m_snapshot.phase = m_snapshot.owner
            ? ArbitrationPhase::Active
            : ArbitrationPhase::Idle;
        return rememberDecision(decision(
            ClaimStatus::Rejected,
            ClaimReason::ConfirmationRejected,
            pending.protocol,
            pending.claimId,
            m_snapshot));
    }

    ClaimRequest request;
    request.claimId = pending.claimId;
    request.protocol = pending.protocol;
    request.peer = pending.peer;
    request.mediaTitle = pending.mediaTitle;

    ClaimDecision result;
    if (m_snapshot.owner) {
        result = beginHandoff(request, now);
    } else {
        result = acceptFirstClaim(request, now);
    }
    result.reason = ClaimReason::ConfirmationAccepted;
    return rememberDecision(result);
}

ClaimDecision CastArbiter::expirePending(CastTime now)
{
    (void)now;
    const PendingClaim pending = *m_snapshot.pendingClaim;
    m_snapshot.pendingClaim.reset();
    m_snapshot.phase = m_snapshot.owner
        ? ArbitrationPhase::Active
        : ArbitrationPhase::Idle;
    ++m_snapshot.revision;
    return rememberDecision(decision(
        ClaimStatus::Rejected,
        ClaimReason::ConfirmationTimedOut,
        pending.protocol,
        pending.claimId,
        m_snapshot));
}

ArbitrationTick CastArbiter::tick(CastTime now)
{
    ArbitrationTick result;

    const auto expired = std::stable_partition(
        m_snapshot.retiringSessions.begin(),
        m_snapshot.retiringSessions.end(),
        [&](const RetiringSession &session) {
            return now < session.deadline;
        });
    if (expired != m_snapshot.retiringSessions.end()) {
        result.expiredRetirements.assign(
            expired, m_snapshot.retiringSessions.end());
        m_snapshot.retiringSessions.erase(
            expired, m_snapshot.retiringSessions.end());
        ++m_snapshot.revision;
    }

    if (m_snapshot.pendingClaim
        && now >= m_snapshot.pendingClaim->deadline) {
        result.pendingTimeout = expirePending(now);
    }

    result.revision = m_snapshot.revision;
    return result;
}

bool CastArbiter::sessionReady(std::uint64_t sessionId)
{
    if (!m_snapshot.owner || m_snapshot.owner->id != sessionId
        || (m_snapshot.phase != ArbitrationPhase::Preparing
            && m_snapshot.phase != ArbitrationPhase::Handoff)) {
        return false;
    }
    ++m_snapshot.revision;
    m_snapshot.phase = ArbitrationPhase::Active;
    return true;
}

bool CastArbiter::sessionEnded(
    std::uint64_t sessionId, EndReason reason)
{
    if (!m_snapshot.owner || m_snapshot.owner->id != sessionId)
        return false;

    ++m_snapshot.revision;
    m_snapshot.owner.reset();
    if (m_snapshot.pendingClaim) {
        m_snapshot.phase = ArbitrationPhase::IdleWithPendingClaim;
        m_snapshot.lastFailure =
            reason == EndReason::Failed
                || reason == EndReason::ProtocolLost
            ? std::optional<EndReason>{reason}
            : std::nullopt;
    } else if (reason == EndReason::Failed
        || reason == EndReason::ProtocolLost) {
        m_snapshot.phase = ArbitrationPhase::Faulted;
        m_snapshot.lastFailure = reason;
    } else {
        m_snapshot.phase = ArbitrationPhase::Idle;
        m_snapshot.lastFailure.reset();
    }
    return true;
}

bool CastArbiter::sessionRetired(std::uint64_t sessionId)
{
    const auto it = std::find_if(
        m_snapshot.retiringSessions.begin(),
        m_snapshot.retiringSessions.end(),
        [&](const RetiringSession &session) {
            return session.session.id == sessionId;
        });
    if (it == m_snapshot.retiringSessions.end())
        return false;
    m_snapshot.retiringSessions.erase(it);
    ++m_snapshot.revision;
    return true;
}

} // namespace mcast::cast
