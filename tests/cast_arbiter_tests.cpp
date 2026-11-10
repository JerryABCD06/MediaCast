// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "cast/CastArbiter.h"
#include "cast/CastClaimCoordinator.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace mcast::cast;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ClaimRequest request(
    std::uint64_t claimId,
    ProtocolKind protocol,
    const std::string &peer)
{
    return {
        claimId,
        protocol,
        {peer, peer, "192.0.2.1"},
        "Test media",
    };
}

std::uint64_t acceptedSession(const ClaimDecision &decision)
{
    check(decision.status == ClaimStatus::Accepted,
          "expected accepted claim");
    check(decision.owner.has_value(), "accepted claim has an owner");
    return decision.owner->id;
}

void testFirstClaimAndReady()
{
    CastArbiter arbiter;
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(first.status == ClaimStatus::Accepted
              && first.reason == ClaimReason::FirstClaim,
          "first claim accepted");
    check(first.owner->epoch == 1 && first.owner->protocol == ProtocolKind::Dlna,
          "first owner has epoch and protocol");
    check(arbiter.snapshot().phase == ArbitrationPhase::Preparing,
          "first owner starts preparing");

    check(arbiter.sessionReady(first.owner->id),
          "owner session becomes ready");
    check(arbiter.snapshot().phase == ArbitrationPhase::Active,
          "ready owner becomes active");
    check(!arbiter.sessionReady(first.owner->id),
          "ready transition is not repeated");
}

void testProtectedActiveClaim()
{
    CastArbiter arbiter;
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(arbiter.sessionReady(first.owner->id),
          "protected owner becomes ready");
    const auto oldSnapshot = arbiter.snapshot();

    const auto second = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(second.status == ClaimStatus::Busy
              && second.reason == ClaimReason::ActiveClaimProtected,
          "new claim is busy when preemption is disabled");
    check(arbiter.snapshot().owner->id == oldSnapshot.owner->id
              && arbiter.snapshot().lastEpoch == oldSnapshot.lastEpoch
              && arbiter.snapshot().revision == oldSnapshot.revision,
          "busy claim does not mutate ownership");
}

void testImmediatePreemption()
{
    CastArbiter arbiter({
        true,
        false,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t firstId = acceptedSession(first);
    check(arbiter.sessionReady(firstId), "DLNA owner becomes ready");

    const auto second = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{2});
    check(second.status == ClaimStatus::Accepted
              && second.reason == ClaimReason::Preempted,
          "new protocol preempts immediately");
    check(second.owner->protocol == ProtocolKind::AirPlay
              && second.owner->epoch == 2,
          "new owner gets the next epoch");
    check(second.displaced && second.displaced->id == firstId,
          "old owner is reported as displaced");
    check(arbiter.snapshot().phase == ArbitrationPhase::Handoff,
          "immediate preemption enters handoff");
    check(arbiter.snapshot().retiringSessions.size() == 1
              && arbiter.snapshot().retiringSessions[0].session.id == firstId
              && arbiter.snapshot().retiringSessions[0].reason
                  == EndReason::Preempted,
          "old owner is recorded as retiring");

    check(arbiter.sessionReady(second.owner->id),
          "new owner becomes ready after handoff");
    check(arbiter.snapshot().phase == ArbitrationPhase::Active,
          "handoff completes as active");
    check(!arbiter.sessionEnded(firstId, EndReason::UserDisconnected),
          "late old-session end is ignored");
    check(arbiter.sessionRetired(firstId),
          "old protocol reports farewell completion");
    check(arbiter.snapshot().retiringSessions.empty(),
          "completed farewell leaves no retiring session");
}

void testConfirmationKeepsOldOwner()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t oldId = acceptedSession(first);
    check(arbiter.sessionReady(oldId), "old owner becomes ready");
    const std::uint64_t epochBefore = arbiter.snapshot().lastEpoch;

    const auto pending = arbiter.claim(
        request(2, ProtocolKind::GoogleCast, "phone-b"), CastTime{1});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "cross-protocol claim waits for confirmation");
    check(arbiter.snapshot().owner->id == oldId
              && arbiter.snapshot().lastEpoch == epochBefore,
          "pending confirmation keeps the old owner and epoch");
    check(arbiter.snapshot().phase
              == ArbitrationPhase::ActiveWithPendingClaim,
          "active owner exposes a pending claim");
    check(arbiter.snapshot().pendingClaim
              && arbiter.snapshot().pendingClaim->protocol
                  == ProtocolKind::GoogleCast,
          "pending claim contains the candidate protocol");

    const auto extra = arbiter.claim(
        request(3, ProtocolKind::AirPlay, "phone-c"), CastTime{2});
    check(extra.status == ClaimStatus::Busy
              && extra.reason == ClaimReason::PendingClaimExists,
          "only one claim may await confirmation");

    const auto rejected = arbiter.confirmPending(
        {ProtocolKind::GoogleCast, 2}, false, CastTime{3});
    check(rejected.status == ClaimStatus::Rejected
              && rejected.reason == ClaimReason::ConfirmationRejected,
          "rejected confirmation reports rejection");
    check(arbiter.snapshot().owner->id == oldId
              && arbiter.snapshot().lastEpoch == epochBefore
              && !arbiter.snapshot().pendingClaim
              && arbiter.snapshot().phase == ArbitrationPhase::Active,
          "rejection leaves the old session untouched");

    const auto repeatedRejection = arbiter.confirmPending(
        {ProtocolKind::GoogleCast, 2}, false, CastTime{4});
    check(repeatedRejection.status == ClaimStatus::Rejected
              && repeatedRejection.reason
                  == ClaimReason::ConfirmationRejected
              && repeatedRejection.duplicate,
          "repeated rejection is idempotent");
}

void testConfirmationAcceptancePreempts()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t oldId = acceptedSession(first);
    check(arbiter.sessionReady(oldId), "old owner ready");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "candidate waits for user");

    const auto accepted = arbiter.confirmPending(
        {ProtocolKind::AirPlay, 2}, true, CastTime{2});
    check(accepted.status == ClaimStatus::Accepted
              && accepted.reason == ClaimReason::ConfirmationAccepted,
          "confirmation accepts the candidate");
    check(accepted.owner->protocol == ProtocolKind::AirPlay
              && accepted.owner->epoch == 2,
          "accepted candidate becomes the new owner");
    check(accepted.displaced && accepted.displaced->id == oldId,
          "accepted confirmation preempts the old owner");
}

void testTimeoutKeepsCurrentSession()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{100},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t oldId = acceptedSession(first);
    check(arbiter.sessionReady(oldId), "timeout test owner ready");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{10});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "timeout test claim is pending");

    const ArbitrationTick early = arbiter.tick(CastTime{109});
    check(!early.pendingTimeout && arbiter.snapshot().owner->id == oldId,
          "confirmation does not time out early");
    const ArbitrationTick expired = arbiter.tick(CastTime{110});
    check(expired.pendingTimeout
              && expired.pendingTimeout->reason
                  == ClaimReason::ConfirmationTimedOut,
          "confirmation timeout produces a rejection");
    check(arbiter.snapshot().owner->id == oldId
              && arbiter.snapshot().phase == ArbitrationPhase::Active,
          "confirmation timeout keeps the current owner");

    const auto repeatedConfirm = arbiter.confirmPending(
        {ProtocolKind::AirPlay, 2}, true, CastTime{111});
    check(repeatedConfirm.status == ClaimStatus::Rejected
              && repeatedConfirm.reason
                  == ClaimReason::ConfirmationTimedOut
              && repeatedConfirm.duplicate,
          "confirmation after timeout is idempotently rejected");
}

void testOwnerCanEndWhileConfirmationPending()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t oldId = acceptedSession(first);
    check(arbiter.sessionReady(oldId), "owner ready before pending test");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "owner-end test creates pending claim");

    check(arbiter.sessionEnded(oldId, EndReason::Completed),
          "old owner may end naturally during confirmation");
    check(!arbiter.snapshot().owner
              && arbiter.snapshot().phase
                  == ArbitrationPhase::IdleWithPendingClaim
              && arbiter.snapshot().pendingClaim,
          "pending claim survives after the old owner ends");

    const auto accepted = arbiter.confirmPending(
        {ProtocolKind::AirPlay, 2}, true, CastTime{2});
    check(accepted.status == ClaimStatus::Accepted
              && accepted.owner->epoch == 2 && !accepted.displaced,
          "accepted idle candidate becomes owner without displacement");
    check(arbiter.snapshot().phase == ArbitrationPhase::Preparing,
          "accepted idle candidate starts preparing");
}

void testExpiredPendingRejectsDuplicateClaim()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{100},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t oldId = acceptedSession(first);
    check(arbiter.sessionReady(oldId), "expired duplicate owner ready");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{10});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "expired duplicate claim becomes pending");

    const auto repeated = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{110});
    check(repeated.status == ClaimStatus::Rejected
              && repeated.reason == ClaimReason::ConfirmationTimedOut,
          "duplicate claim at deadline expires instead of waiting again");
    check(arbiter.snapshot().owner->id == oldId
              && !arbiter.snapshot().pendingClaim,
          "expired duplicate cannot alter the current owner");
}

void testDuplicateClaimIsIdempotent()
{
    CastArbiter arbiter;
    const auto first = arbiter.claim(
        request(11, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(first.status == ClaimStatus::Accepted, "duplicate test first claim");
    const auto repeated = arbiter.claim(
        request(11, ProtocolKind::Dlna, "phone-a"), CastTime{1});
    check(repeated.duplicate && repeated.status == ClaimStatus::Accepted
              && repeated.owner->id == first.owner->id,
          "repeated claim ID returns the original decision");
    check(arbiter.snapshot().lastEpoch == 1,
          "duplicate claim does not advance epoch");
}

void testClaimIdScopedByProtocol()
{
    CastArbiter arbiter;
    const auto dlna = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t oldId = acceptedSession(dlna);
    check(arbiter.sessionReady(oldId), "scoped claim ID owner ready");

    const auto airplay = arbiter.claim(
        request(1, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(airplay.status == ClaimStatus::Busy
              && airplay.reason == ClaimReason::ActiveClaimProtected
              && !airplay.duplicate,
          "same numeric claim ID in another protocol is independent");
    check(arbiter.snapshot().owner->id == oldId,
          "cross-protocol claim ID collision cannot replace ownership");
}

void testLateConfirmationTimesOut()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{100},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t oldId = acceptedSession(first);
    check(arbiter.sessionReady(oldId), "late confirmation owner ready");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{10});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "late confirmation claim is pending");
    const std::uint64_t epoch = arbiter.snapshot().lastEpoch;

    const auto late = arbiter.confirmPending(
        {ProtocolKind::AirPlay, 2}, true, CastTime{110});
    check(late.status == ClaimStatus::Rejected
              && late.reason == ClaimReason::ConfirmationTimedOut,
          "confirmation exactly at deadline is rejected as timeout");
    check(arbiter.snapshot().owner->id == oldId
              && arbiter.snapshot().lastEpoch == epoch
              && !arbiter.snapshot().pendingClaim,
          "late confirmation cannot preempt the current owner");
}

void testRepeatedConfirmationIsIdempotent()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(arbiter.sessionReady(first.owner->id),
          "repeat confirmation owner ready");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::GoogleCast, "phone-b"), CastTime{1});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "repeat confirmation claim pending");
    const auto accepted = arbiter.confirmPending(
        {ProtocolKind::GoogleCast, 2}, true, CastTime{2});
    check(accepted.status == ClaimStatus::Accepted,
          "first confirmation accepted");
    const std::uint64_t epoch = arbiter.snapshot().lastEpoch;

    const auto repeated = arbiter.confirmPending(
        {ProtocolKind::GoogleCast, 2}, true, CastTime{3});
    check(repeated.status == ClaimStatus::Accepted && repeated.duplicate,
          "repeated confirmation returns the accepted decision");
    check(arbiter.snapshot().lastEpoch == epoch,
          "repeated confirmation cannot advance epoch again");
}

void testCurrentDecisionSurvivesCacheEviction()
{
    CastArbiter arbiter;
    const auto owner = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t ownerId = acceptedSession(owner);
    check(arbiter.sessionReady(ownerId), "cache eviction owner ready");

    for (std::uint64_t id = 100; id < 140; ++id) {
        const auto busy = arbiter.claim(
            request(id, ProtocolKind::AirPlay, "phone-b"),
            CastTime{static_cast<long long>(id)});
        check(busy.status == ClaimStatus::Busy,
              "cache eviction fills history with busy claims");
    }

    arbiter.setPolicy({true, false, CastTime{10000}, CastTime{5000}, 8});
    const auto repeated = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{200});
    check(repeated.status == ClaimStatus::Accepted
              && repeated.duplicate
              && repeated.owner->id == ownerId,
          "current owner claim remains idempotent after cache eviction");
    check(arbiter.snapshot().lastEpoch == 1,
          "cache eviction cannot make the owner preempt itself");
}

void testPendingDecisionSurvivesCacheEviction()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto owner = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(arbiter.sessionReady(owner.owner->id),
          "pending cache eviction owner ready");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "pending cache eviction candidate waits");

    for (std::uint64_t id = 100; id < 140; ++id) {
        const auto busy = arbiter.claim(
            request(id, ProtocolKind::GoogleCast, "phone-c"),
            CastTime{static_cast<long long>(id)});
        check(busy.status == ClaimStatus::Busy,
              "pending cache eviction fills history with busy claims");
    }

    const auto repeated = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{200});
    check(repeated.status == ClaimStatus::AwaitingConfirmation
              && repeated.duplicate,
          "pending claim remains idempotent after cache eviction");
}

void testOwnerFailureState()
{
    CastArbiter arbiter;
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t ownerId = acceptedSession(first);
    check(arbiter.sessionReady(ownerId), "failure owner ready");
    check(arbiter.sessionEnded(ownerId, EndReason::ProtocolLost),
          "protocol loss ends the owner");
    check(!arbiter.snapshot().owner
              && arbiter.snapshot().phase == ArbitrationPhase::Faulted
              && arbiter.snapshot().lastFailure
              && *arbiter.snapshot().lastFailure
                  == EndReason::ProtocolLost,
          "protocol loss enters Faulted");

    const auto replacement = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(replacement.status == ClaimStatus::Accepted
              && replacement.reason == ClaimReason::FirstClaim
              && !arbiter.snapshot().lastFailure,
          "new claim recovers from Faulted and clears failure");
}

void testOwnerFailureKeepsPendingClaim()
{
    CastArbiter arbiter({
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    const std::uint64_t ownerId = acceptedSession(first);
    check(arbiter.sessionReady(ownerId), "failure pending owner ready");
    const auto pending = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(pending.status == ClaimStatus::AwaitingConfirmation,
          "failure pending candidate waits");

    check(arbiter.sessionEnded(ownerId, EndReason::Failed),
          "failed owner ends during confirmation");
    check(!arbiter.snapshot().owner
              && arbiter.snapshot().phase
                  == ArbitrationPhase::IdleWithPendingClaim
              && arbiter.snapshot().lastFailure
              && *arbiter.snapshot().lastFailure == EndReason::Failed,
          "failure keeps the pending candidate in an idle state");
}

void testRetirementTimeout()
{
    CastArbiter arbiter({
        true,
        false,
        CastTime{10000},
        CastTime{50},
        8,
    });
    const auto first = arbiter.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(arbiter.sessionReady(first.owner->id), "retirement owner ready");
    const auto second = arbiter.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{1});
    check(second.status == ClaimStatus::Accepted
              && arbiter.snapshot().retiringSessions.size() == 1,
          "preemption starts retirement");

    const ArbitrationTick expired = arbiter.tick(CastTime{51});
    check(expired.expiredRetirements.size() == 1
              && arbiter.snapshot().retiringSessions.empty(),
          "retirement deadline removes stale farewell tracking");
}

void testClaimCoordinatorConfirmationFlow()
{
    CastClaimCoordinator coordinator({
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    const auto first = coordinator.claim(
        request(1, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(first.decision.status == ClaimStatus::Accepted
              && !first.confirmation,
          "coordinator first claim needs no confirmation");
    check(coordinator.sessionReady(first.decision.owner->id),
          "coordinator first owner ready");

    const auto pending = coordinator.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{100});
    check(pending.decision.status == ClaimStatus::AwaitingConfirmation
              && pending.confirmation,
          "coordinator exposes a neutral confirmation prompt");
    check(pending.confirmation->key.protocol == ProtocolKind::AirPlay
              && pending.confirmation->key.id == 2
              && pending.confirmation->peer.id == "phone-b"
              && pending.confirmation->peer.displayName == "phone-b"
              && pending.confirmation->peer.address == "192.0.2.1"
              && pending.confirmation->mediaTitle == "Test media"
              && pending.confirmation->deadline == CastTime{10100}
              && pending.confirmation->remaining == CastTime{10000},
          "coordinator prompt carries key, peer, title, and deadline");

    const auto repeated = coordinator.claim(
        request(2, ProtocolKind::AirPlay, "phone-b"), CastTime{150});
    check(repeated.decision.status == ClaimStatus::AwaitingConfirmation
              && repeated.decision.duplicate
              && !repeated.confirmation,
          "coordinator duplicate pending claim reuses the existing prompt");

    const auto rejected = coordinator.resolveConfirmation(
        pending.confirmation->key, false, CastTime{200});
    check(rejected.status == ClaimStatus::Rejected
              && coordinator.snapshot().owner->protocol
                  == ProtocolKind::Dlna,
          "coordinator rejection keeps the old owner");

    const auto preempt = coordinator.claim(
        request(3, ProtocolKind::AirPlay, "phone-b"), CastTime{300});
    check(preempt.decision.status == ClaimStatus::AwaitingConfirmation
              && preempt.confirmation,
          "coordinator exposes a second confirmation prompt");
    const auto accepted = coordinator.resolveConfirmation(
        preempt.confirmation->key, true, CastTime{400});
    check(accepted.status == ClaimStatus::Accepted
              && accepted.reason == ClaimReason::ConfirmationAccepted
              && coordinator.snapshot().owner->protocol
                  == ProtocolKind::AirPlay,
          "coordinator accepted confirmation starts handoff");

    const auto unknown = coordinator.resolveConfirmation(
        {ProtocolKind::AirPlay, 999}, true, CastTime{500});
    check(unknown.status == ClaimStatus::NotFound,
          "coordinator rejects unknown confirmation key");

    CastClaimCoordinator timeoutCoordinator({
        true,
        true,
        CastTime{100},
        CastTime{5000},
        8,
    });
    const auto timeoutOwner = timeoutCoordinator.claim(
        request(10, ProtocolKind::Dlna, "phone-a"), CastTime{0});
    check(timeoutCoordinator.sessionReady(timeoutOwner.decision.owner->id),
          "coordinator timeout owner ready");
    const auto timeoutPrompt = timeoutCoordinator.claim(
        request(11, ProtocolKind::GoogleCast, "phone-b"), CastTime{10});
    check(timeoutPrompt.confirmation.has_value(),
          "coordinator timeout prompt present");
    const auto timeoutTick = timeoutCoordinator.tick(CastTime{111});
    check(timeoutTick.pendingTimeout
              && timeoutTick.pendingTimeout->reason
                  == ClaimReason::ConfirmationTimedOut
              && timeoutCoordinator.snapshot().owner->protocol
                  == ProtocolKind::Dlna,
          "coordinator propagates pending confirmation timeout");
    const auto late = timeoutCoordinator.resolveConfirmation(
        timeoutPrompt.confirmation->key, true, CastTime{112});
    check(late.reason == ClaimReason::ConfirmationTimedOut
              && late.status == ClaimStatus::Rejected,
          "coordinator cannot accept a timed-out confirmation");
}

} // namespace

int main()
{
    testFirstClaimAndReady();
    testProtectedActiveClaim();
    testImmediatePreemption();
    testConfirmationKeepsOldOwner();
    testConfirmationAcceptancePreempts();
    testTimeoutKeepsCurrentSession();
    testOwnerCanEndWhileConfirmationPending();
    testExpiredPendingRejectsDuplicateClaim();
    testDuplicateClaimIsIdempotent();
    testClaimIdScopedByProtocol();
    testLateConfirmationTimesOut();
    testRepeatedConfirmationIsIdempotent();
    testCurrentDecisionSurvivesCacheEviction();
    testPendingDecisionSurvivesCacheEviction();
    testOwnerFailureState();
    testOwnerFailureKeepsPendingClaim();
    testRetirementTimeout();
    testClaimCoordinatorConfirmationFlow();
    std::cout << "Cast arbiter tests passed\n";
    return 0;
}
