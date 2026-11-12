// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "core/MediaPlayer.h"
#include "core/PlaybackController.h"
#include "ipc/NamedPipe.h"
#include "ipc/PlaybackPipeGateway.h"
#include "ui_process/PlaybackBridge.h"

#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
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
    void load(const MediaLoadRequest &request) override
    {
        m_uri = request.logicalUri;
        m_loadGeneration = request.loadGeneration;
        m_lastRoute = request.route;
        m_lastFrameCount = request.imageSequenceFrames.size();
        ++m_loadCalls;
        emit loadStarted(m_loadGeneration);
        if (m_autoReady)
            emit ready(m_loadGeneration);
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
    void simulateMetadata(
        const QVariantMap &tags, const QString &mediaUri)
    {
        emit metadataChanged(tags, mediaUri);
    }
    void simulateArtwork(
        const QString &artwork, const QString &mediaUri)
    {
        emit artworkChanged(artwork, mediaUri);
    }
    void simulateMediaKind(
        ::MediaKind kind, const QString &mediaUri)
    {
        emit mediaKindChanged(kind, mediaUri);
    }
    void simulateLoadFailed(const QString &reason)
    {
        emit loadFailed(m_loadGeneration, reason);
    }
    void simulateLoadFailedForGeneration(
        std::uint64_t generation, const QString &reason)
    {
        emit loadFailed(generation, reason);
    }
    void setAutoReady(bool ready) { m_autoReady = ready; }
    MediaLoadRoute lastRoute() const { return m_lastRoute; }
    qsizetype lastFrameCount() const { return m_lastFrameCount; }
    int loadCalls() const { return m_loadCalls; }
    std::uint64_t loadGeneration() const { return m_loadGeneration; }

private:
    QString m_uri;
    double m_position = 0.0;
    int m_volume = 100;
    bool m_muted = false;
    bool m_paused = false;
    int m_brightness = 50;
    bool m_autoReady = true;
    MediaLoadRoute m_lastRoute = MediaLoadRoute::Native;
    qsizetype m_lastFrameCount = 0;
    int m_loadCalls = 0;
    std::uint64_t m_loadGeneration = 0;
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

void testStalePlayerSignalsAreIgnored()
{
    FakeMediaPlayer player;
    PlaybackController controller(&player);
    PlaybackController::MediaRequest first;
    first.uri = QStringLiteral("http://example.test/first.mp4");
    first.title = QStringLiteral("First");
    controller.openUri(first, ::MediaSource::Cast);

    player.simulateMetadata(
        {{QStringLiteral("title"), QStringLiteral("Old title")}},
        QStringLiteral("http://example.test/old.mp4"));
    check(controller.nowPlaying().title == QStringLiteral("First"),
          "stale metadata from another media URI is ignored");
    QTemporaryDir temporary;
    check(temporary.isValid(), "stale artwork temporary directory");
    const QString artworkPath =
        temporary.filePath(QStringLiteral("cover.png"));
    QFile artworkFile(artworkPath);
    check(artworkFile.open(QIODevice::WriteOnly),
          "create stale artwork test file");
    artworkFile.write("image");
    artworkFile.close();
    const QString artworkUri = QUrl::fromLocalFile(artworkPath).toString();
    player.simulateArtwork(
        artworkUri, QStringLiteral("http://example.test/old.mp4"));
    check(controller.nowPlaying().artworkSource.isEmpty(),
          "stale artwork from another media URI is ignored");
    player.simulateArtwork(artworkUri, first.uri);
    check(controller.nowPlaying().artworkSource == artworkUri,
          "artwork for the active media URI still applies");

    PlaybackController::MediaRequest second;
    second.uri = QStringLiteral("http://example.test/second.mp4");
    second.kind = ::MediaKind::Audio;
    controller.openUri(second, ::MediaSource::Cast);
    player.simulateMetadata(
        {{QStringLiteral("title"), QStringLiteral("First stale")}},
        first.uri);
    check(controller.nowPlaying().title != QStringLiteral("First stale"),
          "metadata arriving after a media switch cannot contaminate it");
    player.simulateArtwork(artworkUri, first.uri);
    check(controller.nowPlaying().artworkSource.isEmpty(),
          "artwork arriving after a media switch cannot contaminate it");
    player.simulateMediaKind(
        ::MediaKind::Video, second.uri);
    check(controller.nowPlaying().kind == ::MediaKind::Video,
          "MPV actual track kind overrides a false protocol kind");
    player.simulateMetadata(
        {{QStringLiteral("title"), QStringLiteral("Second")}},
        second.uri);
    check(controller.nowPlaying().title == QStringLiteral("Second"),
          "metadata for the active media URI still applies");
    check(controller.nowPlaying().artworkSource.isEmpty(),
          "previous artwork is cleared when switching media");
}

void testExplicitLoadFailureStopsPreparation()
{
    FakeMediaPlayer player;
    player.setAutoReady(false);
    PlaybackController controller(&player);
    PlaybackController::MediaRequest request;
    request.uri = QStringLiteral("http://example.test/broken.mp4");
    controller.openUri(request, ::MediaSource::Cast);
    check(controller.state() == PlaybackController::State::Preparing,
          "media starts in the preparing state");

    player.simulateLoadFailed(QStringLiteral("decoder rejected media"));
    check(controller.loadStatus() == PlaybackController::LoadStatus::Failed,
          "an explicit player failure updates load status");
    check(controller.state() == PlaybackController::State::Stopped,
          "an explicit player failure leaves preparing immediately");
}

void testCompatibilityRouteFallsBackToNativeOnce()
{
    FakeMediaPlayer player;
    player.setAutoReady(false);
    PlaybackController controller(&player);
    PlaybackController::MediaRequest request;
    request.uri = QStringLiteral("http://example.test/show.m3u8");
    request.kind = ::MediaKind::Video;
    request.route = MediaLoadRoute::ImageSequence;
    request.imageSequenceFrames = {
        {QStringLiteral("http://example.test/frame.png"), 1.0},
    };
    controller.openUri(request, ::MediaSource::Cast);
    check(player.loadCalls() == 1
              && player.lastRoute() == MediaLoadRoute::ImageSequence,
          "the requested compatibility strategy is attempted first");

    player.simulateLoadFailed(QStringLiteral("image strategy failed"));
    check(player.loadCalls() == 2
              && player.lastRoute() == MediaLoadRoute::Native
              && player.lastFrameCount() == 0,
          "a failed compatibility route retries the original URI once");
    check(controller.state() == PlaybackController::State::Preparing,
          "the one native fallback remains in loading state");

    player.simulateLoadFailed(QStringLiteral("native fallback failed"));
    check(player.loadCalls() == 2,
          "the native fallback cannot loop");
    check(controller.loadStatus() == PlaybackController::LoadStatus::Failed
              && controller.state() == PlaybackController::State::Stopped,
          "failure of the native fallback is final");
}

void testStaleLoadFailureCannotAffectNewMedia()
{
    FakeMediaPlayer player;
    player.setAutoReady(false);
    PlaybackController controller(&player);

    PlaybackController::MediaRequest first;
    first.uri = QStringLiteral("http://example.test/first.m3u8");
    first.kind = ::MediaKind::Video;
    controller.openUri(first, ::MediaSource::Cast);
    const std::uint64_t firstGeneration = player.loadGeneration();

    PlaybackController::MediaRequest second;
    second.uri = QStringLiteral("http://example.test/second.mp4");
    second.kind = ::MediaKind::Video;
    controller.openUri(second, ::MediaSource::Cast);
    const int loadsBeforeStaleFailure = player.loadCalls();

    player.simulateLoadFailedForGeneration(
        firstGeneration, QStringLiteral("late failure from old media"));
    check(player.loadCalls() == loadsBeforeStaleFailure,
          "a stale load failure cannot trigger another load");
    check(controller.state() == PlaybackController::State::Preparing,
          "a stale load failure leaves the new media preparing");
}

void testPeerLossUnloadsCastMedia()
{
    FakeMediaPlayer player;
    PlaybackController controller(&player);
    controller.setPeerConnected(true);

    PlaybackController::MediaRequest cast;
    cast.uri = QStringLiteral("http://example.test/ended-cast.mp4");
    cast.title = QStringLiteral("Ended cast");
    controller.openUri(cast, ::MediaSource::Cast);
    check(controller.state() == PlaybackController::State::Playing
              && controller.hasMedia(),
          "cast media is loaded before peer loss");

    controller.setPeerConnected(false);
    check(controller.state() == PlaybackController::State::NoMedia
              && !controller.hasMedia()
              && controller.currentUri().isEmpty(),
          "peer loss unloads stale cast media");

    controller.setPeerConnected(true);
    PlaybackController::MediaRequest local;
    local.uri = QStringLiteral("file:///C:/media/local.wav");
    local.title = QStringLiteral("Local");
    controller.openUri(local, ::MediaSource::Local);
    controller.setPeerConnected(false);
    check(controller.state() == PlaybackController::State::Playing
              && controller.hasMedia(),
          "peer loss does not stop locally opened media");
    controller.endSession();
    check(controller.state() == PlaybackController::State::Playing
              && controller.hasMedia(),
          "a DLNA end-session command does not unload local playback");

    controller.setPeerConnected(true);
    controller.openUri(cast, ::MediaSource::Cast);
    controller.setPeerConnected(false);
    controller.openUri(cast, ::MediaSource::Cast);
    controller.setPeerConnected(false);
    check(controller.state() == PlaybackController::State::NoMedia,
          "a repeated peer=false still removes an inconsistent cast item");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    testStalePlayerSignalsAreIgnored();
    testExplicitLoadFailureStopsPreparation();
    testCompatibilityRouteFallsBackToNativeOnce();
    testStaleLoadFailureCannotAffectNewMedia();
    testPeerLossUnloadsCastMedia();
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
    std::atomic_bool endSessionRequested = false;
    PlaybackPipeGateway gateway(
        std::move(server), [](const std::string &) {},
        [&] { endSessionRequested = true; });
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

    bridge.requestEndSession();
    check(pumpUntil(application, [&] {
        return endSessionRequested.load();
    }), "UI end-session request reaches Protocol through the urgent path");

    PlaybackCommand open;
    open.type = PlaybackCommandType::Open;
    open.expectedGeneration = 42;
    open.media.uri = "http://example.test/movie.mp4";
    open.media.metadata = "<DIDL-Lite/>";
    open.media.title = "Movie";
    open.media.artist = "Artist";
    open.media.creator = "Creator";
    open.media.album = "Album";
    open.media.description = "Description";
    open.media.subtitle = "Subtitle";
    open.media.longDescription = "Long Description";
    open.media.artworkUri = "https://example.test/cover.jpg";
    open.media.protocol = "DLNA";
    open.media.kind = types::MediaKind::Video;
    const auto beforeUiReady = gateway.submit(open);
    check(!beforeUiReady.queued
              && beforeUiReady.status
                  == PlaybackSubmissionStatus::WaitingForPlayer,
          "open waits for PlaybackUiReady");
    std::size_t eventsBeforeReady = 0;
    {
        std::scoped_lock lock(events.mutex);
        eventsBeforeReady = events.events.size();
    }
    bridge.notifyUiReady();
    check(pumpUntil(application, [&] {
        std::scoped_lock lock(events.mutex);
        return events.events.size() > eventsBeforeReady;
    }), "UI ready reaches the protocol gateway");
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
              && gateway.snapshot().current.creator == "Creator"
              && gateway.snapshot().current.album == "Album"
              && gateway.snapshot().current.description == "Description"
              && gateway.snapshot().current.subtitle == "Subtitle"
              && gateway.snapshot().current.longDescription
                  == "Long Description"
              && gateway.snapshot().current.artworkUri
                  == "https://example.test/cover.jpg",
          "full media request survives snapshot");
    check(gateway.snapshot().current.metadata == "<DIDL-Lite/>",
          "metadata survives snapshot");
    check(gateway.snapshot().current.protocol == "DLNA",
          "protocol survives snapshot");
    check(gateway.snapshot().current.kind == types::MediaKind::Video,
          "kind survives snapshot");

    PlaybackCommand castSession;
    castSession.type = PlaybackCommandType::SetCastSession;
    castSession.expectedGeneration = 42;
    castSession.castSession.connected = true;
    castSession.castSession.protocol = "DLNA";
    castSession.castSession.controlPointName = "BubbleUPnP 4.6.7.1";
    castSession.castSession.controlPointAddress = "192.168.10.3";
    castSession.castSession.remoteControlRegistered = true;
    check(gateway.submit(castSession).queued,
          "cast session info queues");
    check(pumpUntil(application, [&] {
        const QVariantMap info = controller.castSession();
        return info.value(QStringLiteral("connected")).toBool()
            && info.value(QStringLiteral("protocol")).toString()
                == QStringLiteral("DLNA")
            && info.value(QStringLiteral("remoteControlRegistered")).toBool();
    }), "cast session info applies");
    check(pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.castSession.connected
            && snapshot.castSession.protocol == "DLNA"
            && snapshot.castSession.controlPointName
                == "BubbleUPnP 4.6.7.1";
    }), "cast session survives the UI playback snapshot");

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
    setNext.media.route = types::MediaRoute::ImageSequence;
    setNext.media.imageSequenceFrames = {
        {"http://example.test/frame-1.png", 1.5},
        {"http://example.test/frame-2.png", 2.0},
    };
    check(gateway.submit(setNext).queued, "set next queues");
    const bool nextPlanReached = pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.next.uri == "http://example.test/next.mp4"
            && snapshot.next.title == "Next"
            && snapshot.next.route == types::MediaRoute::ImageSequence
            && snapshot.next.imageSequenceFrames.size() == 2;
    });
    if (!nextPlanReached) {
        const auto snapshot = gateway.snapshot();
        std::cerr << "next plan uri=" << snapshot.next.uri
                  << " title=" << snapshot.next.title
                  << " route="
                  << static_cast<int>(snapshot.next.route)
                  << " frames="
                  << snapshot.next.imageSequenceFrames.size() << '\n';
    }
    check(nextPlanReached, "complete next request reaches Protocol");

    PlaybackCommand planPreservingDelta;
    planPreservingDelta.type = PlaybackCommandType::SetVolume;
    planPreservingDelta.intValue = 36;
    planPreservingDelta.expectedGeneration = 42;
    check(gateway.submit(planPreservingDelta).queued,
          "plan-preserving delta queues");
    check(pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.volumePercent == 36
            && snapshot.next.route == types::MediaRoute::ImageSequence
            && snapshot.next.imageSequenceFrames.size() == 2
            && snapshot.next.imageSequenceFrames[1].durationSeconds == 2.0;
    }), "state deltas preserve the cached next playback plan");

    PlaybackCommand updatedNext = setNext;
    updatedNext.media.imageSequenceFrames = {
        {"http://example.test/frame-1.png", 3.25},
    };
    check(gateway.submit(updatedNext).queued,
          "same-URI plan update queues");
    const bool updatedPlanReached = pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.next.imageSequenceFrames.size() == 1
            && snapshot.next.imageSequenceFrames[0].durationSeconds == 3.25;
    });
    if (!updatedPlanReached) {
        const auto snapshot = gateway.snapshot();
        std::cerr << "updated next frames="
                  << snapshot.next.imageSequenceFrames.size();
        if (!snapshot.next.imageSequenceFrames.empty()) {
            std::cerr << " duration="
                      << snapshot.next.imageSequenceFrames[0].durationSeconds;
        }
        const auto controllerNext = controller.nextRequest();
        std::cerr << " controllerFrames="
                  << controllerNext.imageSequenceFrames.size();
        if (!controllerNext.imageSequenceFrames.isEmpty()) {
            std::cerr << " controllerDuration="
                      << controllerNext.imageSequenceFrames[0].durationSeconds;
        }
        std::cerr << " stateVersion=" << snapshot.stateVersion << '\n';
    }
    check(updatedPlanReached,
          "same-URI frame-table changes are serialized again");

    controller.next();
    check(pumpUntil(application, [&] {
        const auto snapshot = gateway.snapshot();
        return snapshot.current.uri == "http://example.test/next.mp4"
            && snapshot.current.title == "Next"
            && snapshot.next.uri.empty()
            && snapshot.current.route
                == types::MediaRoute::ImageSequence
            && snapshot.current.imageSequenceFrames.size() == 1
            && snapshot.current.imageSequenceFrames[0].durationSeconds
                == 3.25;
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
