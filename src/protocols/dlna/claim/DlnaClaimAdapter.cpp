// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/claim/DlnaClaimAdapter.h"

#include <utility>

namespace mcast::dlna::claim {

DlnaClaimAdapter::DlnaClaimAdapter(
    cast::CastClaimCoordinator &coordinator)
    : m_coordinator(coordinator)
{
}

DlnaClaimOutcome DlnaClaimAdapter::beginMedia(
    cast::PeerIdentity peer, playback::MediaRequest media,
    cast::CastTime now)
{
    DlnaClaimOutcome outcome;
    outcome.invalidatedPending = expirePending(now);
    if (peer.id.empty() || peer.address.empty() || media.uri.empty())
        return outcome;
    if (peer.displayName.empty())
        peer.displayName = peer.address;

    if (m_pending) {
        if (!pendingMatches(peer)) {
            outcome.disposition = DlnaClaimDisposition::Rejected;
            outcome.reason = cast::ClaimReason::PendingClaimExists;
            outcome.transactionId = m_pending->claimId;
            return outcome;
        }

        if (sameMedia(m_pending->media, media)) {
            outcome.disposition = DlnaClaimDisposition::Pending;
            outcome.reason = cast::ClaimReason::DuplicateClaim;
            outcome.transactionId = m_pending->claimId;
            outcome.deadline = m_pending->deadline;
            outcome.media = m_pending->media;
            return outcome;
        }

        // 用户尚未看到的媒体发生变化时，旧确认不能被延用于新媒体。
        // 作废旧候选并重新申请，确保旧 UI decision 不会授权新媒体。
        PendingMedia superseded = std::move(*m_pending);
        const cast::ClaimKey oldKey{
            cast::ProtocolKind::Dlna, superseded.claimId};
        cast::ClaimDecision decision =
            m_coordinator.resolveConfirmation(oldKey, false, now);
        invalidatePending();
        outcome.invalidatedPending = DlnaClaimInvalidation{
            std::move(decision),
            std::move(superseded.media),
        };
    }

    if (ownerMatches(peer)) {
        const cast::Session &owner = *m_coordinator.snapshot().owner;
        const bool duplicate =
            m_ownerMedia && m_ownerMedia->sessionId == owner.id
            && sameMedia(m_ownerMedia->media, media);
        outcome.disposition = DlnaClaimDisposition::Execute;
        outcome.reason = duplicate
            ? cast::ClaimReason::DuplicateClaim
            : cast::ClaimReason::None;
        outcome.media = std::move(media);
        publishOwner(outcome, owner, !duplicate);
        return outcome;
    }

    const std::uint64_t claimId = nextClaimId();
    cast::ClaimStart start = m_coordinator.claim(
        cast::ClaimRequest{
            claimId,
            cast::ProtocolKind::Dlna,
            std::move(peer),
            media.title,
        },
        now);

    outcome.transactionId = claimId;
    outcome.reason = start.decision.reason;
    if (start.decision.status == cast::ClaimStatus::Accepted) {
        outcome.disposition = DlnaClaimDisposition::Execute;
        outcome.media = std::move(media);
        if (start.decision.owner) {
            outcome.claimedSessionId = start.decision.owner->id;
            publishOwner(outcome, *start.decision.owner, true);
        }
        return outcome;
    }
    if (start.decision.status
        == cast::ClaimStatus::AwaitingConfirmation) {
        const auto &pendingClaim =
            m_coordinator.snapshot().pendingClaim;
        if (!pendingClaim) {
            outcome.disposition = DlnaClaimDisposition::Rejected;
            outcome.reason = cast::ClaimReason::InvalidClaim;
            return outcome;
        }
        m_pending = PendingMedia{
            claimId,
            pendingClaim->peer,
            media,
            pendingClaim->deadline,
            false,
        };
        outcome.disposition = DlnaClaimDisposition::Pending;
        outcome.deadline = pendingClaim->deadline;
        outcome.confirmation = std::move(start.confirmation);
        outcome.media = std::move(media);
        return outcome;
    }

    outcome.disposition = DlnaClaimDisposition::Rejected;
    return outcome;
}

DlnaClaimOutcome DlnaClaimAdapter::requestPlay(
    cast::PeerIdentity peer, cast::CastTime now)
{
    DlnaClaimOutcome outcome;
    outcome.invalidatedPending = expirePending(now);
    if (peer.id.empty() || peer.address.empty())
        return outcome;

    if (pendingMatches(peer)) {
        m_pending->playRequested = true;
        outcome.disposition = DlnaClaimDisposition::Pending;
        outcome.reason = cast::ClaimReason::DuplicateClaim;
        outcome.transactionId = m_pending->claimId;
        outcome.deadline = m_pending->deadline;
        outcome.media = m_pending->media;
        return outcome;
    }
    if (canControl(peer)) {
        outcome.disposition = DlnaClaimDisposition::Execute;
        outcome.reason = cast::ClaimReason::None;
        publishOwner(
            outcome, *m_coordinator.snapshot().owner, false);
        return outcome;
    }
    return outcome;
}

DlnaClaimOutcome DlnaClaimAdapter::requestStop(
    cast::PeerIdentity peer, cast::CastTime now)
{
    DlnaClaimOutcome outcome;
    outcome.invalidatedPending = expirePending(now);
    if (peer.id.empty() || peer.address.empty())
        return outcome;

    if (pendingMatches(peer)) {
        outcome.disposition = DlnaClaimDisposition::Ignored;
        outcome.reason = cast::ClaimReason::PendingClaimExists;
        outcome.transactionId = m_pending->claimId;
        outcome.deadline = m_pending->deadline;
        return outcome;
    }
    if (ownerMatches(peer)) {
        outcome.disposition = DlnaClaimDisposition::Execute;
        outcome.reason = cast::ClaimReason::None;
        publishOwner(
            outcome, *m_coordinator.snapshot().owner, false);
        return outcome;
    }

    outcome.disposition = DlnaClaimDisposition::Ignored;
    outcome.reason = cast::ClaimReason::ActiveClaimProtected;
    return outcome;
}

DlnaClaimResolution DlnaClaimAdapter::resolveConfirmation(
    cast::ClaimKey key, bool accepted, cast::CastTime now)
{
    DlnaClaimResolution result;
    result.invalidatedPending = expirePending(now);
    if (result.invalidatedPending
        && result.invalidatedPending->decision.protocol == key.protocol
        && result.invalidatedPending->decision.claimId == key.id) {
        result.decision = result.invalidatedPending->decision;
        return result;
    }

    const bool belongsToPending =
        m_pending && key.protocol == cast::ProtocolKind::Dlna
        && key.id == m_pending->claimId;
    result.decision = m_coordinator.resolveConfirmation(
        key, accepted, now);
    if (belongsToPending) {
        playback::MediaRequest pendingMedia = m_pending->media;
        m_pending.reset();
        result.executeMedia =
            accepted && result.decision.status == cast::ClaimStatus::Accepted
            && !result.decision.duplicate;
        if (result.executeMedia) {
            result.media = std::move(pendingMedia);
            if (result.decision.owner)
                publishOwner(result, *result.decision.owner, true);
        }
    }
    return result;
}

DlnaClaimResolution DlnaClaimAdapter::cancelConfirmation(
    cast::ClaimKey key, cast::CastTime now)
{
    DlnaClaimResolution result;
    result.invalidatedPending = expirePending(now);
    if (result.invalidatedPending
        && result.invalidatedPending->decision.protocol == key.protocol
        && result.invalidatedPending->decision.claimId == key.id) {
        result.decision = result.invalidatedPending->decision;
        return result;
    }
    if (!m_pending || key.protocol != cast::ProtocolKind::Dlna
        || key.id != m_pending->claimId) {
        result.decision = rejected(
            cast::ClaimReason::UnknownClaim,
            m_coordinator.snapshot().revision, key.id);
        return result;
    }
    return resolveConfirmation(key, false, now);
}

DlnaClaimResolution DlnaClaimAdapter::cancelAllPending(
    cast::CastTime now)
{
    DlnaClaimResolution result;
    result.invalidatedPending = expirePending(now);
    if (!m_pending) {
        result.decision = result.invalidatedPending
            ? result.invalidatedPending->decision
            : rejected(
                  cast::ClaimReason::UnknownClaim,
                  m_coordinator.snapshot().revision);
        return result;
    }
    const cast::ClaimKey key{
        cast::ProtocolKind::Dlna, m_pending->claimId};
    return resolveConfirmation(key, false, now);
}

DlnaClaimTick DlnaClaimAdapter::tick(cast::CastTime now)
{
    DlnaClaimTick result;
    result.arbitration = m_coordinator.tick(now);
    if (m_pending && result.arbitration.pendingTimeout
        && result.arbitration.pendingTimeout->protocol
            == cast::ProtocolKind::Dlna
        && result.arbitration.pendingTimeout->claimId
            == m_pending->claimId) {
        result.pendingInvalidation = DlnaClaimInvalidation{
            *result.arbitration.pendingTimeout,
            m_pending->media,
        };
        invalidatePending();
    } else if (m_pending && now >= m_pending->deadline) {
        result.pendingInvalidation = expirePending(now);
    } else if (m_pending && !pendingProjectionMatches()) {
        result.pendingInvalidation = expirePending(now);
    }
    return result;
}

bool DlnaClaimAdapter::sessionReady(std::uint64_t sessionId)
{
    return m_coordinator.sessionReady(sessionId);
}

bool DlnaClaimAdapter::sessionEnded(
    std::uint64_t sessionId, cast::EndReason reason)
{
    return m_coordinator.sessionEnded(sessionId, reason);
}

bool DlnaClaimAdapter::sessionRetired(std::uint64_t sessionId)
{
    return m_coordinator.sessionRetired(sessionId);
}

const cast::ArbitrationSnapshot &
DlnaClaimAdapter::snapshot() const noexcept
{
    return m_coordinator.snapshot();
}

bool DlnaClaimAdapter::ownsPeer(
    const cast::PeerIdentity &peer) const
{
    return ownerMatches(peer);
}

bool DlnaClaimAdapter::canControl(
    const cast::PeerIdentity &peer) const
{
    if (ownerMatches(peer))
        return true;
    const auto &snapshot = m_coordinator.snapshot();
    return !peer.address.empty() && snapshot.owner
        && snapshot.owner->protocol == cast::ProtocolKind::Dlna
        && snapshot.owner->peer.address == peer.address;
}

bool DlnaClaimAdapter::ownsOperation(
    std::uint64_t sessionId, std::uint64_t epoch,
    std::uint64_t mediaGeneration) const
{
    const auto &snapshot = m_coordinator.snapshot();
    return sessionId != 0 && epoch != 0 && mediaGeneration != 0
        && snapshot.owner
        && snapshot.owner->protocol == cast::ProtocolKind::Dlna
        && snapshot.owner->id == sessionId
        && snapshot.owner->epoch == epoch
        && m_mediaGeneration == mediaGeneration;
}

bool DlnaClaimAdapter::sessionNeedsReady(
    std::uint64_t sessionId) const
{
    const auto &snapshot = m_coordinator.snapshot();
    return sessionId != 0 && snapshot.owner
        && snapshot.owner->protocol == cast::ProtocolKind::Dlna
        && snapshot.owner->id == sessionId
        && (snapshot.phase == cast::ArbitrationPhase::Preparing
            || snapshot.phase == cast::ArbitrationPhase::Handoff);
}

std::optional<cast::ClaimKey>
DlnaClaimAdapter::pendingKey() const
{
    if (!m_pending)
        return std::nullopt;
    return cast::ClaimKey{
        cast::ProtocolKind::Dlna, m_pending->claimId};
}

bool DlnaClaimAdapter::ownerMatches(
    const cast::PeerIdentity &peer) const
{
    const auto &snapshot = m_coordinator.snapshot();
    return !peer.id.empty() && !peer.address.empty() && snapshot.owner
        && snapshot.owner->protocol == cast::ProtocolKind::Dlna
        && snapshot.owner->peer.id == peer.id
        && snapshot.owner->peer.address == peer.address;
}

bool DlnaClaimAdapter::pendingMatches(
    const cast::PeerIdentity &peer) const
{
    return !peer.id.empty() && !peer.address.empty() && m_pending
        && m_pending->peer.id == peer.id
        && m_pending->peer.address == peer.address;
}

bool DlnaClaimAdapter::pendingProjectionMatches() const
{
    if (!m_pending)
        return true;
    const auto &pending = m_coordinator.snapshot().pendingClaim;
    return pending
        && pending->protocol == cast::ProtocolKind::Dlna
        && pending->claimId == m_pending->claimId
        && pending->peer.id == m_pending->peer.id
        && pending->peer.address == m_pending->peer.address
        && pending->deadline == m_pending->deadline;
}

std::optional<DlnaClaimInvalidation>
DlnaClaimAdapter::expirePending(cast::CastTime now)
{
    if (!m_pending || (now < m_pending->deadline
                       && pendingProjectionMatches())) {
        return std::nullopt;
    }

    PendingMedia expired = std::move(*m_pending);
    m_pending.reset();
    cast::ClaimDecision decision;
    const auto &pending = m_coordinator.snapshot().pendingClaim;
    if (pending && now >= expired.deadline
        && pending->protocol == cast::ProtocolKind::Dlna
        && pending->claimId == expired.claimId) {
        decision = m_coordinator.resolveConfirmation(
            cast::ClaimKey{
                cast::ProtocolKind::Dlna, expired.claimId},
            false, now);
    } else {
        decision = rejected(
            cast::ClaimReason::InvalidClaim,
            m_coordinator.snapshot().revision, expired.claimId);
    }
    return DlnaClaimInvalidation{
        std::move(decision),
        std::move(expired.media),
    };
}

void DlnaClaimAdapter::publishOwner(
    DlnaClaimOutcome &outcome, const cast::Session &owner,
    bool newMedia)
{
    outcome.ownerSessionId = owner.id;
    outcome.ownerEpoch = owner.epoch;
    if (newMedia) {
        m_ownerMedia = OwnerMedia{
            owner.id,
            outcome.media.value_or(playback::MediaRequest{}),
        };
        outcome.mediaGeneration = ++m_mediaGeneration;
    } else {
        outcome.mediaGeneration = m_mediaGeneration;
    }
}

void DlnaClaimAdapter::publishOwner(
    DlnaClaimResolution &result, const cast::Session &owner,
    bool newMedia)
{
    result.ownerSessionId = owner.id;
    result.ownerEpoch = owner.epoch;
    if (newMedia) {
        m_ownerMedia = OwnerMedia{
            owner.id, result.media.value_or(playback::MediaRequest{})};
        result.mediaGeneration = ++m_mediaGeneration;
    } else {
        result.mediaGeneration = m_mediaGeneration;
    }
}

void DlnaClaimAdapter::invalidatePending()
{
    m_pending.reset();
}

std::uint64_t DlnaClaimAdapter::nextClaimId()
{
    if (m_nextClaimId == 0)
        m_nextClaimId = 1;
    return m_nextClaimId++;
}

cast::ClaimDecision DlnaClaimAdapter::rejected(
    cast::ClaimReason reason, std::uint64_t revision,
    std::uint64_t claimId)
{
    cast::ClaimDecision decision;
    decision.status = cast::ClaimStatus::Rejected;
    decision.reason = reason;
    decision.protocol = cast::ProtocolKind::Dlna;
    decision.claimId = claimId;
    decision.revision = revision;
    return decision;
}

bool DlnaClaimAdapter::sameMedia(
    const playback::MediaRequest &left,
    const playback::MediaRequest &right)
{
    return left.uri == right.uri && left.metadata == right.metadata
        && left.title == right.title
        && left.artist == right.artist
        && left.creator == right.creator
        && left.album == right.album
        && left.description == right.description
        && left.subtitle == right.subtitle
        && left.longDescription == right.longDescription
        && left.artworkUri == right.artworkUri;
}

} // namespace mcast::dlna::claim
