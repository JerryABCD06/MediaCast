// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "AvaloniaHostNative.h"

#include "QtFree/EventLoop.h"
#include "QtFree/Json.h"
#include "QtFree/MpvCore.h"
#include "QtFree/PlaybackBridge.h"
#include "QtFree/PlaybackController.h"
#include "QtFree/RenderWakeLatch.h"
#include "QtFree/UiConfirmationBridge.h"
#include "QtFree/UiSettingsModel.h"
#include "QtFree/WindowsMediaControls.h"
#include "ui_process/UiSession.h"

#include <mpv/render_gl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using mcast::avalonia::qtfree::ConfirmationEvent;
using mcast::avalonia::qtfree::Connection;
using mcast::avalonia::qtfree::EventLoop;
using mcast::avalonia::qtfree::JsonError;
using mcast::avalonia::qtfree::JsonValue;
using mcast::avalonia::qtfree::MediaKind;
using mcast::avalonia::qtfree::MediaRequest;
using mcast::avalonia::qtfree::MediaSource;
using mcast::avalonia::qtfree::MpvCore;
using mcast::avalonia::qtfree::NowPlaying;
using mcast::avalonia::qtfree::PlaybackBridge;
using mcast::avalonia::qtfree::PlaybackController;
using mcast::avalonia::qtfree::RenderWakeLatch;
using mcast::avalonia::qtfree::SettingsSnapshotEvent;
using mcast::avalonia::qtfree::UiConfirmationBridge;
using mcast::avalonia::qtfree::UiSettingsModel;
using mcast::avalonia::qtfree::WindowsMediaControls;

struct MpvProcContext
{
    McastAvaloniaGetProcAddress callback = nullptr;
    void *context = nullptr;

    static void *getProc(void *opaque, const char *name)
    {
        auto *self = static_cast<MpvProcContext *>(opaque);
        return self && self->callback
            ? self->callback(self->context, name)
            : nullptr;
    }
};

struct HostState
{
    McastAvaloniaLogCallback logCallback = nullptr;
    std::atomic<McastAvaloniaProjectionUpdateCallback>
        projectionCallback{nullptr};
    std::atomic<McastAvaloniaRenderUpdateCallback>
        renderUpdateCallback{nullptr};
    void *logContext = nullptr;
    void *projectionContext = nullptr;
    void *renderUpdateContext = nullptr;

    UiSession::Options options;
    std::vector<std::string> argumentStorage;
    std::vector<char *> arguments;

    EventLoop loop;
    UiSession session;
    std::unique_ptr<UiSettingsModel> settings;
    std::unique_ptr<MpvCore> player;
    std::unique_ptr<PlaybackController> playback;
    std::unique_ptr<PlaybackBridge> playbackBridge;
    std::unique_ptr<UiConfirmationBridge> confirmation;
    std::shared_ptr<WindowsMediaControls> mediaControls;
    Connection playbackLogConnection;
    std::vector<Connection> connections;

    std::jthread reconnectWorker;
    std::mutex reconnectMutex;
    std::condition_variable_any reconnectReady;
    std::optional<std::uint64_t> reconnectRequest;
    bool reconnectInFlight = false;
    bool reconnectPending = false;

    std::atomic_bool stopping{false};
    std::atomic_bool closing{false};
    std::atomic_bool controlConnected{true};
    std::atomic_bool windowPresented{false};
    std::atomic_bool playerRunning{false};
    std::atomic_bool rendererReady{false};
    std::atomic_bool contextLost{false};
    std::atomic_bool uiFirstFrameSent{false};
    std::atomic_bool playbackUiReadySent{false};

    std::mutex renderMutex;
    mpv_render_context *renderContext = nullptr;
    std::unique_ptr<MpvProcContext> renderProcContext;
    std::atomic_uint64_t renderContextGeneration{0};
    std::atomic_uint64_t renderCalls{0};
    std::atomic_uint64_t renderFailures{0};
    std::atomic_int lastFramebuffer{0};
    std::atomic_int lastWidth{0};
    std::atomic_int lastHeight{0};
    std::atomic_bool renderTargetRefreshPending{false};
    RenderWakeLatch renderWake;
    std::string glBackend = "not-attached";

    std::mutex snapshotMutex;
    std::string shellJson = "{}";
    std::string languageJson = "{}";
    std::string playbackJson = "{}";
    std::string confirmationJson = "{}";
    std::string rendererJson =
        R"({"playerRunning":false,"rendererReady":false,"contextLost":false,"renderContextGeneration":0,"renderCalls":0,"renderFailures":0,"lastFramebuffer":0,"lastWidth":0,"lastHeight":0,"glBackend":"not-attached"})";
    std::string resolvedLanguage = "en_US";
    std::string fatalReason;

    std::uint64_t playbackRefreshTimer = 0;
    bool applyingAuthoritativePicture = false;
};

void renderDetachImpl(bool lost, const std::string &reason);

std::mutex g_lifecycleMutex;
std::unique_ptr<HostState> g_state;

void log(HostState &state, const std::string &message)
{
    if (state.logCallback)
        state.logCallback(message.c_str(), state.logContext);
}

std::optional<std::chrono::milliseconds> testDelay(
    const char *name)
{
    const char *value = std::getenv(name);
    if (!value || *value == '\0')
        return std::nullopt;
    char *end = nullptr;
    const long milliseconds = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || milliseconds < 0
        || milliseconds > 600000) {
        return std::nullopt;
    }
    return std::chrono::milliseconds(milliseconds);
}

void notifyChanged(
    HostState &state,
    std::uint32_t dirtyDomains = MCAST_AVALONIA_DIRTY_ALL)
{
    const auto callback = state.projectionCallback.load();
    if (callback)
        callback(dirtyDomains, state.projectionContext);
}

void notifyRender(HostState &state)
{
    if (!state.renderWake.mark())
        return;
    const auto callback = state.renderUpdateCallback.load();
    if (callback)
        callback(state.renderUpdateContext);
}

void replaceSnapshot(
    HostState &state, std::string &target, JsonValue value)
{
    const std::string json = value.dump();
    const std::scoped_lock lock(state.snapshotMutex);
    target = json;
}

int themeMode(const std::string &value)
{
    if (value == "Light")
        return 1;
    if (value == "Dark")
        return 2;
    return 0;
}

int accentMode(const std::string &value)
{
    return value == "System" ? 1 : 0;
}

int playbackState(PlaybackController::State state)
{
    switch (state) {
    case PlaybackController::State::NoMedia: return 0;
    case PlaybackController::State::Stopped: return 1;
    case PlaybackController::State::Preparing: return 2;
    case PlaybackController::State::Playing: return 3;
    case PlaybackController::State::Paused: return 4;
    }
    return 0;
}

int castState(PlaybackController::CastState state)
{
    switch (state) {
    case PlaybackController::CastState::NoViewer: return 0;
    case PlaybackController::CastState::ViewerIdle: return 1;
    case PlaybackController::CastState::ViewerVideo: return 2;
    case PlaybackController::CastState::ViewerAudio: return 3;
    case PlaybackController::CastState::ViewerImage: return 4;
    }
    return 0;
}

int mediaKind(MediaKind kind)
{
    switch (kind) {
    case MediaKind::Unknown: return 0;
    case MediaKind::Video: return 1;
    case MediaKind::Audio: return 2;
    case MediaKind::Image: return 3;
    }
    return 0;
}

int mediaSource(MediaSource source)
{
    return source == MediaSource::Local ? 1 : 0;
}

JsonValue mediaProjection(
    const MediaRequest &request,
    const NowPlaying *nowPlaying,
    std::int64_t mediaSizeBytes)
{
    JsonValue value = JsonValue::object();
    value.set("uri", JsonValue(request.uri));
    value.set(
        "title",
        JsonValue(
            nowPlaying && !nowPlaying->title.empty()
                ? nowPlaying->title : request.title));
    value.set(
        "artist",
        JsonValue(
            nowPlaying && !nowPlaying->artist.empty()
                ? nowPlaying->artist : request.artist));
    value.set(
        "album",
        JsonValue(
            nowPlaying && !nowPlaying->album.empty()
                ? nowPlaying->album : request.album));
    value.set("creator", JsonValue(request.creator));
    value.set("subtitle", JsonValue(request.subtitle));
    value.set("description", JsonValue(request.description));
    value.set("longDescription", JsonValue(request.longDescription));
    value.set(
        "artworkUri",
        JsonValue(
            nowPlaying && !nowPlaying->artworkSource.empty()
                ? nowPlaying->artworkSource : request.artworkUri));
    value.set("protocol", JsonValue(request.protocol));
    value.set(
        "kind",
        JsonValue(static_cast<double>(
            mediaKind(
                nowPlaying ? nowPlaying->kind : request.kind))));
    value.set(
        "source",
        JsonValue(static_cast<double>(
            mediaSource(
                nowPlaying ? nowPlaying->source : MediaSource::Cast))));
    value.set(
        "mediaSizeBytes",
        mediaSizeBytes > 0
            ? JsonValue(static_cast<double>(mediaSizeBytes))
            : JsonValue());
    return value;
}

void updateShell(
    HostState &state, const std::string &resolvedLanguage)
{
    if (!state.settings)
        return;
    JsonValue root = JsonValue::object();
    root.set(
        "settingsVersion",
        JsonValue(static_cast<double>(state.settings->version())));
    root.set("resolvedLanguage", JsonValue(resolvedLanguage));

    std::string requestedLanguage = "System";
    int theme = 0;
    int accent = 1;
    const auto parsed =
        JsonValue::parse(state.settings->snapshotJson());
    if (parsed && parsed->isObject()) {
        const JsonValue *ui = parsed->find("ui");
        if (ui && ui->isObject()) {
            requestedLanguage =
                ui->at("language").asString("System");
            theme = themeMode(
                ui->at("darkmode").asString("System"));
            accent = accentMode(
                ui->at("accentmode").asString("System"));
        }
    }
    root.set("requestedLanguage", JsonValue(requestedLanguage));
    root.set("themeMode", JsonValue(static_cast<double>(theme)));
    root.set("accentMode", JsonValue(static_cast<double>(accent)));
    root.set("micaEnabled", JsonValue(state.settings->mica()));
    root.set(
        "acceptNewCast", JsonValue(state.settings->acceptNewCast()));
    root.set(
        "broadcastEnabled", JsonValue(state.settings->broadcast()));
    root.set(
        "broadcastIntervalMs",
        JsonValue(static_cast<double>(
            state.settings->broadcastIntervalMs())));
    root.set("deviceName", JsonValue(state.settings->deviceName()));
    root.set("deviceUdn", JsonValue(state.settings->deviceUdn()));
    root.set("deviceAddress", JsonValue(std::string{}));
    root.set("appVersion", JsonValue(std::string("0.1.0")));
    root.set(
        "confirmationOnly", JsonValue(state.options.confirmationOnly));
    root.set(
        "testTakeoverWindow",
        JsonValue(state.options.testTakeoverWindow));
    root.set(
        "uiSessionConnected",
        JsonValue(state.controlConnected.load()));
    root.set("fatalReason", JsonValue(state.fatalReason));
    replaceSnapshot(state, state.shellJson, std::move(root));
    notifyChanged(state, MCAST_AVALONIA_DIRTY_SHELL);
}

void updatePlayback(HostState &state)
{
    if (!state.playback) {
        JsonValue root = JsonValue::object();
        root.set("playerAvailable", JsonValue(false));
        root.set("state", JsonValue(0.0));
        root.set("peerConnected", JsonValue(false));
        root.set("hasMedia", JsonValue(false));
        root.set("positionSeconds", JsonValue(0.0));
        root.set("durationSeconds", JsonValue(0.0));
        root.set("volumePercent", JsonValue(100.0));
        root.set("muted", JsonValue(false));
        root.set("generation", JsonValue(0.0));
        root.set("stateVersion", JsonValue(0.0));
        replaceSnapshot(state, state.playbackJson, std::move(root));
        notifyChanged(state, MCAST_AVALONIA_DIRTY_PLAYBACK);
        return;
    }

    PlaybackController &playback = *state.playback;
    JsonValue root = JsonValue::object();
    root.set(
        "generation",
        JsonValue(static_cast<double>(
            state.playbackBridge
                ? state.playbackBridge->generation()
                : state.options.activationId)));
    root.set(
        "stateVersion",
        JsonValue(static_cast<double>(
            state.playbackBridge
                ? state.playbackBridge->stateVersion() : 0)));
    root.set(
        "state",
        JsonValue(static_cast<double>(playbackState(playback.state()))));
    root.set(
        "loadStatus",
        JsonValue(static_cast<double>(
            playback.loadStatus()
                    == PlaybackController::LoadStatus::Failed
                ? 1 : 0)));
    root.set(
        "playerAvailable",
        JsonValue(state.playerRunning.load()
                  && !state.contextLost.load()));
    root.set("playbackPlanIncluded", JsonValue(true));
    root.set("hasMedia", JsonValue(playback.hasMedia()));
    root.set("idle", JsonValue(playback.isIdle()));
    root.set("paused", JsonValue(playback.isPaused()));
    root.set("peerConnected", JsonValue(playback.peerConnected()));
    root.set(
        "castState",
        JsonValue(static_cast<double>(castState(playback.castState()))));
    root.set("showsPicture", JsonValue(playback.showsPicture()));
    root.set("mediaIsVideo", JsonValue(playback.mediaIsVideo()));

    const auto &now = playback.nowPlaying();
    root.set(
        "currentMedia",
        mediaProjection(
            playback.currentRequest(),
            now.kind == MediaKind::Unknown ? nullptr : &now,
            playback.mediaSizeBytes()));
    root.set(
        "nextMedia",
        mediaProjection(playback.nextRequest(), nullptr, 0));
    root.set("hasNext", JsonValue(playback.hasNext()));
    root.set("hasPrevious", JsonValue(playback.hasPrevious()));

    const auto &cast = playback.castSession();
    JsonValue session = JsonValue::object();
    session.set("connected", JsonValue(cast.connected));
    session.set("protocol", JsonValue(cast.protocol));
    session.set(
        "controlPointName", JsonValue(cast.controlPointName));
    session.set(
        "controlPointAddress", JsonValue(cast.controlPointAddress));
    session.set(
        "remoteControlRegistered",
        JsonValue(cast.remoteControlRegistered));
    root.set("castSession", std::move(session));

    root.set(
        "positionSeconds", JsonValue(playback.positionSeconds()));
    root.set(
        "durationSeconds", JsonValue(playback.durationSeconds()));
    root.set(
        "volumePercent",
        JsonValue(static_cast<double>(playback.volumePercent())));
    root.set("muted", JsonValue(playback.isMuted()));

    JsonValue controls = JsonValue::array();
    for (const auto &control : playback.pictureControls()) {
        JsonValue item = JsonValue::object();
        item.set("name", JsonValue(control.name));
        item.set(
            "minimum",
            JsonValue(static_cast<double>(control.minimum)));
        item.set(
            "maximum",
            JsonValue(static_cast<double>(control.maximum)));
        item.set(
            "neutral",
            JsonValue(static_cast<double>(control.neutral)));
        item.set(
            "value",
            JsonValue(static_cast<double>(
                playback.pictureControlValue(control.name))));
        controls.push(std::move(item));
    }
    root.set("pictureControls", std::move(controls));
    root.set(
        "mediaInfo",
        state.player
            ? JsonValue::parse(state.player->mediaInfoJson())
                  .value_or(JsonValue::object())
            : JsonValue::object());
    replaceSnapshot(state, state.playbackJson, std::move(root));
    notifyChanged(state, MCAST_AVALONIA_DIRTY_PLAYBACK);
}

void schedulePlaybackRefresh(HostState &state)
{
    if (state.playbackRefreshTimer != 0 || state.stopping.load())
        return;
    state.playbackRefreshTimer = state.loop.scheduleAfter(
        16ms, [&state] {
            state.playbackRefreshTimer = 0;
            updatePlayback(state);
        });
}

void updateRenderer(HostState &state)
{
    JsonValue root = JsonValue::object();
    root.set("playerRunning", JsonValue(state.playerRunning.load()));
    root.set("rendererReady", JsonValue(state.rendererReady.load()));
    root.set("contextLost", JsonValue(state.contextLost.load()));
    root.set(
        "renderContextGeneration",
        JsonValue(static_cast<double>(
            state.renderContextGeneration.load())));
    root.set(
        "renderCalls",
        JsonValue(static_cast<double>(state.renderCalls.load())));
    root.set(
        "renderFailures",
        JsonValue(static_cast<double>(state.renderFailures.load())));
    root.set(
        "lastFramebuffer",
        JsonValue(static_cast<double>(
            state.lastFramebuffer.load())));
    root.set(
        "lastWidth",
        JsonValue(static_cast<double>(state.lastWidth.load())));
    root.set(
        "lastHeight",
        JsonValue(static_cast<double>(state.lastHeight.load())));
    {
        const std::scoped_lock lock(state.snapshotMutex);
        root.set("glBackend", JsonValue(state.glBackend));
    }
    replaceSnapshot(state, state.rendererJson, std::move(root));
    notifyChanged(state, MCAST_AVALONIA_DIRTY_RENDERER);
}

void evaluatePlaybackReady(HostState &state)
{
    if (state.stopping.load() || state.closing.load()
        || !state.playbackBridge
        || !state.playerRunning.load()
        || !state.windowPresented.load()
        || !state.rendererReady.load()
        || state.contextLost.load()
        || state.playbackUiReadySent.exchange(true)) {
        return;
    }
    if (!state.playbackBridge->notifyUiReady()) {
        state.playbackUiReadySent = false;
        log(state, "PlaybackUiReady rejected");
        return;
    }
    log(state, "PlaybackUiReady sent");
}

void applyAuthoritativePicture(HostState &state)
{
    if (!state.settings || !state.playback)
        return;
    state.applyingAuthoritativePicture = true;
    state.playback->resetPictureControls();
    for (const auto &[name, value] :
         state.settings->pictureValues()) {
        state.playback->setPictureControl(name, value);
    }
    state.applyingAuthoritativePicture = false;
}

void attachPlaybackBridge(
    HostState &state,
    std::shared_ptr<mcast::ipc::PipeConnection> connection)
{
    if (state.stopping.load() || state.closing.load())
        return;
    state.playbackLogConnection.disconnect();
    if (state.playbackBridge)
        state.playbackBridge->stop();
    state.playbackBridge.reset();
    state.playbackUiReadySent = false;
    if (!state.playback || !connection || !connection->valid()) {
        log(state, "Playback reconnect failed: invalid connection");
        return;
    }
    state.playbackBridge = std::make_unique<PlaybackBridge>(
        std::move(connection), *state.playback,
        state.loop, state.options.activationId);
    state.playbackLogConnection =
        state.playbackBridge->logMessage.connect(
            [&state](const std::string &message) {
                log(state, "PlaybackBridge: " + message);
            });
    updatePlayback(state);
    evaluatePlaybackReady(state);
}

void requestReconnect(HostState &state, std::uint64_t requestId)
{
    {
        const std::scoped_lock lock(state.reconnectMutex);
        if (state.reconnectInFlight) {
            state.reconnectPending = true;
            log(
                state,
                "Playback reconnect " + std::to_string(requestId)
                    + " merged into the in-flight attempt");
            return;
        }
        if (state.reconnectRequest) {
            state.reconnectPending = true;
            return;
        }
        state.reconnectRequest = requestId;
    }
    state.reconnectReady.notify_all();
}

void reconnectLoop(HostState &state, std::stop_token stop)
{
    while (!stop.stop_requested()) {
        std::uint64_t requestId = 0;
        {
            std::unique_lock lock(state.reconnectMutex);
            state.reconnectReady.wait(
                lock, stop, [&state] {
                    return state.stopping.load()
                        || state.reconnectRequest.has_value();
                });
            if (stop.stop_requested() || state.stopping.load())
                break;
            requestId = *state.reconnectRequest;
            state.reconnectRequest.reset();
            state.reconnectInFlight = true;
        }

        std::string error;
        auto connection = state.session.reconnectPlayback(error);
        auto shared = std::shared_ptr<mcast::ipc::PipeConnection>(
            std::move(connection));
        auto *host = &state;
        state.loop.post(
            [host, requestId, shared, error = std::move(error)] {
                if (!host->stopping.load()) {
                    if (shared) {
                        attachPlaybackBridge(*host, std::move(shared));
                        log(
                            *host,
                            "Playback reconnect "
                                + std::to_string(requestId)
                                + " completed");
                    } else {
                        log(
                            *host,
                            "Playback reconnect "
                                + std::to_string(requestId)
                                + " failed: " + error);
                    }
                }
                bool rerun = false;
                {
                    const std::scoped_lock lock(
                        host->reconnectMutex);
                    host->reconnectInFlight = false;
                    if (host->reconnectPending) {
                        host->reconnectPending = false;
                        host->reconnectRequest = requestId + 1;
                        rerun = true;
                    }
                }
                if (rerun)
                    host->reconnectReady.notify_all();
            });
    }
}

void stopReconnectWorker(HostState &state)
{
    {
        const std::scoped_lock lock(state.reconnectMutex);
        state.reconnectPending = false;
        state.reconnectRequest.reset();
    }
    state.reconnectReady.notify_all();
    if (state.reconnectWorker.joinable()) {
        state.reconnectWorker.request_stop();
        state.reconnectReady.notify_all();
        state.reconnectWorker.join();
    }
}

void updateConfirmation(
    HostState &state, const ConfirmationEvent *event)
{
    if (!event) {
        replaceSnapshot(
            state, state.confirmationJson, JsonValue::object());
    } else {
        JsonValue root = JsonValue::object();
        root.set(
            "requestId",
            JsonValue(static_cast<double>(event->requestId)));
        root.set(
            "protocol",
            JsonValue(static_cast<double>(
                static_cast<int>(event->request.protocol))));
        root.set(
            "claimId",
            JsonValue(static_cast<double>(event->request.claimId)));
        root.set("peerId", JsonValue(event->request.peerId));
        root.set(
            "peerName",
            JsonValue(event->request.peerDisplayName));
        root.set(
            "peerAddress",
            JsonValue(event->request.peerAddress));
        root.set(
            "mediaTitle",
            JsonValue(event->resolvedMediaTitle));
        root.set(
            "remainingTimeoutMs",
            JsonValue(static_cast<double>(
                event->request.remainingTimeoutMs)));
        replaceSnapshot(
            state, state.confirmationJson, std::move(root));
    }
    notifyChanged(state, MCAST_AVALONIA_DIRTY_CONFIRMATION);
}

bool setupHost(HostState &state, std::string &error)
{
    if (state.options.enabled) {
        if (!state.session.connectToPeers(state.options, error))
            return false;
        state.languageJson = state.session.languageCatalogJson();
        if (state.languageJson.empty())
            state.languageJson = "{}";
        state.resolvedLanguage = state.session.resolvedLanguage();

        state.settings = std::make_unique<UiSettingsModel>(state.loop);
        if (!state.settings->applySnapshot(
                state.session.settingsJson(),
                state.session.settingsVersion(), &error)) {
            return false;
        }
    } else if (state.options.standalone) {
        state.settings = std::make_unique<UiSettingsModel>(state.loop);
        constexpr const char *standaloneSettings = R"({
            "ui":{"language":"System","darkmode":"System","accentmode":"System","mica":true},
            "cast":{"newcast":true,"broadcast":false,"broadcast_interval":10000},
            "device":{"name":"Standalone Ui","udn":"uuid:00000000-0000-4000-8000-000000000001"},
            "picture":{}
        })";
        if (!state.settings->applySnapshot(
                standaloneSettings, 1, &error)) {
            return false;
        }
        state.languageJson =
            R"({"resolvedLanguage":"en_US","languages":[{"code":"en_US","name":"English"}]})";
        state.resolvedLanguage = "en_US";
    } else {
        return false;
    }

    updateShell(state, state.resolvedLanguage);

    if (auto control = state.session.takeControlConnection()) {
        state.confirmation = std::make_unique<UiConfirmationBridge>(
            std::shared_ptr<mcast::ipc::PipeConnection>(
                std::move(control)),
            state.loop,
            [](const std::string &title) {
                return title.empty() ? std::string("未知") : title;
            });
        state.connections.push_back(
            state.confirmation->requestReceived.connect(
            [&state](const ConfirmationEvent &event) {
                updateConfirmation(state, &event);
            }));
        state.connections.push_back(
            state.confirmation->cancelReceived.connect(
            [&state](std::uint64_t) {
                updateConfirmation(state, nullptr);
            }));
        state.connections.push_back(
            state.confirmation->playbackReconnectRequested.connect(
            [&state](std::uint64_t id) {
                requestReconnect(state, id);
            }));
        state.connections.push_back(
            state.confirmation->settingsReceived.connect(
            [&state](
                const SettingsSnapshotEvent &snapshot) {
                if (!state.settings)
                    return;
                std::string error;
                if (!state.settings->applySnapshot(
                        snapshot.json, snapshot.stateVersion, &error)) {
                    log(state, "settings snapshot rejected: " + error);
                    return;
                }
                state.resolvedLanguage = snapshot.resolvedLanguage;
                applyAuthoritativePicture(state);
                updateShell(state, state.resolvedLanguage);
            }));
        state.connections.push_back(
            state.confirmation->settingsSubmitFailed.connect(
            [&state](const std::string &reason) {
                if (state.settings)
                    state.settings->rejectPendingPatch();
                applyAuthoritativePicture(state);
                updateShell(state, state.resolvedLanguage);
                log(state, "settings submit failed: " + reason);
            }));
        state.connections.push_back(
            state.confirmation->settingsUpdateRejected.connect(
            [&state](const std::string &reason) {
                if (state.settings)
                    state.settings->rejectPendingPatch();
                applyAuthoritativePicture(state);
                updateShell(state, state.resolvedLanguage);
                log(state, "settings update rejected: " + reason);
            }));
        state.connections.push_back(
            state.confirmation->settingsRuntimeApplyFailed.connect(
            [&state](const std::string &reason) {
                log(state, "settings saved but runtime failed: " + reason);
            }));
        state.connections.push_back(
            state.confirmation->settingsCommitted.connect(
            [&state](
                std::uint64_t version,
                const std::string &language) {
                if (!state.settings)
                    return;
                state.settings->acceptPendingPatch(version);
                state.resolvedLanguage = language;
                updateShell(state, state.resolvedLanguage);
            }));
        state.connections.push_back(
            state.confirmation->connectionLost.connect(
            [&state](const std::string &reason) {
                state.controlConnected = false;
                state.fatalReason =
                    "ControlPipe disconnected: " + reason;
                if (state.playbackBridge)
                    state.playbackBridge->stop();
                updatePlayback(state);
                updateShell(state, state.resolvedLanguage);
                log(state, state.fatalReason);
            }));
        state.connections.push_back(
            state.confirmation->logMessage.connect(
            [&state](const std::string &message) {
                log(state, "ControlBridge: " + message);
            }));
        state.connections.push_back(
            state.settings->settingsPatchReady.connect(
            [&state](const std::string &json) {
                if (!state.confirmation || !state.settings)
                    return;
                std::string error;
                state.confirmation->submitSettings(
                    json, state.settings->version(), error);
            }));
    }

    if (!state.options.confirmationOnly) {
        state.player = std::make_unique<MpvCore>(state.loop);
        if (!state.player->start())
            return false;
        state.playerRunning = true;
        state.connections.push_back(state.player->renderUpdate.connect(
            [&state] { notifyRender(state); }));
        state.connections.push_back(state.player->logMessage.connect(
            [&state](const std::string &message) {
                log(state, "MPV: " + message);
            }));
        state.connections.push_back(state.player->runningChanged.connect(
            [&state](bool running) {
                state.playerRunning = running;
                state.loop.post([&state] {
                    updatePlayback(state);
                    updateRenderer(state);
                });
            }));

        state.playback = std::make_unique<PlaybackController>(
            state.player.get(), state.loop);
        state.mediaControls =
            std::make_shared<WindowsMediaControls>(state.loop);
        state.connections.push_back(
            state.mediaControls->logMessage.connect(
                [&state](const std::string &message) {
                    log(state, message);
                }));
        if (!state.mediaControls->attach()) {
            log(state, "Windows media controls will remain unavailable");
        }
        state.connections.push_back(
            state.mediaControls->playRequested.connect(
                [state = &state] {
                    if (state->playback)
                        state->playback->play();
                }));
        state.connections.push_back(
            state.mediaControls->pauseRequested.connect(
                [state = &state] {
                    if (state->playback)
                        state->playback->pause();
                }));
        state.connections.push_back(
            state.mediaControls->stopRequested.connect(
                [state = &state] {
                    if (state->playback)
                        state->playback->stop();
                }));
        state.connections.push_back(
            state.mediaControls->nextRequested.connect(
                [state = &state] {
                    if (state->playback)
                        state->playback->next();
                }));
        state.connections.push_back(
            state.mediaControls->previousRequested.connect(
                [state = &state] {
                    if (state->playback)
                        state->playback->previous();
                }));
        state.connections.push_back(
            state.mediaControls->seekRequested.connect(
                [state = &state](double seconds) {
                    if (state->playback)
                        state->playback->seekTo(seconds);
                }));
        state.connections.push_back(state.playback->logMessage.connect(
            [&state](const std::string &message) {
                log(state, "Playback: " + message);
            }));
        state.connections.push_back(state.playback->stateChanged.connect(
            [&state](PlaybackController::State) {
                schedulePlaybackRefresh(state);
                if (state.mediaControls)
                    state.mediaControls->setTransportState(
                        state.playback->state());
            }));
        state.connections.push_back(state.playback->nowPlayingChanged.connect(
            [&state](const NowPlaying &) {
                schedulePlaybackRefresh(state);
                if (state.mediaControls) {
                    state.mediaControls->setNowPlaying(
                        state.playback->nowPlaying());
                    state.mediaControls->setTransportState(
                        state.playback->state());
                }
            }));
        state.connections.push_back(state.playback->positionChanged.connect(
            [&state](double seconds) {
                schedulePlaybackRefresh(state);
                if (state.mediaControls)
                    state.mediaControls->setPosition(seconds);
            }));
        state.connections.push_back(state.playback->durationChanged.connect(
            [&state](double seconds) {
                schedulePlaybackRefresh(state);
                if (state.mediaControls)
                    state.mediaControls->setDuration(seconds);
            }));
        state.connections.push_back(state.playback->volumeChanged.connect(
            [&state](int) { schedulePlaybackRefresh(state); }));
        state.connections.push_back(state.playback->muteChanged.connect(
            [&state](bool) { schedulePlaybackRefresh(state); }));
        state.connections.push_back(state.playback->pictureControlChanged.connect(
            [&state](const std::string &name, int value) {
                schedulePlaybackRefresh(state);
                if (!state.applyingAuthoritativePicture
                    && state.settings) {
                    state.settings->setPictureValue(name, value);
                }
            }));
        state.connections.push_back(state.playback->peerConnectedChanged.connect(
            [&state] { schedulePlaybackRefresh(state); }));
        state.connections.push_back(state.playback->castSessionChanged.connect(
            [&state] { schedulePlaybackRefresh(state); }));
        state.connections.push_back(state.playback->queueChanged.connect(
            [&state](
                bool hasNext, bool hasPrevious,
                const std::string &, PlaybackController::PlayMode) {
                if (state.mediaControls) {
                    state.mediaControls->setQueueAvailability(
                        hasNext, hasPrevious);
                }
            }));

        applyAuthoritativePicture(state);
        if (auto playback = state.session.takePlaybackConnection()) {
            attachPlaybackBridge(
                state,
                std::shared_ptr<mcast::ipc::PipeConnection>(
                    std::move(playback)));
        } else {
            error = "PlaybackPipe was not connected";
            return false;
        }
        updatePlayback(state);
    }

    if (const auto delay =
            testDelay("MCAST_UI_TEST_DISCONNECT_PLAYBACK_MS")) {
        state.loop.scheduleAfter(*delay, [&state] {
            log(state, "Test fault: dropping PlaybackPipe");
            if (state.playbackBridge)
                state.playbackBridge->dropConnectionForTest();
        });
    }
    if (const auto delay =
            testDelay("MCAST_UI_TEST_DISCONNECT_CONTROL_MS")) {
        state.loop.scheduleAfter(*delay, [&state] {
            log(state, "Test fault: dropping ControlPipe");
            if (state.confirmation)
                state.confirmation->dropConnectionForTest();
        });
    }
    if (const auto delay =
            testDelay("MCAST_UI_TEST_CONTEXT_LOST_MS")) {
        state.loop.scheduleAfter(*delay, [&state] {
            log(state, "Test fault: losing OpenGL context");
            renderDetachImpl(
                true, "injected OpenGL context loss");
        });
    }

    state.reconnectWorker = std::jthread(
        [&state](std::stop_token stop) {
            reconnectLoop(state, stop);
        });
    updateRenderer(state);
    log(state, "Avalonia native host started");
    return true;
}

bool postToLoop(HostState &state, std::function<void()> task)
{
    if (state.stopping.load())
        return false;
    return state.loop.post(std::move(task));
}

int readSnapshot(
    const std::string HostState::*member,
    char *buffer, int capacity, int *required)
{
    if (required)
        *required = 0;
    std::string snapshot;
    {
        const std::scoped_lock lifecycle(g_lifecycleMutex);
        if (!g_state)
            return 0;
        const std::scoped_lock lock(g_state->snapshotMutex);
        snapshot = g_state.get()->*member;
    }
    const int bytes =
        static_cast<int>(snapshot.size() + 1);
    if (required)
        *required = bytes;
    if (!buffer || capacity < bytes)
        return 0;
    std::memcpy(buffer, snapshot.c_str(), snapshot.size() + 1);
    return 1;
}

bool renderAttachImpl(
    McastAvaloniaGetProcAddress getProcAddress,
    void *context)
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (!g_state || !g_state->player
        || !getProcAddress || g_state->contextLost.load()) {
        return false;
    }
    const std::scoped_lock renderLock(g_state->renderMutex);
    if (g_state->renderContext)
        return true;
    mpv_handle *mpv = g_state->player->handle();
    if (!mpv)
        return false;

    auto proc = std::make_unique<MpvProcContext>(
        MpvProcContext{getProcAddress, context});
    using GlGetString = const unsigned char *(*)(unsigned int);
    auto *getString = reinterpret_cast<GlGetString>(
        getProcAddress(context, "glGetString"));
    if (getString) {
        const unsigned char *renderer = getString(0x1F01);
        const std::string text =
            renderer
            ? reinterpret_cast<const char *>(renderer)
            : std::string{};
        const std::scoped_lock lock(g_state->snapshotMutex);
        g_state->glBackend =
            text.find("ANGLE") != std::string::npos
            ? "ANGLE" : "native-gl";
    }

    mpv_opengl_init_params init{
        &MpvProcContext::getProc, proc.get()};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE,
         const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &init},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    mpv_render_context *render = nullptr;
    const int result =
        mpv_render_context_create(&render, mpv, params);
    if (result < 0 || !render) {
        log(
            *g_state,
            "Render context create failed: "
                + std::string(mpv_error_string(result)));
        ++g_state->renderFailures;
        return false;
    }
    g_state->renderContext = render;
    g_state->renderProcContext = std::move(proc);
    g_state->rendererReady = false;
    const std::uint64_t generation =
        ++g_state->renderContextGeneration;
    mpv_render_context_set_update_callback(
        render,
        [](void *opaque) {
            auto *state = static_cast<HostState *>(opaque);
            if (state)
                notifyRender(*state);
        },
        g_state.get());
    log(
        *g_state,
        "Render context attached generation="
            + std::to_string(generation));
    return true;
}

void renderDetachImpl(bool lost, const std::string &reason)
{
    HostState *state = nullptr;
    mpv_render_context *render = nullptr;
    {
        const std::scoped_lock lifecycle(g_lifecycleMutex);
        if (!g_state)
            return;
        state = g_state.get();
        {
            const std::scoped_lock renderLock(state->renderMutex);
            render = state->renderContext;
            state->renderContext = nullptr;
        }
        if (lost) {
            state->contextLost = true;
            state->fatalReason = reason;
        }
        state->rendererReady = false;
        state->playbackUiReadySent = false;
        state->renderWake.clear();
    }
    if (render) {
        mpv_render_context_set_update_callback(render, nullptr, nullptr);
        mpv_render_context_free(render);
    }
    if (!state)
        return;
    state->renderProcContext.reset();
    state->loop.post([state] {
        if (state->player)
            state->player->setRendererAttached(false);
        updateRenderer(*state);
        updatePlayback(*state);
    });
    log(
        *state,
        lost ? "Render context lost: " + reason
             : "Render context detached");
}

} // namespace

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_create(
    int argumentCount, const char *const *arguments,
    McastAvaloniaLogCallback logCallback, void *context)
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (g_state)
        return 0;

    auto state = std::make_unique<HostState>();
    state->logCallback = logCallback;
    state->logContext = context;
    state->argumentStorage.emplace_back("MCast.Ui.exe");
    state->argumentStorage.reserve(
        static_cast<std::size_t>(std::max(argumentCount, 0)) + 1);
    for (int index = 0; index < argumentCount; ++index) {
        state->argumentStorage.emplace_back(
            arguments && arguments[index] ? arguments[index] : "");
    }
    for (std::string &argument : state->argumentStorage)
        state->arguments.push_back(argument.data());

    log(
        *state,
        "Native arguments count=" + std::to_string(argumentCount)
            + (state->argumentStorage.empty()
                ? std::string{}
                : " first=" + state->argumentStorage.front()));

    std::string error;
    state->options = UiSession::parseArguments(
        static_cast<int>(state->arguments.size()),
        state->arguments.data(), error);
    if (!error.empty()) {
        log(*state, "UiSession arguments failed: " + error);
        return 0;
    }
    if (!state->loop.start()) {
        log(*state, "Avalonia native host loop failed to start");
        return 0;
    }

    auto ready = std::make_shared<std::promise<bool>>();
    auto readyFuture = ready->get_future();
    state->loop.post([&state, ready] {
        std::string setupError;
        const bool ok = setupHost(*state, setupError);
        if (!ok)
            log(*state, "UiSession setup failed: " + setupError);
        ready->set_value(ok);
    });
    if (readyFuture.wait_for(20s) != std::future_status::ready
        || !readyFuture.get()) {
        state->loop.stop();
        return 0;
    }
    g_state = std::move(state);
    return 1;
}

MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_destroy()
{
    std::unique_ptr<HostState> state;
    {
        const std::scoped_lock lifecycle(g_lifecycleMutex);
        if (!g_state)
            return;
        state = std::move(g_state);
        state->stopping = true;
        state->projectionCallback = nullptr;
        state->renderUpdateCallback = nullptr;
    }

    stopReconnectWorker(*state);
    log(*state, "Avalonia shutdown: producers stopped");
    state->playbackLogConnection.disconnect();
    if (state->playbackBridge)
        state->playbackBridge->stop();
    if (state->confirmation)
        state->confirmation->stop();
    if (state->playbackRefreshTimer != 0) {
        state->loop.cancelTimer(state->playbackRefreshTimer);
        state->playbackRefreshTimer = 0;
    }

    auto barrierDone = std::make_shared<std::promise<void>>();
    auto barrier = barrierDone->get_future();
    const bool queued =
        state->loop.post([barrierDone] { barrierDone->set_value(); });
    const bool passed = queued
        && barrier.wait_for(10s) == std::future_status::ready;

    if (!passed) {
        log(*state, "Avalonia shutdown barrier did not complete");
        state->loop.stop();
    } else {
        log(*state, "Avalonia shutdown: event loop drained");
    }
    state->connections.clear();
    if (state->mediaControls)
        state->mediaControls->detach();
    if (state->player)
        state->player->setRendererAttached(false);
    mpv_render_context *render = nullptr;
    {
        const std::scoped_lock renderLock(state->renderMutex);
        render = state->renderContext;
        state->renderContext = nullptr;
    }
    if (render) {
        mpv_render_context_set_update_callback(render, nullptr, nullptr);
        mpv_render_context_free(render);
    }
    state->renderProcContext.reset();
    state->rendererReady = false;
    state->renderWake.clear();
    log(*state, "Avalonia shutdown: render context released");
    if (state->player)
        state->player->shutdown();
    log(*state, "Avalonia shutdown: MPV event thread stopped");
    state->loop.stop();
    log(*state, "Avalonia native host stopped");
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_projection_update_callback(
    McastAvaloniaProjectionUpdateCallback callback, void *context)
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (!g_state)
        return 0;
    g_state->projectionContext = context;
    g_state->projectionCallback = callback;
    return 1;
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_render_update_callback(
    McastAvaloniaRenderUpdateCallback callback, void *context)
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (!g_state)
        return 0;
    g_state->renderUpdateContext = context;
    g_state->renderUpdateCallback = callback;
    return 1;
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_shell_json(
    char *buffer, int capacity, int *required)
{
    return readSnapshot(&HostState::shellJson, buffer, capacity, required);
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_language_json(
    char *buffer, int capacity, int *required)
{
    return readSnapshot(
        &HostState::languageJson, buffer, capacity, required);
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_playback_json(
    char *buffer, int capacity, int *required)
{
    return readSnapshot(
        &HostState::playbackJson, buffer, capacity, required);
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_read_renderer_json(
    char *buffer, int capacity, int *required)
{
    return readSnapshot(
        &HostState::rendererJson, buffer, capacity, required);
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_read_confirmation_json(
    char *buffer, int capacity, int *required)
{
    return readSnapshot(
        &HostState::confirmationJson, buffer, capacity, required);
}

MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_notify_first_frame()
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (!g_state)
        return;
    postToLoop(*g_state, [state = g_state.get()] {
        if (state->options.confirmationOnly)
            return;
        state->windowPresented = true;
        if (state->confirmation
            && !state->uiFirstFrameSent.exchange(true)) {
            std::string error;
            if (!state->confirmation->sendFirstFrame(
                    state->options.activationId, error)) {
                state->uiFirstFrameSent = false;
                log(*state, "UiFirstFrame failed: " + error);
            } else {
                log(*state, "UiFirstFrame sent");
            }
        }
        evaluatePlaybackReady(*state);
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_render_attach(
    McastAvaloniaGetProcAddress getProcAddress, void *context)
{
    return renderAttachImpl(getProcAddress, context) ? 1 : 0;
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_render_frame(
    int framebuffer, int width, int height, int flipY)
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (!g_state || g_state->contextLost.load())
        return 0;
    const std::scoped_lock renderLock(g_state->renderMutex);
    if (!g_state->renderContext)
        return 0;
    g_state->renderWake.consume();
    mpv_render_context_update(g_state->renderContext);
    mpv_opengl_fbo fbo{framebuffer, width, height, 0};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo},
        {MPV_RENDER_PARAM_FLIP_Y, &flipY},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
    const int result =
        mpv_render_context_render(g_state->renderContext, params);
    ++g_state->renderCalls;
    const int previousFramebuffer =
        g_state->lastFramebuffer.exchange(framebuffer);
    const int previousWidth = g_state->lastWidth.exchange(width);
    const int previousHeight = g_state->lastHeight.exchange(height);
    bool targetChanged =
        previousFramebuffer != framebuffer
        || previousWidth != width
        || previousHeight != height;
    if (result < 0) {
        ++g_state->renderFailures;
        targetChanged = true;
    }
    if (targetChanged
        && !g_state->renderTargetRefreshPending.exchange(true)) {
        auto *state = g_state.get();
        state->loop.post([state] {
            state->renderTargetRefreshPending = false;
            updateRenderer(*state);
        });
    }
    if (result < 0) {
        return 0;
    }
    if (!g_state->rendererReady.exchange(true)) {
        auto *state = g_state.get();
        state->loop.post([state] {
            if (state->player)
                state->player->setRendererAttached(true);
            updateRenderer(*state);
            evaluatePlaybackReady(*state);
        });
    }
    return 1;
}

MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_render_detach()
{
    renderDetachImpl(false, {});
}

MCAST_AVALONIA_HOST_EXPORT void mcast_avalonia_host_render_lost(
    const char *reason)
{
    renderDetachImpl(true, reason ? reason : "OpenGL context lost");
}

namespace {

template <typename Function>
int postIntent(Function function)
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (!g_state)
        return 0;
    return postToLoop(
        *g_state,
        [state = g_state.get(), function = std::move(function)]() mutable {
            function(*state);
        })
        ? 1 : 0;
}

bool playbackCommandsAvailable(const HostState &state)
{
    return state.playback
        && state.playerRunning.load()
        && state.rendererReady.load()
        && !state.contextLost.load()
        && !state.stopping.load()
        && !state.closing.load();
}

} // namespace

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_toggle_play_pause()
{
    return postIntent([](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->togglePlayPause();
        log(state, "UI intent: TogglePlayPause");
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_seek(double seconds)
{
    return postIntent([seconds](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->seekTo(seconds);
        log(state, "UI intent: Seek " + std::to_string(seconds));
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_previous()
{
    return postIntent([](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->previous();
        log(state, "UI intent: Previous");
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_next()
{
    return postIntent([](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->next();
        log(state, "UI intent: Next");
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_set_volume(int percent)
{
    return postIntent([percent](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->setVolumePercent(percent);
        log(state, "UI intent: Volume " + std::to_string(percent));
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_set_muted(int muted)
{
    return postIntent([muted](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->setMuted(muted != 0);
        log(
            state,
            "UI intent: Muted "
                + std::string(muted ? "true" : "false"));
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_picture_control(
    const char *name, int value)
{
    if (!name)
        return 0;
    const std::string controlName(name);
    return postIntent([controlName, value](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->setPictureControl(controlName, value);
        log(
            state,
            "UI intent: Picture " + controlName
                + "=" + std::to_string(value));
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_reset_picture_controls()
{
    return postIntent([](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        state.playback->resetPictureControls();
        log(state, "UI intent: Reset picture");
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_begin_control_adjustment()
{
    return postIntent([](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        if (state.playbackBridge)
            state.playbackBridge->beginControlAdjustment();
        log(state, "UI intent: BeginControlAdjustment");
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_end_control_adjustment()
{
    return postIntent([](HostState &state) {
        if (!playbackCommandsAvailable(state)) {
            log(state, "UI intent rejected: player unavailable");
            return;
        }
        if (state.playbackBridge)
            state.playbackBridge->endControlAdjustment();
        log(state, "UI intent: EndControlAdjustment");
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_end_cast()
{
    return postIntent([](HostState &state) {
        if (state.playbackBridge)
            state.playbackBridge->requestEndSession();
        log(state, "UI intent: EndCast");
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_pause_and_flush_for_window_close()
{
    const std::scoped_lock lifecycle(g_lifecycleMutex);
    if (!g_state)
        return 0;
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    if (!g_state->loop.post([state = g_state.get(), done] {
            state->closing = true;
            if (state->playbackBridge)
                state->playbackBridge->pauseAndFlushState();
            if (state->playbackBridge)
                state->playbackBridge->stop();
            log(*state, "UI intent: PauseAndFlushForWindowClose");
            done->set_value();
        })) {
        return 0;
    }
    return future.wait_for(3s) == std::future_status::ready ? 1 : 0;
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_language(const char *language)
{
    if (!language)
        return 0;
    const std::string value(language);
    return postIntent([value](HostState &state) {
        if (state.settings) {
            state.settings->queueUiSetting(
                "language",
                JsonValue(value.empty() ? "System" : value));
        }
        log(
            state,
            "UI intent: Language "
                + (value.empty() ? std::string("System") : value));
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_theme_mode(int mode)
{
    return postIntent([mode](HostState &state) {
        const std::string value =
            mode == 1 ? "Light" : mode == 2 ? "Dark" : "System";
        if (state.settings)
            state.settings->queueUiSetting(
                "darkmode", JsonValue(value));
        log(state, "UI intent: Theme " + value);
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_accent_mode(int mode)
{
    return postIntent([mode](HostState &state) {
        const std::string value =
            mode == 1 ? "System" : "WindowsBlue";
        if (state.settings)
            state.settings->queueUiSetting(
                "accentmode", JsonValue(value));
        log(state, "UI intent: Accent " + value);
    });
}

MCAST_AVALONIA_HOST_EXPORT int mcast_avalonia_host_set_mica(int enabled)
{
    return postIntent([enabled](HostState &state) {
        if (state.settings)
            state.settings->setMica(enabled != 0);
        log(
            state,
            "UI intent: Mica "
                + std::string(enabled ? "true" : "false"));
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_accept_new_cast(int enabled)
{
    return postIntent([enabled](HostState &state) {
        if (state.settings)
            state.settings->setAcceptNewCast(enabled != 0);
        log(
            state,
            "UI intent: AcceptNewCast "
                + std::string(enabled ? "true" : "false"));
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_broadcast(int enabled)
{
    return postIntent([enabled](HostState &state) {
        if (state.settings)
            state.settings->setBroadcast(enabled != 0);
        log(
            state,
            "UI intent: Broadcast "
                + std::string(enabled ? "true" : "false"));
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_set_broadcast_interval(int milliseconds)
{
    return postIntent([milliseconds](HostState &state) {
        if (state.settings)
            state.settings->setBroadcastIntervalMs(milliseconds);
        log(
            state,
            "UI intent: BroadcastInterval "
                + std::to_string(milliseconds));
    });
}

MCAST_AVALONIA_HOST_EXPORT int
mcast_avalonia_host_respond_cast_confirmation(
    std::uint64_t requestId, int accepted)
{
    return postIntent([requestId, accepted](HostState &state) {
        if (state.confirmation)
            state.confirmation->submitDecision(
                requestId, accepted != 0);
        updateConfirmation(state, nullptr);
        log(
            state,
            "UI intent: Confirmation id="
                + std::to_string(requestId)
                + " accepted="
                + (accepted ? "true" : "false"));
    });
}
