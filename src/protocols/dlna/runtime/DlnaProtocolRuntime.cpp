// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/runtime/DlnaProtocolRuntime.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace mcast::dlna::runtime {
namespace {

bool sameMedia(
    const playback::MediaRequest &left,
    const playback::MediaRequest &right)
{
    return left.uri == right.uri && left.metadata == right.metadata;
}

} // namespace

DlnaProtocolRuntime::DlnaProtocolRuntime(
    playback::PlaybackGateway &playback,
    DlnaClaimConfirmationPort &confirmationPort,
    DlnaProtocolRuntimeOptions options)
    : m_options(std::move(options))
    , m_playback(playback)
    , m_confirmationPort(confirmationPort)
    , m_coordinator(cast::ArbitrationPolicy{
          true, true, cast::CastTime{10000},
          cast::CastTime{5000}, 8})
    , m_adapter(m_coordinator)
    , m_claimRuntime(m_adapter)
    , m_session(m_playback, sessionCallbacks())
    , m_router(m_session, soap::SoapRouterOptions{
          [] { return sessionNow(); }, m_options.log})
    , m_handler(
          m_router, m_claimRuntime, [] { return castNow(); },
          httpCallbacks())
    , m_frontend(
          m_handler,
          DlnaHttpFrontendOptions{
              m_options.friendlyName, m_options.udn})
{
    m_playbackListener = m_playback.addEventListener(
        [this](const PlaybackEvent &event) {
            handlePlaybackEvent(event);
        });
}

DlnaProtocolRuntime::~DlnaProtocolRuntime()
{
    stop();
    if (m_playbackListener != 0)
        m_playback.removeEventListener(m_playbackListener);
}

bool DlnaProtocolRuntime::start(std::string &error)
{
    error.clear();
    if (m_running.exchange(true))
        return true;
    m_confirmationWorker =
        std::make_unique<DlnaClaimConfirmationWorker>(
            m_handler, m_confirmationPort,
            [] { return castNow(); });
    m_mediaStopping = false;
    m_mediaThread = std::jthread(
        [this](std::stop_token stop) { mediaLoop(stop); });

    net::HttpServerOptions options;
    options.logger = [this](const std::string &message) {
        log(message);
    };
    const bool bound = m_options.bindLoopback
        ? m_http.listenLoopback(
              m_options.httpPort,
              [this](const core::HttpRequest &request) {
                  return m_frontend.handle(request);
              },
              options, error)
        : m_http.listenAny(
              m_options.httpPort,
              [this](const core::HttpRequest &request) {
                  return m_frontend.handle(request);
              },
              options, error);
    if (!bound) {
        stop();
        return false;
    }
    m_httpThread = std::jthread(
        [this](std::stop_token stop) { httpLoop(stop); });
    if (!m_session.start()) {
        error = "DLNA session failed to start";
        stop();
        return false;
    }
    m_sessionThread = std::jthread(
        [this](std::stop_token stop) { sessionLoop(stop); });
    return true;
}

void DlnaProtocolRuntime::stop() noexcept
{
    if (!m_running.exchange(false))
        return;
    {
        std::lock_guard lock(m_mediaMutex);
        m_mediaStopping = true;
        m_mediaTasks.clear();
        m_pendingMedia.reset();
        m_earlyCommandCompletions.clear();
    }
    m_mediaReady.notify_all();
    if (m_mediaThread.joinable())
        m_mediaThread.request_stop();
    if (m_sessionThread.joinable())
        m_sessionThread.request_stop();
    if (m_sessionThread.joinable())
        m_sessionThread.join();
    if (m_confirmationWorker) {
        m_confirmationWorker->controlLost(castNow());
        m_confirmationWorker->stop();
    }
    if (m_httpThread.joinable())
        m_httpThread.request_stop();
    m_http.wake();
    if (m_httpThread.joinable())
        m_httpThread.join();
    m_http.stop();

    if (m_mediaThread.joinable())
        m_mediaThread.join();
    static_cast<void>(m_session.endSession(sessionNow()));
}

std::uint16_t DlnaProtocolRuntime::localPort() const noexcept
{
    return m_http.localPort();
}

DlnaSessionCallbacks DlnaProtocolRuntime::sessionCallbacks()
{
    DlnaSessionCallbacks callbacks;
    callbacks.requestUiActivation = [this] {
        if (!m_options.requestUiActivation)
            return;
        try {
            static_cast<void>(m_options.requestUiActivation());
        } catch (const std::exception &exception) {
            log(
                std::string("Ui activation failed: ")
                + exception.what());
        } catch (...) {
            log("Ui activation failed");
        }
    };
    callbacks.startNetwork = [] { return true; };
    callbacks.requestFailed = [this](const std::string &message) {
        log("DLNA request failed: " + message);
    };
    callbacks.log = [this](const std::string &message) {
        log(message);
    };
    return callbacks;
}

claim::DlnaClaimHttpCallbacks DlnaProtocolRuntime::httpCallbacks()
{
    claim::DlnaClaimHttpCallbacks callbacks;
    callbacks.completeHttp =
        [this](std::uint64_t requestId, net::HttpResponse response) {
            return m_http.completeRequest(
                requestId, std::move(response));
        };
    callbacks.requestConfirmation =
        [this](const cast::ConfirmationPrompt &prompt) {
            if (m_confirmationWorker)
                m_confirmationWorker->requestConfirmation(prompt);
        };
    callbacks.executeMedia =
        [this](const claim::DlnaMediaExecution &execution) {
            MediaTask task;
            task.kind = MediaTask::Kind::ExecuteMedia;
            task.media = execution;
            enqueue(std::move(task));
        };
    callbacks.playNow = [this] {
        MediaTask task;
        task.kind = MediaTask::Kind::PlayNow;
        enqueue(std::move(task));
    };
    callbacks.stopNow = [this] {
        MediaTask task;
        task.kind = MediaTask::Kind::StopNow;
        enqueue(std::move(task));
    };
    callbacks.log = [this](const std::string &message) {
        log(message);
    };
    return callbacks;
}

void DlnaProtocolRuntime::enqueue(MediaTask task)
{
    {
        std::lock_guard lock(m_mediaMutex);
        if (m_mediaStopping || !m_running)
            return;
        m_mediaTasks.push_back(std::move(task));
    }
    m_mediaReady.notify_one();
}

void DlnaProtocolRuntime::mediaLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        MediaTask task;
        {
            std::unique_lock lock(m_mediaMutex);
            m_mediaReady.wait(lock, [&] {
                return stop.stop_requested() || m_mediaStopping
                    || !m_mediaTasks.empty();
            });
            if (stop.stop_requested() || m_mediaStopping)
                return;
            task = std::move(m_mediaTasks.front());
            m_mediaTasks.pop_front();
        }

        if (task.kind == MediaTask::Kind::PlayNow
            || task.kind == MediaTask::Kind::StopNow) {
            PlaybackCommand command;
            command.type = task.kind == MediaTask::Kind::PlayNow
                ? PlaybackCommandType::Play
                : PlaybackCommandType::Stop;
            const auto submission = m_session.submit(std::move(command));
            if (!submission.queued) {
                log(
                    "DLNA playback command was not queued: "
                    + submission.error);
            }
            continue;
        }

        {
            std::lock_guard lock(m_mediaMutex);
            PendingMedia pending;
            pending.execution = task.media;
            pending.baselineStateVersion =
                m_playback.snapshot().stateVersion;
            m_pendingMedia = std::move(pending);
        }
        const auto submission = m_session.openMedia(
            task.media.media, sessionNow());
        std::optional<std::uint64_t> failedOperation;
        bool readyFromSnapshot = false;
        {
            std::lock_guard lock(m_mediaMutex);
            if (!m_pendingMedia
                || m_pendingMedia->execution.operationId
                    != task.media.operationId) {
                continue;
            }
            if (submission.queued) {
                m_pendingMedia->commandId = submission.commandId;
                m_pendingMedia->waitingForPlayer =
                    submission.commandId == 0;
                if (m_pendingMedia->commandId != 0) {
                    const auto early = m_earlyCommandCompletions.find(
                        m_pendingMedia->commandId);
                    if (early != m_earlyCommandCompletions.end()) {
                        if (early->second.outcome
                            == PlaybackOutcomeStatus::Applied) {
                            m_pendingMedia->waitingForSnapshot = true;
                        } else {
                            failedOperation =
                                m_pendingMedia->execution.operationId;
                            m_pendingMedia.reset();
                        }
                        m_earlyCommandCompletions.erase(early);
                    }
                }
                const auto snapshot = m_playback.snapshot();
                readyFromSnapshot = m_pendingMedia
                    && (m_pendingMedia->waitingForPlayer
                        || m_pendingMedia->waitingForSnapshot)
                    && snapshot.playerAvailable
                    && snapshot.loadStatus == playback::LoadStatus::Ok
                    && (snapshot.state == PlaybackState::Playing
                        || snapshot.state == PlaybackState::Paused)
                    && sameMedia(
                        snapshot.current, task.media.media)
                    && snapshot.stateVersion
                        > m_pendingMedia->baselineStateVersion;
                if (readyFromSnapshot)
                    m_pendingMedia.reset();
            } else {
                failedOperation = task.media.operationId;
                m_pendingMedia.reset();
            }
        }
        if (failedOperation) {
            m_handler.mediaReady(
                *failedOperation, false, castNow());
        } else if (readyFromSnapshot) {
            m_handler.mediaReady(
                task.media.operationId, true, castNow());
        }
    }
}

void DlnaProtocolRuntime::httpLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        std::string error;
        if (!m_http.pollOnce(
                std::chrono::milliseconds{100}, error)) {
            if (!stop.stop_requested())
                log("DLNA HTTP poll failed: " + error);
            return;
        }
    }
}

void DlnaProtocolRuntime::sessionLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        m_session.tick(sessionNow());
        for (int slice = 0; slice < 10 && !stop.stop_requested(); ++slice)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

void DlnaProtocolRuntime::handlePlaybackEvent(
    const PlaybackEvent &event)
{
    std::optional<std::uint64_t> completedOperation;
    bool success = false;
    {
        std::lock_guard lock(m_mediaMutex);
        if (!m_pendingMedia)
            return;
        if (event.kind == PlaybackEventKind::CommandCompleted) {
            constexpr std::size_t kMaxEarlyCompletions = 64;
            if (m_earlyCommandCompletions.size() >= kMaxEarlyCompletions)
                m_earlyCommandCompletions.erase(
                    m_earlyCommandCompletions.begin());
            m_earlyCommandCompletions[event.commandId] = event;
        }
        if (event.kind == PlaybackEventKind::CommandCompleted
            && m_pendingMedia->commandId != 0
            && event.commandId == m_pendingMedia->commandId) {
            m_earlyCommandCompletions.erase(event.commandId);
            m_pendingMedia->commandId = 0;
            if (event.outcome == PlaybackOutcomeStatus::Applied) {
                m_pendingMedia->waitingForSnapshot = true;
            } else {
                completedOperation =
                    m_pendingMedia->execution.operationId;
                m_pendingMedia.reset();
            }
        }

        if (m_pendingMedia
            && (event.kind == PlaybackEventKind::SnapshotChanged
                || event.kind == PlaybackEventKind::Reconnected)
            && (m_pendingMedia->waitingForPlayer
                || m_pendingMedia->waitingForSnapshot)
            && event.snapshot.playerAvailable
            && event.snapshot.stateVersion
                > m_pendingMedia->baselineStateVersion
            && sameMedia(
                event.snapshot.current,
                m_pendingMedia->execution.media)) {
            if (event.snapshot.loadStatus
                == playback::LoadStatus::Failed) {
                completedOperation =
                    m_pendingMedia->execution.operationId;
                success = false;
                m_pendingMedia.reset();
            } else if (
                event.snapshot.state == PlaybackState::Playing
                || event.snapshot.state == PlaybackState::Paused) {
                completedOperation =
                    m_pendingMedia->execution.operationId;
                success = true;
                m_pendingMedia.reset();
            }
        }
    }
    if (completedOperation) {
        m_handler.mediaReady(
            *completedOperation, success, castNow());
    }
}

void DlnaProtocolRuntime::log(const std::string &message) const
{
    if (!m_options.log)
        return;
    try {
        m_options.log(message);
    } catch (...) {
    }
}

cast::CastTime DlnaProtocolRuntime::castNow()
{
    return std::chrono::duration_cast<cast::CastTime>(
        std::chrono::steady_clock::now().time_since_epoch());
}

SessionTime DlnaProtocolRuntime::sessionNow()
{
    return std::chrono::duration_cast<SessionTime>(
        std::chrono::steady_clock::now().time_since_epoch());
}

} // namespace mcast::dlna::runtime
