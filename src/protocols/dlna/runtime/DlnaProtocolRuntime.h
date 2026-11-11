// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "cast/CastClaimCoordinator.h"
#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"
#include "protocols/dlna/runtime/DlnaClaimConfirmationWorker.h"
#include "protocols/dlna/runtime/DlnaHttpFrontend.h"
#include "protocols/dlna/session/DlnaSession.h"

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
    bool bindLoopback = true;
    std::function<bool()> requestUiActivation;
    std::function<void(const std::string &)> log;
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
    std::uint16_t localPort() const noexcept;

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
    void handlePlaybackEvent(const PlaybackEvent &event);
    void completePendingMedia(
        std::uint64_t operationId, bool success);
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
    soap::SoapRouter m_router;
    claim::DlnaClaimHttpHandler m_handler;
    DlnaHttpFrontend m_frontend;
    net::HttpServer m_http;
    std::unique_ptr<DlnaClaimConfirmationWorker> m_confirmationWorker;

    mutable std::mutex m_mediaMutex;
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
    bool m_mediaStopping = false;
};

} // namespace mcast::dlna::runtime
