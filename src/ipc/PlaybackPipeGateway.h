// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/NamedPipe.h"
#include "playback/PlaybackGateway.h"
#include "playback/PlaybackCommandCoalescer.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mcast::playback {

class PlaybackPipeGateway final : public PlaybackGateway
{
public:
    using Log = std::function<void(const std::string &)>;
    using EndSessionRequest = std::function<void()>;

    explicit PlaybackPipeGateway(
        std::unique_ptr<ipc::PipeConnection> connection,
        Log log = {}, EndSessionRequest endSessionRequest = {});
    ~PlaybackPipeGateway() override;

    PlaybackPipeGateway(const PlaybackPipeGateway &) = delete;
    PlaybackPipeGateway &operator=(const PlaybackPipeGateway &) = delete;

    PlaybackSnapshot snapshot() const override;
    PlaybackCommandSubmission submit(
        const PlaybackCommand &command) override;
    std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) override;
    void removeEventListener(std::uint64_t listenerId) override;

    bool requestSnapshot(std::string &error);
    bool connected() const noexcept { return m_connected.load(); }

private:
    struct DeferredCommandResult {
        std::uint64_t commandId = 0;
        std::uint64_t expectedGeneration = 0;
        std::uint64_t generation = 0;
        std::uint64_t stateVersion = 0;
        PlaybackSubmissionStatus submissionStatus =
            PlaybackSubmissionStatus::InvalidCommand;
        PlaybackOutcomeStatus outcome =
            PlaybackOutcomeStatus::Rejected;
        std::string error;
    };

    void readLoop(std::stop_token stop);
    void writerLoop(std::stop_token stop);
    void timeoutLoop(std::stop_token stop);
    void handleMessage(const ipc::Message &message);
    void markDisconnected(const std::string &reason);
    void completeUnsentCommands(
        const std::vector<std::uint64_t> &commandIds,
        PlaybackOutcomeStatus outcome, const std::string &error);
    void notifyCommandResult(
        const DeferredCommandResult &result,
        const PlaybackSnapshot &snapshot);
    void notify(const PlaybackEvent &event);
    std::uint64_t nextMessageId();

    std::unique_ptr<ipc::PipeConnection> m_connection;
    Log m_log;
    EndSessionRequest m_endSessionRequest;
    mutable std::mutex m_snapshotMutex;
    PlaybackSnapshot m_snapshot;
    bool m_hasSnapshot = false;
    std::mutex m_listenerMutex;
    struct ListenerEntry {
        std::function<void(const PlaybackEvent &)> callback;
        std::size_t activeCallbacks = 0;
        bool removing = false;
    };
    std::map<std::uint64_t, ListenerEntry> m_listeners;
    std::condition_variable m_listenerIdle;
    std::mutex m_writeMutex;
    std::mutex m_outboundMutex;
    std::condition_variable m_outboundReady;
    PlaybackCommandCoalescer m_outbound;
    std::chrono::steady_clock::time_point m_dispatchAfter;
    std::mutex m_pendingMutex;
    struct PendingCommand {
        std::uint64_t generation = 0;
        std::uint64_t canonicalId = 0;
        std::chrono::steady_clock::time_point deadline;
        bool inFlight = false;
    };
    std::map<std::uint64_t, PendingCommand> m_pendingCommands;
    std::map<std::uint64_t, DeferredCommandResult>
        m_deferredCommandResults;
    std::atomic_uint64_t m_nextMessageId{1};
    std::atomic_uint64_t m_nextListenerId{1};
    std::atomic_bool m_connected{true};
    std::atomic_bool m_uiReady{false};
    std::jthread m_readThread;
    std::jthread m_writerThread;
    std::jthread m_timeoutThread;
    std::atomic_bool m_disconnectReported{false};
};

} // namespace mcast::playback
