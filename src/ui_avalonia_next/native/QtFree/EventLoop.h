// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <thread>

namespace mcast::avalonia::qtfree {

// Serial task loop owned by the Qt-free Ui host. It replaces the event-loop
// role QCoreApplication previously filled without becoming a playback owner.
class EventLoop
{
public:
    using Clock = std::chrono::steady_clock;
    using Task = std::function<void()>;

    EventLoop() = default;
    ~EventLoop();

    EventLoop(const EventLoop &) = delete;
    EventLoop &operator=(const EventLoop &) = delete;

    bool start();
    void stop();
    bool isRunning() const noexcept { return m_running.load(); }

    bool post(Task task);
    std::uint64_t scheduleAfter(
        std::chrono::milliseconds delay, Task task);
    bool cancelTimer(std::uint64_t id);

private:
    struct Timer
    {
        Clock::time_point deadline;
        std::uint64_t id = 0;
        Task task;
    };

    void run(std::stop_token stop);

    std::jthread m_thread;
    std::mutex m_mutex;
    std::condition_variable_any m_condition;
    std::deque<Task> m_tasks;
    std::multimap<Clock::time_point, Timer> m_timers;
    std::uint64_t m_nextTimerId = 1;
    std::atomic_bool m_running{false};
};

} // namespace mcast::avalonia::qtfree

