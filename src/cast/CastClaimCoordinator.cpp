// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "cast/CastClaimCoordinator.h"

#include <algorithm>
#include <utility>

namespace mcast::cast {

CastClaimCoordinator::CastClaimCoordinator(ArbitrationPolicy policy)
    : m_arbiter(policy)
{
}

ClaimStart CastClaimCoordinator::claim(
    ClaimRequest request, CastTime now)
{
    const ClaimKey key{request.protocol, request.claimId};
    ClaimStart result;
    result.decision = m_arbiter.claim(std::move(request), now);
    if (result.decision.status != ClaimStatus::AwaitingConfirmation
        || result.decision.reason != ClaimReason::ConfirmationRequired
        || result.decision.duplicate) {
        return result;
    }

    const ArbitrationSnapshot &snapshot = m_arbiter.snapshot();
    if (!snapshot.pendingClaim)
        return result;
    result.confirmation = ConfirmationPrompt{
        key,
        snapshot.pendingClaim->peer,
        snapshot.pendingClaim->mediaTitle,
        snapshot.pendingClaim->deadline,
        std::max(
            CastTime::zero(), snapshot.pendingClaim->deadline - now),
    };
    return result;
}

ClaimDecision CastClaimCoordinator::resolveConfirmation(
    ClaimKey key, bool accepted, CastTime now)
{
    return m_arbiter.confirmPending(key, accepted, now);
}

ArbitrationTick CastClaimCoordinator::tick(CastTime now)
{
    return m_arbiter.tick(now);
}

bool CastClaimCoordinator::sessionReady(std::uint64_t sessionId)
{
    return m_arbiter.sessionReady(sessionId);
}

bool CastClaimCoordinator::sessionEnded(
    std::uint64_t sessionId, EndReason reason)
{
    return m_arbiter.sessionEnded(sessionId, reason);
}

bool CastClaimCoordinator::sessionRetired(std::uint64_t sessionId)
{
    return m_arbiter.sessionRetired(sessionId);
}

void CastClaimCoordinator::setPolicy(ArbitrationPolicy policy)
{
    m_arbiter.setPolicy(policy);
}

const ArbitrationPolicy &
CastClaimCoordinator::policy() const noexcept
{
    return m_arbiter.policy();
}

const ArbitrationSnapshot &
CastClaimCoordinator::snapshot() const noexcept
{
    return m_arbiter.snapshot();
}

} // namespace mcast::cast
