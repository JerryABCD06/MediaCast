// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/PlanningPlaybackGateway.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace mcast::playback {
namespace {

thread_local std::uint64_t t_activeListenerId = 0;
constexpr int kPlanningWorkerCount = 3;
constexpr std::size_t kMaxPlanningJobs = 32;

bool cancelsPlanning(const PlaybackCommand &command)
{
    return command.type == PlaybackCommandType::Open
        || command.type == PlaybackCommandType::EndSession
        || (command.type == PlaybackCommandType::SetPeerConnected
            && !command.boolValue)
        || (command.type == PlaybackCommandType::SetCastSession
            && !command.castSession.connected);
}

} // namespace

PlanningPlaybackGateway::PlanningPlaybackGateway(
    PlaybackGateway &upstream, Planner planner)
    : PlanningPlaybackGateway(
          upstream, std::move(planner),
          std::chrono::milliseconds{8000})
{
}

PlanningPlaybackGateway::PlanningPlaybackGateway(
    PlaybackGateway &upstream, Planner planner,
    std::chrono::milliseconds planningTimeout)
    : m_upstream(upstream)
    , m_planner(std::move(planner))
    , m_planningTimeout(planningTimeout)
    , m_upstreamListenerId(upstream.addEventListener(
          [this](const PlaybackEvent &event) {
              onUpstreamEvent(event);
          }))
{
    m_workers.reserve(kPlanningWorkerCount);
    for (int index = 0; index < kPlanningWorkerCount; ++index) {
        m_workers.emplace_back(
            [this](std::stop_token stop) { workerLoop(stop); });
    }
}

PlanningPlaybackGateway::~PlanningPlaybackGateway()
{
    for (std::jthread &worker : m_workers)
        worker.request_stop();
    m_jobReady.notify_all();
    for (std::jthread &worker : m_workers) {
        if (worker.joinable())
            worker.join();
    }
    if (m_upstreamListenerId != 0)
        m_upstream.removeEventListener(m_upstreamListenerId);
}

PlaybackSnapshot PlanningPlaybackGateway::snapshot() const
{
    return m_upstream.snapshot();
}

PlaybackCommandSubmission PlanningPlaybackGateway::submit(
    const PlaybackCommand &command)
{
    if (cancelsPlanning(command))
        cancelPlanningForBoundary();

    if (!m_planner || !needsPlanning(command))
        return submitUpstream(command, nextSyntheticCommandId());

    const PlaybackSnapshot current = m_upstream.snapshot();
    if (!current.playerAvailable)
        return submitUpstream(command, nextSyntheticCommandId());

    const bool open = isOpen(command);
    Job job;
    job.command = command;
    job.syntheticCommandId = nextSyntheticCommandId();
    job.open = open;
    job.deadline = std::chrono::steady_clock::now() + m_planningTimeout;
    {
        std::scoped_lock lock(m_jobMutex);
        if (m_jobs.size() >= kMaxPlanningJobs) {
            return {
                false,
                PlaybackSubmissionStatus::Busy,
                0,
                current.generation,
                "too many pending playback plans",
                current.stateVersion,
            };
        }
        if (open) {
            job.epoch = ++m_openEpoch;
            m_pendingOpenCommandId = job.syntheticCommandId;
        } else {
            job.epoch = ++m_nextEpoch;
            m_pendingNextCommandId = job.syntheticCommandId;
        }
        job.planEpoch = m_planEpoch;
        m_jobs.push_back(job);
    }
    m_jobReady.notify_one();

    return {
        true,
        PlaybackSubmissionStatus::Queued,
        job.syntheticCommandId,
        current.generation,
        {},
        current.stateVersion,
    };
}

std::uint64_t PlanningPlaybackGateway::addEventListener(
    std::function<void(const PlaybackEvent &)> listener)
{
    const std::uint64_t id = m_nextListenerId++;
    if (listener) {
        std::scoped_lock lock(m_listenerMutex);
        m_listeners.emplace(
            id, ListenerEntry{std::move(listener), 0, false});
    }
    return id;
}

void PlanningPlaybackGateway::removeEventListener(
    std::uint64_t listenerId)
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

void PlanningPlaybackGateway::workerLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        Job job;
        {
            std::unique_lock lock(m_jobMutex);
            m_jobReady.wait(lock, [&] {
                return stop.stop_requested() || !m_jobs.empty();
            });
            if (stop.stop_requested())
                return;
            job = std::move(m_jobs.front());
            m_jobs.pop_front();
        }

        bool current = false;
        {
            std::scoped_lock lock(m_jobMutex);
            current = job.open
                ? job.epoch == m_openEpoch
                : job.epoch == m_nextEpoch;
            current = current && job.planEpoch == m_planEpoch;
            current = current
                && std::chrono::steady_clock::now() < job.deadline;
            if (!current) {
                auto &pending = job.open
                    ? m_pendingOpenCommandId
                    : m_pendingNextCommandId;
                if (pending && *pending == job.syntheticCommandId)
                    pending.reset();
            }
        }
        if (!current) {
            reject(
                job.syntheticCommandId,
                std::chrono::steady_clock::now() >= job.deadline
                    ? "playback plan timed out"
                    : "playback plan was superseded");
            continue;
        }

        std::string planningError;
        PlaybackCommand planned = job.command;
        try {
            planned.media =
                m_planner(job.command.media, planningError);
        } catch (...) {
            reject(
                job.syntheticCommandId,
                "playback planner threw an exception");
            continue;
        }

        std::unique_lock commitLock(m_planCommitMutex);
        {
            std::scoped_lock lock(m_jobMutex);
            current = job.open
                ? job.epoch == m_openEpoch
                : job.epoch == m_nextEpoch;
            current = current && job.planEpoch == m_planEpoch;
            current = current
                && std::chrono::steady_clock::now() < job.deadline;
            if (current) {
                auto &pending = job.open
                    ? m_pendingOpenCommandId
                    : m_pendingNextCommandId;
                if (pending && *pending == job.syntheticCommandId)
                    pending.reset();
            }
        }
        if (!current) {
            commitLock.unlock();
            reject(
                job.syntheticCommandId,
                std::chrono::steady_clock::now() >= job.deadline
                    ? "playback plan timed out"
                    : "playback plan was superseded");
            continue;
        }

        const PlaybackCommandSubmission submitted =
            submitUpstream(
                planned, job.syntheticCommandId, &commitLock);
        if (commitLock.owns_lock())
            commitLock.unlock();
        if (!submitted.queued) {
            reject(
                job.syntheticCommandId,
                submitted.error.empty()
                    ? "playback planning could not be submitted"
                    : submitted.error);
            continue;
        }
        (void)planningError;
    }
}

void PlanningPlaybackGateway::cancelPlanningForBoundary()
{
    std::vector<std::uint64_t> cancelled;
    {
        std::scoped_lock commitLock(m_planCommitMutex);
        std::scoped_lock jobLock(m_jobMutex);
        ++m_planEpoch;
        ++m_openEpoch;
        ++m_nextEpoch;
        m_pendingOpenCommandId.reset();
        m_pendingNextCommandId.reset();
        for (const Job &job : m_jobs)
            cancelled.push_back(job.syntheticCommandId);
        m_jobs.clear();
    }
    for (const std::uint64_t commandId : cancelled)
        reject(commandId, "playback plan was superseded");
}

PlaybackCommandSubmission PlanningPlaybackGateway::submitUpstream(
    const PlaybackCommand &command,
    std::uint64_t syntheticCommandId,
    std::unique_lock<std::mutex> *planCommitLock)
{
    std::unique_lock submitLock(m_upstreamSubmitMutex);
    {
        std::scoped_lock eventLock(m_upstreamEventMutex);
        m_installingUpstreamSynthetic = syntheticCommandId;
    }

    PlaybackCommandSubmission submitted;
    bool submitFailed = false;
    try {
        submitted = m_upstream.submit(command);
    } catch (...) {
        submitFailed = true;
    }

    std::vector<PlaybackEvent> translatedEvents;
    {
        std::scoped_lock eventLock(m_upstreamEventMutex);
        if (submitted.queued && submitted.commandId != 0) {
            m_upstreamToSynthetic[submitted.commandId] =
                syntheticCommandId;
        }
        for (auto &[commandId, event] : m_earlyUpstreamEvents) {
            const auto link = m_upstreamToSynthetic.find(commandId);
            if (link == m_upstreamToSynthetic.end())
                continue;
            PlaybackEvent translated = std::move(event);
            translated.commandId = link->second;
            translatedEvents.push_back(std::move(translated));
            m_upstreamToSynthetic.erase(link);
        }
        m_earlyUpstreamEvents.clear();
        m_installingUpstreamSynthetic = 0;
    }
    submitLock.unlock();
    if (planCommitLock && planCommitLock->owns_lock())
        planCommitLock->unlock();
    for (const PlaybackEvent &event : translatedEvents)
        notify(event);

    if (submitFailed) {
        const PlaybackSnapshot current = m_upstream.snapshot();
        return {
            false,
            PlaybackSubmissionStatus::PlayerUnavailable,
            0,
            current.generation,
            "playback gateway threw during submission",
            current.stateVersion,
        };
    }
    if (!submitted.queued) {
        submitted.commandId = 0;
        return submitted;
    }
    if (submitted.commandId == 0) {
        const PlaybackSnapshot current = m_upstream.snapshot();
        return {
            false,
            PlaybackSubmissionStatus::InvalidCommand,
            0,
            current.generation,
            "upstream queued command has no ID",
            current.stateVersion,
        };
    }
    submitted.commandId = syntheticCommandId;
    return submitted;
}

void PlanningPlaybackGateway::onUpstreamEvent(
    const PlaybackEvent &event)
{
    if (event.kind != PlaybackEventKind::CommandCompleted) {
        notify(event);
        return;
    }

    std::uint64_t synthetic = 0;
    {
        std::scoped_lock lock(m_upstreamEventMutex);
        if (m_installingUpstreamSynthetic != 0) {
            m_earlyUpstreamEvents[event.commandId] = event;
            return;
        }
        const auto link =
            m_upstreamToSynthetic.find(event.commandId);
        if (link == m_upstreamToSynthetic.end())
            return;
        synthetic = link->second;
        m_upstreamToSynthetic.erase(link);
    }
    PlaybackEvent translated = event;
    translated.commandId = synthetic;
    notify(translated);
}

void PlanningPlaybackGateway::reject(
    std::uint64_t syntheticCommandId, const std::string &reason)
{
    PlaybackEvent event;
    event.kind = PlaybackEventKind::CommandCompleted;
    event.commandId = syntheticCommandId;
    event.outcome = PlaybackOutcomeStatus::Rejected;
    event.error = reason;
    event.snapshot = m_upstream.snapshot();
    notify(event);
}

void PlanningPlaybackGateway::notify(const PlaybackEvent &event)
{
    struct Callback {
        std::uint64_t id = 0;
        std::function<void(const PlaybackEvent &)> callback;
    };
    std::vector<Callback> callbacks;
    {
        std::scoped_lock lock(m_listenerMutex);
        for (auto &[id, entry] : m_listeners) {
            if (entry.removing)
                continue;
            ++entry.activeCallbacks;
            callbacks.push_back({id, entry.callback});
        }
    }
    for (const Callback &callback : callbacks) {
        t_activeListenerId = callback.id;
        try {
            callback.callback(event);
        } catch (...) {
        }
        t_activeListenerId = 0;
        {
            std::scoped_lock lock(m_listenerMutex);
            const auto it = m_listeners.find(callback.id);
            if (it == m_listeners.end())
                continue;
            if (it->second.activeCallbacks > 0)
                --it->second.activeCallbacks;
            if (it->second.removing
                && it->second.activeCallbacks == 0) {
                m_listeners.erase(it);
            }
        }
        m_listenerIdle.notify_all();
    }
}

std::uint64_t PlanningPlaybackGateway::nextSyntheticCommandId()
{
    std::uint64_t id = m_syntheticCommandId.fetch_add(1);
    if (id == 0)
        id = m_syntheticCommandId.fetch_add(1);
    return id;
}

bool PlanningPlaybackGateway::needsPlanning(
    const PlaybackCommand &command)
{
    return (command.type == PlaybackCommandType::Open
            || command.type == PlaybackCommandType::SetNext)
        && !command.media.uri.empty()
        && command.media.route == MediaRoute::Native
        && command.media.imageSequenceFrames.empty();
}

bool PlanningPlaybackGateway::isOpen(
    const PlaybackCommand &command)
{
    return command.type == PlaybackCommandType::Open;
}

} // namespace mcast::playback
