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
constexpr std::size_t kMaxPendingCommands = 1024;
constexpr auto kCoalesceWindow = 4ms;
thread_local std::uint64_t t_activeListenerId = 0;

void preservePlaybackPlan(
    MediaRequest &incoming, const MediaRequest &cached)
{
    if (incoming.uri.empty() || incoming.uri != cached.uri)
        return;
    incoming.route = cached.route;
    incoming.imageSequenceFrames = cached.imageSequenceFrames;
}

void mergePlanOmittedFromDelta(
    PlaybackSnapshot &incoming, const PlaybackSnapshot &cached)
{
    preservePlaybackPlan(incoming.current, cached.current);
    preservePlaybackPlan(incoming.next, cached.next);

    // next()/previous() shift the cached slots. A state delta intentionally
    // omits the frame table, so recover the plan from the slot the media just
    // moved out of.
    if (!incoming.current.uri.empty()
        && incoming.current.uri == cached.next.uri) {
        incoming.current.route = cached.next.route;
        incoming.current.imageSequenceFrames =
            cached.next.imageSequenceFrames;
    }
    if (!incoming.next.uri.empty()
        && incoming.next.uri == cached.current.uri) {
        incoming.next.route = cached.current.route;
        incoming.next.imageSequenceFrames =
            cached.current.imageSequenceFrames;
    }
}

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
    m_writerThread = std::jthread(
        [this](std::stop_token stop) { writerLoop(stop); });
    m_timeoutThread = std::jthread(
        [this](std::stop_token stop) { timeoutLoop(stop); });
}

PlaybackPipeGateway::~PlaybackPipeGateway()
{
    m_readThread.request_stop();
    m_writerThread.request_stop();
    m_timeoutThread.request_stop();
    m_outboundReady.notify_all();
    if (m_connection)
        m_connection->close();
    if (m_readThread.joinable())
        m_readThread.join();
    if (m_writerThread.joinable())
        m_writerThread.join();
    if (m_timeoutThread.joinable())
        m_timeoutThread.join();
}

PlaybackSnapshot PlaybackPipeGateway::snapshot() const
{
    std::scoped_lock lock(m_snapshotMutex);
    PlaybackSnapshot result = m_snapshot;
    // A connected Ui can publish its player snapshot before the first frame,
    // but Protocol must keep treating it as unavailable until PlaybackUiReady.
    if (!m_hasSnapshot || !m_uiReady)
        result.playerAvailable = false;
    return result;
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
    std::vector<std::uint64_t> superseded;
    const auto deadline =
        std::chrono::steady_clock::now() + kCommandTimeout;
    bool wasEmpty = false;
    {
        std::scoped_lock outboundLock(m_outboundMutex);
        if (!m_connected) {
            return {false, PlaybackSubmissionStatus::PlayerUnavailable, 0,
                    current.generation, "playback pipe is unavailable",
                    current.stateVersion};
        }
        if (bound.type != PlaybackCommandType::EndSession) {
            std::scoped_lock pendingLock(m_pendingMutex);
            if (m_pendingCommands.size() >= kMaxPendingCommands) {
                return {false, PlaybackSubmissionStatus::Busy, 0,
                        current.generation,
                        "too many pending playback commands",
                        current.stateVersion};
            }
        }
        wasEmpty = m_outbound.empty();
        auto queued = m_outbound.enqueue({
            commandId,
            bound,
            {},
        });
        if (!queued.queued) {
            return {false, PlaybackSubmissionStatus::Busy, 0,
                    current.generation,
                    "too many pending playback commands",
                    current.stateVersion};
        }
        superseded = std::move(queued.supersededCommandIds);
        {
            std::scoped_lock pendingLock(m_pendingMutex);
            m_pendingCommands[commandId] =
                PendingCommand{
                    current.generation, commandId, deadline, false};
            for (const std::uint64_t id : superseded) {
                const auto pending = m_pendingCommands.find(id);
                if (pending != m_pendingCommands.end()) {
                    pending->second.canonicalId = commandId;
                    pending->second.deadline = deadline;
                }
            }
        }
        if (bound.type == PlaybackCommandType::EndSession
            || wasEmpty) {
            m_dispatchAfter =
                bound.type == PlaybackCommandType::EndSession
                ? std::chrono::steady_clock::now()
                : std::chrono::steady_clock::now()
                    + kCoalesceWindow;
        }
    }
    std::vector<std::uint64_t> rejected;
    {
        std::scoped_lock lock(m_pendingMutex);
        if (bound.type == PlaybackCommandType::EndSession) {
            for (const std::uint64_t id : superseded) {
                if (id == commandId)
                    continue;
                if (m_pendingCommands.erase(id) != 0)
                    rejected.push_back(id);
            }
        }
    }
    completeUnsentCommands(
        rejected, PlaybackOutcomeStatus::Rejected,
        "superseded by EndSession");
    m_outboundReady.notify_one();
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
    if (!message) {
        error = "cannot encode playback snapshot request";
        return false;
    }
    std::scoped_lock writeLock(m_writeMutex);
    return m_connection->writeMessage(*message, 2000ms, error);
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

void PlaybackPipeGateway::writerLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        PlaybackCommandCoalescer::QueuedCommand queued;
        {
            std::unique_lock lock(m_outboundMutex);
            m_outboundReady.wait(lock, [&] {
                return stop.stop_requested() || !m_outbound.empty();
            });
            if (stop.stop_requested())
                return;
            if (std::chrono::steady_clock::now() < m_dispatchAfter) {
                m_outboundReady.wait_until(
                    lock, m_dispatchAfter, [&] {
                        return stop.stop_requested();
                    });
                if (stop.stop_requested())
                    return;
            }
            auto next = m_outbound.takeNext();
            if (!next)
                continue;
            {
                std::scoped_lock pendingLock(m_pendingMutex);
                bool found = false;
                for (auto &[id, pending] : m_pendingCommands) {
                    (void)id;
                    if (pending.canonicalId != next->commandId)
                        continue;
                    pending.inFlight = true;
                    found = true;
                }
                if (!found)
                    continue;
            }
            queued = std::move(*next);
        }

        std::string error;
        if (!wire::encodePlaybackCommand(queued.command, error)) {
            std::vector<std::uint64_t> failedIds = queued.aliases;
            failedIds.push_back(queued.commandId);
            completeUnsentCommands(
                failedIds, PlaybackOutcomeStatus::Rejected,
                error.empty() ? "invalid playback command" : error);
            continue;
        }
        auto message = wire::makePlaybackCommandMessage(
            queued.commandId, queued.command, error);
        bool written = false;
        if (message) {
            std::scoped_lock writeLock(m_writeMutex);
            written = m_connection
                && m_connection->writeMessage(*message, 2000ms, error);
        } else if (error.empty()) {
            error = "cannot encode playback command";
        }
        if (!written && !stop.stop_requested()) {
            markDisconnected(
                error.empty() ? "cannot send playback command" : error);
            return;
        }
        if (written) {
            std::scoped_lock outboundLock(m_outboundMutex);
            std::scoped_lock pendingLock(m_pendingMutex);
            const auto deadline =
                std::chrono::steady_clock::now() + kCommandTimeout;
            for (auto &[id, pending] : m_pendingCommands) {
                (void)id;
                if (pending.canonicalId != queued.commandId)
                    continue;
                pending.inFlight = false;
                pending.deadline = deadline;
            }
        }
    }
}

void PlaybackPipeGateway::timeoutLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        for (int slice = 0; slice < 10 && !stop.stop_requested(); ++slice)
            std::this_thread::sleep_for(100ms);
        if (stop.stop_requested())
            return;

        std::map<std::uint64_t, std::vector<std::uint64_t>> expired;
        const auto now = std::chrono::steady_clock::now();
        {
            std::scoped_lock outboundLock(m_outboundMutex);
            std::scoped_lock pendingLock(m_pendingMutex);
            for (auto it = m_pendingCommands.begin();
                 it != m_pendingCommands.end();) {
                if (!it->second.inFlight
                    && now >= it->second.deadline) {
                    expired[it->second.canonicalId].push_back(it->first);
                    it = m_pendingCommands.erase(it);
                } else {
                    ++it;
                }
            }
            for (const auto &[canonicalId, commandIds] : expired) {
                (void)commandIds;
                static_cast<void>(m_outbound.discard(canonicalId));
            }
        }
        const PlaybackSnapshot current = snapshot();
        for (const auto &[canonicalId, commandIds] : expired) {
            (void)canonicalId;
            for (const std::uint64_t commandId : commandIds) {
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
}

void PlaybackPipeGateway::handleMessage(const ipc::Message &message)
{
    if (m_disconnectReported)
        return;
    if (message.header.type == ipc::MessageType::PlaybackUiReady) {
        m_uiReady = true;
        if (m_log)
            m_log("Playback UI ready");
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
        std::vector<std::uint64_t> commandIds;
        {
            std::scoped_lock lock(m_pendingMutex);
            const auto pending = m_pendingCommands.find(result.commandId);
            if (pending == m_pendingCommands.end()) {
                if (m_log)
                    m_log("Ignoring unknown or duplicate playback result");
                return;
            }
            expectedGeneration = pending->second.generation;
            const std::uint64_t canonicalId =
                pending->second.canonicalId;
            for (auto it = m_pendingCommands.begin();
                 it != m_pendingCommands.end();) {
                if (it->second.canonicalId == canonicalId) {
                    commandIds.push_back(it->first);
                    it = m_pendingCommands.erase(it);
                } else {
                    ++it;
                }
            }
        }

        const PlaybackSnapshot current = snapshot();
        for (const std::uint64_t commandId : commandIds) {
            DeferredCommandResult completed;
            completed.commandId = commandId;
            completed.expectedGeneration = expectedGeneration;
            completed.generation = result.generation;
            completed.stateVersion = result.stateVersion;
            completed.submissionStatus = result.submissionStatus;
            completed.outcome = result.outcome;
            completed.error = result.error;
            if (current.stateVersion < completed.stateVersion
                || current.generation < completed.generation) {
                std::scoped_lock lock(m_snapshotMutex);
                m_deferredCommandResults[commandId] = completed;
            } else {
                notifyCommandResult(completed, current);
            }
        }
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

    std::vector<DeferredCommandResult> readyResults;
    {
        std::scoped_lock lock(m_snapshotMutex);
        const bool includesPlaybackPlan =
            incoming.playbackPlanIncluded;
        if (m_hasSnapshot && !includesPlaybackPlan)
            mergePlanOmittedFromDelta(incoming, m_snapshot);
        if (m_hasSnapshot
            && (incoming.generation < m_snapshot.generation
                || (incoming.generation == m_snapshot.generation
                    && incoming.stateVersion
                        <= m_snapshot.stateVersion)))
            return;
        if (m_disconnectReported)
            return;
        m_snapshot = incoming;
        m_hasSnapshot = true;
        m_connected = incoming.playerAvailable;
        if (incoming.playerAvailable)
            m_disconnectReported = false;
        for (auto it = m_deferredCommandResults.begin();
             it != m_deferredCommandResults.end();) {
            const bool newerGeneration =
                incoming.generation > it->second.generation;
            const bool sameGenerationReady =
                incoming.generation == it->second.generation
                && incoming.stateVersion >= it->second.stateVersion;
            if (kind == PlaybackEventKind::PlayerLost
                || newerGeneration || sameGenerationReady) {
                DeferredCommandResult result = std::move(it->second);
                if (kind == PlaybackEventKind::PlayerLost) {
                    result.outcome = PlaybackOutcomeStatus::Rejected;
                    result.expectedGeneration = incoming.generation;
                    result.error = "playback player was lost";
                }
                readyResults.push_back(std::move(result));
                it = m_deferredCommandResults.erase(it);
            } else {
                ++it;
            }
        }
    }
    PlaybackEvent event;
    event.kind = kind;
    event.snapshot = incoming;
    for (const DeferredCommandResult &result : readyResults)
        notifyCommandResult(result, incoming);
    notify(event);
}

void PlaybackPipeGateway::notifyCommandResult(
    const DeferredCommandResult &result,
    const PlaybackSnapshot &snapshot)
{
    PlaybackEvent event;
    event.kind = PlaybackEventKind::CommandCompleted;
    event.commandId = result.commandId;
    event.outcome = result.outcome;
    event.error = result.error;
    event.snapshot = snapshot;
    if (event.outcome == PlaybackOutcomeStatus::Applied
        && (result.generation < snapshot.generation
            || result.generation != result.expectedGeneration)) {
        event.outcome = PlaybackOutcomeStatus::StaleGeneration;
        if (event.error.empty())
            event.error = "command completed in an old generation";
    }
    notify(event);
}

void PlaybackPipeGateway::completeUnsentCommands(
    const std::vector<std::uint64_t> &commandIds,
    PlaybackOutcomeStatus outcome, const std::string &error)
{
    if (commandIds.empty())
        return;
    const PlaybackSnapshot current = snapshot();
    for (const std::uint64_t commandId : commandIds) {
        PlaybackEvent event;
        event.kind = PlaybackEventKind::CommandCompleted;
        event.commandId = commandId;
        event.outcome = outcome;
        event.error = error;
        event.snapshot = current;
        notify(event);
    }
}

void PlaybackPipeGateway::markDisconnected(const std::string &reason)
{
    if (m_disconnectReported.exchange(true))
        return;
    m_connected = false;
    m_uiReady = false;
    m_readThread.request_stop();
    m_writerThread.request_stop();
    {
        std::scoped_lock lock(m_outboundMutex);
        m_outbound.clear();
    }
    m_outboundReady.notify_all();
    PlaybackSnapshot final;
    std::vector<DeferredCommandResult> deferred;
    {
        std::scoped_lock lock(m_snapshotMutex);
        const auto generation = m_snapshot.generation;
        const auto stateVersion = m_snapshot.stateVersion + 1;
        const bool peerConnected = m_snapshot.peerConnected;
        const auto castSession = m_snapshot.castSession;
        const int volume = m_snapshot.volumePercent;
        const bool muted = m_snapshot.muted;
        const auto controls = m_snapshot.pictureControls;
        m_snapshot = PlaybackSnapshot{};
        m_snapshot.generation = generation;
        m_snapshot.stateVersion = stateVersion;
        m_snapshot.playerAvailable = false;
        m_snapshot.peerConnected = peerConnected;
        m_snapshot.castSession = castSession;
        m_snapshot.volumePercent = volume;
        m_snapshot.muted = muted;
        m_snapshot.pictureControls = controls;
        for (auto &[commandId, result] : m_deferredCommandResults) {
            (void)commandId;
            deferred.push_back(std::move(result));
        }
        m_deferredCommandResults.clear();
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
    for (const DeferredCommandResult &result : deferred) {
        PlaybackEvent completed;
        completed.kind = PlaybackEventKind::CommandCompleted;
        completed.commandId = result.commandId;
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
