// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ui_process/PlaybackBridge.h"

#include "core/PlaybackController.h"
#include "ipc/PlaybackCodec.h"

#include <QMetaObject>
#include <QStringList>

#include <utility>

namespace mcast::ui_process {
using namespace std::chrono_literals;
namespace {

constexpr std::size_t kMaxOrderedOutbound = 64;
constexpr std::size_t kMaxUrgentOutbound = 8;

using playback::LoadStatus;
using playback::ImageSequenceFrame;
using playback::MediaKind;
using playback::MediaRequest;
using playback::MediaRoute;
using playback::MediaSource;
using playback::PictureControl;
using playback::PlayMode;
using playback::PlaybackCommand;
using playback::PlaybackCommandType;
using playback::PlaybackOutcomeStatus;
using playback::PlaybackSnapshot;
using playback::PlaybackState;

PlaybackState playbackState(PlaybackController::State state)
{
    switch (state) {
    case PlaybackController::State::NoMedia:
        return PlaybackState::NoMedia;
    case PlaybackController::State::Stopped:
        return PlaybackState::Stopped;
    case PlaybackController::State::Preparing:
        return PlaybackState::Preparing;
    case PlaybackController::State::Playing:
        return PlaybackState::Playing;
    case PlaybackController::State::Paused:
        return PlaybackState::Paused;
    }
    return PlaybackState::NoMedia;
}

PlayMode playMode(PlaybackController::PlayMode mode)
{
    switch (mode) {
    case PlaybackController::PlayMode::Normal:
        return PlayMode::Normal;
    case PlaybackController::PlayMode::RepeatOne:
        return PlayMode::RepeatOne;
    case PlaybackController::PlayMode::RepeatAll:
        return PlayMode::RepeatAll;
    case PlaybackController::PlayMode::Direct:
        return PlayMode::Direct;
    }
    return PlayMode::Normal;
}

PlaybackController::PlayMode controllerPlayMode(PlayMode mode)
{
    switch (mode) {
    case PlayMode::Normal:
        return PlaybackController::PlayMode::Normal;
    case PlayMode::RepeatOne:
        return PlaybackController::PlayMode::RepeatOne;
    case PlayMode::RepeatAll:
        return PlaybackController::PlayMode::RepeatAll;
    case PlayMode::Direct:
        return PlaybackController::PlayMode::Direct;
    }
    return PlaybackController::PlayMode::Normal;
}

QString playbackPlanFingerprint(
    const PlaybackController::MediaRequest &request)
{
    QStringList parts;
    parts.append(request.uri);
    parts.append(QString::number(
        static_cast<int>(request.route)));
    parts.append(QString::number(
        static_cast<int>(request.kind)));
    parts.append(QString::number(request.imageSequenceFrames.size()));
    for (const MediaLoadFrame &frame : request.imageSequenceFrames) {
        parts.append(frame.uri);
        parts.append(QString::number(
            frame.durationSeconds, 'g', 17));
    }
    return parts.join(QChar(0x1f));
}

PlaybackController::MediaRequest controllerRequest(
    const MediaRequest &request)
{
    PlaybackController::MediaRequest result;
    result.uri = QString::fromStdString(request.uri);
    result.metadata = QString::fromStdString(request.metadata);
    result.title = QString::fromStdString(request.title);
    result.artist = QString::fromStdString(request.artist);
    result.creator = QString::fromStdString(request.creator);
    result.album = QString::fromStdString(request.album);
    result.description = QString::fromStdString(request.description);
    result.subtitle = QString::fromStdString(request.subtitle);
    result.longDescription =
        QString::fromStdString(request.longDescription);
    result.artworkUri = QString::fromStdString(request.artworkUri);
    result.protocol = QString::fromStdString(request.protocol);
    switch (request.kind) {
    case MediaKind::Unknown:
        result.kind = ::MediaKind::Unknown;
        break;
    case MediaKind::Video:
        result.kind = ::MediaKind::Video;
        break;
    case MediaKind::Audio:
        result.kind = ::MediaKind::Audio;
        break;
    case MediaKind::Image:
        result.kind = ::MediaKind::Image;
        break;
    }
    switch (request.route) {
    case MediaRoute::ImageSequence:
        result.route = MediaLoadRoute::ImageSequence;
        break;
    case MediaRoute::WrappedTransportStream:
        result.route = MediaLoadRoute::WrappedTransportStream;
        break;
    case MediaRoute::Native:
        result.route = MediaLoadRoute::Native;
        break;
    }
    result.imageSequenceFrames.reserve(
        static_cast<qsizetype>(request.imageSequenceFrames.size()));
    for (const ImageSequenceFrame &frame : request.imageSequenceFrames) {
        result.imageSequenceFrames.push_back(MediaLoadFrame{
            QString::fromStdString(frame.uri),
            frame.durationSeconds,
        });
    }
    return result;
}

::MediaSource controllerSource(MediaSource source)
{
    return source == MediaSource::Local
        ? ::MediaSource::Local : ::MediaSource::Cast;
}

} // namespace

PlaybackBridge::PlaybackBridge(
    std::shared_ptr<ipc::PipeConnection> connection,
    PlaybackController &controller, std::uint64_t generation,
    QObject *parent)
    : QObject(parent)
    , m_connection(std::move(connection))
    , m_controller(controller)
    , m_generation(generation == 0 ? 1 : generation)
{
    m_stateTimer.setSingleShot(true);
    m_stateTimer.setInterval(16);
    connect(&m_stateTimer, &QTimer::timeout, this, [this] { flushState(); });

    connect(&m_controller, &PlaybackController::stateChanged,
            this, [this] { scheduleState(); });
    connect(&m_controller, &PlaybackController::nowPlayingChanged,
            this, [this] { scheduleState(); });
    connect(&m_controller, &PlaybackController::mediaChanged,
            this, [this](const QString &uri, const QString &metadata) {
        const std::string nextUri = uri.toStdString();
        if (nextUri.empty()) {
            m_currentMedia = {};
            m_nextMedia = {};
            m_previousMedia = {};
        } else if (!m_nextMedia.uri.empty()
                   && m_nextMedia.uri == nextUri) {
            m_previousMedia = m_currentMedia;
            m_currentMedia = m_nextMedia;
            m_nextMedia = {};
        } else if (!m_previousMedia.uri.empty()
                   && m_previousMedia.uri == nextUri) {
            m_nextMedia = m_currentMedia;
            m_currentMedia = m_previousMedia;
            m_previousMedia = {};
        } else if (m_currentMedia.uri != nextUri) {
            m_currentMedia = {};
            m_currentMedia.uri = nextUri;
            m_currentMedia.metadata = metadata.toStdString();
        }
        if (!m_controller.hasNext())
            m_nextMedia = {};
        if (!m_controller.hasPrevious())
            m_previousMedia = {};
        scheduleState();
    });
    connect(&m_controller, &PlaybackController::queueChanged,
            this, [this](
                bool, bool, const QString &nextUri,
                PlaybackController::PlayMode) {
        const std::string uri = nextUri.toStdString();
        if (!uri.empty() && m_nextMedia.uri != uri) {
            m_nextMedia = {};
            m_nextMedia.uri = uri;
        }
        scheduleState();
    });
    connect(&m_controller, &PlaybackController::positionChanged,
            this, [this] { scheduleState(); });
    connect(&m_controller, &PlaybackController::durationChanged,
            this, [this] { scheduleState(); });
    connect(&m_controller, &PlaybackController::volumeChanged,
            this, [this] { scheduleState(); });
    connect(&m_controller, &PlaybackController::muteChanged,
            this, [this] { scheduleState(); });
    connect(&m_controller, &PlaybackController::pictureControlChanged,
            this, [this] { scheduleState(); });
    connect(&m_controller, &PlaybackController::playerLost,
            this, [this] {
        m_playerLost = true;
        m_sendPlayerLost = true;
        m_currentMedia = {};
        m_nextMedia = {};
        m_previousMedia = {};
        scheduleState();
    });

    if (m_connection && m_connection->valid()) {
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
            emit logMessage(
                QStringLiteral("Playback Reconnected 编码失败：%1")
                    .arg(QString::fromStdString(error)));
        }
    } else {
        m_connected = false;
    }
}

PlaybackBridge::~PlaybackBridge()
{
    m_stateTimer.stop();
    {
        std::scoped_lock lock(m_outboundMutex);
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
}

void PlaybackBridge::requestEndSession()
{
    ipc::Message message;
    message.header.type = ipc::MessageType::UiEndCastRequest;
    message.header.source = ipc::ProcessRole::Ui;
    message.header.destination = ipc::ProcessRole::Protocol;
    message.header.messageId = m_nextRequestId.fetch_add(1);
    message.header.sessionId = 1;
    if (sendMessage(
            std::move(message), "end session", false, true)) {
        emit logMessage(QStringLiteral("断开当前投送的请求已进入优先发送队列"));
    } else {
        emit logMessage(QStringLiteral("断开连接请求发送失败：发送队列不可用"));
    }
}

void PlaybackBridge::pauseAndFlushState()
{
    if (m_controller.hasMedia() && !m_controller.isPaused())
        m_controller.pause();
    m_stateTimer.stop();
    ++m_stateVersion;
    std::string error;
    // This path writes synchronously because the window may be about to exit
    // and the bridge destructor will close the writer. Always include the
    // current plan so this direct, higher-version message cannot hide a plan
    // that was still queued behind the writer.
    auto message = playback::wire::makePlaybackStateMessage(
        ipc::MessageType::PlaybackStateDelta,
        m_stateVersion, snapshot(true), error);
    if (!message) {
        emit logMessage(
            QStringLiteral("暂停状态编码失败：%1")
                .arg(QString::fromStdString(error)));
        return;
    }
    bool written = false;
    if (m_connection) {
        std::scoped_lock writeLock(m_writeMutex);
        written = m_connection->writeMessage(*message, 2000ms, error);
    }
    if (!written) {
        emit logMessage(
            QStringLiteral("暂停状态同步失败：%1")
                .arg(QString::fromStdString(error)));
    } else {
        markPlaybackPlanSerialized();
    }
}

void PlaybackBridge::notifyUiReady()
{
    ipc::Message message;
    message.header.type = ipc::MessageType::PlaybackUiReady;
    message.header.source = ipc::ProcessRole::Ui;
    message.header.destination = ipc::ProcessRole::Protocol;
    message.header.messageId = m_nextRequestId.fetch_add(1);
    message.header.sessionId = 1;
    if (!sendMessage(
            std::move(message), "playback UI ready", false, true)) {
        emit logMessage(QStringLiteral("Playback UI 就绪上报失败：发送队列不可用"));
    }
}

void PlaybackBridge::readLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        ipc::Message message;
        std::string error;
        if (!m_connection->readMessage(message, 24h, error)) {
            m_connected = false;
            if (!stop.stop_requested())
                emit logMessage(
                    QStringLiteral("Playback IPC 读取失败：%1")
                        .arg(QString::fromStdString(error)));
            return;
        }
        QMetaObject::invokeMethod(
            this, [this, message = std::move(message)] {
                dispatchMessage(message);
            }, Qt::QueuedConnection);
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
                std::scoped_lock writeLock(m_writeMutex);
                written = m_connection->writeMessage(
                    *message, 2000ms, error);
            }
        }
        if (!written) {
            m_connected = false;
            const QString failureText = urgent
                ? QStringLiteral("Playback IPC 优先消息写入失败：%1")
                : QStringLiteral("Playback IPC 写入失败：%1");
            emit logMessage(
                failureText.arg(QString::fromStdString(error)));
            if (urgent)
                return;
        }
    }
}

void PlaybackBridge::dispatchMessage(const ipc::Message &message)
{
    if (message.header.type == ipc::MessageType::PlaybackCommand) {
        std::uint64_t commandId = 0;
        PlaybackCommand command;
        std::string error;
        if (!playback::wire::decodePlaybackCommandMessage(
                message, commandId, command, error)) {
            emit logMessage(
                QStringLiteral("Playback 命令无效：%1")
                    .arg(QString::fromStdString(error)));
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
            emit logMessage(
                QStringLiteral("Playback Snapshot 请求无效：%1")
                    .arg(QString::fromStdString(error)));
            return;
        }
        sendSnapshotResponse(requestId);
    }
}

void PlaybackBridge::handleCommand(
    std::uint64_t commandId, const PlaybackCommand &command)
{
    if (command.expectedGeneration != m_generation) {
        sendCommandResult(
            commandId, PlaybackOutcomeStatus::StaleGeneration,
            "stale playback generation");
        return;
    }
    std::string error;
    const PlaybackOutcomeStatus outcome = execute(command, error);
    if (outcome == PlaybackOutcomeStatus::Applied) {
        ++m_stateVersion;
    }
    sendCommandResult(commandId, outcome, error);
    m_stateTimer.stop();
    if (outcome == PlaybackOutcomeStatus::Applied)
        flushState();
}

void PlaybackBridge::sendCommandResult(
    std::uint64_t commandId, PlaybackOutcomeStatus outcome,
    const std::string &errorText)
{
    playback::wire::PlaybackCommandResultWire result;
    result.commandId = commandId;
    result.generation = m_generation;
    result.stateVersion = m_stateVersion;
    result.submissionStatus = playback::PlaybackSubmissionStatus::Queued;
    result.outcome = outcome;
    result.error = errorText;
    auto message =
        playback::wire::makePlaybackCommandResultMessage(result, result.error);
    if (!message) {
        emit logMessage(
            QStringLiteral("Playback 结果编码失败：%1")
                .arg(QString::fromStdString(result.error)));
        return;
    }
    sendMessage(*message, "command result");
}

void PlaybackBridge::sendSnapshotResponse(std::uint64_t requestId)
{
    if (m_stateVersion == 0)
        m_stateVersion = 1;
    std::string error;
    auto message = playback::wire::makePlaybackSnapshotResponseMessage(
        requestId + 1, requestId, snapshot(true), error);
    if (!message) {
        emit logMessage(
            QStringLiteral("Playback Snapshot 编码失败：%1")
                .arg(QString::fromStdString(error)));
        return;
    }
    sendMessage(*message, "snapshot response");
    markPlaybackPlanSerialized();
}

void PlaybackBridge::scheduleState()
{
    if (m_connected && !m_stateTimer.isActive())
        m_stateTimer.start();
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
    auto message =
        playback::wire::makePlaybackStateMessage(
            type, m_stateVersion,
            snapshot(includePlaybackPlan), error);
    if (!message) {
        emit logMessage(
            QStringLiteral("Playback 状态编码失败：%1")
                .arg(QString::fromStdString(error)));
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

PlaybackSnapshot PlaybackBridge::snapshot(bool includePlaybackPlan) const
{
    PlaybackSnapshot result;
    result.playbackPlanIncluded = includePlaybackPlan;
    result.state = playbackState(m_controller.state());
    result.loadStatus = m_controller.loadStatus()
            == PlaybackController::LoadStatus::Failed
        ? LoadStatus::Failed : LoadStatus::Ok;
    result.current = m_currentMedia;
    if (!m_controller.currentUri().isEmpty())
        result.current.uri = m_controller.currentUri().toStdString();
    if (!m_controller.currentMetadata().isEmpty())
        result.current.metadata =
            m_controller.currentMetadata().toStdString();
    const auto controllerCurrent = m_controller.currentRequest();
    if (result.current.uri.empty())
        result.current.uri = controllerCurrent.uri.toStdString();
    if (result.current.metadata.empty())
        result.current.metadata = controllerCurrent.metadata.toStdString();
    if (result.current.description.empty())
        result.current.description =
            controllerCurrent.description.toStdString();
    if (result.current.creator.empty())
        result.current.creator =
            controllerCurrent.creator.toStdString();
    if (result.current.subtitle.empty())
        result.current.subtitle =
            controllerCurrent.subtitle.toStdString();
    if (result.current.longDescription.empty())
        result.current.longDescription =
            controllerCurrent.longDescription.toStdString();
    if (result.current.artworkUri.empty())
        result.current.artworkUri =
            controllerCurrent.artworkUri.toStdString();
    if (result.current.protocol.empty())
        result.current.protocol =
            controllerCurrent.protocol.toStdString();
    result.current.imageSequenceFrames.clear();
    if (includePlaybackPlan
        && controllerCurrent.route != MediaLoadRoute::Native) {
        result.current.route =
            controllerCurrent.route == MediaLoadRoute::ImageSequence
            ? MediaRoute::ImageSequence
            : MediaRoute::WrappedTransportStream;
        for (const MediaLoadFrame &frame :
             controllerCurrent.imageSequenceFrames) {
            result.current.imageSequenceFrames.push_back(ImageSequenceFrame{
                frame.uri.toStdString(),
                frame.durationSeconds,
            });
        }
    } else {
        result.current.route = MediaRoute::Native;
    }
    const NowPlaying now = m_controller.nowPlaying();
    if (now.kind != ::MediaKind::Unknown) {
        switch (now.kind) {
        case ::MediaKind::Unknown:
            break;
        case ::MediaKind::Video:
            result.current.kind = MediaKind::Video;
            break;
        case ::MediaKind::Audio:
            result.current.kind = MediaKind::Audio;
            break;
        case ::MediaKind::Image:
            result.current.kind = MediaKind::Image;
            break;
        }
    }
    result.current.source = now.source == ::MediaSource::Local
        ? MediaSource::Local : MediaSource::Cast;
    if (result.current.title.empty())
        result.current.title = now.title.toStdString();
    if (result.current.artist.empty())
        result.current.artist = now.artist.toStdString();
    if (result.current.album.empty())
        result.current.album = now.album.toStdString();
    if (m_controller.mediaSizeBytes() > 0) {
        result.current.mediaSizeBytes =
            static_cast<std::uint64_t>(m_controller.mediaSizeBytes());
    }
    result.next = m_nextMedia;
    const auto controllerNext = m_controller.nextRequest();
    if (result.next.uri.empty())
        result.next.uri = controllerNext.uri.toStdString();
    if (result.next.metadata.empty())
        result.next.metadata = controllerNext.metadata.toStdString();
    if (result.next.title.empty())
        result.next.title = controllerNext.title.toStdString();
    if (result.next.artist.empty())
        result.next.artist = controllerNext.artist.toStdString();
    if (result.next.creator.empty())
        result.next.creator = controllerNext.creator.toStdString();
    if (result.next.album.empty())
        result.next.album = controllerNext.album.toStdString();
    if (result.next.description.empty())
        result.next.description =
            controllerNext.description.toStdString();
    if (result.next.subtitle.empty())
        result.next.subtitle = controllerNext.subtitle.toStdString();
    if (result.next.longDescription.empty())
        result.next.longDescription =
            controllerNext.longDescription.toStdString();
    if (result.next.artworkUri.empty())
        result.next.artworkUri =
            controllerNext.artworkUri.toStdString();
    if (result.next.protocol.empty())
        result.next.protocol =
            controllerNext.protocol.toStdString();
    result.next.imageSequenceFrames.clear();
    if (includePlaybackPlan
        && controllerNext.route != MediaLoadRoute::Native) {
        result.next.route =
            controllerNext.route == MediaLoadRoute::ImageSequence
            ? MediaRoute::ImageSequence
            : MediaRoute::WrappedTransportStream;
        for (const MediaLoadFrame &frame :
             controllerNext.imageSequenceFrames) {
            result.next.imageSequenceFrames.push_back(ImageSequenceFrame{
                frame.uri.toStdString(),
                frame.durationSeconds,
            });
        }
    } else {
        result.next.route = MediaRoute::Native;
    }
    switch (controllerNext.kind) {
    case ::MediaKind::Unknown:
        break;
    case ::MediaKind::Video:
        result.next.kind = MediaKind::Video;
        break;
    case ::MediaKind::Audio:
        result.next.kind = MediaKind::Audio;
        break;
    case ::MediaKind::Image:
        result.next.kind = MediaKind::Image;
        break;
    }
    result.next.source = result.current.source;
    if (!m_controller.nextUri().isEmpty())
        result.next.uri = m_controller.nextUri().toStdString();
    if (!m_controller.nextMetadata().isEmpty())
        result.next.metadata = m_controller.nextMetadata().toStdString();
    if (m_controller.state() == PlaybackController::State::NoMedia
        && result.current.uri.empty())
        result.current = {};
    result.playMode = playMode(m_controller.playMode());
    result.hasNext = m_controller.hasNext();
    result.hasPrevious = m_controller.hasPrevious();
    result.peerConnected = m_controller.peerConnected();
    const QVariantMap castSession = m_controller.castSession();
    result.castSession.connected =
        castSession.value(QStringLiteral("connected")).toBool();
    result.castSession.protocol =
        castSession.value(QStringLiteral("protocol"))
            .toString()
            .toStdString();
    result.castSession.controlPointName =
        castSession.value(QStringLiteral("controlPointName"))
            .toString()
            .toStdString();
    result.castSession.controlPointAddress =
        castSession.value(QStringLiteral("controlPointAddress"))
            .toString()
            .toStdString();
    result.castSession.remoteControlRegistered =
        castSession
            .value(QStringLiteral("remoteControlRegistered"))
            .toBool();
    result.playerAvailable = !m_playerLost;
    result.positionSeconds = m_controller.positionSeconds();
    result.durationSeconds = m_controller.durationSeconds();
    result.volumePercent = m_controller.volumePercent();
    result.muted = m_controller.isMuted();
    const auto controls = m_controller.pictureControls();
    result.pictureControls.reserve(
        static_cast<std::size_t>(controls.size()));
    for (const PictureControlInfo &control : controls) {
        result.pictureControls.push_back(PictureControl{
            control.name.toStdString(), control.min, control.max,
            control.neutral,
            m_controller.pictureControlValue(control.name),
        });
    }
    result.generation = m_generation;
    result.stateVersion = m_stateVersion;
    return result;
}

PlaybackOutcomeStatus PlaybackBridge::execute(
    const PlaybackCommand &command, std::string &error)
{
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
                QString::fromStdString(command.text), command.intValue)) {
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
        m_controller.setCastSession(
            command.castSession.connected,
            QString::fromStdString(command.castSession.protocol),
            QString::fromStdString(
                command.castSession.controlPointName),
            QString::fromStdString(
                command.castSession.controlPointAddress),
            command.castSession.remoteControlRegistered);
        break;
    }
    return PlaybackOutcomeStatus::Applied;
}

bool PlaybackBridge::sendMessage(
    ipc::Message message, const char *what, bool coalescible,
    bool urgent)
{
    (void)what;
    if (!m_connected || !m_connection)
        return false;
    bool queueOverflow = false;
    {
        std::scoped_lock lock(m_outboundMutex);
        if (!m_acceptOutbound)
            return false;
        if (coalescible) {
            m_latestState = std::move(message);
        } else if (urgent) {
            if (m_urgentOutbound.size() >= kMaxUrgentOutbound)
                queueOverflow = true;
            else
                m_urgentOutbound.push_back(std::move(message));
        } else {
            if (m_orderedOutbound.size() >= kMaxOrderedOutbound) {
                queueOverflow = true;
            } else {
                m_orderedOutbound.push_back(std::move(message));
            }
        }
    }
    if (queueOverflow) {
        emit logMessage(
            QStringLiteral("Playback 发送队列已满：%1")
                .arg(QString::fromUtf8(what)));
        return false;
    }
    m_outboundReady.notify_one();
    return true;
}

} // namespace mcast::ui_process
