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

void testOpenDeadlineClearsOnProgressSnapshot()
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
    PlaybackEvent event;
    event.kind = PlaybackEventKind::SnapshotChanged;
    event.snapshot = playback.snapshot();
    event.snapshot.state = PlaybackState::Preparing;
    event.snapshot.playerAvailable = true;
    ++event.snapshot.stateVersion;
    session.handlePlaybackEvent(event);
    session.tick(SessionTime{20000});
    check(failures == 0,
          "a progress snapshot clears the open deadline before command ack");
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
    testQueuedOpenPause();
    testCallbacksRunOutsideLock();
    testOpenDeadlineClearsAfterSuccess();
    testOpenDeadlineClearsOnProgressSnapshot();
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

    session.setSubscriptionCount(
        1, SubscriptionChangeReason::Added, SessionTime{400});
    check(session.snapshot().peerConnected, "new subscription reconnects");
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
    check(goingAwayCalls == 1 && stopCalls == 0,
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
    check(goingAwayCalls == 1 && stopCalls == 0,
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
    check(goingAwayCalls == 2 && stopCalls == 1,
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
    check(goingAwayCalls == 3 && stopCalls == 1,
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
