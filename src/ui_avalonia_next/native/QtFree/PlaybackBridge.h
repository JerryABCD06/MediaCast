// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/EventLoop.h"
#include "QtFree/PlaybackController.h"
#include "QtFree/Signal.h"
#include "ipc/NamedPipe.h"
#include "playback/PlaybackTypes.h"

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
#include <vector>

namespace mcast::avalonia::qtfree {

class PlaybackBridge
{
public:
    PlaybackBridge(
        std::shared_ptr<ipc::PipeConnection> connection,
        PlaybackController &controller,
        EventLoop &loop, std::uint64_t generation);
    ~PlaybackBridge();

    PlaybackBridge(const PlaybackBridge &) = delete;
    PlaybackBridge &operator=(const PlaybackBridge &) = delete;

    void stop();
    void requestEndSession();
    void pauseAndFlushState();
    void beginControlAdjustment();
    void endControlAdjustment();
    bool notifyUiReady();
    void dropConnectionForTest();

    std::uint64_t generation() const noexcept { return m_generation; }
    std::uint64_t stateVersion() const noexcept { return m_stateVersion; }

    Signal<const std::string &> logMessage;

private:
    void connectController();
    void readLoop(std::stop_token stop);
    void writerLoop(std::stop_token stop);
    void dispatchMessage(ipc::Message message);
    void handleCommand(
        std::uint64_t commandId,
        const playback::PlaybackCommand &command);
    void sendCommandResult(
        std::uint64_t commandId,
        playback::PlaybackOutcomeStatus outcome,
        const std::string &error);
    void sendSnapshotResponse(std::uint64_t requestId);
    void scheduleState();
    void cancelStateTimer();
    void flushState();
    bool playbackPlanNeedsSerialization() const;
    void markPlaybackPlanSerialized();
    playback::PlaybackSnapshot snapshot(
        bool includePlaybackPlan = true) const;
    playback::PlaybackOutcomeStatus execute(
        const playback::PlaybackCommand &command,
        std::string &error);
    bool sendMessage(
        ipc::Message message, const char *what,
        bool coalescible = false, bool urgent = false);
    bool post(std::function<void()> task);

    std::shared_ptr<ipc::PipeConnection> m_connection;
    PlaybackController &m_controller;
    EventLoop &m_loop;
    std::vector<Connection> m_controllerConnections;
    std::atomic_bool m_connected{true};
    std::atomic_bool m_stopped{false};
    std::atomic_uint64_t m_nextRequestId{1};
    std::jthread m_readThread;
    std::jthread m_writerThread;
    std::mutex m_writeMutex;
    std::mutex m_outboundMutex;
    std::condition_variable m_outboundReady;
    std::deque<ipc::Message> m_urgentOutbound;
    std::deque<ipc::Message> m_orderedOutbound;
    std::optional<ipc::Message> m_latestState;
    bool m_acceptOutbound = true;
    std::uint64_t m_stateTimer = 0;
    std::uint64_t m_generation = 1;
    std::atomic_uint64_t m_stateVersion{1};
    std::string m_lastPlanCurrentFingerprint;
    std::string m_lastPlanNextFingerprint;
    bool m_playerLost = false;
    bool m_controlAdjustmentActive = false;
    bool m_sendReconnected = true;
    bool m_sendPlayerLost = false;
    playback::MediaRequest m_currentMedia;
    playback::MediaRequest m_nextMedia;
    playback::MediaRequest m_previousMedia;
};

} // namespace mcast::avalonia::qtfree

