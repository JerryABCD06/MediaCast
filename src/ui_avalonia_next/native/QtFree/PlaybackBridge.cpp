// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/PlaybackBridge.h"

#include "ipc/PlaybackCodec.h"

#include <chrono>
#include <sstream>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kMaxOrderedOutbound = 64;
constexpr std::size_t kMaxUrgentOutbound = 8;

playback::PlaybackState playbackState(
    PlaybackController::State state)
{
    switch (state) {
    case PlaybackController::State::NoMedia:
        return playback::PlaybackState::NoMedia;
    case PlaybackController::State::Stopped:
        return playback::PlaybackState::Stopped;
    case PlaybackController::State::Preparing:
        return playback::PlaybackState::Preparing;
    case PlaybackController::State::Playing:
        return playback::PlaybackState::Playing;
    case PlaybackController::State::Paused:
        return playback::PlaybackState::Paused;
    }
    return playback::PlaybackState::NoMedia;
}

playback::PlayMode wirePlayMode(PlaybackController::PlayMode mode)
{
    switch (mode) {
    case PlaybackController::PlayMode::Normal:
        return playback::PlayMode::Normal;
    case PlaybackController::PlayMode::RepeatOne:
        return playback::PlayMode::RepeatOne;
    case PlaybackController::PlayMode::RepeatAll:
        return playback::PlayMode::RepeatAll;
    case PlaybackController::PlayMode::Direct:
        return playback::PlayMode::Direct;
    }
    return playback::PlayMode::Normal;
}

const char *commandName(playback::PlaybackCommandType type)
{
    using playback::PlaybackCommandType;
    switch (type) {
    case PlaybackCommandType::Open: return "Open";
    case PlaybackCommandType::Play: return "Play";
    case PlaybackCommandType::Pause: return "Pause";
    case PlaybackCommandType::Stop: return "Stop";
    case PlaybackCommandType::Seek: return "Seek";
    case PlaybackCommandType::Next: return "Next";
    case PlaybackCommandType::Previous: return "Previous";
    case PlaybackCommandType::EndSession: return "EndSession";
    case PlaybackCommandType::SetNext: return "SetNext";
    case PlaybackCommandType::SetPlayMode: return "SetPlayMode";
    case PlaybackCommandType::SetVolume: return "SetVolume";
    case PlaybackCommandType::SetMuted: return "SetMuted";
    case PlaybackCommandType::SetPictureControl: return "SetPictureControl";
    case PlaybackCommandType::ResetPictureControls:
        return "ResetPictureControls";
    case PlaybackCommandType::SetPeerConnected: return "SetPeerConnected";
    case PlaybackCommandType::SetCastSession: return "SetCastSession";
    }
    return "Unknown";
}

const char *outcomeName(playback::PlaybackOutcomeStatus outcome)
{
    using playback::PlaybackOutcomeStatus;
    switch (outcome) {
    case PlaybackOutcomeStatus::Applied: return "Applied";
    case PlaybackOutcomeStatus::Rejected: return "Rejected";
    case PlaybackOutcomeStatus::StaleGeneration: return "StaleGeneration";
    }
    return "Unknown";
}

PlaybackController::PlayMode controllerPlayMode(
    playback::PlayMode mode)
{
    switch (mode) {
    case playback::PlayMode::Normal:
        return PlaybackController::PlayMode::Normal;
    case playback::PlayMode::RepeatOne:
        return PlaybackController::PlayMode::RepeatOne;
    case playback::PlayMode::RepeatAll:
        return PlaybackController::PlayMode::RepeatAll;
    case playback::PlayMode::Direct:
        return PlaybackController::PlayMode::Direct;
    }
    return PlaybackController::PlayMode::Normal;
}

std::string playbackPlanFingerprint(const MediaRequest &request)
{
    std::ostringstream stream;
    stream << request.uri << '\x1f'
           << static_cast<int>(request.route) << '\x1f'
           << static_cast<int>(request.kind) << '\x1f'
           << request.imageSequenceFrames.size();
    for (const auto &frame : request.imageSequenceFrames) {
        stream << '\x1f' << frame.uri << '\x1f'
               << frame.durationSeconds;
    }
    return stream.str();
}

MediaRequest controllerRequest(const playback::MediaRequest &request)
{
    MediaRequest result;
    result.uri = request.uri;
    result.metadata = request.metadata;
    result.title = request.title;
    result.artist = request.artist;
    result.creator = request.creator;
    result.album = request.album;
    result.description = request.description;
    result.subtitle = request.subtitle;
    result.longDescription = request.longDescription;
    result.artworkUri = request.artworkUri;
    result.protocol = request.protocol;
    switch (request.kind) {
    case playback::MediaKind::Unknown:
        result.kind = MediaKind::Unknown;
        break;
    case playback::MediaKind::Video:
        result.kind = MediaKind::Video;
        break;
    case playback::MediaKind::Audio:
        result.kind = MediaKind::Audio;
        break;
    case playback::MediaKind::Image:
        result.kind = MediaKind::Image;
        break;
    }
    switch (request.route) {
    case playback::MediaRoute::Native:
        result.route = MediaLoadRoute::Native;
        break;
    case playback::MediaRoute::ImageSequence:
        result.route = MediaLoadRoute::ImageSequence;
        break;
    case playback::MediaRoute::WrappedTransportStream:
        result.route = MediaLoadRoute::WrappedTransportStream;
        break;
    }
    result.imageSequenceFrames.reserve(
        request.imageSequenceFrames.size());
    for (const auto &frame : request.imageSequenceFrames) {
        result.imageSequenceFrames.push_back(
            MediaLoadFrame{frame.uri, frame.durationSeconds});
    }
    return result;
}

MediaSource controllerSource(playback::MediaSource source)
{
    return source == playback::MediaSource::Local
        ? MediaSource::Local : MediaSource::Cast;
}

void copyMediaFields(
    playback::MediaRequest &target,
    const MediaRequest &source)
{
    if (target.uri.empty())
        target.uri = source.uri;
    if (target.metadata.empty())
        target.metadata = source.metadata;
    if (target.title.empty())
        target.title = source.title;
    if (target.artist.empty())
        target.artist = source.artist;
    if (target.creator.empty())
        target.creator = source.creator;
    if (target.album.empty())
        target.album = source.album;
    if (target.description.empty())
        target.description = source.description;
    if (target.subtitle.empty())
        target.subtitle = source.subtitle;
    if (target.longDescription.empty())
        target.longDescription = source.longDescription;
    if (target.artworkUri.empty())
        target.artworkUri = source.artworkUri;
    if (target.protocol.empty())
        target.protocol = source.protocol;
}

void copyPlan(
    playback::MediaRequest &target, const MediaRequest &source,
    bool includePlan)
{
    target.imageSequenceFrames.clear();
    if (includePlan && source.route != MediaLoadRoute::Native) {
        target.route = source.route == MediaLoadRoute::ImageSequence
            ? playback::MediaRoute::ImageSequence
            : playback::MediaRoute::WrappedTransportStream;
        for (const auto &frame : source.imageSequenceFrames) {
            target.imageSequenceFrames.push_back(
                playback::ImageSequenceFrame{
                    frame.uri, frame.durationSeconds});
        }
    } else {
        target.route = playback::MediaRoute::Native;
    }
}

} // namespace

PlaybackBridge::PlaybackBridge(
    std::shared_ptr<ipc::PipeConnection> connection,
    PlaybackController &controller, EventLoop &loop,
    std::uint64_t generation)
    : m_connection(std::move(connection))
    , m_controller(controller)
    , m_loop(loop)
    , m_generation(generation == 0 ? 1 : generation)
{
    connectController();
    if (!m_connection || !m_connection->valid()) {
        m_connected = false;
        return;
    }

    m_writerThread = std::jthread(
        [this](std::stop_token stop) { writerLoop(stop); });
    m_readThread = std::jthread(
        [this](std::stop_token stop) { readLoop(stop); });

    m_sendReconnected = false;
    std::string error;
    auto reconnected = playback::wire::makePlaybackStateMessage(
        ipc::MessageType::PlaybackReconnected,
        m_stateVersion, snapshot(true), error);
    if (reconnected) {
        sendMessage(std::move(*reconnected), "reconnected");
        markPlaybackPlanSerialized();
        ++m_stateVersion;
    } else {
        logMessage.emit("Playback Reconnected 编码失败：" + error);
    }
}

PlaybackBridge::~PlaybackBridge()
{
    stop();
}

void PlaybackBridge::stop()
{
    if (m_stopped.exchange(true))
        return;
    m_connected = false;
    {
        const std::scoped_lock lock(m_outboundMutex);
        m_acceptOutbound = false;
    }
    m_writerThread.request_stop();
    m_outboundReady.notify_all();
    m_readThread.request_stop();
    if (m_connection)
        m_connection->close();
    if (m_readThread.joinable())
        m_readThread.join();
    if (m_writerThread.joinable())
        m_writerThread.join();
    m_controllerConnections.clear();
    cancelStateTimer();
}

void PlaybackBridge::connectController()
{
    m_controllerConnections.reserve(12);
    m_controllerConnections.push_back(
        m_controller.stateChanged.connect(
            [this](PlaybackController::State) { scheduleState(); }));
    m_controllerConnections.push_back(
        m_controller.nowPlayingChanged.connect(
            [this](const NowPlaying &) { scheduleState(); }));
    m_controllerConnections.push_back(
        m_controller.mediaChanged.connect(
            [this](const std::string &uri, const std::string &metadata) {
                if (uri.empty()) {
                    m_currentMedia = {};
                    m_nextMedia = {};
                    m_previousMedia = {};
                } else if (!m_nextMedia.uri.empty()
                           && m_nextMedia.uri == uri) {
                    m_previousMedia = m_currentMedia;
                    m_currentMedia = m_nextMedia;
                    m_nextMedia = {};
                } else if (!m_previousMedia.uri.empty()
                           && m_previousMedia.uri == uri) {
                    m_nextMedia = m_currentMedia;
                    m_currentMedia = m_previousMedia;
                    m_previousMedia = {};
                } else if (m_currentMedia.uri != uri) {
                    m_currentMedia = {};
                    m_currentMedia.uri = uri;
                    m_currentMedia.metadata = metadata;
                }
                if (!m_controller.hasNext())
                    m_nextMedia = {};
                if (!m_controller.hasPrevious())
                    m_previousMedia = {};
                scheduleState();
            }));
    m_controllerConnections.push_back(
        m_controller.queueChanged.connect(
            [this](
                bool, bool, const std::string &nextUri,
                PlaybackController::PlayMode) {
                if (!nextUri.empty() && m_nextMedia.uri != nextUri) {
                    m_nextMedia = {};
                    m_nextMedia.uri = nextUri;
                }
                if (nextUri.empty())
                    m_nextMedia = {};
                scheduleState();
            }));
    m_controllerConnections.push_back(
        m_controller.positionChanged.connect(
            [this](double) { scheduleState(); }));
    m_controllerConnections.push_back(
        m_controller.durationChanged.connect(
            [this](double) { scheduleState(); }));
    m_controllerConnections.push_back(
        m_controller.volumeChanged.connect(
            [this](int) { scheduleState(); }));
    m_controllerConnections.push_back(
        m_controller.muteChanged.connect(
            [this](bool) { scheduleState(); }));
    m_controllerConnections.push_back(
        m_controller.pictureControlChanged.connect(
            [this](const std::string &, int) { scheduleState(); }));
    m_controllerConnections.push_back(
        m_controller.playerLost.connect([this] {
            m_playerLost = true;
            m_sendPlayerLost = true;
            m_currentMedia = {};
            m_nextMedia = {};
            m_previousMedia = {};
            scheduleState();
        }));
}

bool PlaybackBridge::post(std::function<void()> task)
{
    if (m_stopped)
        return false;
    return m_loop.post(std::move(task));
}

void PlaybackBridge::requestEndSession()
{
    ipc::Message message;
    message.header.type = ipc::MessageType::UiEndCastRequest;
    message.header.source = ipc::ProcessRole::Ui;
    message.header.destination = ipc::ProcessRole::Protocol;
    message.header.messageId = m_nextRequestId.fetch_add(1);
    message.header.sessionId = 1;
    if (sendMessage(std::move(message), "end session", false, true)) {
        logMessage.emit("断开当前投送的请求已进入优先发送队列");
    } else {
        logMessage.emit("断开连接请求发送失败：发送队列不可用");
    }
}

void PlaybackBridge::dropConnectionForTest()
{
    if (m_connection)
        m_connection->close();
}

void PlaybackBridge::pauseAndFlushState()
{
    if (m_controller.hasMedia() && !m_controller.isPaused())
        m_controller.pause();
    cancelStateTimer();
    ++m_stateVersion;
    std::string error;
    auto message = playback::wire::makePlaybackStateMessage(
        ipc::MessageType::PlaybackStateDelta,
        m_stateVersion, snapshot(true), error);
    if (!message) {
        logMessage.emit("暂停状态编码失败：" + error);
        return;
    }
    bool written = false;
    if (m_connection) {
        const std::scoped_lock writeLock(m_writeMutex);
        written = m_connection->writeMessage(*message, 2s, error);
    }
    if (!written) {
        logMessage.emit("暂停状态同步失败：" + error);
    } else {
        markPlaybackPlanSerialized();
    }
}

void PlaybackBridge::beginControlAdjustment()
{
    if (m_controlAdjustmentActive)
        return;
    m_controlAdjustmentActive = true;
    cancelStateTimer();
    logMessage.emit(
        "UI adjustment began; Protocol publication deferred");
}

void PlaybackBridge::endControlAdjustment()
{
    if (!m_controlAdjustmentActive)
        return;
    m_controlAdjustmentActive = false;
    logMessage.emit(
        "UI adjustment ended; publishing final state");
    flushState();
}

bool PlaybackBridge::notifyUiReady()
{
    ipc::Message message;
    message.header.type = ipc::MessageType::PlaybackUiReady;
    message.header.source = ipc::ProcessRole::Ui;
    message.header.destination = ipc::ProcessRole::Protocol;
    message.header.messageId = m_nextRequestId.fetch_add(1);
    message.header.sessionId = 1;
    if (!sendMessage(
            std::move(message), "playback UI ready", false, true)) {
        logMessage.emit("Playback UI 就绪上报失败：发送队列不可用");
        return false;
    }
    return true;
}

void PlaybackBridge::readLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        ipc::Message message;
        std::string error;
        if (!m_connection
            || !m_connection->readMessage(message, 24h, error)) {
            m_connected = false;
            if (!stop.stop_requested()) {
                const std::string reason =
                    "Playback IPC 读取失败：" + error;
                post([this, reason] { logMessage.emit(reason); });
            }
            return;
        }
        post([this, message = std::move(message)]() mutable {
            dispatchMessage(std::move(message));
        });
    }
}

void PlaybackBridge::writerLoop(std::stop_token stop)
{
    while (true) {
        std::optional<ipc::Message> message;
        bool urgent = false;
        {
            std::unique_lock lock(m_outboundMutex);
            m_outboundReady.wait(lock, [&] {
                return stop.stop_requested() || !m_acceptOutbound
                    || !m_urgentOutbound.empty()
                    || !m_orderedOutbound.empty()
                    || m_latestState.has_value();
            });
            if (stop.stop_requested() || !m_acceptOutbound)
                return;
            if (!m_urgentOutbound.empty()) {
                message = std::move(m_urgentOutbound.front());
                m_urgentOutbound.pop_front();
                urgent = true;
            } else if (!m_orderedOutbound.empty()) {
                message = std::move(m_orderedOutbound.front());
                m_orderedOutbound.pop_front();
            } else if (m_latestState) {
                message = std::move(m_latestState);
                m_latestState.reset();
            } else {
                return;
            }
        }

        std::string error;
        bool written = false;
        const int attempts = urgent ? 3 : 1;
        for (int attempt = 0;
             attempt < attempts && !written
             && !stop.stop_requested();
             ++attempt) {
            if (attempt > 0)
                std::this_thread::sleep_for(50ms);
            if (m_connection) {
                const std::scoped_lock writeLock(m_writeMutex);
                written = m_connection->writeMessage(
                    *message, 2s, error);
            }
        }
        if (!written) {
            m_connected = false;
            const std::string failure = urgent
                ? "Playback IPC 优先消息写入失败：" + error
                : "Playback IPC 写入失败：" + error;
            post([this, failure] { logMessage.emit(failure); });
            if (urgent)
                return;
        }
    }
}

void PlaybackBridge::dispatchMessage(ipc::Message message)
{
    if (m_stopped)
        return;
    if (message.header.type == ipc::MessageType::PlaybackCommand) {
        std::uint64_t commandId = 0;
        playback::PlaybackCommand command;
        std::string error;
        if (!playback::wire::decodePlaybackCommandMessage(
                message, commandId, command, error)) {
            logMessage.emit("Playback 命令无效：" + error);
            return;
        }
        handleCommand(commandId, command);
    } else if (
        message.header.type
        == ipc::MessageType::PlaybackSnapshotRequest) {
        std::uint64_t requestId = 0;
        std::string error;
        if (!playback::wire::decodePlaybackSnapshotRequestMessage(
                message, requestId, error)) {
            logMessage.emit("Playback Snapshot 请求无效：" + error);
            return;
        }
        sendSnapshotResponse(requestId);
    }
}

void PlaybackBridge::handleCommand(
    std::uint64_t commandId,
    const playback::PlaybackCommand &command)
{
    logMessage.emit(
        "Playback command " + std::string(commandName(command.type))
        + " commandId=" + std::to_string(commandId)
        + " expectedGeneration="
        + std::to_string(command.expectedGeneration)
        + " currentGeneration=" + std::to_string(m_generation));
    if (command.expectedGeneration != m_generation) {
        sendCommandResult(
            commandId, playback::PlaybackOutcomeStatus::StaleGeneration,
            "stale playback generation");
        logMessage.emit(
            "Playback command StaleGeneration commandId="
            + std::to_string(commandId));
        return;
    }
    std::string error;
    const auto outcome = execute(command, error);
    logMessage.emit(
        "Playback command " + std::string(outcomeName(outcome))
        + " commandId=" + std::to_string(commandId)
        + (error.empty() ? std::string{} : " error=" + error));
    if (outcome == playback::PlaybackOutcomeStatus::Applied)
        ++m_stateVersion;
    sendCommandResult(commandId, outcome, error);
    cancelStateTimer();
    if (outcome == playback::PlaybackOutcomeStatus::Applied)
        flushState();
}

void PlaybackBridge::sendCommandResult(
    std::uint64_t commandId,
    playback::PlaybackOutcomeStatus outcome,
    const std::string &errorText)
{
    playback::wire::PlaybackCommandResultWire result;
    result.commandId = commandId;
    result.generation = m_generation;
    result.stateVersion = m_stateVersion;
    result.submissionStatus =
        playback::PlaybackSubmissionStatus::Queued;
    result.outcome = outcome;
    result.error = errorText;
    auto message =
        playback::wire::makePlaybackCommandResultMessage(
            result, result.error);
    if (!message) {
        logMessage.emit("Playback 结果编码失败：" + result.error);
        return;
    }
    sendMessage(*message, "command result");
}

void PlaybackBridge::sendSnapshotResponse(std::uint64_t requestId)
{
    if (m_stateVersion == 0)
        m_stateVersion = 1;
    std::string error;
    auto message =
        playback::wire::makePlaybackSnapshotResponseMessage(
            requestId + 1, requestId, snapshot(true), error);
    if (!message) {
        logMessage.emit("Playback Snapshot 编码失败：" + error);
        return;
    }
    sendMessage(*message, "snapshot response");
    markPlaybackPlanSerialized();
}

void PlaybackBridge::scheduleState()
{
    if (m_stopped || m_controlAdjustmentActive)
        return;
    if (m_connected && m_stateTimer == 0) {
        m_stateTimer = m_loop.scheduleAfter(16ms, [this] {
            m_stateTimer = 0;
            flushState();
        });
    }
}

void PlaybackBridge::cancelStateTimer()
{
    if (m_stateTimer != 0) {
        m_loop.cancelTimer(m_stateTimer);
        m_stateTimer = 0;
    }
}

void PlaybackBridge::flushState()
{
    if (!m_connected)
        return;
    ++m_stateVersion;
    const auto type = m_sendPlayerLost
        ? ipc::MessageType::PlaybackPlayerLost
        : (m_sendReconnected
            ? ipc::MessageType::PlaybackReconnected
            : ipc::MessageType::PlaybackStateDelta);
    m_sendPlayerLost = false;
    m_sendReconnected = false;
    std::string error;
    const bool includePlaybackPlan =
        playbackPlanNeedsSerialization();
    auto message = playback::wire::makePlaybackStateMessage(
        type, m_stateVersion, snapshot(includePlaybackPlan), error);
    if (!message) {
        logMessage.emit("Playback 状态编码失败：" + error);
        return;
    }
    sendMessage(
        std::move(*message), "state",
        type == ipc::MessageType::PlaybackStateDelta
            && !includePlaybackPlan);
    if (includePlaybackPlan)
        markPlaybackPlanSerialized();
}

bool PlaybackBridge::playbackPlanNeedsSerialization() const
{
    return playbackPlanFingerprint(m_controller.currentRequest())
            != m_lastPlanCurrentFingerprint
        || playbackPlanFingerprint(m_controller.nextRequest())
            != m_lastPlanNextFingerprint;
}

void PlaybackBridge::markPlaybackPlanSerialized()
{
    m_lastPlanCurrentFingerprint =
        playbackPlanFingerprint(m_controller.currentRequest());
    m_lastPlanNextFingerprint =
        playbackPlanFingerprint(m_controller.nextRequest());
}

playback::PlaybackSnapshot PlaybackBridge::snapshot(
    bool includePlaybackPlan) const
{
    playback::PlaybackSnapshot result;
    result.playbackPlanIncluded = includePlaybackPlan;
    result.state = playbackState(m_controller.state());
    result.loadStatus = m_controller.loadStatus()
            == PlaybackController::LoadStatus::Failed
        ? playback::LoadStatus::Failed : playback::LoadStatus::Ok;

    result.current = m_currentMedia;
    const auto current = m_controller.currentRequest();
    copyMediaFields(result.current, current);
    copyPlan(result.current, current, includePlaybackPlan);
    const auto &now = m_controller.nowPlaying();
    switch (now.kind) {
    case MediaKind::Unknown: break;
    case MediaKind::Video:
        result.current.kind = playback::MediaKind::Video;
        break;
    case MediaKind::Audio:
        result.current.kind = playback::MediaKind::Audio;
        break;
    case MediaKind::Image:
        result.current.kind = playback::MediaKind::Image;
        break;
    }
    result.current.source = now.source == MediaSource::Local
        ? playback::MediaSource::Local : playback::MediaSource::Cast;
    if (result.current.title.empty())
        result.current.title = now.title;
    if (result.current.artist.empty())
        result.current.artist = now.artist;
    if (result.current.album.empty())
        result.current.album = now.album;
    if (m_controller.mediaSizeBytes() > 0) {
        result.current.mediaSizeBytes =
            static_cast<std::uint64_t>(m_controller.mediaSizeBytes());
    }

    result.next = m_nextMedia;
    const auto next = m_controller.nextRequest();
    copyMediaFields(result.next, next);
    copyPlan(result.next, next, includePlaybackPlan);
    switch (next.kind) {
    case MediaKind::Unknown: break;
    case MediaKind::Video:
        result.next.kind = playback::MediaKind::Video;
        break;
    case MediaKind::Audio:
        result.next.kind = playback::MediaKind::Audio;
        break;
    case MediaKind::Image:
        result.next.kind = playback::MediaKind::Image;
        break;
    }
    result.next.source = result.current.source;
    if (!m_controller.hasNext())
        result.next = {};
    if (m_controller.state() == PlaybackController::State::NoMedia
        && result.current.uri.empty()) {
        result.current = {};
    }

    result.playMode = wirePlayMode(m_controller.playMode());
    result.hasNext = m_controller.hasNext();
    result.hasPrevious = m_controller.hasPrevious();
    result.peerConnected = m_controller.peerConnected();
    const auto &session = m_controller.castSession();
    result.castSession.connected = session.connected;
    result.castSession.protocol = session.protocol;
    result.castSession.controlPointName = session.controlPointName;
    result.castSession.controlPointAddress =
        session.controlPointAddress;
    result.castSession.remoteControlRegistered =
        session.remoteControlRegistered;
    result.playerAvailable = !m_playerLost;
    result.positionSeconds = m_controller.positionSeconds();
    result.durationSeconds = m_controller.durationSeconds();
    result.volumePercent = m_controller.volumePercent();
    result.muted = m_controller.isMuted();
    const auto controls = m_controller.pictureControls();
    result.pictureControls.reserve(controls.size());
    for (const auto &control : controls) {
        result.pictureControls.push_back(playback::PictureControl{
            control.name,
            control.minimum,
            control.maximum,
            control.neutral,
            m_controller.pictureControlValue(control.name),
        });
    }
    result.generation = m_generation;
    result.stateVersion = m_stateVersion;
    return result;
}

playback::PlaybackOutcomeStatus PlaybackBridge::execute(
    const playback::PlaybackCommand &command, std::string &error)
{
    using playback::PlaybackCommandType;
    using playback::PlaybackOutcomeStatus;
    if (m_playerLost
        && command.type != PlaybackCommandType::SetPeerConnected
        && command.type != PlaybackCommandType::SetCastSession
        && command.type != PlaybackCommandType::EndSession) {
        error = "player is unavailable";
        return PlaybackOutcomeStatus::Rejected;
    }

    switch (command.type) {
    case PlaybackCommandType::Open:
        if (command.media.uri.empty()) {
            error = "open requires a URI";
            return PlaybackOutcomeStatus::Rejected;
        }
        if (command.boolValue)
            m_controller.clearPlaylist();
        m_controller.openUri(
            controllerRequest(command.media),
            controllerSource(command.media.source));
        m_currentMedia = command.media;
        break;
    case PlaybackCommandType::Play:
        if (m_controller.state() == PlaybackController::State::NoMedia) {
            error = "no media";
            return PlaybackOutcomeStatus::Rejected;
        }
        m_controller.play();
        break;
    case PlaybackCommandType::Pause:
        if (m_controller.state() == PlaybackController::State::NoMedia) {
            error = "no media";
            return PlaybackOutcomeStatus::Rejected;
        }
        m_controller.pause();
        break;
    case PlaybackCommandType::Stop:
        if (m_controller.state() == PlaybackController::State::NoMedia) {
            error = "no media";
            return PlaybackOutcomeStatus::Rejected;
        }
        m_controller.stop();
        break;
    case PlaybackCommandType::Seek:
        if (m_controller.state() == PlaybackController::State::NoMedia) {
            error = "no media";
            return PlaybackOutcomeStatus::Rejected;
        }
        m_controller.seekTo(command.doubleValue);
        break;
    case PlaybackCommandType::Next:
        if (!m_controller.hasNext()) {
            error = "no next item";
            return PlaybackOutcomeStatus::Rejected;
        }
        if (!m_nextMedia.uri.empty()) {
            m_previousMedia = m_currentMedia;
            m_currentMedia = m_nextMedia;
            m_nextMedia = {};
        }
        m_controller.next();
        break;
    case PlaybackCommandType::Previous:
        if (!m_controller.hasPrevious()) {
            error = "no previous item";
            return PlaybackOutcomeStatus::Rejected;
        }
        if (!m_previousMedia.uri.empty()) {
            m_nextMedia = m_currentMedia;
            m_currentMedia = m_previousMedia;
            m_previousMedia = {};
        }
        m_controller.previous();
        break;
    case PlaybackCommandType::EndSession:
        m_currentMedia = {};
        m_nextMedia = {};
        m_previousMedia = {};
        m_controller.endSession();
        break;
    case PlaybackCommandType::SetNext:
        m_nextMedia = command.media;
        m_controller.setNextUri(controllerRequest(command.media));
        break;
    case PlaybackCommandType::SetPlayMode:
        m_controller.setPlayMode(controllerPlayMode(command.playMode));
        break;
    case PlaybackCommandType::SetVolume:
        m_controller.setVolumePercent(command.intValue);
        break;
    case PlaybackCommandType::SetMuted:
        m_controller.setMuted(command.boolValue);
        break;
    case PlaybackCommandType::SetPictureControl:
        if (!m_controller.setPictureControl(
                command.text, command.intValue)) {
            error = "unknown picture control";
            return PlaybackOutcomeStatus::Rejected;
        }
        break;
    case PlaybackCommandType::ResetPictureControls:
        m_controller.resetPictureControls();
        break;
    case PlaybackCommandType::SetPeerConnected:
        if (m_controller.peerConnected() != command.boolValue) {
            m_nextMedia = {};
            m_previousMedia = {};
        }
        m_controller.setPeerConnected(command.boolValue);
        break;
    case PlaybackCommandType::SetCastSession:
        m_controller.setCastSession(CastSession{
            command.castSession.connected,
            command.castSession.protocol,
            command.castSession.controlPointName,
            command.castSession.controlPointAddress,
            command.castSession.remoteControlRegistered,
        });
        break;
    }
    return PlaybackOutcomeStatus::Applied;
}

bool PlaybackBridge::sendMessage(
    ipc::Message message, const char *what,
    bool coalescible, bool urgent)
{
    if (!m_connected || !m_connection)
        return false;
    bool overflow = false;
    {
        const std::scoped_lock lock(m_outboundMutex);
        if (!m_acceptOutbound)
            return false;
        if (coalescible) {
            m_latestState = std::move(message);
        } else if (urgent) {
            if (m_urgentOutbound.size() >= kMaxUrgentOutbound)
                overflow = true;
            else
                m_urgentOutbound.push_back(std::move(message));
        } else {
            if (m_orderedOutbound.size() >= kMaxOrderedOutbound)
                overflow = true;
            else
                m_orderedOutbound.push_back(std::move(message));
        }
    }
    if (overflow) {
        logMessage.emit(
            "Playback 发送队列已满：" + std::string(what));
        return false;
    }
    m_outboundReady.notify_one();
    return true;
}

} // namespace mcast::avalonia::qtfree

