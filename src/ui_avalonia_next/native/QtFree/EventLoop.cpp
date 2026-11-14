// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace mcast::avalonia::qtfree {

EventLoop::~EventLoop()
{
    stop();
}

bool EventLoop::start()
{
    if (m_running.exchange(true))
        return true;
    m_thread = std::jthread(
        [this](std::stop_token stop) { run(stop); });
    return true;
}

void EventLoop::stop()
{
    if (!m_running.exchange(false))
        return;

    m_thread.request_stop();
    m_condition.notify_all();
    if (m_thread.joinable())
        m_thread.join();

    std::deque<Task> discardedTasks;
    {
        const std::scoped_lock lock(m_mutex);
        discardedTasks.swap(m_tasks);
        m_timers.clear();
    }
}

bool EventLoop::post(Task task)
{
    if (!task || !m_running.load())
        return false;
    {
        const std::scoped_lock lock(m_mutex);
        if (!m_running.load())
            return false;
        m_tasks.push_back(std::move(task));
    }
    m_condition.notify_all();
    return true;
}

std::uint64_t EventLoop::scheduleAfter(
    std::chrono::milliseconds delay, Task task)
{
    if (!task || !m_running.load())
        return 0;

    const std::scoped_lock lock(m_mutex);
    if (!m_running.load())
        return 0;
    const std::uint64_t id = m_nextTimerId++;
    const auto deadline =
        Clock::now() + std::max(delay, std::chrono::milliseconds::zero());
    m_timers.emplace(
        deadline, Timer{deadline, id, std::move(task)});
    m_condition.notify_all();
    return id;
}

bool EventLoop::cancelTimer(std::uint64_t id)
{
    if (id == 0)
        return false;

    const std::scoped_lock lock(m_mutex);
    for (auto iterator = m_timers.begin();
         iterator != m_timers.end(); ++iterator) {
        if (iterator->second.id != id)
            continue;
        m_timers.erase(iterator);
        m_condition.notify_all();
        return true;
    }
    return false;
}

void EventLoop::run(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        std::deque<Task> readyTasks;
        std::vector<Timer> readyTimers;
        {
            std::unique_lock lock(m_mutex);
            const auto hasReadyTask = [&] {
                return stop.stop_requested() || !m_tasks.empty()
                    || (!m_timers.empty()
                        && m_timers.begin()->first <= Clock::now());
            };

            if (!hasReadyTask()) {
                if (m_timers.empty()) {
                    m_condition.wait(
                        lock, stop,
                        [&] {
                            return stop.stop_requested()
                                || !m_tasks.empty()
                                || !m_timers.empty();
                        });
                } else {
                    const auto deadline = m_timers.begin()->first;
                    m_condition.wait_until(
                        lock, stop, deadline,
                        [&] {
                            return stop.stop_requested()
                                || !m_tasks.empty()
                                || m_timers.empty()
                                || m_timers.begin()->first
                                    <= Clock::now();
                        });
                }
            }

            if (stop.stop_requested())
                break;

            readyTasks.swap(m_tasks);
            const auto now = Clock::now();
            while (!m_timers.empty()
                   && m_timers.begin()->first <= now) {
                readyTimers.push_back(
                    std::move(m_timers.begin()->second));
                m_timers.erase(m_timers.begin());
            }
        }

        for (auto &task : readyTasks)
            task();
        for (auto &timer : readyTimers)
            timer.task();
    }

    m_running.store(false);
}

} // namespace mcast::avalonia::qtfree

