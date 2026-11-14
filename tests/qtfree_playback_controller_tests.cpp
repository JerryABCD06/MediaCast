// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"
#include "QtFree/MediaPlayer.h"
#include "QtFree/PlaybackController.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace mcast::avalonia::qtfree;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakePlayer final : public MediaPlayer
{
public:
    void load(const MediaLoadRequest &request) override
    {
        loads.push_back(request);
        if (emitLoadStarted)
            loadStarted.emit(request.loadGeneration);
        if (emitReady)
            ready.emit(request.loadGeneration);
    }
    void play() override { ++playCalls; }
    void pause() override { ++pauseCalls; }
    void stop() override { ++stopCalls; }
    void unload() override { ++unloadCalls; }
    void seekTo(double seconds) override { seek = seconds; }
    void setVolumePercent(int percent) override { volume = percent; }
    void setMuted(bool value) override { muted = value; }
    double positionSeconds() const override { return position; }
    double durationSeconds() const override { return duration; }
    std::int64_t mediaSizeBytes() const override { return size; }
    int volumePercent() const override { return volume; }
    bool isMuted() const override { return muted; }
    std::vector<PictureControlInfo> pictureControls() const override
    {
        return {{"brightness", -100, 100, 0}};
    }
    bool setPictureControl(
        const std::string &name, int value) override
    {
        if (name != "brightness")
            return false;
        brightness = value;
        pictureControlChanged.emit(name, value);
        return true;
    }
    int pictureControlValue(const std::string &name) const override
    {
        return name == "brightness" ? brightness : 0;
    }

    bool emitLoadStarted = false;
    bool emitReady = false;
    int playCalls = 0;
    int pauseCalls = 0;
    int stopCalls = 0;
    int unloadCalls = 0;
    double seek = 0;
    double position = 12.5;
    double duration = 120;
    std::int64_t size = 4096;
    int volume = 70;
    bool muted = false;
    int brightness = 0;
    std::vector<MediaLoadRequest> loads;
};

MediaRequest media(
    std::string uri, std::string title,
    MediaKind kind = MediaKind::Video)
{
    MediaRequest request;
    request.uri = std::move(uri);
    request.title = std::move(title);
    request.kind = kind;
    request.protocol = "DLNA";
    return request;
}

} // namespace

int main()
{
    EventLoop loop;
    check(loop.start(), "event loop starts");
    FakePlayer player;
    PlaybackController controller(&player, loop);
    check(controller.state() == PlaybackController::State::NoMedia,
          "controller starts without media");

    int queueSignals = 0;
    auto queueConnection = controller.queueChanged.connect(
        [&](bool, bool, const std::string &,
            PlaybackController::PlayMode) {
            ++queueSignals;
        });

    player.emitLoadStarted = true;
    player.emitReady = true;
    controller.openUri(
        media("file:///A.mp4", "Movie A"), MediaSource::Cast);
    check(controller.state() == PlaybackController::State::Playing,
          "ready transitions Preparing to Playing");
    check(controller.hasMedia() && !controller.isIdle(),
          "playing media is not idle");
    check(controller.nowPlaying().title == "Movie A",
          "protocol title reaches NowPlaying");
    check(player.loads.size() == 1
              && player.loads[0].logicalUri == "file:///A.mp4",
          "neutral load plan reaches player");

    controller.pause();
    check(controller.state() == PlaybackController::State::Paused,
          "pause updates state");
    controller.togglePlayPause();
    check(controller.state() == PlaybackController::State::Playing,
          "toggle resumes from pause");
    controller.stop();
    check(controller.state() == PlaybackController::State::Stopped
              && controller.hasMedia(),
          "stop retains media");
    controller.togglePlayPause();
    check(controller.state() == PlaybackController::State::Playing,
          "toggle plays from stopped");

    controller.setNextUri(media("file:///B.mp4", "Movie B"));
    check(controller.hasNext(), "next becomes available");
    controller.next();
    check(controller.currentUri() == "file:///B.mp4"
              && !controller.hasNext()
              && controller.hasPrevious(),
          "next shifts queue exactly once");
    controller.previous();
    check(controller.currentUri() == "file:///A.mp4"
              && controller.hasNext()
              && !controller.hasPrevious(),
          "previous shifts queue back");
    check(queueSignals >= 3, "queue changes are observable");

    controller.seekTo(30);
    controller.setVolumePercent(40);
    controller.setMuted(true);
    check(player.seek == 30 && player.volume == 40 && player.muted,
          "commands pass through unchanged");
    check(controller.setPictureControl("brightness", 15)
              && player.brightness == 15,
          "picture value reaches player");

    controller.setCastSession({
        true,
        "DLNA",
        "Phone",
        "127.0.0.1",
        true,
    });
    check(
        controller.peerConnected()
            && controller.castState()
                == PlaybackController::CastState::ViewerVideo,
        "cast state derives from peer and media kind");
    controller.setPeerConnected(false);
    check(
        !controller.peerConnected()
            && controller.state() == PlaybackController::State::NoMedia
            && player.unloadCalls == 1,
        "peer loss ends cast session");

    controller.openUri(
        media("file:///C.mp3", "C", MediaKind::Audio),
        MediaSource::Cast);
    check(controller.nowPlaying().kind == MediaKind::Audio,
          "audio kind survives projection");
    controller.endSession();
    check(controller.state() == PlaybackController::State::NoMedia,
          "end session clears media");

    queueConnection.disconnect();
    loop.stop();
    std::cout << "Qt-free playback controller tests passed\n";
    return 0;
}
