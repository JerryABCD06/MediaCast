// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "core/MediaPlayer.h"
#include "core/PlaybackController.h"
#include "ipc/NamedPipe.h"
#include "ipc/PlaybackPipeGateway.h"
#include "ui_process/PlaybackBridge.h"

#include <QCoreApplication>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mcast::playback;
using namespace std::chrono_literals;
namespace types = mcast::playback;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

class FakeMediaPlayer final : public MediaPlayer
{
public:
    void load(const QString &uri) override
    {
        m_uri = uri;
        emit loadStarted();
        emit ready();
    }
    void play() override { m_paused = false; emit pausedChanged(false); }
    void pause() override { m_paused = true; emit pausedChanged(true); }
    void stop() override { m_paused = false; emit ended(); }
    void seekTo(double seconds) override
    {
        m_position = seconds;
        emit positionChanged(m_position);
    }
    void setVolumePercent(int percent) override
    {
        m_volume = percent;
        emit volumeChanged(percent);
    }
    void setMuted(bool muted) override
    {
        m_muted = muted;
        emit muteChanged(muted);
    }
    double positionSeconds() const override { return m_position; }
    double durationSeconds() const override { return 120.0; }
    qint64 mediaSizeBytes() const override { return 4096; }
    int volumePercent() const override { return m_volume; }
    bool isMuted() const override { return m_muted; }
    void setVideoWindow(quintptr) override {}
    QVector<PictureControlInfo> pictureControls() const override
    {
        return {{QStringLiteral("brightness"), 0, 100, 50}};
    }
    bool setPictureControl(const QString &name, int value) override
    {
        if (name != QStringLiteral("brightness"))
            return false;
        m_brightness = value;
        emit pictureControlChanged(name, value);
        return true;
    }
    int pictureControlValue(const QString &name) const override
    {
        return name == QStringLiteral("brightness") ? m_brightness : 0;
    }
    void simulateLost() { emit lost(); }

private:
    QString m_uri;
    double m_position = 0.0;
    int m_volume = 100;
    bool m_muted = false;
    bool m_paused = false;
    int m_brightness = 50;
};

struct EventState {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<PlaybackEvent> events;

    bool waitFor(
        const std::function<bool(const std::vector<PlaybackEvent> &)> &match,
        std::chrono::milliseconds timeout = 5s)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] {
            return match(events);
        });
    }
};

bool pumpUntil(
    QCoreApplication &application,
    const std::function<bool()> &condition,
    std::chrono::milliseconds timeout = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        application.processEvents();
        if (condition())
            return true;
        std::this_thread::sleep_for(1ms);
    }
    application.processEvents();
    return condition();
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    const std::wstring pipeName =
        mcast::ipc::makePlaybackPipeName(L"ui-bridge-test");
    mcast::ipc::PipeListener listener(pipeName, 2);
    check(listener.valid(), "listener starts");
    std::string error;
    auto client = mcast::ipc::PipeClient::connect(
        pipeName, 2s, error);
    check(client != nullptr, "client connects");
    auto server = listener.accept(2s, error);
    check(server != nullptr, "server accepts");

    FakeMediaPlayer player;
    PlaybackController controller(&player);
    EventState events;
    PlaybackPipeGateway gateway(
        std::move(server), [](const std::string &) {});
    gateway.addEventListener([&](const PlaybackEvent &event) {
        {
            std::scoped_lock lock(events.mutex);
            events.events.push_back(event);
        }
        events.changed.notify_all();
    });
    mcast::ui_process::PlaybackBridge bridge(
        std::shared_ptr<mcast::ipc::PipeConnection>(std::move(client)),
        controller, 42);

    check(gateway.requestSnapshot(error), "snapshot request");
    check(pumpUntil(application, [&] {
        std::scoped_lock lock(events.mutex);
        return !events.events.empty()
            && events.events.back().kind
                == PlaybackEventKind::SnapshotChanged;
    }), "initial snapshot");
    check(gateway.snapshot().generation == 42, "activation generation");

    PlaybackCommand open;
    open.type = PlaybackCommandType::Open;
    open.expectedGeneration = 42;
    open.media.uri = "http://example.test/movie.mp4";
    open.media.metadata = "<DIDL-Lite/>";
    open.media.title = "Movie";
    open.media.artist = "Artist";
    open.media.album = "Album";
    open.media.description = "Description";
    open.media.protocol = "DLNA";
    open.media.kind = types::MediaKind::Video;
    check(gateway.submit(open).queued, "open queues");
    check(pumpUntil(application, [&] {
        std::scoped_lock lock(events.mutex);
        return std::any_of(
            events.events.begin(), events.events.end(),
            [](const auto &event) {
                return event.kind == PlaybackEventKind::CommandCompleted
                    && event.outcome == PlaybackOutcomeStatus::Applied;
            });
    }), "open command applies");
    check(controller.currentUri()
              == QStringLiteral("http://example.test/movie.mp4"),
          "controller receives open");
    const bool openDelta = pumpUntil(application, [&] {
        std::scoped_lock lock(events.mutex);
        return std::any_of(
            events.events.begin(), events.events.end(),
            [](const auto &event) {
                return event.kind == PlaybackEventKind::SnapshotChanged
                    && event.snapshot.playerAvailable
                    && event.snapshot.current.uri
                        == "http://example.test/movie.mp4";
            });
    });
    if (!openDelta) {
        std::scoped_lock lock(events.mutex);
        for (const auto &event : events.events) {
            std::cerr << "event kind=" << static_cast<int>(event.kind)
                      << " outcome=" << static_cast<int>(event.outcome)
                      << " gen=" << event.snapshot.generation
                      << " version=" << event.snapshot.stateVersion
                      << " available=" << event.snapshot.playerAvailable
                      << " uri=" << event.snapshot.current.uri << '\n';
        }
    }
    check(openDelta, "state delta reports open");
    check(gateway.snapshot().current.title == "Movie"
              && gateway.snapshot().current.artist == "Artist"
              && gateway.snapshot().current.album == "Album"
              && gateway.snapshot().current.description == "Description",
          "full media request survives snapshot");
    check(gateway.snapshot().current.metadata == "<DIDL-Lite/>",
          "metadata survives snapshot");
    check(gateway.snapshot().current.protocol == "DLNA",
          "protocol survives snapshot");
    check(gateway.snapshot().current.kind == types::MediaKind::Video,
          "kind survives snapshot");

    PlaybackCommand volume;
    volume.type = PlaybackCommandType::SetVolume;
    volume.intValue = 35;
    volume.expectedGeneration = 42;
    check(gateway.submit(volume).queued, "volume queues");
    check(pumpUntil(application, [&] {
        return gateway.snapshot().volumePercent == 35;
    }), "volume applies");

    PlaybackCommand picture;
    picture.type = PlaybackCommandType::SetPictureControl;
    picture.text = "brightness";
    picture.intValue = 72;
    picture.expectedGeneration = 42;
    check(gateway.submit(picture).queued, "picture control queues");
    check(pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return !snapshot.pictureControls.empty()
            && snapshot.pictureControls[0].value == 72;
    }), "picture control applies");

    PlaybackCommand setNext;
    setNext.type = PlaybackCommandType::SetNext;
    setNext.expectedGeneration = 42;
    setNext.media.uri = "http://example.test/next.mp4";
    setNext.media.metadata = "<DIDL-Lite><next/></DIDL-Lite>";
    setNext.media.title = "Next";
    setNext.media.kind = types::MediaKind::Video;
    check(gateway.submit(setNext).queued, "set next queues");
    check(pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.next.uri == "http://example.test/next.mp4"
            && snapshot.next.title == "Next";
    }), "complete next request reaches Protocol");

    controller.next();
    check(pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.current.uri == "http://example.test/next.mp4"
            && snapshot.current.title == "Next"
            && snapshot.next.uri.empty();
    }), "controller-driven next updates cache");

    controller.endSession();
    check(pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.state == PlaybackState::NoMedia
            && snapshot.current.uri.empty()
            && snapshot.next.uri.empty();
    }), "controller-driven endSession clears cache");

    player.simulateLost();
    check(pumpUntil(application, [&] {
        std::scoped_lock lock(events.mutex);
        return std::any_of(
            events.events.begin(), events.events.end(),
            [](const auto &event) {
                return event.kind == PlaybackEventKind::PlayerLost
                    && !event.snapshot.playerAvailable
                    && event.snapshot.state == PlaybackState::NoMedia;
            });
    }), "playerLost reaches Protocol");

    std::cout << "Playback bridge tests passed\n";
    return 0;
}
