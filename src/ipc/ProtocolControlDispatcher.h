// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace mcast::ipc {

struct UiActivationOutcome {
    bool started = false;
    std::optional<PeerIdentityGrant> grant;
};

// Protocol 侧 ControlPipe 的唯一 reader。接管 PipeConnection 后，除本类外
// 任何组件都不能再调用 readMessage()。所有出站消息也共用同一个消息 ID 序列。
class ProtocolControlDispatcher
{
public:
    explicit ProtocolControlDispatcher(
        std::unique_ptr<PipeConnection> connection,
        std::uint64_t firstMessageId = 1);
    ~ProtocolControlDispatcher();

    ProtocolControlDispatcher(const ProtocolControlDispatcher &) = delete;
    ProtocolControlDispatcher &operator=(
        const ProtocolControlDispatcher &) = delete;

    bool start(std::string &error);
    void stop() noexcept;
    bool connected() const noexcept { return m_connected.load(); }

    bool sendHeartbeat(std::string &error);
    bool sendUiOpenReady(std::string &error);
    bool requestPlaybackReconnect(std::string &error);
    bool waitForUiOpenRequest(
        std::chrono::milliseconds timeout, std::string &error);
    bool waitForTermination(
        std::chrono::milliseconds timeout, std::string &error);

    bool requestUiActivation(
        UiActivationOutcome &outcome,
        std::chrono::milliseconds timeout, std::string &error);
    bool requestCastConfirmation(
        const CastConfirmationRequestInfo &request,
        std::chrono::milliseconds timeout, bool &accepted,
        std::string &error);
    bool cancelCastConfirmation() noexcept;
    void clearCastConfirmationCancel() noexcept;

private:
    enum class ActivationPhase {
        AwaitingDecision,
        AwaitingGrant,
        Done,
    };

    struct ActivationWait {
        std::uint64_t requestMessageId = 0;
        std::uint64_t sessionId = 1;
        std::uint64_t expectedActivationId = 0;
        ActivationPhase phase = ActivationPhase::AwaitingDecision;
        bool started = false;
        std::optional<PeerIdentityGrant> grant;
    };

    struct ConfirmationWait {
        std::uint64_t requestMessageId = 0;
        cast::ProtocolKind protocol = cast::ProtocolKind::None;
        std::uint64_t claimId = 0;
        bool done = false;
        bool accepted = false;
        bool cancelled = false;
    };

    void readLoop(std::stop_token stop);
    bool dispatch(const Message &message, std::string &error);
    bool sendMessage(Message message, std::string &error);
    std::uint64_t nextMessageId() noexcept;
    void terminate(std::string error) noexcept;
    void sendCastConfirmationCancel(
        std::uint64_t requestMessageId) noexcept;

    std::unique_ptr<PipeConnection> m_connection;
    mutable std::mutex m_mutex;
    std::mutex m_confirmationTransportMutex;
    std::condition_variable m_condition;
    std::jthread m_reader;
    std::optional<ActivationWait> m_activation;
    std::optional<ConfirmationWait> m_confirmation;
    bool m_confirmationCancelRequested = false;
    std::string m_terminalError;
    std::uint64_t m_openRequestCount = 0;
    std::atomic_uint64_t m_nextMessageId{1};
    std::atomic_bool m_connected{true};
    bool m_started = false;
    bool m_stopping = false;
};

} // namespace mcast::ipc
