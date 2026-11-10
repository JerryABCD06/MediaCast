// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/IpcProtocol.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace mcast::ipc {

std::wstring currentUserSid();
std::wstring makeControlPipeName(std::wstring_view instance);
std::wstring makePlaybackPipeName(std::wstring_view instance);
std::wstring makeShellCommandPipeName(std::wstring_view instance);
std::wstring makeProtocolCommandPipeName(std::wstring_view instance);

class PipeConnection
{
public:
    enum class Kind {
        Client,
        Server,
    };

    ~PipeConnection();

    PipeConnection(const PipeConnection &) = delete;
    PipeConnection &operator=(const PipeConnection &) = delete;

    bool valid() const noexcept;
    std::uint32_t peerProcessId() const noexcept { return m_peerProcessId; }
    bool readMessage(
        Message &message, std::chrono::milliseconds timeout,
        std::string &error);
    bool writeMessage(
        const Message &message, std::chrono::milliseconds timeout,
        std::string &error);
    void close();

private:
    friend class PipeListener;
    friend class PipeClient;
    PipeConnection(void *handle, Kind kind, std::uint32_t peerProcessId);

    std::atomic<void *> m_handle;
    std::atomic_bool m_closing{ false };
    Kind m_kind = Kind::Client;
    std::uint32_t m_peerProcessId = 0;
    std::shared_mutex m_lifecycleMutex;
    std::mutex m_ioStartMutex;
    std::mutex m_readMutex;
    std::mutex m_writeMutex;
};

class PipeListener
{
public:
    explicit PipeListener(
        std::wstring name, std::uint32_t maxInstances = 4);
    ~PipeListener();

    PipeListener(const PipeListener &) = delete;
    PipeListener &operator=(const PipeListener &) = delete;

    bool valid() const noexcept { return m_valid; }
    const std::wstring &name() const noexcept { return m_name; }
    void stop();
    std::unique_ptr<PipeConnection> accept(
        std::chrono::milliseconds timeout, std::string &error);

private:
    bool ensurePendingInstance(std::string &error);
    void discardPendingInstance();

    std::wstring m_name;
    std::uint32_t m_maxInstances = 4;
    std::atomic<void *> m_pendingHandle{ nullptr };
    bool m_firstInstance = true;
    std::atomic_bool m_valid{ false };
    std::atomic_bool m_stopping{ false };
    void *m_stopEvent = nullptr;
    std::mutex m_stateMutex;
    std::mutex m_acceptMutex;
};

class PipeClient
{
public:
    static std::unique_ptr<PipeConnection> connect(
        std::wstring_view name,
        std::chrono::milliseconds timeout,
        std::string &error);
};

} // namespace mcast::ipc
