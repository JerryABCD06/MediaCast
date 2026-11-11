// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "cast/CastClaimCoordinator.h"
#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"
#include "protocols/dlna/runtime/DlnaClaimConfirmationWorker.h"
#include "protocols/dlna/runtime/DlnaHttpFrontend.h"
#include "protocols/dlna/session/DlnaSession.h"
#include "protocols/dlna/net/SsdpService.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace mcast::dlna::runtime {

struct DlnaProtocolRuntimeOptions {
    std::string friendlyName;
    std::string udn;
    std::uint16_t httpPort = 0;
    std::uint16_t ssdpPort = 1900;
    bool bindLoopback = true;
    bool enableSsdp = true;
    bool startReceivingEnabled = true;
    std::function<bool()> requestUiActivation;
    std::function<void(const std::string &)> log;
    std::function<void(bool, std::uint64_t)> receivingStateChanged;
};

// 一个可运行的 DLNA HTTP/Claim 子系统。播放器可以始终缺席；播放命令通过
// LateBindingPlaybackGateway 失败关闭或等待 Ui 挂接，媒体线程不与 HTTP 线程混用。
class DlnaProtocolRuntime
{
public:
    DlnaProtocolRuntime(
        playback::PlaybackGateway &playback,
        DlnaClaimConfirmationPort &confirmationPort,
        DlnaProtocolRuntimeOptions options);
    ~DlnaProtocolRuntime();

    DlnaProtocolRuntime(const DlnaProtocolRuntime &) = delete;
    DlnaProtocolRuntime &operator=(
        const DlnaProtocolRuntime &) = delete;

    bool start(std::string &error);
    void stop() noexcept;
    bool setReceivingEnabled(bool enabled, std::string &error);
    bool requestDisconnect(std::string &error);
    bool receivingEnabled() const noexcept
    {
        return m_receivingEnabled.load();
    }
    std::uint64_t receivingStateVersion() const noexcept
    {
        return m_receivingStateVersion.load();
    }
    std::uint16_t localPort() const noexcept;
    std::uint16_t localSsdpPort() const noexcept
    {
        return m_ssdp.localPort();
    }

private:
    struct MediaTask {
        enum class Kind {
            ExecuteMedia,
            PlayNow,
            StopNow,
        };

        Kind kind = Kind::ExecuteMedia;
        claim::DlnaMediaExecution media;
    };

    struct PendingMedia {
        claim::DlnaMediaExecution execution;
        std::uint64_t baselineStateVersion = 0;
        std::uint64_t commandId = 0;
        bool waitingForPlayer = false;
        bool waitingForSnapshot = false;
    };

    DlnaSessionCallbacks sessionCallbacks();
    claim::DlnaClaimHttpCallbacks httpCallbacks();
    void enqueue(MediaTask task);
    void mediaLoop(std::stop_token stop);
    void httpLoop(std::stop_token stop);
    void sessionLoop(std::stop_token stop);
    bool startNetworkSurface(std::string &error);
    void stopNetworkSurface() noexcept;
    void stopSsdpSurface() noexcept;
    bool startHttpLocked(std::string &error);
    void stopHttpLocked() noexcept;
    bool ensureConfirmationWorker(std::string &error);
    void stopConfirmationWorker() noexcept;
    void discardPendingMedia() noexcept;
    void disableReceivingLocked() noexcept;
    void handleNetworkFailure() noexcept;
    void handlePlaybackEvent(const PlaybackEvent &event);
    void completePendingMedia(
        std::uint64_t operationId, bool success);
    void notifyReceivingState() const noexcept;
    void log(const std::string &message) const;
    static cast::CastTime castNow();
    static SessionTime sessionNow();

    DlnaProtocolRuntimeOptions m_options;
    playback::PlaybackGateway &m_playback;
    DlnaClaimConfirmationPort &m_confirmationPort;
    cast::CastClaimCoordinator m_coordinator;
    claim::DlnaClaimAdapter m_adapter;
    claim::DlnaClaimRuntime m_claimRuntime;
    DlnaSession m_session;
    GenaManager m_gena;
    soap::SoapRouter m_router;
    claim::DlnaClaimHttpHandler m_handler;
    DlnaHttpFrontend m_frontend;
    net::HttpServer m_http;
    net::SsdpService m_ssdp;
    std::unique_ptr<DlnaClaimConfirmationWorker> m_confirmationWorker;

    mutable std::mutex m_mediaMutex;
    mutable std::mutex m_networkMutex;
    std::mutex m_receivingMutex;
    std::mutex m_requestGateMutex;
    std::mutex m_confirmationMutex;
    std::mutex m_completionMutex;
    std::condition_variable m_mediaReady;
    std::deque<MediaTask> m_mediaTasks;
    std::optional<PendingMedia> m_pendingMedia;
    std::unordered_map<std::uint64_t, PlaybackEvent>
        m_earlyCommandCompletions;
    std::jthread m_mediaThread;
    std::jthread m_httpThread;
    std::jthread m_sessionThread;
    std::uint64_t m_playbackListener = 0;
    std::atomic_bool m_running{false};
    std::atomic_bool m_receivingEnabled{false};
    std::atomic_uint64_t m_receivingStateVersion{1};
    std::atomic_bool m_surfaceKeptForActiveSession{false};
    std::atomic_bool m_httpPolling{false};
    std::atomic_bool m_networkRecoveryPending{false};
    std::optional<SessionTime> m_disconnectRestartAt;
    bool m_mediaStopping = false;
};

} // namespace mcast::dlna::runtime
