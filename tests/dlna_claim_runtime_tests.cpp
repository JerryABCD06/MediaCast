// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/claim/DlnaClaimRuntime.h"

#include <algorithm>
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

bool hasCompletion(
    const DlnaClaimRuntimeActions &actions,
    std::uint64_t requestId, bool success)
{
    return std::any_of(
        actions.http.begin(), actions.http.end(),
        [&](const DlnaHttpCompletion &item) {
            return item.requestId == requestId
                && item.success == success;
        });
}

void testInitialMediaAndAttachedPlay()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);

    auto actions = runtime.beginSetUri(
        101, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(actions.media && actions.media->newSession
              && actions.media->sessionId != 0
              && runtime.mediaOpening(),
          "initial SetURI starts media preparation");
    const std::uint64_t operationId = actions.media->operationId;
    check(actions.http.empty(),
          "initial SetURI HTTP waits for media preparation");

    actions = runtime.attachPlay(
        102, peer("192.0.2.10"), CastTime{1});
    check(actions.http.empty()
              && runtime.operationId()
              && *runtime.operationId() == operationId,
          "Play during Open is attached to the operation");

    actions = runtime.mediaReady(
        operationId, true, CastTime{2});
    check(actions.playNow
              && hasCompletion(actions, 101, true)
              && hasCompletion(actions, 102, true)
              && coordinator.snapshot().phase
                  == ArbitrationPhase::Active,
          "media readiness completes both HTTP requests");
}

void testDuplicateOwnerSetUriIsImmediate()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto actions = runtime.beginSetUri(
        201, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(actions.media.has_value(),
          "duplicate owner initial media exists");
    check(runtime.mediaReady(
              actions.media->operationId, true, CastTime{1})
                  .playNow
              == false,
          "initial media without Play does not play");

    actions = runtime.beginSetUri(
        202, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{2});
    check(!actions.media && hasCompletion(actions, 202, true)
              && !runtime.mediaOpening(),
          "duplicate owner SetURI completes without another Open");
}

void testConfirmationDecisionCompletesHeldRequests()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto initial = runtime.beginSetUri(
        301, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(runtime.mediaReady(
              initial.media->operationId, true, CastTime{1})
                  .playNow
              == false,
          "confirmation owner ready");

    auto actions = runtime.beginSetUri(
        302, peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{2});
    check(actions.confirmation && actions.http.empty(),
          "candidate SetURI waits for confirmation");
    const ClaimKey key = actions.confirmation->key;
    actions = runtime.attachPlay(
        303, peer("192.0.2.20"), CastTime{3});
    check(actions.http.empty(),
          "candidate Play waits with confirmation");

    const auto stopPeer =
        runtime.confirmationPreemptionStopPeer(key);
    check(stopPeer && stopPeer->address == "192.0.2.10",
          "accepted confirmation exposes the displaced owner for Stop");
    actions = runtime.confirmationDecision(
        key, true, CastTime{4});
    check(actions.media && actions.media->playRequested
              && actions.http.empty() && !actions.stopNow,
          "ownership handoff starts candidate Open after owner Stop");
    actions = runtime.mediaReady(
        actions.media->operationId, true, CastTime{5});
    check(actions.playNow
              && hasCompletion(actions, 302, true)
              && hasCompletion(actions, 303, true),
          "candidate media readiness releases held SetURI and Play");
}

void testRejectionAndTimeoutReleaseRequests()
{
    CastClaimCoordinator coordinator({
        true,
        true,
        CastTime{100},
        CastTime{5000},
        8,
    });
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto initial = runtime.beginSetUri(
        401, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(runtime.mediaReady(
              initial.media->operationId, true, CastTime{1})
                  .playNow
              == false,
          "timeout owner ready");

    auto candidate = runtime.beginSetUri(
        402, peer("192.0.2.20"), media("http://b/two.mp4"),
        CastTime{2});
    check(candidate.confirmation.has_value(),
          "rejection candidate pending");
    auto rejected = runtime.confirmationDecision(
        candidate.confirmation->key, false, CastTime{3});
    check(hasCompletion(rejected, 402, false),
          "rejection releases SetURI as failure");

    candidate = runtime.beginSetUri(
        403, peer("192.0.2.20"), media("http://b/three.mp4"),
        CastTime{4});
    check(candidate.confirmation.has_value(),
          "timeout candidate pending");
    auto timedOut = runtime.tick(CastTime{104});
    check(hasCompletion(timedOut, 403, false),
          "tick timeout releases SetURI as failure");
}

void testChangedCandidateInvalidatesOldRequests()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto initial = runtime.beginSetUri(
        501, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(runtime.mediaReady(
              initial.media->operationId, true, CastTime{1})
                  .playNow
              == false,
          "changed candidate owner ready");
    auto original = runtime.beginSetUri(
        502, peer("192.0.2.20"), media("http://b/original.mp4"),
        CastTime{2});
    check(original.confirmation.has_value(),
          "changed candidate original pending");
    auto originalPlay = runtime.attachPlay(
        504, peer("192.0.2.20"), CastTime{2});
    check(originalPlay.http.empty(),
          "changed candidate original Play waits");
    auto changed = runtime.beginSetUri(
        503, peer("192.0.2.20"), media("http://b/changed.mp4"),
        CastTime{3});
    check(hasCompletion(changed, 502, false)
              && hasCompletion(changed, 504, false)
              && changed.confirmation
              && hasCompletion(changed, 503, false) == false,
          "changed candidate rejects old HTTP and keeps new one pending");
}

void testReplacingSlowOpenAndControlLoss()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto first = runtime.beginSetUri(
        601, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(first.media.has_value(),
          "slow-open first operation exists");
    const std::uint64_t firstOperation = first.media->operationId;
    auto second = runtime.beginSetUri(
        602, peer("192.0.2.10"), media("http://a/two.mp4"),
        CastTime{1});
    check(second.media
              && second.media->operationId != firstOperation
              && hasCompletion(second, 601, false),
          "new owner media rejects the stale slow Open request");
    const std::uint64_t secondOperation = second.media->operationId;

    auto stale = runtime.mediaReady(
        firstOperation, true, CastTime{2});
    check(stale.staleOperation,
          "stale media readiness is rejected");

    auto lost = runtime.controlLost(CastTime{3});
    check(hasCompletion(lost, 602, false)
              && !runtime.mediaOpening(),
          "control loss rejects the active preparation");
    static_cast<void>(secondOperation);
}

void testInvalidAndDuplicateRequestIdsHaveNoSideEffects()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    const std::uint64_t revision = coordinator.snapshot().revision;

    auto invalid = runtime.beginSetUri(
        0, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(invalid.http.size() == 1 && !invalid.http.front().success
              && coordinator.snapshot().revision == revision
              && !runtime.mediaOpening() && !runtime.pendingKey(),
          "zero SetURI request ID has no arbitration side effects");

    auto started = runtime.beginSetUri(
        701, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{1});
    check(started.media.has_value(),
          "duplicate ID test starts one media operation");
    const std::uint64_t operationId = started.media->operationId;
    auto duplicateSet = runtime.beginSetUri(
        701, peer("192.0.2.10"), media("http://a/two.mp4"),
        CastTime{2});
    check(hasCompletion(duplicateSet, 701, false)
              && runtime.operationId()
              && *runtime.operationId() == operationId,
          "duplicate SetURI request ID cannot replace the operation");
    auto duplicatePlay = runtime.attachPlay(
        701, peer("192.0.2.10"), CastTime{3});
    check(hasCompletion(duplicatePlay, 701, false)
              && runtime.operationId()
              && *runtime.operationId() == operationId,
          "same ID cannot be reused as an attached Play");
}

void testSessionReadyFailureFailsMedia()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto started = runtime.beginSetUri(
        801, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(started.media.has_value(),
          "session-ready failure operation exists");
    const std::uint64_t operationId = started.media->operationId;
    check(coordinator.sessionEnded(
              started.media->sessionId, EndReason::Failed),
          "session is ended before media readiness");
    auto failed = runtime.mediaReady(
        operationId, true, CastTime{1});
    check(hasCompletion(failed, 801, false)
              && !runtime.mediaOpening(),
          "failed sessionReady cannot report HTTP success");
}

void testMediaReadyRejectsChangedOwnership()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto first = runtime.beginSetUri(
        901, peer("192.0.2.10"), media("http://a/one.mp4"),
        CastTime{0});
    check(first.media.has_value()
              && coordinator.snapshot().owner.has_value(),
          "ownership validation starts with an owner");
    check(runtime.mediaReady(
              first.media->operationId, true, CastTime{1})
              .playNow
              == false,
          "first owner media becomes ready");

    auto replacement = runtime.beginSetUri(
        902, peer("192.0.2.10"), media("http://a/two.mp4"),
        CastTime{2});
    check(replacement.media.has_value()
              && !replacement.media->newSession,
          "same-peer replacement reuses the session");
    const std::uint64_t ownerId = coordinator.snapshot().owner->id;
    check(coordinator.sessionEnded(
              ownerId, EndReason::Preempted),
          "owner can disappear while replacement media opens");
    auto failed = runtime.mediaReady(
        replacement.media->operationId, true, CastTime{3});
    check(!failed.playNow
              && hasCompletion(failed, 902, false)
              && !runtime.mediaOpening(),
          "late readiness cannot authorize media in an ended session");
}

void testSlowOpenDeadlineAndActiveControlLoss()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto started = runtime.beginSetUri(
        1001, peer("192.0.2.10"), media("http://a/slow.mp4"),
        CastTime{0});
    check(started.media.has_value() && runtime.mediaOpening(),
          "slow-open operation starts");
    auto timedOut = runtime.tick(CastTime{10000});
    check(hasCompletion(timedOut, 1001, false)
              && !runtime.mediaOpening()
              && !coordinator.snapshot().owner,
          "slow Open has a bounded runtime deadline");

    auto active = runtime.beginSetUri(
        1002, peer("192.0.2.10"), media("http://a/active.mp4"),
        CastTime{10001});
    check(active.media.has_value(),
          "active-owner loss scenario starts media");
    runtime.mediaReady(
        active.media->operationId, true, CastTime{10002});
    check(coordinator.snapshot().owner.has_value(),
          "active owner exists before control loss");
    runtime.controlLost(CastTime{10003});
    check(!coordinator.snapshot().owner,
          "control loss ends an already-ready owner session");
}

void testReplacingPreparingOwnerRequiresReadiness()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto first = runtime.beginSetUri(
        1101, peer("192.0.2.10"), media("http://a/first.mp4"),
        CastTime{0});
    check(first.media.has_value()
              && coordinator.snapshot().phase
                  == ArbitrationPhase::Preparing,
          "first media leaves the owner preparing");
    auto replacement = runtime.beginSetUri(
        1102, peer("192.0.2.10"), media("http://a/replacement.mp4"),
        CastTime{1});
    check(replacement.media.has_value()
              && replacement.media->newSession
              && hasCompletion(replacement, 1101, false),
          "replacement while preparing still requires session readiness");
    auto ready = runtime.mediaReady(
        replacement.media->operationId, true, CastTime{2});
    check(hasCompletion(ready, 1102, true)
              && coordinator.snapshot().phase
                  == ArbitrationPhase::Active,
          "replacement readiness moves the reused session to active");
}

void testLateReadinessCannotBeatDeadline()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto started = runtime.beginSetUri(
        1201, peer("192.0.2.10"), media("http://a/late.mp4"),
        CastTime{0});
    check(started.media.has_value(),
          "late-readiness operation starts");
    auto late = runtime.mediaReady(
        started.media->operationId, true, CastTime{10000});
    check(hasCompletion(late, 1201, false)
              && !late.playNow && !runtime.mediaOpening()
              && !coordinator.snapshot().owner,
          "mediaReady enforces the same deadline without waiting for tick");
}

void testReplacingHandoffOwnerRequiresReadiness()
{
    CastClaimCoordinator coordinator(confirmationPolicy());
    DlnaClaimAdapter adapter(coordinator);
    DlnaClaimRuntime runtime(adapter);
    auto owner = runtime.beginSetUri(
        1301, peer("192.0.2.10"), media("http://a/owner.mp4"),
        CastTime{0});
    runtime.mediaReady(
        owner.media->operationId, true, CastTime{1});
    auto candidate = runtime.beginSetUri(
        1302, peer("192.0.2.20"), media("http://b/first.mp4"),
        CastTime{2});
    check(candidate.confirmation.has_value(),
          "handoff test candidate waits for confirmation");
    auto accepted = runtime.confirmationDecision(
        candidate.confirmation->key, true, CastTime{3});
    check(accepted.media.has_value()
              && coordinator.snapshot().phase
                  == ArbitrationPhase::Handoff,
          "accepted preemption enters handoff");
    auto replacement = runtime.beginSetUri(
        1303, peer("192.0.2.20"), media("http://b/replacement.mp4"),
        CastTime{4});
    check(replacement.media.has_value()
              && replacement.media->newSession
              && hasCompletion(replacement, 1302, false),
          "replacement during handoff still marks the session ready");
    auto ready = runtime.mediaReady(
        replacement.media->operationId, true, CastTime{5});
    check(hasCompletion(ready, 1303, true)
              && coordinator.snapshot().phase
                  == ArbitrationPhase::Active,
          "handoff replacement readiness activates the owner");
}

} // namespace

int main()
{
    testInitialMediaAndAttachedPlay();
    testDuplicateOwnerSetUriIsImmediate();
    testConfirmationDecisionCompletesHeldRequests();
    testRejectionAndTimeoutReleaseRequests();
    testChangedCandidateInvalidatesOldRequests();
    testReplacingSlowOpenAndControlLoss();
    testInvalidAndDuplicateRequestIdsHaveNoSideEffects();
    testSessionReadyFailureFailsMedia();
    testMediaReadyRejectsChangedOwnership();
    testSlowOpenDeadlineAndActiveControlLoss();
    testReplacingPreparingOwnerRequiresReadiness();
    testLateReadinessCannotBeatDeadline();
    testReplacingHandoffOwnerRequiresReadiness();
    std::cout << "DLNA claim runtime tests passed\n";
    return 0;
}
