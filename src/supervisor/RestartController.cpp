// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "supervisor/RestartController.h"

#include <algorithm>
#include <chrono>

namespace mcast::supervisor {

RestartController::RestartController(RestartPolicy policy)
    : m_policy()
{
    constexpr auto minDelay = std::chrono::milliseconds(1);
    constexpr auto maxDelay = std::chrono::milliseconds(60 * 60 * 1000);
    constexpr auto minStable = std::chrono::seconds(1);
    constexpr auto maxStable = std::chrono::seconds(24 * 60 * 60);
    m_policy.maxAttempts = std::min<std::size_t>(policy.maxAttempts, 64);
    m_policy.initialDelay = std::clamp(
        policy.initialDelay, minDelay, maxDelay);
    m_policy.maxDelay = std::clamp(
        policy.maxDelay, m_policy.initialDelay, maxDelay);
    m_policy.stableReset = std::clamp(
        policy.stableReset, minStable, maxStable);
}

void RestartController::markStarted(
    std::chrono::steady_clock::time_point now)
{
    ++m_generation;
    m_startedAt = now;
}

std::optional<RestartPlan> RestartController::markExited(
    std::chrono::steady_clock::time_point now)
{
    if (m_startedAt && now - *m_startedAt >= m_policy.stableReset)
        m_attempts = 0;
    m_startedAt.reset();

    if (m_attempts >= m_policy.maxAttempts)
        return std::nullopt;

    ++m_attempts;
    ++m_generation;
    std::chrono::milliseconds delay = m_policy.initialDelay;
    for (std::size_t index = 1; index < m_attempts; ++index) {
        if (delay >= m_policy.maxDelay / 2) {
            delay = m_policy.maxDelay;
            break;
        }
        delay *= 2;
    }
    delay = std::min(delay, m_policy.maxDelay);
    return RestartPlan{ m_attempts, delay, m_generation };
}

void RestartController::cancelPendingRestart()
{
    ++m_generation;
}

bool RestartController::isCurrent(std::uint64_t generation) const noexcept
{
    return generation == m_generation;
}

void RestartController::reset()
{
    ++m_generation;
    m_attempts = 0;
    m_startedAt.reset();
}

} // namespace mcast::supervisor
