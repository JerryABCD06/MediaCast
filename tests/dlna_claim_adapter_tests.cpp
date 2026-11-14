// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/claim/DlnaClaimAdapter.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace mcast::cast;
using namespace mcast::dlna::claim;
using mcast::playback::MediaRequest;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

PeerIdentity peer(const std::string &address)
{
    return {address, "Phone " + address, address};
}

PeerIdentity control(
    const std::string &id, const std::string &address)
{
    return {id, "Phone " + id, address};
}

MediaRequest media(const std::string &uri)
{
    MediaRequest result;
    result.uri = uri;
    result.title = "Title " + uri;
    return result;
}

ArbitrationPolicy confirmationPolicy()
{
    return {
        true,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    };
}

void testOwnerMediaChangeDoesNotPrompt()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(first.disposition == DlnaClaimDisposition::Execute
              && first.claimedSessionId
              && first.ownerSessionId == first.claimedSessionId
              && first.ownerEpoch != 0
              && first.mediaGeneration == 1,
          "first DLNA media claim executes");
    const std::uint64_t sessionId = *first.claimedSessionId;
    check(coordinator.sessionReady(sessionId),
          "first DLNA claim becomes ready");

    const auto second = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/two.mp4"),
        CastTime{1});
    check(second.disposition == DlnaClaimDisposition::Execute
              && !second.claimedSessionId && !adapter.hasPending()
              && second.ownerSessionId == sessionId
              && second.mediaGeneration == 2,
          "same owner media change bypasses a second claim prompt");
    check(coordinator.snapshot().owner->id == sessionId,
          "same owner media change keeps the session");

    const auto repeated = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/two.mp4"),
        CastTime{2});
    check(repeated.disposition == DlnaClaimDisposition::Execute
              && repeated.reason == ClaimReason::DuplicateClaim
              && repeated.mediaGeneration == 2,
          "same owner identical SetURI does not advance media generation");
}

void testEmptyTitleDoesNotBecomeUri()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    MediaRequest request = media("http://a/no-title.mp4");
    request.title.clear();
    const auto outcome = adapter.beginMedia(
        peer("192.0.2.10"), std::move(request), CastTime{0});
    check(outcome.disposition == DlnaClaimDisposition::Execute
              && outcome.media
              && outcome.media->title.empty(),
          "empty DLNA title stays empty for playback fallback");
}

void testDifferentPeerConfirmationLifecycle()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    const std::uint64_t oldSession = *first.claimedSessionId;
    check(coordinator.sessionReady(oldSession),
          "confirmation lifecycle old owner ready");

    auto candidate = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/first.mp4"),
        CastTime{10});
    check(candidate.disposition == DlnaClaimDisposition::Pending
              && candidate.confirmation
              && candidate.confirmation->key.protocol
                  == ProtocolKind::Dlna,
          "different peer starts one confirmation transaction");
    const ClaimKey key = candidate.confirmation->key;
    check(coordinator.snapshot().owner->id == oldSession,
          "pending candidate leaves old owner unchanged");

    const auto repeated = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/first.mp4"),
        CastTime{20});
    check(repeated.disposition == DlnaClaimDisposition::Pending
              && repeated.transactionId == candidate.transactionId
              && !repeated.confirmation,
          "same candidate media retry reuses its claim and prompt");

    const auto candidatePlay =
        adapter.requestPlay(peer("192.0.2.20"), CastTime{21});
    check(candidatePlay.disposition == DlnaClaimDisposition::Pending
              && candidatePlay.transactionId
                  == candidate.transactionId,
          "candidate Play waits with SetURI");
    const auto candidateStop =
        adapter.requestStop(peer("192.0.2.20"), CastTime{22});
    check(candidateStop.disposition == DlnaClaimDisposition::Ignored,
          "candidate Stop cannot stop the old owner");
    const auto ownerPlay =
        adapter.requestPlay(peer("192.0.2.10"), CastTime{23});
    check(ownerPlay.disposition == DlnaClaimDisposition::Execute,
          "current owner keeps playing during confirmation");
    check(ownerPlay.mediaGeneration == 1,
          "owner control commands do not advance media generation");

    const auto accepted = adapter.resolveConfirmation(
        key, true, CastTime{30});
    check(accepted.executeMedia && accepted.media
              && accepted.media->uri == "http://b/first.mp4"
              && accepted.decision.status == ClaimStatus::Accepted
              && accepted.ownerSessionId
              && accepted.mediaGeneration == 2,
          "acceptance releases the confirmed candidate media");
    check(!adapter.hasPending()
              && coordinator.snapshot().owner->protocol
                  == ProtocolKind::Dlna
              && coordinator.snapshot().owner->peer.address
                  == "192.0.2.20",
          "acceptance transfers DLNA ownership once");

    const auto repeatedAcceptance = adapter.resolveConfirmation(
        key, true, CastTime{31});
    check(repeatedAcceptance.decision.duplicate
              && !repeatedAcceptance.executeMedia
              && !repeatedAcceptance.media,
          "duplicate acceptance cannot open media twice");
}

void testRejectionKeepsOwner()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    const std::uint64_t oldSession = *first.claimedSessionId;
    check(coordinator.sessionReady(oldSession),
          "rejection old owner ready");
    const auto candidate = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{1});
    check(candidate.confirmation.has_value(),
          "rejection candidate has confirmation");

    const auto rejected = adapter.resolveConfirmation(
        candidate.confirmation->key, false, CastTime{2});
    check(!rejected.executeMedia && !rejected.media
              && rejected.decision.reason
                  == ClaimReason::ConfirmationRejected,
          "rejection does not release candidate media");
    check(coordinator.snapshot().owner->id == oldSession
              && adapter.ownsPeer(peer("192.0.2.10"))
              && !adapter.ownsPeer(peer("192.0.2.20")),
          "rejection preserves the old DLNA owner");
}

void testChangedMediaRequiresNewConfirmation()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    const std::uint64_t oldSession = *first.claimedSessionId;
    check(coordinator.sessionReady(oldSession),
          "changed media old owner ready");

    const auto original = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/original.mp4"),
        CastTime{1});
    check(original.confirmation.has_value(),
          "changed media original confirmation exists");
    const ClaimKey originalKey = original.confirmation->key;

    const auto changed = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/changed.mp4"),
        CastTime{2});
    check(changed.disposition == DlnaClaimDisposition::Pending
              && changed.confirmation
              && changed.transactionId != original.transactionId
              && changed.invalidatedPending
              && changed.invalidatedPending->decision.claimId
                  == original.transactionId
              && changed.invalidatedPending->media
              && changed.invalidatedPending->media->uri
                  == "http://b/original.mp4",
          "changing candidate content starts a new confirmation");
    const ClaimKey changedKey = changed.confirmation->key;

    const auto staleAcceptance = adapter.resolveConfirmation(
        originalKey, true, CastTime{3});
    check(!staleAcceptance.executeMedia
              && !staleAcceptance.media
              && staleAcceptance.decision.status != ClaimStatus::Accepted,
          "old confirmation cannot authorize changed media");
    check(coordinator.snapshot().owner->id == oldSession,
          "stale confirmation leaves the old owner in place");

    const auto currentAcceptance = adapter.resolveConfirmation(
        changedKey, true, CastTime{4});
    check(currentAcceptance.executeMedia && currentAcceptance.media
              && currentAcceptance.media->uri
                  == "http://b/changed.mp4",
          "new confirmation authorizes the new media");
}

void testSameAddressDifferentControlIsNotOwner()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        control("app-a", "192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(coordinator.sessionReady(*first.claimedSessionId),
          "control identity test owner ready");
    check(!adapter.ownsPeer(control("app-b", "192.0.2.10")),
          "different control identity is not the media owner");
    check(adapter.canControl(control("app-b", "192.0.2.10")),
          "control from the owner address may operate the current session");
    check(
        adapter.requestPlay(
            control("app-b", "192.0.2.10"), CastTime{1}).disposition
            == DlnaClaimDisposition::Execute,
        "split control traffic on the owner address can play");
    check(
        adapter.requestStop(
            control("app-b", "192.0.2.10"), CastTime{1}).disposition
            == DlnaClaimDisposition::Ignored,
        "same-address control identity cannot stop before claiming");
    check(
        adapter.requestStop(
            control("app-a", "192.0.2.10"), CastTime{1}).disposition
            == DlnaClaimDisposition::Execute,
        "the complete owner identity can stop immediately");

    const auto other = adapter.beginMedia(
        control("app-b", "192.0.2.10"), media("http://b/two.mp4"),
        CastTime{2});
    check(other.disposition == DlnaClaimDisposition::Pending
              && other.confirmation,
          "same IP with another control identity requires confirmation");
    check(coordinator.snapshot().owner->peer.id == "app-a",
          "same-IP control identity cannot bypass the owner");
}

void testPendingSurvivesNaturalOwnerEnd()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    const std::uint64_t oldSession = *first.claimedSessionId;
    check(coordinator.sessionReady(oldSession),
          "natural-owner-end old owner ready");
    const auto candidate = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{1});
    check(candidate.confirmation.has_value(),
          "natural-owner-end candidate pending");
    check(coordinator.sessionEnded(oldSession, EndReason::Completed),
          "old owner may end while candidate waits");

    const auto accepted = adapter.resolveConfirmation(
        candidate.confirmation->key, true, CastTime{2});
    check(accepted.executeMedia && accepted.media
              && accepted.decision.owner
              && accepted.decision.owner->id != oldSession,
          "pending candidate can become a new owner after old completion");
}

void testTimeoutAndTick()
{
    CastClaimCoordinator coordinator({
        true,
        true,
        CastTime{100},
        CastTime{5000},
        8,
    });
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(coordinator.sessionReady(*first.claimedSessionId),
          "timeout old owner ready");
    const auto candidate = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{10});
    check(candidate.confirmation.has_value(),
          "timeout candidate has confirmation");

    const auto early = adapter.tick(CastTime{109});
    check(!early.pendingInvalidation && adapter.hasPending(),
          "pending DLNA claim does not expire early");
    const auto expired = adapter.tick(CastTime{110});
    check(expired.pendingInvalidation
              && expired.pendingInvalidation->decision.reason
                  == ClaimReason::ConfirmationTimedOut
              && expired.pendingInvalidation->media
              && expired.pendingInvalidation->media->uri
                  == "http://b/two.mp4"
              && !adapter.hasPending(),
          "DLNA pending timeout releases its transaction state");
    check(adapter.ownsPeer(peer("192.0.2.10")),
          "DLNA timeout keeps the current owner");
}

void testEntryExpiresStalePending()
{
    CastClaimCoordinator coordinator({
        true,
        true,
        CastTime{100},
        CastTime{5000},
        8,
    });
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    const std::uint64_t oldSession = *first.claimedSessionId;
    check(coordinator.sessionReady(oldSession),
          "entry expiry old owner ready");
    const auto candidate = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{10});
    check(candidate.confirmation.has_value(),
          "entry expiry candidate exists");

    const auto latePlay = adapter.requestPlay(
        peer("192.0.2.20"), CastTime{110});
    check(latePlay.disposition == DlnaClaimDisposition::Rejected
              && latePlay.invalidatedPending
              && latePlay.invalidatedPending->decision.reason
                  == ClaimReason::ConfirmationTimedOut
              && !adapter.hasPending(),
          "any new request expires a stale pending before dispatch");
    check(coordinator.snapshot().owner->id == oldSession,
          "entry expiry cannot change the old owner");

    const auto replacement = adapter.beginMedia(
        peer("192.0.2.30"), media("http://c/three.mp4"),
        CastTime{111});
    check(replacement.disposition == DlnaClaimDisposition::Pending
              && replacement.confirmation,
          "a new source can claim after stale pending expiry");
}

void testPendingOtherPeerAndUnknownPlayAreRejected()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(coordinator.sessionReady(*first.claimedSessionId),
          "busy pending old owner ready");
    const auto candidate = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{1});
    check(candidate.confirmation.has_value(),
          "busy pending candidate starts");

    const auto extra = adapter.beginMedia(
        peer("192.0.2.30"), media("http://c/three.mp4"),
        CastTime{2});
    check(extra.disposition == DlnaClaimDisposition::Rejected
              && extra.reason == ClaimReason::PendingClaimExists,
          "second candidate cannot replace the pending claim");

    const auto unknownPlay =
        adapter.requestPlay(peer("192.0.2.30"), CastTime{3});
    check(unknownPlay.disposition == DlnaClaimDisposition::Rejected,
          "Play without SetURI or ownership is rejected");
    const auto unknownStop =
        adapter.requestStop(peer("192.0.2.30"), CastTime{4});
    check(unknownStop.disposition == DlnaClaimDisposition::Ignored,
          "unknown Stop cannot affect the active owner");
}

void testProtectedOwnerAndInvalidPeer()
{
    CastClaimCoordinator coordinator({
        false,
        true,
        CastTime{10000},
        CastTime{5000},
        8,
    });
    DlnaClaimAdapter adapter(coordinator);
    const auto invalid = adapter.beginMedia(
        {"", "", ""}, media("http://a/one.mp4"), CastTime{0});
    check(invalid.disposition == DlnaClaimDisposition::Rejected,
          "DLNA claim requires a source address");

    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{1});
    check(coordinator.sessionReady(*first.claimedSessionId),
          "protected DLNA owner ready");
    const auto second = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{2});
    check(second.disposition == DlnaClaimDisposition::Rejected
              && second.reason == ClaimReason::ActiveClaimProtected,
          "DLNA candidate is rejected when preemption is disabled");
}

void testCancelPending()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    const auto first = adapter.beginMedia(
        peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    const std::uint64_t oldSession = *first.claimedSessionId;
    check(coordinator.sessionReady(oldSession),
          "cancel old owner ready");
    const auto candidate = adapter.beginMedia(
        peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{1});
    check(candidate.confirmation.has_value(),
          "cancel candidate pending");

    const auto wrongCancellation = adapter.cancelConfirmation(
        {ProtocolKind::Dlna, candidate.transactionId + 100},
        CastTime{2});
    check(wrongCancellation.decision.status == ClaimStatus::Rejected
              && wrongCancellation.decision.reason
                  == ClaimReason::UnknownClaim
              && adapter.hasPending(),
          "stale cancel key cannot cancel another pending claim");

    const auto cancelled = adapter.cancelConfirmation(
        candidate.confirmation->key, CastTime{3});
    check(!cancelled.executeMedia
              && cancelled.decision.reason
                  == ClaimReason::ConfirmationRejected,
          "protocol disconnect cancels the pending claim");
    check(coordinator.snapshot().owner->id == oldSession,
          "pending cancellation keeps the old owner");
}

} // namespace

int main()
{
    testEmptyTitleDoesNotBecomeUri();
    testOwnerMediaChangeDoesNotPrompt();
    testDifferentPeerConfirmationLifecycle();
    testRejectionKeepsOwner();
    testChangedMediaRequiresNewConfirmation();
    testSameAddressDifferentControlIsNotOwner();
    testPendingSurvivesNaturalOwnerEnd();
    testTimeoutAndTick();
    testEntryExpiresStalePending();
    testPendingOtherPeerAndUnknownPlayAreRejected();
    testProtectedOwnerAndInvalidPeer();
    testCancelPending();
    std::cout << "DLNA claim adapter tests passed\n";
    return 0;
}
