// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mcast::supervisor {

std::wstring quoteWindowsArgument(const std::wstring &argument);

class ChildProcess
{
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess &) = delete;
    ChildProcess &operator=(const ChildProcess &) = delete;
    ChildProcess(ChildProcess &&other) noexcept;
    ChildProcess &operator=(ChildProcess &&other) noexcept;

    bool start(
        const std::filesystem::path &executable,
        const std::wstring &arguments,
        const std::filesystem::path &workingDirectory,
        std::string &error);
    bool assignToJob(void *job, std::string &error);
    bool running() const;
    std::optional<std::uint32_t>
    waitForExit(std::chrono::milliseconds timeout, std::string &error);
    bool terminate(
        std::uint32_t exitCode, std::chrono::milliseconds timeout,
        std::string &error);
    void close();

    std::uint32_t processId() const noexcept { return m_processId; }

private:
    void *m_process = nullptr;
    void *m_thread = nullptr;
    std::uint32_t m_processId = 0;
};

} // namespace mcast::supervisor
