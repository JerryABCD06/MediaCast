// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/runtime/DlnaProtocolRuntime.h"

#include "protocols/dlna/net/SsdpSocket.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace mcast::dlna::runtime {
namespace {

bool sameMedia(
    const playback::MediaRequest &left,
    const playback::MediaRequest &right)
{
    // Raw DIDL intentionally stays in Protocol and is never sent through the
    // Ui playback projection. Readiness is about one concrete network item;
    // presentation fields may legitimately gain MPV/URI fallbacks after that.
    return left.uri == right.uri;
}

std::string controlPointName(std::string_view userAgent)
{
    const std::string value(userAgent);
    if (value.empty())
        return {};

    const auto productVersion = [&](std::string_view product) {
        const std::size_t start = value.find(product);
        if (start == std::string::npos)
            return std::string{};
        const std::size_t versionStart = start + product.size();
        const std::size_t versionEnd =
            value.find_first_of(" ;)(", versionStart);
        const std::string version = value.substr(
            versionStart, versionEnd == std::string::npos
                ? std::string::npos : versionEnd - versionStart);
        return std::string(product) + " "
            + (version.empty() ? std::string{} : version);
    };

    if (!productVersion("BubbleUPnP/").empty())
        return productVersion("BubbleUPnP/");
    if (!productVersion("Cling/").empty())
        return productVersion("Cling/");
    if (!productVersion("Dart/").empty())
        return productVersion("Dart/");
    return value.size() > 120 ? value.substr(0, 120) : value;
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
    , m_gena(
          GenaManagerCallbacks{
              [this](
                  std::size_t count,
                  SubscriptionChangeReason reason) {
                  m_session.setSubscriptionCount(
                      count, reason, sessionNow());
              },
              [] {},
              [this](const std::string &message) {
                  log("GENA: " + message);
              },
          },
          GenaManagerOptions{})
    , m_router(m_session, soap::SoapRouterOptions{
          [] { return sessionNow(); }, m_options.log, true,
          SessionTime{5000}})
    , m_handler(
          m_router, m_claimRuntime, [] { return castNow(); },
          httpCallbacks())
    , m_frontend(
          m_handler,
          DlnaHttpFrontendOptions{
              m_options.friendlyName, m_options.udn, &m_gena})
{
    m_broadcasting = m_options.startBroadcasting;
    m_broadcastIntervalMs =
        m_options.broadcastInterval.count();
    m_gena.updateState(m_playback.snapshot());
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
    m_networkRecoveryPending = false;
    m_mediaStopping = false;
    m_mediaThread = std::jthread(
        [this](std::stop_token stop) { mediaLoop(stop); });
    m_receivingEnabled = m_options.startReceivingEnabled;
    m_surfaceKeptForActiveSession = false;
    if (m_receivingEnabled) {
        if (!ensureConfirmationWorker(error)
            || !startNetworkSurface(error)
            || !m_session.start()) {
            if (error.empty())
                error = "DLNA session failed to start";
            stop();
            return false;
        }
    } else if (!m_session.start(false)) {
        error = "DLNA session failed to start paused";
        stop();
        return false;
    }
    m_sessionThread = std::jthread(
        [this](std::stop_token stop) { sessionLoop(stop); });
    notifyReceivingState();
    return true;
}

void DlnaProtocolRuntime::stop() noexcept
{
    if (!m_running.exchange(false))
        return;
    m_receivingEnabled = false;
    m_surfaceKeptForActiveSession = false;
    {
        std::lock_guard receivingLock(m_receivingMutex);
        m_disconnectRestartAt.reset();
        m_disconnectOverallDeadline.reset();
        m_disconnectDrainDeadline.reset();
        m_disconnectDrainPending = false;
    }
    {
        std::lock_guard requestLock(m_requestGateMutex);
    }
    stopConfirmationWorker();
    {
        std::lock_guard lock(m_mediaMutex);
        m_mediaStopping = true;
        m_mediaTasks.clear();
        m_pendingMedia.reset();
        m_earlyCommandCompletions.clear();
        ++m_mediaGeneration;
    }
    m_mediaReady.notify_all();
    if (m_mediaThread.joinable())
        m_mediaThread.request_stop();
    if (m_sessionThread.joinable())
        m_sessionThread.request_stop();
    if (m_sessionThread.joinable())
        m_sessionThread.join();
    if (m_mediaThread.joinable())
        m_mediaThread.join();
    stopNetworkSurface();
    static_cast<void>(m_session.endSession(sessionNow()));
    m_session.resetForRestart();
}

std::uint16_t DlnaProtocolRuntime::localPort() const noexcept
{
    return m_http.localPort();
}

bool DlnaProtocolRuntime::setReceivingEnabled(
    bool enabled, std::string &error)
{
    error.clear();
    std::lock_guard lock(m_receivingMutex);
    if (!m_running.load()) {
        error = "DLNA runtime is not running";
        return false;
    }
    bool recovered = false;
    if (m_networkRecoveryPending.exchange(false)) {
        recovered = m_receivingEnabled.exchange(false);
        if (recovered)
            ++m_receivingStateVersion;
        log("DLNA network failure detected before receiving command");
        disableReceivingLocked();
    }
    if (m_receivingEnabled.exchange(enabled) == enabled) {
        if (recovered)
            notifyReceivingState();
        return true;
    }
    ++m_receivingStateVersion;
    if (!enabled) {
        if (m_session.snapshot().peerConnected) {
            m_surfaceKeptForActiveSession = true;
            {
                std::lock_guard confirmationLock(
                    m_confirmationMutex);
                if (m_confirmationWorker)
                    m_confirmationWorker->controlLost(castNow());
            }
            discardPendingMedia();
            stopSsdpSurface();
        } else {
            disableReceivingLocked();
        }
        notifyReceivingState();
        return true;
    }
    std::string workerError;
    if (!ensureConfirmationWorker(workerError)) {
        error = "DLNA confirmation worker failed to start: "
            + workerError;
        m_receivingEnabled = false;
        ++m_receivingStateVersion;
        notifyReceivingState();
        return false;
    }
    if (m_surfaceKeptForActiveSession.exchange(false)) {
        std::string surfaceError;
        if (!startNetworkSurface(surfaceError)) {
            m_receivingEnabled = false;
            m_surfaceKeptForActiveSession = true;
            ++m_receivingStateVersion;
            error = "DLNA network surface failed to resume: "
                + surfaceError;
            notifyReceivingState();
            return false;
        }
        notifyReceivingState();
        return true;
    }
    if (!m_session.resumeAccepting()) {
        error = "DLNA session failed to resume";
        m_receivingEnabled = false;
        ++m_receivingStateVersion;
        stopConfirmationWorker();
        notifyReceivingState();
        return false;
    }
    notifyReceivingState();
    return true;
}

bool DlnaProtocolRuntime::requestDisconnect(std::string &error)
{
    error.clear();
    std::lock_guard lock(m_receivingMutex);
    if (!m_running.load()) {
        error = "DLNA runtime is not running";
        return false;
    }
    const auto now = sessionNow();
    const auto submission = m_session.endSession(now);
    if (!submission.queued) {
        const auto state = m_session.snapshot();
        const bool alreadyEnded =
            state.disconnectedByUs && !state.ending
            && state.playback.state == PlaybackState::NoMedia;
        if (!alreadyEnded) {
            error = submission.error.empty()
                ? "DLNA could not end the session"
                : submission.error;
            return false;
        }
    }
    m_disconnectRestartAt.reset();
    m_disconnectOverallDeadline =
        now + SessionTime{8000};
    m_disconnectDrainDeadline.reset();
    m_disconnectDrainPending = true;
    m_surfaceKeptForActiveSession = true;
    stopConfirmationWorker();
    discardPendingMedia();
    log(
        "DLNA disconnect requested; draining EndSession and GENA before "
        "network teardown; EndSession command="
        + std::to_string(submission.commandId));
    return true;
}

void DlnaProtocolRuntime::setBroadcasting(bool enabled) noexcept
{
    m_broadcasting = enabled;
    m_ssdp.setBroadcasting(enabled);
}

void DlnaProtocolRuntime::setBroadcastInterval(
    std::chrono::milliseconds interval) noexcept
{
    m_broadcastIntervalMs = interval.count();
    m_ssdp.setAliveInterval(interval);
}

DlnaSessionCallbacks DlnaProtocolRuntime::sessionCallbacks()
{
    DlnaSessionCallbacks callbacks;
    callbacks.requestUiActivation = [this] {
        if (!m_receivingEnabled || !m_options.requestUiActivation)
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
    callbacks.startNetwork = [this] {
        std::string startError;
        const bool started = startNetworkSurface(startError);
        if (!started)
            log("DLNA network start failed: " + startError);
        return started;
    };
    callbacks.stopNetwork = [this] {
        stopNetworkSurface();
    };
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
            std::lock_guard lock(m_completionMutex);
            if ((!m_receivingEnabled
                 && !m_surfaceKeptForActiveSession)
                || !m_running) {
                return false;
            }
            return m_http.completeRequest(
                requestId, std::move(response));
        };
    callbacks.requestConfirmation =
        [this](const cast::ConfirmationPrompt &prompt) {
            std::lock_guard lock(m_confirmationMutex);
            if (m_receivingEnabled && m_confirmationWorker)
                m_confirmationWorker->requestConfirmation(prompt);
        };
    callbacks.acceptNewMedia = [this] {
        return m_receivingEnabled.load()
            && !m_disconnectDrainPending.load();
    };
    callbacks.executeMedia =
        [this](const claim::DlnaMediaExecution &execution) {
            if (!enqueueNewMedia(execution)) {
                m_handler.mediaReady(
                    execution.operationId, false, castNow());
            }
        };
    callbacks.playNow = [this](
        std::optional<std::uint64_t> ownerEpoch) {
        PlaybackCommand command;
        command.type = PlaybackCommandType::Play;
        const auto result = m_session.submitAndWait(
            std::move(command), SessionTime{5000}, ownerEpoch);
        if (result.timedOut || !result.outcome
            || *result.outcome != PlaybackOutcomeStatus::Applied) {
            throw std::runtime_error(
                result.completionError.empty()
                    ? "DLNA Play was not applied"
                    : result.completionError);
        }
    };
    callbacks.stopNow = [this](
        const cast::PeerIdentity &peer,
        std::optional<std::uint64_t> ownerEpoch) {
        PlaybackCommand command;
        command.type = PlaybackCommandType::Stop;
        const auto result = m_session.submitAndWait(
            std::move(command), SessionTime{5000}, ownerEpoch);
        if (result.timedOut || !result.outcome
            || *result.outcome != PlaybackOutcomeStatus::Applied) {
            throw std::runtime_error(
                result.completionError.empty()
                    ? "DLNA Stop was not applied"
                    : result.completionError);
        }
        m_session.transportStopped(
            sessionNow(), peer.address);
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
        if (m_mediaStopping || !m_running
            || (!m_receivingEnabled
                && !m_surfaceKeptForActiveSession)) {
            return;
        }
        m_mediaTasks.push_back(std::move(task));
    }
    m_mediaReady.notify_one();
}

bool DlnaProtocolRuntime::enqueueNewMedia(
    claim::DlnaMediaExecution execution)
{
    std::scoped_lock startLock(m_mediaStartMutex);
    std::vector<std::uint64_t> cancelledOperations;
    std::uint64_t generation = 0;
    {
        std::lock_guard lock(m_mediaMutex);
        if (m_mediaStopping || !m_running
            || (!m_receivingEnabled
                && !m_surfaceKeptForActiveSession)) {
            return false;
        }
        if (m_pendingMedia)
            cancelledOperations.push_back(
                m_pendingMedia->execution.operationId);
        for (const MediaTask &task : m_mediaTasks) {
            if (task.kind == MediaTask::Kind::ExecuteMedia)
                cancelledOperations.push_back(task.media.operationId);
        }
        m_mediaTasks.clear();
        m_pendingMedia.reset();
        m_earlyCommandCompletions.clear();
        generation = ++m_mediaGeneration;
    }

    // Old owner requests must receive a definite failure before the new owner
    // starts. Their late playback completions are ignored by the Session epoch.
    for (const std::uint64_t operationId : cancelledOperations)
        m_handler.mediaReady(operationId, false, castNow());

    m_session.newMediaRequested(
        sessionNow(), execution.peer.displayName,
        execution.peer.address);

    {
        std::lock_guard lock(m_mediaMutex);
        if (m_mediaStopping || !m_running
            || (!m_receivingEnabled
                && !m_surfaceKeptForActiveSession)) {
            return false;
        }
        MediaTask task;
        task.kind = MediaTask::Kind::ExecuteMedia;
        task.media = std::move(execution);
        task.generation = generation;
        m_mediaTasks.push_back(std::move(task));
    }
    m_mediaReady.notify_one();
    return true;
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
        if (!m_receivingEnabled
            && (!m_surfaceKeptForActiveSession
                || task.kind == MediaTask::Kind::ExecuteMedia)) {
            continue;
        }

        std::unique_lock startLock(m_mediaStartMutex);
        {
            std::lock_guard lock(m_mediaMutex);
            if (!m_receivingEnabled)
                continue;
            if (task.generation != m_mediaGeneration) {
                startLock.unlock();
                m_handler.mediaReady(
                    task.media.operationId, false, castNow());
                continue;
            }
            PendingMedia pending;
            pending.execution = task.media;
            pending.baselineStateVersion =
                m_playback.snapshot().stateVersion;
            pending.generation = task.generation;
            m_pendingMedia = std::move(pending);
        }
        const auto submission = m_session.openMedia(
            task.media.media, sessionNow(), task.media.newSession);
        startLock.unlock();
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
            if (!stop.stop_requested()) {
                log("DLNA HTTP poll failed: " + error);
                m_networkRecoveryPending = true;
            }
            m_httpPolling = false;
            return;
        }
    }
    m_httpPolling = false;
}

bool DlnaProtocolRuntime::startNetworkSurface(std::string &error)
{
    std::lock_guard lock(m_networkMutex);
    if (!m_running.load()) {
        error = "DLNA runtime is not running";
        return false;
    }
    if (!m_gena.start(error)) {
        error = "DLNA GENA start failed: " + error;
        return false;
    }
    if (!startHttpLocked(error)) {
        m_gena.stop();
        return false;
    }
    if (m_options.bindLoopback || !m_options.enableSsdp)
        return true;

    net::SsdpServiceOptions ssdpOptions;
    ssdpOptions.udn = m_options.udn;
    ssdpOptions.httpPort = m_http.localPort();
    ssdpOptions.ssdpPort = m_options.ssdpPort;
    const bool productionDiscovery = m_options.ssdpPort != 0;
    ssdpOptions.joinMulticast = productionDiscovery;
    ssdpOptions.advertise =
        productionDiscovery && m_broadcasting.load();
    ssdpOptions.aliveInterval = std::chrono::milliseconds(
        m_broadcastIntervalMs.load());
    if (!m_ssdp.start(
            std::move(ssdpOptions),
            [this](const std::string &message) { log(message); },
            error)) {
        error = "DLNA SSDP start failed: " + error;
        stopHttpLocked();
        m_gena.stop();
        return false;
    }
    return true;
}

void DlnaProtocolRuntime::stopNetworkSurface() noexcept
{
    std::lock_guard lock(m_networkMutex);
    // An SSDP byebye is a notification, so it must leave before the HTTP and
    // GENA endpoints disappear. Doing this after stopHttp() made control
    // points race against an already-dead LOCATION.
    m_ssdp.announceByeBye();
    m_ssdp.stop();
    m_gena.stop();
    stopHttpLocked();
}

void DlnaProtocolRuntime::stopSsdpSurface() noexcept
{
    std::lock_guard lock(m_networkMutex);
    m_ssdp.stop();
}

bool DlnaProtocolRuntime::ensureConfirmationWorker(std::string &error)
{
    std::lock_guard lock(m_confirmationMutex);
    if (m_confirmationWorker) {
        error.clear();
        return true;
    }
    try {
        m_confirmationWorker =
            std::make_unique<DlnaClaimConfirmationWorker>(
                m_handler, m_confirmationPort,
                [] { return castNow(); });
    } catch (const std::exception &exception) {
        error = exception.what();
        return false;
    } catch (...) {
        error = "unknown confirmation worker error";
        return false;
    }
    error.clear();
    return true;
}

void DlnaProtocolRuntime::stopConfirmationWorker() noexcept
{
    std::unique_ptr<DlnaClaimConfirmationWorker> worker;
    {
        std::lock_guard lock(m_confirmationMutex);
        worker = std::move(m_confirmationWorker);
    }
    if (!worker)
        return;
    worker->controlLost(castNow());
    worker->stop();
}

void DlnaProtocolRuntime::discardPendingMedia() noexcept
{
    std::scoped_lock startLock(m_mediaStartMutex);
    {
        std::lock_guard lock(m_mediaMutex);
        m_mediaTasks.clear();
        m_pendingMedia.reset();
        m_earlyCommandCompletions.clear();
        ++m_mediaGeneration;
    }
    m_mediaReady.notify_all();
}

void DlnaProtocolRuntime::disableReceivingLocked() noexcept
{
    m_disconnectRestartAt.reset();
    m_surfaceKeptForActiveSession = false;
    {
        // Requests that already passed the receiving gate must finish before
        // Claim cancellation. Every later request sees receiving=false.
        std::lock_guard requestLock(m_requestGateMutex);
    }
    stopConfirmationWorker();
    discardPendingMedia();
    m_session.pauseAccepting(sessionNow());
    {
        // Wait for any completion producer that observed the old state.
        std::lock_guard completionLock(m_completionMutex);
    }
    stopNetworkSurface();
}

void DlnaProtocolRuntime::handleNetworkFailure() noexcept
{
    std::lock_guard lock(m_receivingMutex);
    if (!m_running.load()
        || !m_receivingEnabled.exchange(false)) {
        return;
    }
    ++m_receivingStateVersion;
    log("DLNA network failed; receiving has been disabled");
    disableReceivingLocked();
    notifyReceivingState();
}

bool DlnaProtocolRuntime::startHttpLocked(std::string &error)
{
    if (m_httpThread.joinable()) {
        if (m_httpPolling.load())
            return true;
        m_httpThread.join();
        m_http.stop();
    }
    net::HttpServerOptions options;
    options.logger = [this](const std::string &message) {
        log(message);
    };
    const bool bound = m_options.bindLoopback
        ? m_http.listenLoopback(
              m_options.httpPort,
              [this](const core::HttpRequest &request) {
                  m_session.observeHttpClient(
                      controlPointName(
                          request.header("user-agent").value_or("")),
                      request.peerAddress);
                  std::lock_guard requestLock(m_requestGateMutex);
                  if (!m_receivingEnabled
                      && !m_surfaceKeptForActiveSession) {
                      net::HttpResponse response;
                      response.status = 503;
                      response.contentType.clear();
                      return net::HttpDispatchResult{
                          net::HttpDispatchStatus::Completed,
                          std::move(response),
                      };
                  }
                  return m_frontend.handle(request);
              },
              options, error)
        : m_http.listenAny(
              m_options.httpPort,
              [this](const core::HttpRequest &request) {
                  m_session.observeHttpClient(
                      controlPointName(
                          request.header("user-agent").value_or("")),
                      request.peerAddress);
                  std::lock_guard requestLock(m_requestGateMutex);
                  if (!m_receivingEnabled
                      && !m_surfaceKeptForActiveSession) {
                      net::HttpResponse response;
                      response.status = 503;
                      response.contentType.clear();
                      return net::HttpDispatchResult{
                          net::HttpDispatchStatus::Completed,
                          std::move(response),
                      };
                  }
                  return m_frontend.handle(request);
              },
              options, error);
    if (!bound)
        return false;
    m_httpPolling = true;
    m_httpThread = std::jthread(
        [this](std::stop_token stop) { httpLoop(stop); });
    return true;
}

void DlnaProtocolRuntime::stopHttpLocked() noexcept
{
    {
        // Producers check m_receivingEnabled under this lock. Once a pause or
        // stop has cleared it, no completion can enter the next HTTP server.
        std::lock_guard completionLock(m_completionMutex);
    }
    if (m_httpThread.joinable())
        m_httpThread.request_stop();
    m_http.wake();
    if (m_httpThread.joinable())
        m_httpThread.join();
    m_http.stop();
    m_httpPolling = false;
}

void DlnaProtocolRuntime::sessionLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        if (m_networkRecoveryPending.exchange(false))
            handleNetworkFailure();
        m_session.tick(sessionNow());
        {
            std::lock_guard receivingLock(m_receivingMutex);
            if (m_disconnectDrainPending.load()) {
                const auto now = sessionNow();
                const bool forced =
                    m_disconnectOverallDeadline
                    && now >= *m_disconnectOverallDeadline;
                if (forced) {
                    m_session.abandonEndingSession();
                    log(
                        "DLNA disconnect deadline reached; forcing "
                        "network teardown");
                }
                const auto snapshot = m_session.snapshot();
                const bool finalState =
                    !snapshot.ending
                    && snapshot.playback.state
                        == PlaybackState::NoMedia;
                if (finalState) {
                    if (!m_disconnectDrainDeadline) {
                        m_disconnectDrainDeadline =
                            now + SessionTime{1000};
                    }
                }
                if (forced
                    || (finalState
                        && (m_gena.quiescent()
                            || now >= *m_disconnectDrainDeadline))) {
                    m_disconnectDrainPending = false;
                    m_surfaceKeptForActiveSession = false;
                    m_disconnectOverallDeadline.reset();
                    m_disconnectDrainDeadline.reset();
                    stopNetworkSurface();
                    m_disconnectRestartAt =
                        sessionNow() + SessionTime{1500};
                    log(
                        "DLNA final GENA drain complete; network "
                        "surface stopped after EndSession");
                }
            }
        }
        if (!m_receivingEnabled
            && m_surfaceKeptForActiveSession
            && !m_session.snapshot().peerConnected) {
            std::lock_guard lock(m_receivingMutex);
            if (!m_receivingEnabled
                && m_surfaceKeptForActiveSession
                && !m_session.snapshot().peerConnected) {
                disableReceivingLocked();
            }
        }
        {
            std::unique_lock receivingLock(
                m_receivingMutex, std::try_to_lock);
            if (receivingLock.owns_lock()
                && m_disconnectRestartAt
                && sessionNow() >= *m_disconnectRestartAt
                && !m_session.snapshot().ending) {
                if (m_disconnectRestartAt
                    && m_running.load()
                    && m_receivingEnabled
                    && !m_session.snapshot().ending) {
                    std::string restartError;
                    if (ensureConfirmationWorker(restartError)
                        && startNetworkSurface(restartError)) {
                        m_disconnectRestartAt.reset();
                        log("DLNA network surface restored after disconnect");
                    } else {
                        m_disconnectRestartAt =
                            sessionNow() + SessionTime{1000};
                        log(
                            "DLNA network restart after disconnect failed: "
                            + restartError);
                    }
                }
            }
        }
        for (int slice = 0; slice < 10 && !stop.stop_requested(); ++slice)
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
}

void DlnaProtocolRuntime::handlePlaybackEvent(
    const PlaybackEvent &event)
{
    m_gena.updateState(m_session.snapshot().playback);
    if (!m_receivingEnabled
        && !m_surfaceKeptForActiveSession) {
        std::lock_guard lock(m_mediaMutex);
        m_pendingMedia.reset();
        m_earlyCommandCompletions.clear();
        return;
    }
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

void DlnaProtocolRuntime::notifyReceivingState() const noexcept
{
    if (!m_options.receivingStateChanged)
        return;
    try {
        m_options.receivingStateChanged(
            m_receivingEnabled.load(),
            m_receivingStateVersion.load());
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
