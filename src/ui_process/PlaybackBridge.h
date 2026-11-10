// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/NamedPipe.h"
#include "playback/PlaybackTypes.h"

#include <QObject>
#include <QTimer>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

class PlaybackController;

namespace mcast::ui_process {

class PlaybackBridge final : public QObject
{
    Q_OBJECT

public:
    PlaybackBridge(
        std::shared_ptr<ipc::PipeConnection> connection,
        PlaybackController &controller, std::uint64_t generation,
        QObject *parent = nullptr);
    ~PlaybackBridge() override;

signals:
    void logMessage(const QString &text);

private:
    void readLoop(std::stop_token stop);
    void writerLoop(std::stop_token stop);
    void dispatchMessage(const ipc::Message &message);
    void handleCommand(
        std::uint64_t commandId, const playback::PlaybackCommand &command);
    void sendCommandResult(
        std::uint64_t commandId, playback::PlaybackOutcomeStatus outcome,
        const std::string &error);
    void sendSnapshotResponse(std::uint64_t requestId);
    void scheduleState();
    void flushState();
    void enqueueMessage(ipc::Message message, bool coalescible);

    playback::PlaybackSnapshot snapshot() const;
    playback::PlaybackOutcomeStatus execute(
        const playback::PlaybackCommand &command, std::string &error);
    bool sendMessage(
        ipc::Message message, const char *what,
        bool coalescible = false);

    std::shared_ptr<ipc::PipeConnection> m_connection;
    PlaybackController &m_controller;
    std::atomic_bool m_connected = true;
    std::jthread m_readThread;
    std::jthread m_writerThread;
    std::mutex m_outboundMutex;
    std::condition_variable m_outboundReady;
    std::deque<ipc::Message> m_orderedOutbound;
    std::optional<ipc::Message> m_latestState;
    bool m_acceptOutbound = true;
    QTimer m_stateTimer;
    std::uint64_t m_generation = 1;
    std::uint64_t m_stateVersion = 1;
    bool m_playerLost = false;
    bool m_sendReconnected = true;
    bool m_sendPlayerLost = false;
    playback::MediaRequest m_currentMedia;
    playback::MediaRequest m_nextMedia;
    playback::MediaRequest m_previousMedia;
};

} // namespace mcast::ui_process
