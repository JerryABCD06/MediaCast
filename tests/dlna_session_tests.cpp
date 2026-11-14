// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "protocols/dlna/session/DlnaSession.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace mcast::dlna;
using namespace mcast::playback;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class SnapshotBlockingGateway final : public PlaybackGateway
{
public:
    explicit SnapshotBlockingGateway(FakePlaybackGateway &inner)
        : m_inner(inner)
    {
    }

    PlaybackSnapshot snapshot() const override
    {
        if (m_blockNextSnapshot.exchange(false)) {
            m_snapshotEntered = true;
            while (!m_releaseSnapshot.load())
                std::this_thread::yield();
        }
        return m_inner.snapshot();
    }

    PlaybackCommandSubmission submit(
        const PlaybackCommand &command) override
    {
        if (command.type == PlaybackCommandType::EndSession
            && m_failNextEnd.exchange(false)) {
            const auto snapshot = m_inner.snapshot();
            return {false, PlaybackSubmissionStatus::InvalidCommand, 0,
                    snapshot.generation, "injected EndSession failure",
                    snapshot.stateVersion};
        }
        return m_inner.submit(command);
    }

    std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) override
    {
        return m_inner.addEventListener(std::move(listener));
    }

    void removeEventListener(std::uint64_t listenerId) override
    {
        m_inner.removeEventListener(listenerId);
    }

    void blockNextSnapshot()
    {
        m_snapshotEntered = false;
        m_releaseSnapshot = false;
        m_blockNextSnapshot = true;
    }

    bool snapshotEntered() const { return m_snapshotEntered.load(); }
    void releaseSnapshot() { m_releaseSnapshot = true; }
    void failNextEndSubmission() { m_failNextEnd = true; }

private:
    FakePlaybackGateway &m_inner;
    mutable std::atomic_bool m_blockNextSnapshot{false};
    mutable std::atomic_bool m_snapshotEntered{false};
    mutable std::atomic_bool m_releaseSnapshot{false};
    mutable std::atomic_bool m_failNextEnd{false};
};

void testConcurrentSessionAccess()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    PlaybackEvent event;
    event.kind = PlaybackEventKind::SnapshotChanged;
    event.snapshot = playback.snapshot();

    std::atomic_bool done{false};
    std::jthread events([&] {
        for (int index = 0; index < 10000; ++index)
            session.handlePlaybackEvent(event);
        done = true;
    });
    while (!done.load()) {
        static_cast<void>(session.snapshot());
        static_cast<void>(session.transportTransitionPending());
        session.peerActivity(SessionTime{1});
    }
    events.join();
    check(session.snapshot().playback.state == PlaybackState::NoMedia,
          "concurrent session access remains consistent");
}

void testRejectedQueuedCommand()
{
    FakePlaybackGateway playback;
    int failures = 0;
    DlnaSession session(playback, {
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        [&](const std::string &) { ++failures; },
        {},
    });
    MediaRequest media;
    media.uri = "http://example.test/rejected.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "rejection test open queued");
    playback.processAll();
    playback.markReady();
    PlaybackCommand play;
    play.type = PlaybackCommandType::Play;
    check(session.submit(play).queued, "rejection test command queued");
    playback.simulatePlayerLost();
    playback.processAll();
    check(failures != 0,
          "queued command rejection is reported after acceptance");
}

void testSubmitAndWaitObservesAppliedState()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    PlaybackCommand command;
    command.type = PlaybackCommandType::SetNext;
    command.media.uri = "http://example.test/next.mp4";
    std::jthread processor([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        playback.processAll();
    });
    const auto result = session.submitAndWait(
        std::move(command), SessionTime{2000});
    processor.join();
    check(result.outcome
              && *result.outcome == PlaybackOutcomeStatus::Applied
              && !result.timedOut,
          "synchronous command wait completes with an applied result");
    check(playback.snapshot().next.uri
              == "http://example.test/next.mp4"
              && playback.snapshot().hasNext,
          "synchronous wait returns only after playback state is visible");
}

void testUiOriginatedNextAdvancesProtocolRecord()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    MediaRequest current;
    current.uri = "http://example.test/current.mp4";
    current.metadata = "<DIDL-Lite><current/></DIDL-Lite>";
    check(session.openMedia(current, SessionTime{0}).queued,
          "protocol-next test opens current media");
    playback.processAll();
    playback.markReady();

    MediaRequest next;
    next.uri = "http://example.test/next.mp4";
    next.metadata = "<DIDL-Lite><next/></DIDL-Lite>";
    PlaybackCommand setNext;
    setNext.type = PlaybackCommandType::SetNext;
    setNext.media = next;
    check(session.submit(setNext).queued,
          "protocol-next test queues next media");
    playback.processAll();

    // This bypasses DlnaSession::submit, modelling QML, SMTC or automatic
    // queue advance calling PlaybackController directly.
    PlaybackCommand uiNext;
    uiNext.type = PlaybackCommandType::Next;
    check(playback.submit(uiNext).queued,
          "UI-originated next is submitted directly to playback");
    playback.processAll();

    const auto snapshot = session.snapshot().playback;
    check(snapshot.current.uri == next.uri
              && snapshot.current.metadata == next.metadata
              && !snapshot.hasNext,
          "UI-originated next advances the Protocol raw media record");
}

void testOpenCommitsOnlyAfterReady()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});

    MediaRequest current;
    current.uri = "http://example.test/ready-current.mp4";
    current.metadata = "<DIDL-Lite><current/></DIDL-Lite>";
    check(session.openMedia(current, SessionTime{0}).queued,
          "ready commit test opens current media");
    playback.processAll();
    check(session.snapshot().playback.current.uri.empty(),
          "Open is not committed while playback is still preparing");
    playback.markReady();
    check(session.snapshot().playback.current.uri == current.uri
              && session.snapshot().playback.current.metadata
                  == current.metadata,
          "Open commits its raw Protocol record after playback is ready");

    MediaRequest next;
    next.uri = "http://example.test/ready-next.mp4";
    PlaybackCommand setNext;
    setNext.type = PlaybackCommandType::SetNext;
    setNext.media = next;
    check(session.submit(setNext).queued,
          "ready commit test queues next media");
    playback.processAll();

    session.newMediaRequested(SessionTime{1});
    MediaRequest failing;
    failing.uri = "http://example.test/ready-failure.mp4";
    check(session.openMedia(failing, SessionTime{1}).queued,
          "ready commit test queues failing replacement");
    playback.processAll();
    playback.markReady(false);

    const auto snapshot = session.snapshot().playback;
    check(snapshot.current.uri.empty()
              && snapshot.next.uri.empty(),
          "failed replacement clears media instead of exposing a mismatched current");
}

void testOpenDeadlineDropsLateCompletion()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    MediaRequest media;
    media.uri = "http://example.test/late-open.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "late open test queues media");
    session.tick(SessionTime{20000});
    playback.processAll();
    const auto snapshot = session.snapshot();
    check(snapshot.playback.current.uri.empty()
              && !session.transportTransitionPending(),
          "timed-out Open cannot commit a late playback completion");
}

void testNewMediaBoundaryDropsLateQueueAndShowsProtocolPrevious()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});

    MediaRequest current;
    current.uri = "http://example.test/boundary-current.mp4";
    current.metadata = "<DIDL-Lite><boundary-current/></DIDL-Lite>";
    check(session.openMedia(current, SessionTime{0}).queued,
          "boundary test opens current media");
    playback.processAll();
    playback.markReady();

    MediaRequest next;
    next.uri = "http://example.test/boundary-next.mp4";
    next.metadata = "<DIDL-Lite><boundary-next/></DIDL-Lite>";
    PlaybackCommand setNext;
    setNext.type = PlaybackCommandType::SetNext;
    setNext.media = next;
    check(session.submit(setNext).queued,
          "boundary test queues next media");
    playback.processAll();
    check(session.snapshot().playback.hasNext
              && !session.snapshot().playback.hasPrevious,
          "Protocol exposes only the queue it received");

    PlaybackCommand uiNext;
    uiNext.type = PlaybackCommandType::Next;
    check(playback.submit(uiNext).queued,
          "boundary test advances through the Protocol queue");
    playback.processAll();
    check(session.snapshot().playback.current.uri == next.uri
              && session.snapshot().playback.hasPrevious
              && !session.snapshot().playback.hasNext,
          "Protocol previous becomes available only after a known advance");

    PlaybackCommand lateNext;
    lateNext.type = PlaybackCommandType::SetNext;
    lateNext.media.uri = "http://example.test/boundary-stale.mp4";
    check(session.submit(lateNext).queued,
          "boundary test queues a command before owner change");
    session.newMediaRequested(SessionTime{2});
    playback.processAll();
    check(!session.snapshot().playback.hasNext,
          "new media boundary discards a late SetNext completion");
}

void testTimedOutSetNextCannotAppearLater()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    MediaRequest current;
    current.uri = "http://example.test/timeout-current.mp4";
    check(session.openMedia(current, SessionTime{0}).queued,
          "SetNext timeout test opens current media");
    playback.processAll();
    playback.markReady();

    PlaybackCommand initialNext;
    initialNext.type = PlaybackCommandType::SetNext;
    initialNext.media.uri = "http://example.test/timeout-initial-next.mp4";
    check(session.submit(initialNext).queued,
          "SetNext timeout test establishes an existing next");
    playback.processAll();

    PlaybackCommand setNext;
    setNext.type = PlaybackCommandType::SetNext;
    setNext.media.uri = "http://example.test/timeout-next.mp4";
    const auto result = session.submitAndWait(
        std::move(setNext), SessionTime{1});
    check(result.timedOut,
          "SetNext wait reports timeout");
    playback.processAll();
    check(session.snapshot().playback.next.uri
              == "http://example.test/timeout-initial-next.mp4",
          "timed-out SetNext cannot replace the last valid next record");
}

void testCastSessionInfoTracksRegistration()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    session.observeHttpClient(
        "BubbleUPnP 4.6.7.1", "192.168.10.3");
    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{0});
    auto info = session.snapshot().castSession;
    check(info.connected && info.remoteControlRegistered
              && info.protocol == "DLNA"
              && info.controlPointName == "BubbleUPnP 4.6.7.1"
              && info.controlPointAddress == "192.168.10.3",
          "DLNA session reports a registered control point");
    check(session.snapshot().playback.castSession.protocol == "DLNA",
          "cast session is included in the authoritative snapshot");

    session.observeHttpClient(
        "Other Control", "192.168.10.3");
    info = session.snapshot().castSession;
    check(info.controlPointName == "BubbleUPnP 4.6.7.1",
          "same-address request cannot replace the active control point name");
    session.newMediaRequested(
        SessionTime{1}, "Accepted Control", "192.168.10.3");
    info = session.snapshot().castSession;
    check(info.controlPointName == "Accepted Control",
          "accepted media can commit the new control point identity");

    session.setSubscriptionCount(
        0, SubscriptionChangeReason::ExplicitRemoval,
        SessionTime{2});
    info = session.snapshot().castSession;
    check(!info.connected && !info.remoteControlRegistered,
          "control point removal clears the session");
}

void testExplicitUnsubscribeEndsCastMedia()
{
    FakePlaybackGateway playback;
    int goingAway = 0;
    DlnaSession session(playback, {
        {},
        {},
        {},
        [&] { ++goingAway; },
        {},
        {},
        {},
        {},
        {},
    });

    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{0});
    session.newMediaRequested(SessionTime{0});
    MediaRequest media;
    media.uri = "http://example.test/unsubscribe.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "explicit-unsubscribe test opens media");
    playback.processAll();
    playback.markReady();
    check(playback.snapshot().state == PlaybackState::Playing,
          "explicit-unsubscribe test is playing before removal");

    session.setSubscriptionCount(
        0, SubscriptionChangeReason::ExplicitRemoval,
        SessionTime{100});
    playback.processAll();
    const auto snapshot = session.snapshot();
    check(!snapshot.peerConnected
              && snapshot.playback.state == PlaybackState::NoMedia
              && !snapshot.ending && goingAway == 1,
          "explicit unsubscribe ends and unloads the cast session");
    session.peerActivity(SessionTime{150}, "192.168.10.3");
    check(!session.snapshot().peerConnected,
          "a polling query cannot revive an explicit subscription teardown");

    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{200});
    check(session.snapshot().peerConnected,
          "a new subscription can reconnect after explicit removal");
}

void testSubscriptionDuringEndReconnectsAfterCleanup()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    session.observeHttpClient(
        "Xiaohongshu", "192.168.10.3");
    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{0});
    session.newMediaRequested(SessionTime{0});
    MediaRequest media;
    media.uri = "http://example.test/resubscribe-during-end.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "resubscribe-during-end test opens media");
    playback.processAll();
    playback.markReady();

    session.setSubscriptionCount(
        0, SubscriptionChangeReason::ExplicitRemoval,
        SessionTime{100});
    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{101});
    session.peerActivity(SessionTime{102}, "192.168.10.3");
    playback.processAll();

    const auto snapshot = session.snapshot();
    check(snapshot.subscriptionCount == 1
              && snapshot.peerConnected
              && snapshot.playback.state == PlaybackState::NoMedia
              && snapshot.castSession.controlPointName == "Xiaohongshu"
              && snapshot.castSession.controlPointAddress == "192.168.10.3",
          "a subscription added during teardown reconnects the clean idle session");
    session.peerActivity(SessionTime{103});
    check(session.snapshot().peerConnected,
          "the reconnected subscription keeps its peer session");
}

void testStopIdleGraceEndsSession()
{
    FakePlaybackGateway playback;
    int goingAway = 0;
    DlnaSession session(playback, {
        {},
        [] { return true; },
        {},
        [&] { ++goingAway; },
        {},
        {},
        {},
        {},
        {},
    });
    check(session.start(), "stop-idle test starts network");
    MediaRequest media;
    media.uri = "http://example.test/stop-idle.mp4";
    session.newMediaRequested(SessionTime{0});
    check(session.openMedia(media, SessionTime{0}).queued,
          "stop-idle test opens media");
    playback.processAll();
    playback.markReady();

    session.transportStopped(SessionTime{100});
    session.tick(SessionTime{8099});
    check(session.snapshot().peerConnected,
          "stop-idle grace does not disconnect early");
    session.peerActivity(SessionTime{8200});
    session.tick(SessionTime{9000});
    check(session.snapshot().peerConnected,
          "follow-up control traffic cancels stop-idle grace");

    session.transportStopped(SessionTime{10000});
    session.tick(SessionTime{18000});
    check(session.snapshot().peerConnected,
          "Stop boundary leaves a short slot for an arriving new URI");
    session.tick(SessionTime{18250});
    playback.processAll();
    const auto snapshot = session.snapshot();
    check(!snapshot.peerConnected
              && snapshot.playback.state == PlaybackState::NoMedia
              && goingAway == 1,
          "idle Stop ends the session and clears media");
}

void testNewMediaWinsAtStopBoundary()
{
    FakePlaybackGateway playback;
    int goingAway = 0;
    DlnaSession session(playback, {
        {},
        [] { return true; },
        {},
        [&] { ++goingAway; },
        {},
        {},
        {},
        {},
        {},
    });
    check(session.start(), "Stop-boundary media test starts");
    MediaRequest first;
    first.uri = "http://example.test/boundary-first.mp4";
    session.newMediaRequested(
        SessionTime{0}, "Owner", "192.168.10.3");
    check(session.openMedia(first, SessionTime{0}).queued,
          "Stop-boundary test opens first media");
    playback.processAll();
    playback.markReady();
    session.transportStopped(SessionTime{100}, "192.168.10.3");
    session.tick(SessionTime{8100});
    check(session.snapshot().peerConnected,
          "Stop boundary is settling, not closed");

    MediaRequest replacement;
    replacement.uri = "http://example.test/boundary-next.mp4";
    session.newMediaRequested(
        SessionTime{8150}, "Owner", "192.168.10.3");
    check(session.openMedia(replacement, SessionTime{8150}).queued,
          "new media is accepted at the Stop boundary");
    session.tick(SessionTime{9000});
    playback.processAll();
    const auto snapshot = session.snapshot();
    check(snapshot.peerConnected && goingAway == 0,
          "new media cancels the pending idle disconnect");
}

void testAbandonEndingSessionIsImmediate()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    MediaRequest media;
    media.uri = "http://example.test/abandon-end.mp4";
    session.newMediaRequested(
        SessionTime{0}, "Owner", "192.168.10.3");
    check(session.openMedia(media, SessionTime{0}).queued,
          "abandon test opens media");
    playback.processAll();
    playback.markReady();
    check(session.endSession(SessionTime{100}).queued,
          "abandon test queues session end");
    session.abandonEndingSession();
    const auto snapshot = session.snapshot();
    check(!snapshot.ending && snapshot.disconnectedByUs
              && !snapshot.peerConnected,
          "deadline fallback can abandon a stuck EndSession immediately");
}

void testOwnerEpochRejectsLateControl()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    MediaRequest first;
    first.uri = "http://example.test/epoch-first.mp4";
    session.newMediaRequested(
        SessionTime{0}, "Owner", "192.168.10.3");
    check(session.openMedia(first, SessionTime{0}).queued,
          "epoch test opens first media");
    playback.processAll();
    playback.markReady();
    const std::uint64_t ownerEpoch = session.protocolEpoch();

    MediaRequest second;
    second.uri = "http://example.test/epoch-second.mp4";
    session.newMediaRequested(
        SessionTime{1}, "Owner", "192.168.10.3");
    PlaybackCommand pause;
    pause.type = PlaybackCommandType::Pause;
    const auto stale = session.submit(pause, ownerEpoch);
    check(!stale.queued
              && stale.status == PlaybackSubmissionStatus::StaleGeneration,
          "a control command cannot cross a completed media boundary");
}

void testStopIdleGraceBelongsToCurrentOwner()
{
    FakePlaybackGateway playback;
    int goingAway = 0;
    DlnaSession session(playback, {
        {},
        [] { return true; },
        {},
        [&] { ++goingAway; },
        {},
        {},
        {},
        {},
        {},
    });
    check(session.start(), "owner-aware stop test starts");
    session.observeHttpClient(
        "Owner", "192.168.10.3");
    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{0});
    MediaRequest media;
    media.uri = "http://example.test/owner-stop.mp4";
    session.newMediaRequested(
        SessionTime{0}, "Owner", "192.168.10.3");
    check(session.openMedia(media, SessionTime{0}).queued,
          "owner-aware stop test opens media");
    playback.processAll();
    playback.markReady();

    session.transportStopped(SessionTime{100}, "192.168.10.3");
    // Neither another address nor a GENA subscription renewal is activity
    // from the owner and therefore neither may extend the Stop deadline.
    session.peerActivity(SessionTime{7200}, "192.168.10.99");
    session.setSubscriptionCount(
        2, SubscriptionChangeReason::Renewed, SessionTime{7300});
    session.observeHttpClient(
        "Intruder", "192.168.10.99");
    session.tick(SessionTime{8200});
    check(session.snapshot().peerConnected,
          "Stop boundary settles before ending");
    session.tick(SessionTime{8450});
    playback.processAll();

    const auto snapshot = session.snapshot();
    check(!snapshot.peerConnected
              && snapshot.playback.state == PlaybackState::NoMedia
              && goingAway == 1,
          "foreign traffic cannot keep the previous owner's Stop grace alive");
    check(snapshot.castSession.controlPointAddress == "192.168.10.3"
              || !snapshot.castSession.connected,
          "foreign query traffic does not steal the active owner identity");
}

void testQueuedOpenPause()
{
    FakePlaybackGateway playback;
    bool accepting = false;
    DlnaSession session(playback, {
        {},
        [] { return true; },
        {},
        {},
        [&](bool enabled) { accepting = enabled; },
        {},
        {},
        {},
        {},
    });
    check(session.start() && accepting, "queued pause test starts");
    MediaRequest media;
    media.uri = "http://example.test/pause-open.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "queued pause test open queued");
    session.pauseAccepting(SessionTime{1});
    playback.processAll();
    check(!accepting && !session.snapshot().accepting
              && playback.snapshot().state == PlaybackState::NoMedia,
          "pause waits for a queued Open before closing receiving");
}

void testCallbacksRunOutsideLock()
{
    FakePlaybackGateway playback;
    std::atomic_bool callbackStarted{false};
    std::atomic_bool snapshotFinished{false};
    std::atomic_bool callbackTimedOut{false};
    DlnaSession session(playback, {
        {},
        {},
        {},
        {},
        {},
        {},
        [&] {
            callbackStarted = true;
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!snapshotFinished.load()
                   && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::yield();
            }
            if (!snapshotFinished.load())
                callbackTimedOut = true;
        },
        {},
        {},
    });
    std::jthread snapshotThread([&] {
        while (!callbackStarted.load())
            std::this_thread::yield();
        static_cast<void>(session.snapshot());
        snapshotFinished = true;
    });
    PlaybackEvent event;
    event.kind = PlaybackEventKind::SnapshotChanged;
    event.snapshot = playback.snapshot();
    session.handlePlaybackEvent(event);
    snapshotThread.join();
    check(!callbackTimedOut.load() && snapshotFinished.load(),
          "external callback does not run while the session lock is held");
}

void testOpenDeadlineClearsAfterSuccess()
{
    FakePlaybackGateway playback;
    int failures = 0;
    DlnaSession session(playback, {
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        [&](const std::string &) { ++failures; },
        {},
    });
    MediaRequest media;
    media.uri = "http://example.test/deadline.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "deadline test open queued");
    playback.processAll();
    playback.markReady();
    session.tick(SessionTime{20000});
    check(failures == 0
              && playback.snapshot().state == PlaybackState::Playing,
          "successful open clears its deadline");
}

void testPreparingSnapshotDoesNotClearOpenDeadline()
{
    FakePlaybackGateway playback;
    int failures = 0;
    DlnaSession session(playback, {
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        [&](const std::string &) { ++failures; },
        {},
    });
    MediaRequest media;
    media.uri = "http://example.test/snapshot-first.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "snapshot deadline test open queued");
    playback.processAll();
    PlaybackEvent event;
    event.kind = PlaybackEventKind::SnapshotChanged;
    event.snapshot = playback.snapshot();
    event.snapshot.state = PlaybackState::Preparing;
    ++event.snapshot.stateVersion;
    session.handlePlaybackEvent(event);
    session.tick(SessionTime{20000});
    check(failures == 1
              && session.snapshot().playback.current.uri.empty()
              && !session.transportTransitionPending(),
          "Preparing alone does not clear or commit the Open transaction");
}

void testStoppedBeforeReadyCancelsOpen()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    MediaRequest media;
    media.uri = "http://example.test/stopped-before-ready.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "stopped-before-ready test queues Open");
    playback.processAll();

    PlaybackEvent event;
    event.kind = PlaybackEventKind::SnapshotChanged;
    event.snapshot = playback.snapshot();
    event.snapshot.state = PlaybackState::Stopped;
    event.snapshot.loadStatus = LoadStatus::Ok;
    ++event.snapshot.stateVersion;
    session.handlePlaybackEvent(event);

    check(session.snapshot().playback.current.uri.empty()
              && !session.transportTransitionPending(),
          "Stopped before readiness revokes the Open transaction");
}

void testMediaBoundaryIgnoresOldUiSnapshot()
{
    FakePlaybackGateway playback;
    DlnaSession session(playback, {});
    session.newMediaRequested(SessionTime{0});

    PlaybackEvent stale;
    stale.kind = PlaybackEventKind::SnapshotChanged;
    stale.snapshot.current.uri =
        "http://example.test/previous-owner.mp4";
    stale.snapshot.state = PlaybackState::Playing;
    stale.snapshot.stateVersion = 1;
    session.handlePlaybackEvent(stale);

    check(session.snapshot().playback.current.uri.empty(),
          "old Ui snapshot cannot cross a new media boundary");
}

void testPauseCancelsOpenBeforeSubmission()
{
    FakePlaybackGateway playback;
    std::atomic_bool uiEntered{false};
    std::atomic_bool releaseUi{false};
    DlnaSession session(playback, {
        [&] {
            uiEntered = true;
            while (!releaseUi.load())
                std::this_thread::yield();
        },
        [] { return true; },
        {},
        {},
        {},
        {},
        {},
        {},
        {},
    });
    check(session.start(), "open submission cancellation test starts");
    MediaRequest media;
    media.uri = "http://example.test/cancelled-before-submit.mp4";
    PlaybackCommandSubmission submission;
    std::jthread opener([&] {
        submission = session.openMedia(media, SessionTime{1});
    });
    while (!uiEntered.load())
        std::this_thread::yield();
    session.pauseAccepting(SessionTime{2});
    check(session.snapshot().accepting,
          "pause waits while an open intent is awaiting submission");
    releaseUi = true;
    opener.join();
    check(!submission.queued
              && submission.status
                  == PlaybackSubmissionStatus::SessionEnding,
          "cancelled open intent is rejected after UI activation");
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::NoMedia
              && !session.snapshot().accepting,
          "cancelled open cannot revive after pause completes");
}

void testPauseCancelsOpenDuringGatewaySnapshot()
{
    FakePlaybackGateway inner;
    SnapshotBlockingGateway playback(inner);
    std::atomic_bool pauseDisconnected{false};
    DlnaSession session(playback, {
        {},
        [] { return true; },
        {},
        {},
        {},
        [&](bool connected) {
            if (!connected)
                pauseDisconnected = true;
        },
        {},
        {},
        {},
    });
    check(session.start(), "gateway snapshot cancellation test starts");
    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{0});
    inner.processAll();
    check(session.snapshot().peerConnected,
          "gateway snapshot cancellation test connects peer");

    playback.blockNextSnapshot();
    MediaRequest media;
    media.uri = "http://example.test/cancelled-during-snapshot.mp4";
    PlaybackCommandSubmission opening;
    std::jthread opener([&] {
        opening = session.openMedia(media, SessionTime{1});
    });
    while (!playback.snapshotEntered())
        std::this_thread::yield();

    std::jthread pauser([&] {
        session.pauseAccepting(SessionTime{2});
    });
    while (!pauseDisconnected.load())
        std::this_thread::yield();
    playback.releaseSnapshot();
    opener.join();
    pauser.join();
    check(!opening.queued
              && opening.status
                  == PlaybackSubmissionStatus::SessionEnding,
          "open is rejected after Pause cancels it around a gateway snapshot");
    inner.processAll();
    check(inner.snapshot().state == PlaybackState::NoMedia
              && !session.snapshot().accepting,
          "open cannot be submitted after Pause invalidates its token");
}

void testPauseRetriesRejectedEndAfterQueuedOpen()
{
    FakePlaybackGateway inner;
    SnapshotBlockingGateway playback(inner);
    DlnaSession session(playback, {
        {},
        [] { return true; },
        {},
        {},
        {},
        {},
        {},
        {},
        {},
    });
    check(session.start(), "EndSession retry test starts");
    MediaRequest media;
    media.uri = "http://example.test/end-retry.mp4";
    check(session.openMedia(media, SessionTime{0}).queued,
          "EndSession retry test queues Open");
    playback.failNextEndSubmission();
    session.pauseAccepting(SessionTime{1});
    inner.processAll();
    check(inner.snapshot().state == PlaybackState::NoMedia
              && !session.snapshot().accepting,
          "Pause waits for in-flight Open and retries a rejected EndSession");
}

} // namespace

int main()
{
    testConcurrentSessionAccess();
    testRejectedQueuedCommand();
    testSubmitAndWaitObservesAppliedState();
    testUiOriginatedNextAdvancesProtocolRecord();
    testOpenCommitsOnlyAfterReady();
    testOpenDeadlineDropsLateCompletion();
    testNewMediaBoundaryDropsLateQueueAndShowsProtocolPrevious();
    testTimedOutSetNextCannotAppearLater();
    testCastSessionInfoTracksRegistration();
    testExplicitUnsubscribeEndsCastMedia();
    testSubscriptionDuringEndReconnectsAfterCleanup();
    testStopIdleGraceEndsSession();
    testNewMediaWinsAtStopBoundary();
    testAbandonEndingSessionIsImmediate();
    testOwnerEpochRejectsLateControl();
    testStopIdleGraceBelongsToCurrentOwner();
    testQueuedOpenPause();
    testCallbacksRunOutsideLock();
    testOpenDeadlineClearsAfterSuccess();
    testPreparingSnapshotDoesNotClearOpenDeadline();
    testStoppedBeforeReadyCancelsOpen();
    testMediaBoundaryIgnoresOldUiSnapshot();
    testPauseCancelsOpenBeforeSubmission();
    testPauseCancelsOpenDuringGatewaySnapshot();
    testPauseRetriesRejectedEndAfterQueuedOpen();
    FakePlaybackGateway playback;
    int uiRequests = 0;
    int startCalls = 0;
    int stopCalls = 0;
    int goingAwayCalls = 0;
    int networkChanges = 0;
    int peerChanges = 0;
    int stateChanges = 0;
    int failedRequests = 0;
    bool startResult = false;
    bool networkEnabled = false;
    bool peerEnabled = false;
    bool goingAwayAfterNoMedia = false;
    int goingAwayStateChanges = 0;
    DlnaSession session(playback, {
        [&] { ++uiRequests; },
        [&] {
            ++startCalls;
            return startResult;
        },
        [&] { ++stopCalls; },
        [&] {
            ++goingAwayCalls;
            goingAwayAfterNoMedia =
                playback.snapshot().state == PlaybackState::NoMedia;
            goingAwayStateChanges = stateChanges;
        },
        [&](bool enabled) {
            ++networkChanges;
            networkEnabled = enabled;
        },
        [&](bool enabled) {
            ++peerChanges;
            peerEnabled = enabled;
        },
        [&] { ++stateChanges; },
        [&](const std::string &) { ++failedRequests; },
        [](const std::string &) {},
    });
    DlnaSessionSnapshot snapshot = session.snapshot();
    check(!snapshot.accepting && !snapshot.running, "initial network state");
    check(!snapshot.peerConnected, "initial peer state");
    check(!session.start(), "network start failure is reported");
    check(startCalls == 1 && networkChanges == 1 && !networkEnabled,
          "failed start callbacks");

    startResult = true;
    check(session.start(), "session starts after network success");
    snapshot = session.snapshot();
    check(snapshot.accepting && snapshot.running, "network started");
    check(networkEnabled, "network callback");

    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{100});
    check(session.snapshot().peerConnected, "subscription connects peer");
    check(peerEnabled, "peer callback");
    playback.processAll();
    check(playback.snapshot().peerConnected, "peer command applied");

    playback.simulatePlayerLost();
    MediaRequest media;
    media.uri = "http://example.test/movie.mp4";
    media.title = "Movie";
    const auto waiting = session.openMedia(media, SessionTime{200});
    check(waiting.queued
              && waiting.status
                  == PlaybackSubmissionStatus::WaitingForPlayer,
          "open waits for Playback readiness");
    check(uiRequests == 1, "open requests UI before Playback is ready");
    check(playback.snapshot().state == PlaybackState::NoMedia,
          "open is not applied while Playback is unavailable");
    playback.simulateReconnect();
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::Preparing,
          "pending open is submitted after reconnect");
    playback.markReady();
    check(stateChanges > 0, "playback state event observed");

    session.setSubscriptionCount(
        0, SubscriptionChangeReason::ExplicitRemoval, SessionTime{300});
    check(!session.snapshot().peerConnected,
          "explicit unsubscribe disconnects immediately");
    playback.processAll();
    snapshot = session.snapshot();
    check(snapshot.playback.state == PlaybackState::NoMedia,
          "explicit unsubscribe unloads the previous cast media");

    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{400});
    check(session.snapshot().peerConnected,
          "new subscription reconnects after explicit unsubscribe");
    playback.processAll();
    session.setSubscriptionCount(
        0, SubscriptionChangeReason::Unreachable, SessionTime{1000});
    snapshot = session.snapshot();
    check(snapshot.peerConnected && snapshot.peerGraceActive,
          "unreachable peer enters grace");
    session.peerActivity(SessionTime{1001});
    snapshot = session.snapshot();
    check(snapshot.peerConnected && !snapshot.peerGraceActive,
          "any peer activity cancels the unreachable grace period");
    session.setSubscriptionCount(
        0, SubscriptionChangeReason::Unreachable, SessionTime{1002});
    session.tick(SessionTime{60999});
    check(session.snapshot().peerConnected, "grace not expired early");
    session.tick(SessionTime{61002});
    check(!session.snapshot().peerConnected, "grace expiry disconnects");
    playback.processAll();

    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Renewed, SessionTime{62000});
    check(session.snapshot().peerConnected, "renewal reconnects");
    playback.processAll();

    const auto ended = session.endSession(SessionTime{63000});
    check(ended.queued, "end session queued");
    snapshot = session.snapshot();
    check(snapshot.disconnectedByUs && !snapshot.peerConnected,
          "local end disconnects immediately");
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::NoMedia,
          "local end clears media");
    check(goingAwayCalls == 3 && stopCalls == 0,
          "local end announces byebye without stopping network");

    session.peerActivity(SessionTime{64000});
    check(!session.snapshot().peerConnected,
          "queries do not clear an explicit local disconnect");
    session.newMediaRequested(SessionTime{64000});
    check(session.snapshot().peerConnected,
          "a new media request starts the next session");
    playback.processAll();

    const auto reopened = session.openMedia(media, SessionTime{65000});
    check(reopened.queued, "reopen queued");
    playback.processAll();
    const int stateChangesBeforePause = stateChanges;
    session.pauseAccepting(SessionTime{66000});
    snapshot = session.snapshot();
    check(snapshot.accepting && snapshot.running,
          "network stays up until pause is confirmed");
    check(goingAwayCalls == 3 && stopCalls == 0,
          "pause waits for playback completion");
    const int uiRequestsBeforeBlockedOpen = uiRequests;
    const auto blockedOpen =
        session.openMedia(media, SessionTime{66500});
    check(!blockedOpen.queued
              && blockedOpen.status
                  == PlaybackSubmissionStatus::SessionEnding,
          "new open is rejected while pause is ending");
    check(uiRequests == uiRequestsBeforeBlockedOpen,
          "rejected open does not request UI");
    check(!session.resumeAccepting(),
          "resume is rejected while pause is in flight");
    playback.processAll();
    snapshot = session.snapshot();
    check(!snapshot.accepting && !snapshot.running,
          "pause hides network after confirmation");
    check(!networkEnabled, "pause network callback");
    check(snapshot.disconnectedByUs && !snapshot.peerConnected,
          "pause disconnects peer");
    check(goingAwayCalls == 4 && stopCalls == 1,
          "byebye precedes network stop");
    check(goingAwayAfterNoMedia,
          "byebye happens after playback reaches NoMedia");
    check(goingAwayStateChanges > stateChangesBeforePause,
          "stateChanged publishes NoMedia before network stop");

    check(session.resumeAccepting(), "resume accepting");
    snapshot = session.snapshot();
    check(snapshot.accepting && snapshot.running, "network resumed");
    check(networkEnabled, "resume network callback");

    PlaybackCommand residualNext;
    residualNext.type = PlaybackCommandType::SetNext;
    residualNext.media = media;
    check(session.submit(residualNext).queued,
          "next can be set while NoMedia");
    PlaybackCommand residualMode;
    residualMode.type = PlaybackCommandType::SetPlayMode;
    residualMode.playMode = PlayMode::RepeatAll;
    check(session.submit(residualMode).queued,
          "play mode can be set while NoMedia");
    playback.processAll();
    check(playback.snapshot().hasNext
              && playback.snapshot().playMode == PlayMode::RepeatAll,
          "residual queue is present");
    const auto noMediaEnd = session.endSession(SessionTime{68000});
    check(noMediaEnd.queued && noMediaEnd.commandId != 0,
          "NoMedia end still submits a real command");
    playback.processAll();
    check(!playback.snapshot().hasNext
              && playback.snapshot().playMode == PlayMode::Normal,
          "NoMedia end clears residual queue and play mode");
    check(goingAwayCalls == 5 && stopCalls == 1,
          "NoMedia end announces without stopping network");

    playback.simulatePlayerLost();
    check(!playback.snapshot().playerAvailable, "player loss reflected");
    check(session.snapshot().playback.state == PlaybackState::NoMedia,
          "player loss leaves no media");
    const int failuresBeforeCancel = failedRequests;
    const auto pendingBeforePause =
        session.openMedia(media, SessionTime{70000});
    check(pendingBeforePause.status
              == PlaybackSubmissionStatus::WaitingForPlayer,
          "open waits while player is unavailable");
    session.pauseAccepting(SessionTime{71000});
    check(failedRequests == failuresBeforeCancel + 1,
          "pause cancels pending open");
    check(!session.snapshot().accepting,
          "pause with no media stops immediately");
    check(session.resumeAccepting(), "resume after immediate pause");
    playback.simulateReconnect();
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::NoMedia,
          "cancelled pending open does not revive after reconnect");

    (void)peerChanges;
    (void)failedRequests;
    std::cout << "DLNA session tests passed\n";
    return 0;
}
