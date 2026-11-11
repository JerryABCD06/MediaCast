// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/PlaybackPipeGateway.h"

#include "ipc/PlaybackCodec.h"

#include <exception>
#include <utility>
#include <vector>

namespace mcast::playback {
using namespace std::chrono_literals;
namespace {

constexpr auto kCommandTimeout = 5s;
constexpr std::size_t kMaxPendingCommands = 256;
thread_local std::uint64_t t_activeListenerId = 0;

} // namespace

PlaybackPipeGateway::PlaybackPipeGateway(
    std::unique_ptr<ipc::PipeConnection> connection, Log log,
    EndSessionRequest endSessionRequest)
    : m_connection(std::move(connection))
    , m_log(std::move(log))
    , m_endSessionRequest(std::move(endSessionRequest))
{
    if (!m_connection || !m_connection->valid()) {
        m_connected = false;
        m_snapshot.playerAvailable = false;
        return;
    }
    // No authoritative Ui snapshot has arrived yet. In particular, generation
    // 1 is not a valid default once the Ui activation ID advances.
    m_snapshot = {};
    m_snapshot.playerAvailable = false;
    m_snapshot.stateVersion = 0;
    m_readThread = std::jthread(
        [this](std::stop_token stop) { readLoop(stop); });
    m_timeoutThread = std::jthread(
        [this](std::stop_token stop) { timeoutLoop(stop); });
}

PlaybackPipeGateway::~PlaybackPipeGateway()
{
    m_readThread.request_stop();
    m_timeoutThread.request_stop();
    if (m_connection)
        m_connection->close();
    if (m_readThread.joinable())
        m_readThread.join();
    if (m_timeoutThread.joinable())
        m_timeoutThread.join();
}

PlaybackSnapshot PlaybackPipeGateway::snapshot() const
{
    std::scoped_lock lock(m_snapshotMutex);
    return m_snapshot;
}

PlaybackCommandSubmission PlaybackPipeGateway::submit(
    const PlaybackCommand &command)
{
    PlaybackCommand bound = command;
    PlaybackSnapshot current;
    bool hasSnapshot = false;
    {
        std::scoped_lock lock(m_snapshotMutex);
        current = m_snapshot;
        hasSnapshot = m_hasSnapshot;
    }
    if (!m_connected) {
        return {false, PlaybackSubmissionStatus::PlayerUnavailable, 0,
                current.generation, "playback pipe is unavailable",
                current.stateVersion};
    }
    if (!hasSnapshot) {
        return {false, PlaybackSubmissionStatus::WaitingForPlayer, 0,
                current.generation, "playback snapshot is not ready",
                current.stateVersion};
    }
    if (!m_uiReady) {
        return {false, PlaybackSubmissionStatus::WaitingForPlayer, 0,
                current.generation, "playback UI is not ready",
                current.stateVersion};
    }
    if (!current.playerAvailable) {
        return {false, PlaybackSubmissionStatus::PlayerUnavailable, 0,
                current.generation, "playback player is unavailable",
                current.stateVersion};
    }
    if (bound.expectedGeneration == 0)
        bound.expectedGeneration = current.generation;
    if (bound.expectedGeneration != current.generation) {
        return {false, PlaybackSubmissionStatus::StaleGeneration, 0,
                current.generation, "stale playback generation",
                current.stateVersion};
    }

    const std::uint64_t commandId = nextMessageId();
    std::string error;
    if (!wire::encodePlaybackCommand(bound, error)) {
        return {false, PlaybackSubmissionStatus::InvalidCommand, 0,
                current.generation, error, current.stateVersion};
    }
    const auto message =
        wire::makePlaybackCommandMessage(commandId, bound, error);
    {
        std::scoped_lock lock(m_pendingMutex);
        if (m_pendingCommands.size() >= kMaxPendingCommands) {
            return {false, PlaybackSubmissionStatus::InvalidCommand, 0,
                    current.generation, "too many pending playback commands",
                    current.stateVersion};
        }
        m_pendingCommands.emplace(
            commandId,
            PendingCommand{
                current.generation,
                std::chrono::steady_clock::now() + kCommandTimeout});
    }
    if (!message
        || !m_connection->writeMessage(*message, 2000ms, error)) {
        {
            std::scoped_lock lock(m_pendingMutex);
            m_pendingCommands.erase(commandId);
        }
        return {false, PlaybackSubmissionStatus::PlayerUnavailable, 0,
                current.generation,
                error.empty() ? "cannot send playback command" : error,
                current.stateVersion};
    }
    return {true, PlaybackSubmissionStatus::Queued, commandId,
            current.generation, {}, current.stateVersion};
}

std::uint64_t PlaybackPipeGateway::addEventListener(
    std::function<void(const PlaybackEvent &)> listener)
{
    const std::uint64_t id = m_nextListenerId.fetch_add(1);
    if (listener) {
        std::scoped_lock lock(m_listenerMutex);
        m_listeners.emplace(
            id, ListenerEntry{std::move(listener), 0, false});
    }
    return id;
}

void PlaybackPipeGateway::removeEventListener(std::uint64_t listenerId)
{
    std::unique_lock lock(m_listenerMutex);
    const auto it = m_listeners.find(listenerId);
    if (it == m_listeners.end())
        return;
    it->second.removing = true;
    if (t_activeListenerId == listenerId)
        return;
    m_listenerIdle.wait(lock, [&] {
        const auto current = m_listeners.find(listenerId);
        return current == m_listeners.end()
            || current->second.activeCallbacks == 0;
    });
    m_listeners.erase(listenerId);
}

bool PlaybackPipeGateway::requestSnapshot(std::string &error)
{
    if (!m_connected || !m_connection) {
        error = "playback pipe is unavailable";
        return false;
    }
    const auto message =
        wire::makePlaybackSnapshotRequestMessage(nextMessageId());
    return message
        && m_connection->writeMessage(*message, 2000ms, error);
}

void PlaybackPipeGateway::readLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        ipc::Message message;
        std::string error;
        if (!m_connection->readMessage(message, 24h, error)) {
            if (!stop.stop_requested())
                markDisconnected(error);
            return;
        }
        handleMessage(message);
    }
}

void PlaybackPipeGateway::timeoutLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        for (int slice = 0; slice < 10 && !stop.stop_requested(); ++slice)
            std::this_thread::sleep_for(100ms);
        if (stop.stop_requested())
            return;

        std::vector<std::uint64_t> expired;
        const auto now = std::chrono::steady_clock::now();
        {
            std::scoped_lock lock(m_pendingMutex);
            for (auto it = m_pendingCommands.begin();
                 it != m_pendingCommands.end();) {
                if (now >= it->second.deadline) {
                    expired.push_back(it->first);
                    it = m_pendingCommands.erase(it);
                } else {
                    ++it;
                }
            }
        }
        const PlaybackSnapshot current = snapshot();
        for (std::uint64_t commandId : expired) {
            PlaybackEvent event;
            event.kind = PlaybackEventKind::CommandCompleted;
            event.commandId = commandId;
            event.outcome = PlaybackOutcomeStatus::TimedOut;
            event.error = "playback command timed out";
            event.snapshot = current;
            notify(event);
        }
    }
}

void PlaybackPipeGateway::handleMessage(const ipc::Message &message)
{
    if (message.header.type == ipc::MessageType::PlaybackUiReady) {
        m_uiReady = true;
        PlaybackEvent ready;
        ready.kind = PlaybackEventKind::SnapshotChanged;
        ready.snapshot = snapshot();
        notify(ready);
        return;
    }
    if (message.header.type == ipc::MessageType::UiEndCastRequest) {
        if (m_endSessionRequest) {
            try {
                m_endSessionRequest();
            } catch (const std::exception &exception) {
                if (m_log)
                    m_log(
                        std::string("End-session callback failed: ")
                        + exception.what());
            } catch (...) {
                if (m_log)
                    m_log("End-session callback failed");
            }
        }
        return;
    }
    if (message.header.type == ipc::MessageType::PlaybackCommandResult) {
        wire::PlaybackCommandResultWire result;
        std::string error;
        if (!wire::decodePlaybackCommandResultMessage(
                message, result, error)) {
            if (m_log)
                m_log("Invalid playback result: " + error);
            return;
        }
        std::uint64_t expectedGeneration = 0;
        {
            std::scoped_lock lock(m_pendingMutex);
            const auto pending = m_pendingCommands.find(result.commandId);
            if (pending == m_pendingCommands.end()) {
                if (m_log)
                    m_log("Ignoring unknown or duplicate playback result");
                return;
            }
            expectedGeneration = pending->second.generation;
            m_pendingCommands.erase(pending);
        }
        PlaybackEvent event;
        event.kind = PlaybackEventKind::CommandCompleted;
        event.commandId = result.commandId;
        event.outcome = result.outcome;
        event.error = result.error;
        event.snapshot = snapshot();
        if (result.generation < event.snapshot.generation
            || result.generation != expectedGeneration) {
            event.outcome = PlaybackOutcomeStatus::StaleGeneration;
            if (event.error.empty())
                event.error = "command completed in an old generation";
        }
        notify(event);
        return;
    }

    if (message.header.type != ipc::MessageType::PlaybackSnapshotResponse
        && message.header.type != ipc::MessageType::PlaybackStateDelta
        && message.header.type != ipc::MessageType::PlaybackPlayerLost
        && message.header.type != ipc::MessageType::PlaybackReconnected)
        return;

    PlaybackSnapshot incoming;
    std::string error;
    if (!wire::decodePlaybackSnapshotMessage(message, incoming, error)) {
        if (m_log)
            m_log("Invalid playback state: " + error);
        return;
    }

    PlaybackEventKind kind = PlaybackEventKind::SnapshotChanged;
    if (message.header.type == ipc::MessageType::PlaybackPlayerLost)
        kind = PlaybackEventKind::PlayerLost;
    else if (message.header.type == ipc::MessageType::PlaybackReconnected)
        kind = PlaybackEventKind::Reconnected;

    {
        std::scoped_lock lock(m_snapshotMutex);
        if (m_hasSnapshot
            && ((kind == PlaybackEventKind::Reconnected
                 && incoming.generation <= m_snapshot.generation)
                || (kind != PlaybackEventKind::Reconnected
                    && (incoming.generation < m_snapshot.generation
                        || (incoming.generation == m_snapshot.generation
                            && incoming.stateVersion
                                <= m_snapshot.stateVersion)))))
            return;
        m_snapshot = incoming;
        m_hasSnapshot = true;
        m_connected = incoming.playerAvailable;
        if (incoming.playerAvailable)
            m_disconnectReported = false;
    }
    PlaybackEvent event;
    event.kind = kind;
    event.snapshot = incoming;
    notify(event);
}

void PlaybackPipeGateway::markDisconnected(const std::string &reason)
{
    if (m_disconnectReported.exchange(true))
        return;
    m_connected = false;
    m_uiReady = false;
    PlaybackSnapshot final;
    {
        std::scoped_lock lock(m_snapshotMutex);
        const auto generation = m_snapshot.generation;
        const auto stateVersion = m_snapshot.stateVersion + 1;
        const bool peerConnected = m_snapshot.peerConnected;
        const int volume = m_snapshot.volumePercent;
        const bool muted = m_snapshot.muted;
        const auto controls = m_snapshot.pictureControls;
        m_snapshot = PlaybackSnapshot{};
        m_snapshot.generation = generation;
        m_snapshot.stateVersion = stateVersion;
        m_snapshot.playerAvailable = false;
        m_snapshot.peerConnected = peerConnected;
        m_snapshot.volumePercent = volume;
        m_snapshot.muted = muted;
        m_snapshot.pictureControls = controls;
        final = m_snapshot;
    }
    PlaybackEvent event;
    event.kind = PlaybackEventKind::PlayerLost;
    event.error = reason;
    event.snapshot = final;
    std::vector<std::uint64_t> pending;
    {
        std::scoped_lock lock(m_pendingMutex);
        for (const auto &[id, command] : m_pendingCommands) {
            (void)command;
            pending.push_back(id);
        }
        m_pendingCommands.clear();
    }
    for (std::uint64_t commandId : pending) {
        PlaybackEvent completed;
        completed.kind = PlaybackEventKind::CommandCompleted;
        completed.commandId = commandId;
        completed.outcome = PlaybackOutcomeStatus::Rejected;
        completed.error = "playback pipe disconnected";
        completed.snapshot = final;
        notify(completed);
    }
    notify(event);
    if (m_log)
        m_log("Playback pipe disconnected: " + reason);
}

void PlaybackPipeGateway::notify(const PlaybackEvent &event)
{
    struct Callback {
        std::uint64_t id = 0;
        std::function<void(const PlaybackEvent &)> callback;
    };
    std::vector<Callback> listeners;
    {
        std::scoped_lock lock(m_listenerMutex);
        for (auto &[id, entry] : m_listeners) {
            if (entry.removing)
                continue;
            ++entry.activeCallbacks;
            listeners.push_back(Callback{id, entry.callback});
        }
    }
    for (const Callback &callback : listeners) {
        t_activeListenerId = callback.id;
        try {
            callback.callback(event);
        } catch (const std::exception &exception) {
            if (m_log)
                m_log(std::string("Playback listener failed: ")
                    + exception.what());
        } catch (...) {
            if (m_log)
                m_log("Playback listener failed with unknown exception");
        }
        t_activeListenerId = 0;
        {
            std::scoped_lock lock(m_listenerMutex);
            const auto it = m_listeners.find(callback.id);
            if (it == m_listeners.end())
                continue;
            if (it->second.activeCallbacks > 0)
                --it->second.activeCallbacks;
            if (it->second.removing && it->second.activeCallbacks == 0)
                m_listeners.erase(it);
        }
        m_listenerIdle.notify_all();
    }
}

std::uint64_t PlaybackPipeGateway::nextMessageId()
{
    std::uint64_t id = m_nextMessageId.fetch_add(1);
    if (id == 0)
        id = m_nextMessageId.fetch_add(1);
    return id;
}

} // namespace mcast::playback
