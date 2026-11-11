// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mcast::dlna::net {

struct SsdpServiceOptions {
    std::string udn;
    std::uint16_t httpPort = 0;
    std::uint16_t ssdpPort = 1900;
    std::string interfaceAddress;
    bool joinMulticast = true;
    bool advertise = true;
    std::chrono::milliseconds aliveInterval{10000};
};

// Qt-free SSDP responder. It owns one polling thread so stop() can be called
// from either the Protocol event loop or the DLNA session thread.
class SsdpService
{
public:
    using Log = std::function<void(const std::string &)>;

    SsdpService() = default;
    ~SsdpService();

    SsdpService(const SsdpService &) = delete;
    SsdpService &operator=(const SsdpService &) = delete;

    bool start(
        SsdpServiceOptions options, Log log, std::string &error);
    void stop() noexcept;

    bool running() const noexcept { return m_running.load(); }
    std::uint16_t localPort() const noexcept
    {
        return m_localPort.load();
    }
    std::string interfaceAddress() const;
    std::string locationUrl() const;
    void setBroadcasting(bool enabled) noexcept;
    void setAliveInterval(std::chrono::milliseconds interval) noexcept;

private:
    void run(SsdpServiceOptions options, std::stop_token stop);
    void completeStart(bool success, std::string error);
    void log(const std::string &message) const;

    std::mutex m_lifecycleMutex;
    mutable std::mutex m_startMutex;
    std::condition_variable m_startChanged;
    std::jthread m_thread;
    Log m_log;
    bool m_startComplete = false;
    bool m_startSuccess = false;
    std::string m_startError;
    std::string m_interfaceAddress;
    std::string m_locationUrl;
    std::atomic_bool m_running{false};
    std::atomic_bool m_broadcasting{true};
    std::atomic_bool m_sendAliveRequested{false};
    std::atomic<std::int64_t> m_aliveIntervalMs{10000};
    std::atomic<std::uint16_t> m_localPort{0};
};

} // namespace mcast::dlna::net
