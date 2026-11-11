// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/runtime/DlnaClaimConfirmationWorker.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

namespace mcast::dlna::runtime {
namespace {

cast::CastTime steadyNow()
{
    return std::chrono::duration_cast<cast::CastTime>(
        std::chrono::steady_clock::now().time_since_epoch());
}

} // namespace

DlnaClaimConfirmationWorker::DlnaClaimConfirmationWorker(
    claim::DlnaClaimHttpHandler &handler,
    DlnaClaimConfirmationPort &port, Clock now,
    std::chrono::milliseconds tickInterval)
    : m_handler(handler)
    , m_port(port)
    , m_now(now ? std::move(now) : steadyNow)
    , m_tickInterval(
          std::max(tickInterval, std::chrono::milliseconds{10}))
    , m_thread([this](std::stop_token stop) { run(stop); })
    , m_tickThread(
          [this](std::stop_token stop) { tickLoop(stop); })
{
    m_port.reset();
}

DlnaClaimConfirmationWorker::~DlnaClaimConfirmationWorker()
{
    stop();
}

void DlnaClaimConfirmationWorker::requestConfirmation(
    cast::ConfirmationPrompt prompt)
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        if (m_prompt
            && m_prompt->key.protocol == prompt.key.protocol
            && m_prompt->key.id == prompt.key.id) {
            m_prompt->deadline = prompt.deadline;
            m_prompt->remaining = prompt.remaining;
            return;
        }
        m_prompt = std::move(prompt);
    }
    m_ready.notify_one();
}

void DlnaClaimConfirmationWorker::controlLost(cast::CastTime now)
{
    m_port.cancel();
    {
        std::lock_guard lock(m_mutex);
        m_prompt.reset();
    }
    m_handler.controlLost(now);
}

void DlnaClaimConfirmationWorker::stop() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        m_stopping = true;
        m_prompt.reset();
    }
    m_ready.notify_all();
    if (m_tickThread.joinable())
        m_tickThread.request_stop();
    if (m_thread.joinable()) {
        m_thread.request_stop();
    }
    m_port.cancel();
    if (m_tickThread.joinable())
        m_tickThread.join();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void DlnaClaimConfirmationWorker::run(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        cast::ConfirmationPrompt prompt;
        cast::CastTime now;
        {
            std::unique_lock lock(m_mutex);
            m_ready.wait(lock, [&] {
                return stop.stop_requested() || m_stopping
                    || m_prompt.has_value();
            });
            if (stop.stop_requested() || m_stopping)
                return;
            prompt = std::move(*m_prompt);
            m_prompt.reset();
            now = m_now();
        }

        prompt.remaining = std::max(
            cast::CastTime::zero(), prompt.deadline - now);
        bool accepted = false;
        if (prompt.remaining > cast::CastTime::zero()) {
            std::string error;
            try {
                if (!m_port.waitForDecision(prompt, accepted, error))
                    accepted = false;
            } catch (...) {
                accepted = false;
            }
        }

        if (stop.stop_requested())
            return;
        m_handler.confirmationDecision(
            prompt.key, accepted, m_now());
    }
}

void DlnaClaimConfirmationWorker::tickLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        const auto slices = std::max<std::int64_t>(
            1, m_tickInterval.count() / 10);
        for (std::int64_t slice = 0;
             slice < slices && !stop.stop_requested(); ++slice) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds{10});
        }
        if (!stop.stop_requested())
            m_handler.tick(m_now());
    }
}

} // namespace mcast::dlna::runtime
