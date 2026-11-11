// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace mcast::dlna::runtime {

class DlnaClaimConfirmationPort
{
public:
    virtual ~DlnaClaimConfirmationPort() = default;

    // May block until the user, Shell timeout or transport failure produces a
    // decision. The worker consumes prompt.remaining as the maximum wait.
    virtual bool waitForDecision(
        const cast::ConfirmationPrompt &prompt,
        bool &accepted, std::string &error) = 0;
    virtual void cancel() noexcept {}
};

// Serializes DLNA confirmation traffic away from the HTTP owner thread.
// CastClaimCoordinator allows at most one pending claim, so the worker keeps
// only one prompt and never creates parallel confirmation windows.
class DlnaClaimConfirmationWorker
{
public:
    using Clock = std::function<cast::CastTime()>;

    DlnaClaimConfirmationWorker(
        claim::DlnaClaimHttpHandler &handler,
        DlnaClaimConfirmationPort &port,
        Clock now = {},
        std::chrono::milliseconds tickInterval =
            std::chrono::milliseconds{100});
    ~DlnaClaimConfirmationWorker();

    DlnaClaimConfirmationWorker(
        const DlnaClaimConfirmationWorker &) = delete;
    DlnaClaimConfirmationWorker &operator=(
        const DlnaClaimConfirmationWorker &) = delete;

    void requestConfirmation(cast::ConfirmationPrompt prompt);
    void controlLost(cast::CastTime now);
    void stop() noexcept;

private:
    void run(std::stop_token stop);
    void tickLoop(std::stop_token stop);

    claim::DlnaClaimHttpHandler &m_handler;
    DlnaClaimConfirmationPort &m_port;
    Clock m_now;
    mutable std::mutex m_mutex;
    std::condition_variable m_ready;
    std::optional<cast::ConfirmationPrompt> m_prompt;
    bool m_stopping = false;
    std::chrono::milliseconds m_tickInterval;
    std::jthread m_thread;
    std::jthread m_tickThread;
};

} // namespace mcast::dlna::runtime
