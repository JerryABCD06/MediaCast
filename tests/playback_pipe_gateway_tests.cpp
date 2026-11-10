// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"
#include "ipc/PlaybackCodec.h"
#include "ipc/PlaybackPipeGateway.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace mcast::playback;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct EventState {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<PlaybackEvent> events;

    bool waitFor(
        const std::function<bool(const std::vector<PlaybackEvent> &)> &match,
        std::chrono::milliseconds timeout = 5s)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] {
            return match(events);
        });
    }
};

} // namespace

int main()
{
    const std::wstring pipeName =
        mcast::ipc::makePlaybackPipeName(L"pipe-gateway-test");
    mcast::ipc::PipeListener listener(pipeName, 2);
    check(listener.valid(), "listener starts");
    std::string connectError;
    auto client = mcast::ipc::PipeClient::connect(
        pipeName, 2s, connectError);
    check(client != nullptr, "client connects");

    EventState events;
    std::string serverError;
    std::jthread server([&](std::stop_token) {
        try {
            std::string error;
            auto connection = listener.accept(3s, error);
            if (!connection)
                throw std::runtime_error("server accept: " + error);

            mcast::ipc::Message request;
            if (!connection->readMessage(request, 2s, error))
                throw std::runtime_error("snapshot request: " + error);
            std::uint64_t requestId = 0;
            if (!wire::decodePlaybackSnapshotRequestMessage(
                    request, requestId, error))
                throw std::runtime_error("snapshot decode: " + error);

            PlaybackSnapshot initial;
            initial.generation = 1;
            initial.stateVersion = 1;
            initial.playerAvailable = true;
            auto snapshotReply =
                wire::makePlaybackSnapshotResponseMessage(
                    100, requestId, initial, error);
            if (!snapshotReply
                || !connection->writeMessage(*snapshotReply, 2s, error))
                throw std::runtime_error("snapshot reply: " + error);

            mcast::ipc::Message commandMessage;
            if (!connection->readMessage(commandMessage, 2s, error))
                throw std::runtime_error("first command: " + error);
            std::uint64_t commandId = 0;
            PlaybackCommand command;
            if (!wire::decodePlaybackCommandMessage(
                    commandMessage, commandId, command, error)
                || command.type != PlaybackCommandType::Open)
                throw std::runtime_error("first command decode: " + error);
            wire::PlaybackCommandResultWire result;
            result.commandId = commandId;
            result.generation = 1;
            result.stateVersion = 2;
            result.submissionStatus = PlaybackSubmissionStatus::Queued;
            result.outcome = PlaybackOutcomeStatus::Applied;
            auto resultMessage =
                wire::makePlaybackCommandResultMessage(result, error);
            if (!resultMessage
                || !connection->writeMessage(*resultMessage, 2s, error))
                throw std::runtime_error("first result: " + error);

            PlaybackSnapshot preparing;
            preparing.generation = 1;
            preparing.stateVersion = 2;
            preparing.playerAvailable = true;
            preparing.state = PlaybackState::Preparing;
            preparing.current = command.media;
            auto delta = wire::makePlaybackStateMessage(
                mcast::ipc::MessageType::PlaybackStateDelta,
                101, preparing, error);
            if (!delta || !connection->writeMessage(*delta, 2s, error))
                throw std::runtime_error("state delta: " + error);

            PlaybackSnapshot reconnected;
            reconnected.generation = 2;
            reconnected.stateVersion = 1;
            reconnected.playerAvailable = true;
            auto reconnect = wire::makePlaybackStateMessage(
                mcast::ipc::MessageType::PlaybackReconnected,
                102, reconnected, error);
            if (!reconnect
                || !connection->writeMessage(*reconnect, 2s, error))
                throw std::runtime_error("reconnected: " + error);
            reconnect->header.messageId = 103;
            reconnect->header.stateVersion = 2;
            reconnected.stateVersion = 2;
            reconnect = wire::makePlaybackStateMessage(
                mcast::ipc::MessageType::PlaybackReconnected,
                103, reconnected, error);
            if (!reconnect
                || !connection->writeMessage(*reconnect, 2s, error))
                throw std::runtime_error("duplicate reconnect: " + error);

            if (!connection->readMessage(commandMessage, 2s, error))
                throw std::runtime_error("second command: " + error);
            if (!wire::decodePlaybackCommandMessage(
                    commandMessage, commandId, command, error)
                || command.expectedGeneration != 2)
                throw std::runtime_error(
                    "second command generation: " + error);
            result.commandId = commandId;
            result.generation = 2;
            result.stateVersion = 2;
            resultMessage =
                wire::makePlaybackCommandResultMessage(result, error);
            if (!resultMessage
                || !connection->writeMessage(*resultMessage, 2s, error))
                throw std::runtime_error("second result: " + error);

            // A duplicate result must be ignored by the pending-command table.
            if (!resultMessage
                || !connection->writeMessage(*resultMessage, 2s, error))
                throw std::runtime_error("duplicate result: " + error);
            std::this_thread::sleep_for(200ms);

            if (!connection->readMessage(commandMessage, 2s, error))
                throw std::runtime_error("third command: " + error);
            if (!wire::decodePlaybackCommandMessage(
                    commandMessage, commandId, command, error)
                || command.expectedGeneration != 2)
                throw std::runtime_error(
                    "third command generation: " + error);
            std::this_thread::sleep_for(6s);
        } catch (const std::exception &exception) {
            serverError = exception.what();
        }
    });

    PlaybackPipeGateway gateway(
        std::move(client), [](const std::string &) {});
    const auto listenerId = gateway.addEventListener(
        [&](const PlaybackEvent &event) {
            {
                std::scoped_lock lock(events.mutex);
                events.events.push_back(event);
            }
            events.changed.notify_all();
        });

    std::string error;
    PlaybackCommand early;
    early.type = PlaybackCommandType::Play;
    const auto beforeSnapshot = gateway.submit(early);
    check(!beforeSnapshot.queued
              && beforeSnapshot.status
                  == PlaybackSubmissionStatus::WaitingForPlayer,
          "commands wait for the first snapshot");
    check(gateway.requestSnapshot(error), "snapshot request sends");
    check(events.waitFor([](const auto &list) {
        return !list.empty()
            && list.back().kind == PlaybackEventKind::SnapshotChanged;
    }), "snapshot event arrives");
    check(gateway.snapshot().generation == 1, "initial generation cached");

    PlaybackCommand open;
    open.type = PlaybackCommandType::Open;
    open.media.uri = "http://example.test/movie.mp4";
    const auto first = gateway.submit(open);
    check(first.queued && first.commandId != 0, "first command queues");
    check(events.waitFor([](const auto &list) {
        return std::any_of(list.begin(), list.end(), [](const auto &event) {
            return event.kind == PlaybackEventKind::CommandCompleted;
        });
    }), "command result arrives");
    check(events.waitFor([](const auto &list) {
        return std::any_of(list.begin(), list.end(), [](const auto &event) {
            return event.kind == PlaybackEventKind::SnapshotChanged
                && event.snapshot.state == PlaybackState::Preparing;
        });
    }), "state delta event arrives");
    check(gateway.snapshot().state == PlaybackState::Preparing,
          "state delta cached");

    check(events.waitFor([](const auto &list) {
        return !list.empty()
            && list.back().kind == PlaybackEventKind::Reconnected;
    }), "reconnected event arrives");
    check(gateway.snapshot().generation == 2, "generation advances");
    {
        std::scoped_lock lock(events.mutex);
        check(std::count_if(
                  events.events.begin(), events.events.end(),
                  [](const auto &event) {
                      return event.kind == PlaybackEventKind::Reconnected;
                  }) == 1,
              "duplicate reconnected event is ignored");
    }

    PlaybackCommand play;
    play.type = PlaybackCommandType::Play;
    const auto second = gateway.submit(play);
    check(second.queued && second.generation == 2,
          "new command binds current generation");
    check(events.waitFor([](const auto &list) {
        return std::count_if(
            list.begin(), list.end(), [](const auto &event) {
                return event.kind == PlaybackEventKind::CommandCompleted;
            }) >= 2;
    }), "second command result arrives");
    std::this_thread::sleep_for(250ms);
    {
        std::scoped_lock lock(events.mutex);
        check(std::count_if(
                  events.events.begin(), events.events.end(),
                  [](const auto &event) {
                      return event.kind
                          == PlaybackEventKind::CommandCompleted;
                  }) == 2,
              "duplicate command result is ignored");
    }

    std::atomic_bool callbackStarted = false;
    std::atomic_bool releaseCallback = false;
    std::atomic_bool removeReturned = false;
    const auto blockingListener = gateway.addEventListener(
        [&](const PlaybackEvent &event) {
            if (event.kind == PlaybackEventKind::CommandCompleted
                && event.outcome == PlaybackOutcomeStatus::TimedOut) {
                callbackStarted = true;
                events.changed.notify_all();
                while (!releaseCallback)
                    std::this_thread::sleep_for(10ms);
            }
        });
    PlaybackCommand third;
    third.type = PlaybackCommandType::Pause;
    check(gateway.submit(third).queued, "third command queues");
    check(events.waitFor([&](const auto &) {
        return callbackStarted.load();
    }, 7s), "timed-out command completion arrives");
    std::jthread remover([&] {
        gateway.removeEventListener(blockingListener);
        removeReturned = true;
    });
    std::this_thread::sleep_for(100ms);
    check(!removeReturned, "listener removal waits for active callback");
    releaseCallback = true;
    remover.join();
    check(removeReturned, "listener removal finishes after callback");
    PlaybackCommand disconnectedCommand;
    disconnectedCommand.type = PlaybackCommandType::Stop;
    check(gateway.submit(disconnectedCommand).queued,
          "command can be pending when pipe disconnects");

    server.join();
    check(serverError.empty(), "server completed without errors");
    check(events.waitFor([](const auto &list) {
        return std::any_of(list.begin(), list.end(), [](const auto &event) {
            return event.kind == PlaybackEventKind::CommandCompleted
                && event.outcome == PlaybackOutcomeStatus::Rejected
                && event.error == "playback pipe disconnected";
        });
    }), "disconnect rejects pending commands");
    check(events.waitFor([](const auto &list) {
        return !list.empty()
            && list.back().kind == PlaybackEventKind::PlayerLost;
    }), "pipe close reports player lost");
    check(!gateway.snapshot().playerAvailable,
          "disconnected snapshot marks player unavailable");
    gateway.removeEventListener(listenerId);

    std::cout << "Playback pipe gateway tests passed\n";
    return 0;
}
