// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/PlanningPlaybackGateway.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
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

class FakeGateway final : public PlaybackGateway
{
public:
    explicit FakeGateway(bool available = true)
        : m_available(available)
    {
    }

    PlaybackSnapshot snapshot() const override
    {
        PlaybackSnapshot value;
        value.generation = 7;
        value.stateVersion = 3;
        value.playerAvailable = m_available.load();
        return value;
    }

    PlaybackCommandSubmission submit(
        const PlaybackCommand &command) override
    {
        std::uint64_t commandId = 0;
        {
            std::scoped_lock lock(m_mutex);
            commandId = m_nextCommandId++;
            m_submitted.push_back(command);
        }
        m_changed.notify_all();
        if (!m_available)
            return {
                false, PlaybackSubmissionStatus::WaitingForPlayer, 0, 7,
                "playback UI is not ready", 3};
        return {
            true, PlaybackSubmissionStatus::Queued, commandId, 7, {}, 3};
    }

    void setAvailable(bool available)
    {
        m_available = available;
    }

    std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) override
    {
        std::scoped_lock lock(m_mutex);
        m_listener = std::move(listener);
        return 1;
    }

    void removeEventListener(std::uint64_t) override
    {
        std::scoped_lock lock(m_mutex);
        m_listener = {};
    }

    bool waitForSubmissionCount(
        std::size_t count,
        std::chrono::milliseconds timeout = 3s)
    {
        std::unique_lock lock(m_mutex);
        return m_changed.wait_for(lock, timeout, [&] {
            return m_submitted.size() >= count;
        });
    }

    PlaybackCommand submitted(std::size_t index) const
    {
        std::scoped_lock lock(m_mutex);
        return m_submitted.at(index);
    }

    std::size_t submissionCount() const
    {
        std::scoped_lock lock(m_mutex);
        return m_submitted.size();
    }

    void complete(std::uint64_t commandId)
    {
        std::function<void(const PlaybackEvent &)> listener;
        {
            std::scoped_lock lock(m_mutex);
            listener = m_listener;
        }
        check(static_cast<bool>(listener), "upstream listener is installed");
        PlaybackEvent event;
        event.kind = PlaybackEventKind::CommandCompleted;
        event.commandId = commandId;
        event.outcome = PlaybackOutcomeStatus::Applied;
        event.snapshot = snapshot();
        listener(event);
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<PlaybackCommand> m_submitted;
    std::function<void(const PlaybackEvent &)> m_listener;
    std::uint64_t m_nextCommandId = 101;
    std::atomic_bool m_available;
};

struct PlannerGate {
    std::mutex mutex;
    std::condition_variable changed;
    bool released = false;

    void release()
    {
        {
            std::scoped_lock lock(mutex);
            released = true;
        }
        changed.notify_all();
    }

    void wait()
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return released; });
    }
};

struct EventSink {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<PlaybackEvent> events;

    std::uint64_t add(PlaybackGateway &gateway)
    {
        return gateway.addEventListener([this](const PlaybackEvent &event) {
            {
                std::scoped_lock lock(mutex);
                events.push_back(event);
            }
            changed.notify_all();
        });
    }

    bool waitFor(
        const std::function<bool(const PlaybackEvent &)> &match,
        std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, timeout, [&] {
            return std::any_of(events.begin(), events.end(), match);
        });
    }
};

} // namespace

int main()
{
    {
        FakeGateway upstream;
        PlannerGate gate;
        PlanningPlaybackGateway planning(
            upstream,
            [&gate](const MediaRequest &request, std::string &) {
                gate.wait();
                MediaRequest planned = request;
                planned.kind = MediaKind::Video;
                planned.route = MediaRoute::ImageSequence;
                planned.imageSequenceFrames = {
                    {"http://media.test/frame.png", 1.0},
                };
                return planned;
            });
        EventSink events;
        events.add(planning);

        PlaybackCommand open;
        open.type = PlaybackCommandType::Open;
        open.expectedGeneration = 7;
        open.media.uri = "http://media.test/show.m3u8";
        const PlaybackCommandSubmission openSubmission =
            planning.submit(open);
        check(openSubmission.queued
                  && openSubmission.commandId != 0,
              "planning submission is acknowledged immediately");

        PlaybackCommand volume;
        volume.type = PlaybackCommandType::SetVolume;
        volume.expectedGeneration = 7;
        volume.intValue = 30;
        const PlaybackCommandSubmission volumeSubmission =
            planning.submit(volume);
        check(volumeSubmission.queued
                  && volumeSubmission.commandId
                      != openSubmission.commandId,
              "pass-through commands receive a separate local ID");
        check(upstream.waitForSubmissionCount(1),
              "pass-through command reaches upstream");
        upstream.complete(101);
        check(events.waitFor([&](const PlaybackEvent &event) {
            return event.commandId == volumeSubmission.commandId;
        }), "pass-through completion keeps its own local ID");
        check(!events.waitFor([&](const PlaybackEvent &event) {
            return event.commandId == openSubmission.commandId;
        }, 100ms), "pass-through completion cannot complete the planning Open");

        gate.release();
        check(upstream.waitForSubmissionCount(2),
              "planned command reaches upstream");
        const PlaybackCommand forwarded = upstream.submitted(1);
        check(forwarded.media.route == MediaRoute::ImageSequence
                  && forwarded.media.imageSequenceFrames.size() == 1,
              "upstream receives the neutral compatibility plan");
        upstream.complete(102);
        check(events.waitFor([&](const PlaybackEvent &event) {
            return event.commandId == openSubmission.commandId
                && event.outcome == PlaybackOutcomeStatus::Applied;
        }), "planned completion is translated to the local command ID");
    }
    {
        FakeGateway upstream;
        PlannerGate gate;
        PlanningPlaybackGateway planning(
            upstream,
            [&gate](const MediaRequest &request, std::string &) {
                gate.wait();
                return request;
            });
        EventSink events;
        events.add(planning);

        PlaybackCommand open;
        open.type = PlaybackCommandType::Open;
        open.expectedGeneration = 7;
        open.media.uri = "http://media.test/show.m3u8";
        const auto openSubmission = planning.submit(open);

        PlaybackCommand end;
        end.type = PlaybackCommandType::EndSession;
        end.expectedGeneration = 7;
        check(planning.submit(end).queued,
              "session end passes through while planning");
        gate.release();
        check(events.waitFor([&](const PlaybackEvent &event) {
            return event.commandId == openSubmission.commandId
                && event.outcome == PlaybackOutcomeStatus::Rejected;
        }), "a new session boundary rejects the stale plan");
        check(upstream.submissionCount() == 1,
              "a cancelled plan never reaches upstream");
    }
    {
        FakeGateway upstream;
        PlannerGate gate;
        PlanningPlaybackGateway planning(
            upstream,
            [&gate](const MediaRequest &request, std::string &) {
                gate.wait();
                return request;
            },
            50ms);
        EventSink events;
        events.add(planning);

        PlaybackCommand open;
        open.type = PlaybackCommandType::Open;
        open.expectedGeneration = 7;
        open.media.uri = "http://media.test/slow.m3u8";
        const auto submission = planning.submit(open);
        std::this_thread::sleep_for(100ms);
        gate.release();
        check(events.waitFor([&](const PlaybackEvent &event) {
            return event.commandId == submission.commandId
                && event.outcome == PlaybackOutcomeStatus::Rejected
                && event.error.find("timed out") != std::string::npos;
        }), "an expired plan is rejected after the planner returns");
        check(upstream.submissionCount() == 0,
              "an expired plan never reaches upstream");
    }
    {
        FakeGateway upstream(false);
        std::atomic_int planningCalls = 0;
        PlanningPlaybackGateway planning(
            upstream,
            [&planningCalls](const MediaRequest &request, std::string &) {
                ++planningCalls;
                return request;
            });

        PlaybackCommand open;
        open.type = PlaybackCommandType::Open;
        open.expectedGeneration = 7;
        open.media.uri = "http://media.test/cold-start.m3u8";
        const auto waiting = planning.submit(open);
        check(!waiting.queued
                  && waiting.status
                      == PlaybackSubmissionStatus::WaitingForPlayer,
              "planning preserves the upstream waiting state");
        check(upstream.waitForSubmissionCount(1),
              "a not-ready Open reaches upstream without planning");
        check(planningCalls.load() == 0,
              "no compatibility plan starts before the UI is ready");

        upstream.setAvailable(true);
        const auto ready = planning.submit(open);
        check(ready.queued && ready.commandId != 0,
              "the same Open can be planned after readiness");
        check(upstream.waitForSubmissionCount(2),
              "the ready Open reaches upstream");
        check(planningCalls.load() == 1,
              "the ready Open starts exactly one compatibility plan");
    }

    std::cout << "Planning playback gateway tests passed\n";
    return 0;
}
