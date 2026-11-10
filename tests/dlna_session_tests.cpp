// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "protocols/dlna/session/DlnaSession.h"

#include <cstdlib>
#include <iostream>
#include <string>

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

} // namespace

int main()
{
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
    session.tick(SessionTime{60999});
    check(session.snapshot().peerConnected, "grace not expired early");
    session.tick(SessionTime{61000});
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
    check(session.snapshot().peerConnected,
          "peer activity clears local disconnect");
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
