// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackGateway.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mcast::playback {

/**
 * Adds protocol-side playback planning in front of another gateway.
 *
 * Planning can perform network I/O, so Open/SetNext commands are acknowledged
 * with a stable local command ID immediately and submitted upstream only after
 * the plan is ready. Other commands pass through unchanged.
 */
class PlanningPlaybackGateway final : public PlaybackGateway
{
public:
    using Planner = std::function<MediaRequest(
        const MediaRequest &, std::string &error)>;

    PlanningPlaybackGateway(
        PlaybackGateway &upstream, Planner planner);
    PlanningPlaybackGateway(
        PlaybackGateway &upstream, Planner planner,
        std::chrono::milliseconds planningTimeout);
    ~PlanningPlaybackGateway() override;

    PlanningPlaybackGateway(const PlanningPlaybackGateway &) = delete;
    PlanningPlaybackGateway &operator=(
        const PlanningPlaybackGateway &) = delete;

    PlaybackSnapshot snapshot() const override;
    PlaybackCommandSubmission submit(
        const PlaybackCommand &command) override;
    std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) override;
    void removeEventListener(std::uint64_t listenerId) override;

private:
    struct ListenerEntry {
        std::function<void(const PlaybackEvent &)> callback;
        std::size_t activeCallbacks = 0;
        bool removing = false;
    };

    struct Job {
        PlaybackCommand command;
        std::uint64_t syntheticCommandId = 0;
        std::uint64_t epoch = 0;
        std::uint64_t planEpoch = 0;
        std::chrono::steady_clock::time_point deadline;
        bool open = false;
    };

    void workerLoop(std::stop_token stop);
    void cancelPlanningForBoundary();
    void onUpstreamEvent(const PlaybackEvent &event);
    PlaybackCommandSubmission submitUpstream(
        const PlaybackCommand &command,
        std::uint64_t syntheticCommandId,
        std::unique_lock<std::mutex> *planCommitLock = nullptr);
    void reject(
        std::uint64_t syntheticCommandId,
        const std::string &reason);
    void notify(const PlaybackEvent &event);
    std::uint64_t nextSyntheticCommandId();
    static bool needsPlanning(const PlaybackCommand &command);
    static bool isOpen(const PlaybackCommand &command);

    PlaybackGateway &m_upstream;
    Planner m_planner;
    const std::chrono::milliseconds m_planningTimeout;
    const std::uint64_t m_upstreamListenerId = 0;

    mutable std::mutex m_jobMutex;
    std::condition_variable_any m_jobReady;
    std::deque<Job> m_jobs;
    std::optional<std::uint64_t> m_pendingOpenCommandId;
    std::optional<std::uint64_t> m_pendingNextCommandId;
    std::uint64_t m_openEpoch = 0;
    std::uint64_t m_nextEpoch = 0;
    std::uint64_t m_planEpoch = 0;
    std::atomic_uint64_t m_syntheticCommandId{1};
    std::vector<std::jthread> m_workers;

    std::mutex m_planCommitMutex;
    std::mutex m_upstreamSubmitMutex;
    std::mutex m_upstreamEventMutex;
    std::map<std::uint64_t, std::uint64_t> m_upstreamToSynthetic;
    std::map<std::uint64_t, PlaybackEvent> m_earlyUpstreamEvents;
    std::uint64_t m_installingUpstreamSynthetic = 0;

    std::mutex m_listenerMutex;
    std::map<std::uint64_t, ListenerEntry> m_listeners;
    std::condition_variable_any m_listenerIdle;
    std::uint64_t m_nextListenerId = 1;
};

} // namespace mcast::playback
