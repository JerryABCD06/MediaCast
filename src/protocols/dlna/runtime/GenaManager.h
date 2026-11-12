// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "platform/windows/net/SocketRuntime.h"
#include "playback/PlaybackTypes.h"
#include "protocols/dlna/core/HttpRequest.h"
#include "protocols/dlna/net/HttpServer.h"
#include "protocols/dlna/session/DlnaSession.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mcast::dlna::runtime {

struct GenaManagerCallbacks {
    std::function<void(
        std::size_t count, SubscriptionChangeReason reason)>
        subscriptionCountChanged;
    std::function<void()> subscriptionActivity;
    std::function<void(const std::string &)> log;
};

struct GenaManagerOptions {
    std::size_t maxSubscriptions = 32;
    std::size_t workerCount = 4;
    std::chrono::milliseconds subscriptionTimeout{1800000};
    std::chrono::milliseconds initialEventDelay{100};
    std::chrono::milliseconds notifyTimeout{5000};
};

// Qt-free GENA endpoint. SUBSCRIBE/UNSUBSCRIBE are handled on the HTTP owner
// thread; a bounded worker pool delivers at most one NOTIFY per subscription at
// a time and always coalesces pending changes into the latest full snapshot.
class GenaManager
{
public:
    GenaManager(GenaManagerCallbacks callbacks, GenaManagerOptions options);
    ~GenaManager();

    GenaManager(const GenaManager &) = delete;
    GenaManager &operator=(const GenaManager &) = delete;

    bool start(std::string &error);
    void stop() noexcept;

    net::HttpResponse handleSubscribe(
        const core::HttpRequest &request);
    net::HttpResponse handleUnsubscribe(
        const core::HttpRequest &request);

    void updateState(const playback::PlaybackSnapshot &snapshot);
    std::size_t subscriptionCount() const;
    bool quiescent() const;

private:
    enum class NotifyResult {
        Connected,
        ConnectFailed,
        Stopped,
    };

    struct Subscription {
        std::string service;
        std::string callback;
        // Connect failures must not consume a sequence number. The control
        // point never saw the event, so retrying with the same SEQ keeps its
        // strict ordering intact.
        std::uint32_t nextSequence = 0;
        std::chrono::steady_clock::time_point expiresAt;
        std::chrono::steady_clock::time_point retryAt;
        int failures = 0;
        bool dirty = true;
        bool queued = false;
        bool busy = false;
    };

    struct CountNotice {
        std::size_t count = 0;
        SubscriptionChangeReason reason =
            SubscriptionChangeReason::Count;
    };

    void maintenanceLoop(std::stop_token stop);
    void workerLoop(std::stop_token stop);
    void serviceLoop(std::string sid, std::stop_token stop);
    void scheduleLocked(
        const std::string &sid,
        std::chrono::steady_clock::time_point now);
    void markServiceDirtyLocked(const std::string &service);
    CountNotice recountLocked(SubscriptionChangeReason reason);
    void publishCount(const CountNotice &notice);
    void publishActivity();
    void log(const std::string &message) const;
    std::string bodyForLocked(const std::string &service) const;

    GenaManagerCallbacks m_callbacks;
    GenaManagerOptions m_options;
    platform::net::WinsockRuntime m_winsock;
    mutable std::mutex m_mutex;
    std::mutex m_callbackOrderMutex;
    std::condition_variable m_ready;
    std::condition_variable m_maintenanceReady;
    std::unordered_map<std::string, Subscription> m_subscriptions;
    std::deque<std::string> m_readySubscriptions;
    std::vector<std::jthread> m_workers;
    std::jthread m_maintenanceThread;
    std::size_t m_lastReportedCount = 0;
    playback::PlaybackSnapshot m_state;
    std::string m_avTransportBody;
    std::string m_renderingBody;
    std::string m_connectionBody;
    bool m_running = false;
    bool m_stopping = false;
};

} // namespace mcast::dlna::runtime
