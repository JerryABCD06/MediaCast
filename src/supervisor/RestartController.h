// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <chrono>
#include <cstddef>
#include <optional>

namespace mcast::supervisor {

struct RestartPolicy {
    std::size_t maxAttempts = 3;
    std::chrono::milliseconds initialDelay{250};
    std::chrono::milliseconds maxDelay{4000};
    std::chrono::seconds stableReset{30};
};

struct RestartPlan {
    std::size_t attempt = 1;
    std::chrono::milliseconds delay{0};
    std::uint64_t generation = 0;
};

class RestartController
{
public:
    explicit RestartController(RestartPolicy policy = {});

    void markStarted(std::chrono::steady_clock::time_point now);
    std::optional<RestartPlan>
    markExited(std::chrono::steady_clock::time_point now);
    void cancelPendingRestart();
    bool isCurrent(std::uint64_t generation) const noexcept;
    void reset();

    std::size_t attemptCount() const noexcept { return m_attempts; }
    std::uint64_t generation() const noexcept { return m_generation; }
    const RestartPolicy &policy() const noexcept { return m_policy; }

private:
    RestartPolicy m_policy;
    std::size_t m_attempts = 0;
    std::uint64_t m_generation = 1;
    std::optional<std::chrono::steady_clock::time_point> m_startedAt;
};

} // namespace mcast::supervisor
