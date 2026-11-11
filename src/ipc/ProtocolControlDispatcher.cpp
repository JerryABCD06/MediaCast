// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/ProtocolControlDispatcher.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace mcast::ipc {
using namespace std::chrono_literals;
namespace {

constexpr std::chrono::milliseconds kWriteTimeout{2000};
constexpr std::chrono::milliseconds kReadTimeout{24h};

} // namespace

ProtocolControlDispatcher::ProtocolControlDispatcher(
    std::unique_ptr<PipeConnection> connection,
    std::uint64_t firstMessageId)
    : m_connection(std::move(connection))
    , m_nextMessageId(firstMessageId == 0 ? 1 : firstMessageId)
{
    if (!m_connection || !m_connection->valid()) {
        m_connected = false;
        m_terminalError = "control dispatcher requires a valid connection";
    }
}

ProtocolControlDispatcher::~ProtocolControlDispatcher()
{
    stop();
}

bool ProtocolControlDispatcher::start(std::string &error)
{
    std::lock_guard lock(m_mutex);
    error.clear();
    if (!m_connected.load()) {
        error = m_terminalError.empty()
            ? "control dispatcher connection is closed"
            : m_terminalError;
        return false;
    }
    if (m_started)
        return true;
    if (!m_connection || !m_connection->valid()) {
        error = "control dispatcher connection is invalid";
        return false;
    }
    m_started = true;
    m_reader = std::jthread(
        [this](std::stop_token stop) { readLoop(std::move(stop)); });
    return true;
}

void ProtocolControlDispatcher::stop() noexcept
{
    {
        std::lock_guard lock(m_mutex);
        if (m_stopping)
            return;
        m_stopping = true;
    }
    if (m_connection)
        m_connection->close();
    if (m_reader.joinable())
        m_reader.request_stop();
    if (m_reader.joinable())
        m_reader.join();
    terminate("control dispatcher stopped");
}

bool ProtocolControlDispatcher::sendHeartbeat(std::string &error)
{
    Message message;
    message.header.type = MessageType::Heartbeat;
    message.header.source = ProcessRole::Protocol;
    message.header.destination = ProcessRole::Shell;
    message.header.messageId = nextMessageId();
    message.header.sessionId = 1;
    return sendMessage(std::move(message), error);
}

bool ProtocolControlDispatcher::sendUiOpenReady(std::string &error)
{
    Message message;
    message.header.type = MessageType::UiOpenReady;
    message.header.source = ProcessRole::Protocol;
    message.header.destination = ProcessRole::Shell;
    message.header.messageId = nextMessageId();
    message.header.sessionId = 1;
    return sendMessage(std::move(message), error);
}

bool ProtocolControlDispatcher::waitForUiOpenRequest(
    std::chrono::milliseconds timeout, std::string &error)
{
    error.clear();
    std::unique_lock lock(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_openRequestCount == 0 && !m_stopping
           && m_connected.load()) {
        if (m_condition.wait_until(lock, deadline)
            == std::cv_status::timeout) {
            error = "UiOpenRequest wait timed out";
            return false;
        }
    }
    if (m_openRequestCount != 0) {
        --m_openRequestCount;
        return true;
    }
    error = m_terminalError.empty()
        ? "control connection is closed"
        : m_terminalError;
    return false;
}

bool ProtocolControlDispatcher::waitForTermination(
    std::chrono::milliseconds timeout, std::string &error)
{
    error.clear();
    std::unique_lock lock(m_mutex);
    if (m_connected.load())
        m_condition.wait_for(
            lock, timeout,
            [this] { return !m_connected.load(); });
    if (!m_connected.load()) {
        error = m_terminalError;
        return true;
    }
    error = "control dispatcher termination wait timed out";
    return false;
}

bool ProtocolControlDispatcher::requestUiActivation(
    UiActivationOutcome &outcome,
    std::chrono::milliseconds timeout, std::string &error)
{
    error.clear();
    outcome = {};
    const std::uint64_t messageId = nextMessageId();
    {
        std::lock_guard lock(m_mutex);
        if (m_activation) {
            error = "UI activation is already pending";
            return false;
        }
        if (m_stopping || !m_connected.load()) {
            error = m_terminalError.empty()
                ? "control connection is closed"
                : m_terminalError;
            return false;
        }
        m_activation = ActivationWait{messageId};
    }

    Message request;
    request.header.type = MessageType::UiActivateRequest;
    request.header.source = ProcessRole::Protocol;
    request.header.destination = ProcessRole::Shell;
    request.header.messageId = messageId;
    request.header.sessionId = 1;
    if (!sendMessage(std::move(request), error))
        return false;

    std::unique_lock lock(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_activation
           && m_activation->phase != ActivationPhase::Done
           && !m_stopping && m_connected.load()) {
        if (m_condition.wait_until(lock, deadline)
            == std::cv_status::timeout) {
            m_activation.reset();
            error = "UI activation timed out";
            return false;
        }
    }
    if (!m_activation
        || m_activation->phase != ActivationPhase::Done) {
        error = m_terminalError.empty()
            ? "control connection closed during UI activation"
            : m_terminalError;
        return false;
    }
    outcome.started = m_activation->started;
    outcome.grant = std::move(m_activation->grant);
    m_activation.reset();
    return true;
}

bool ProtocolControlDispatcher::requestCastConfirmation(
    const CastConfirmationRequestInfo &request,
    std::chrono::milliseconds timeout, bool &accepted,
    std::string &error)
{
    error.clear();
    accepted = false;
    const std::uint64_t messageId = nextMessageId();
    auto message = makeCastConfirmationRequestMessage(
        messageId, request, error);
    if (!message)
        return false;

    {
        std::lock_guard lock(m_mutex);
        if (m_confirmation) {
            error = "cast confirmation is already pending";
            return false;
        }
        if (m_stopping || !m_connected.load()) {
            error = m_terminalError.empty()
                ? "control connection is closed"
                : m_terminalError;
            return false;
        }
        m_confirmation = ConfirmationWait{
            messageId, request.protocol, request.claimId};
    }
    if (!sendMessage(std::move(*message), error))
        return false;

    std::unique_lock lock(m_mutex);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_confirmation && !m_confirmation->done
           && !m_stopping && m_connected.load()) {
        if (m_condition.wait_until(lock, deadline)
            == std::cv_status::timeout) {
            m_confirmation.reset();
            error = "cast confirmation timed out";
            return false;
        }
    }
    if (!m_confirmation || !m_confirmation->done) {
        error = m_terminalError.empty()
            ? "control connection closed during cast confirmation"
            : m_terminalError;
        return false;
    }
    accepted = m_confirmation->accepted;
    m_confirmation.reset();
    return true;
}

void ProtocolControlDispatcher::readLoop(std::stop_token stop)
{
    while (!stop.stop_requested() && m_connected.load()) {
        Message message;
        std::string error;
        if (!m_connection->readMessage(
                message, kReadTimeout, error)) {
            if (!stop.stop_requested())
                terminate(
                    "control pipe read failed: " + error);
            return;
        }
        if (stop.stop_requested())
            return;
        if (!dispatch(message, error)) {
            terminate(
                error.empty()
                    ? "control pipe message dispatch failed"
                    : error);
            return;
        }
    }
}

bool ProtocolControlDispatcher::dispatch(
    const Message &message, std::string &error)
{
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        return false;
    }

    switch (message.header.type) {
    case MessageType::UiOpenRequest: {
        {
            std::lock_guard lock(m_mutex);
            ++m_openRequestCount;
        }
        m_condition.notify_all();
        return true;
    }
    case MessageType::UiActivateDecision: {
        std::lock_guard lock(m_mutex);
        if (!m_activation
            || message.header.correlationId
                != m_activation->requestMessageId) {
            return true;
        }
        if (m_activation->phase != ActivationPhase::AwaitingDecision)
            return true;
        if (message.header.sessionId != m_activation->sessionId) {
            error = "UI activation decision session mismatch";
            return false;
        }
        const auto decision = decodeUiActivateDecisionMessage(
            message, m_activation->requestMessageId, error);
        if (!decision)
            return false;
        if (!decision->started) {
            m_activation->phase = ActivationPhase::Done;
        } else {
            m_activation->started = true;
            m_activation->expectedActivationId =
                decision->activationId;
            m_activation->phase = ActivationPhase::AwaitingGrant;
        }
        m_condition.notify_all();
        return true;
    }
    case MessageType::PeerIdentityGrant: {
        const auto grant = decodePeerIdentityGrantMessage(
            message, error);
        if (!grant)
            return false;
        std::lock_guard lock(m_mutex);
        if (!m_activation
            || m_activation->phase != ActivationPhase::AwaitingGrant
            || m_activation->expectedActivationId == 0
            || grant->activationId
                != m_activation->expectedActivationId
            || grant->role != ProcessRole::Ui) {
            return true;
        }
        m_activation->grant = *grant;
        m_activation->phase = ActivationPhase::Done;
        m_condition.notify_all();
        return true;
    }
    case MessageType::CastConfirmationDecision: {
        std::lock_guard lock(m_mutex);
        if (!m_confirmation
            || message.header.correlationId
                != m_confirmation->requestMessageId) {
            return true;
        }
        if (m_confirmation->done)
            return true;
        const auto decision = decodeCastConfirmationDecisionMessage(
            message, m_confirmation->requestMessageId, error);
        if (!decision)
            return false;
        if (decision->protocol != m_confirmation->protocol
            || decision->claimId != m_confirmation->claimId) {
            error = "cast confirmation decision does not match its request";
            return false;
        }
        m_confirmation->accepted = decision->accepted;
        m_confirmation->done = true;
        m_condition.notify_all();
        return true;
    }
    case MessageType::HeartbeatAck:
        return true;
    default:
        error = "unexpected message type on Protocol ControlPipe";
        return false;
    }
}

bool ProtocolControlDispatcher::sendMessage(
    Message message, std::string &error)
{
    if (!m_connected.load() || !m_connection) {
        error = m_terminalError.empty()
            ? "control connection is closed"
            : m_terminalError;
        return false;
    }
    if (!m_connection->writeMessage(message, kWriteTimeout, error)) {
        terminate("control pipe write failed: " + error);
        return false;
    }
    return true;
}

std::uint64_t ProtocolControlDispatcher::nextMessageId() noexcept
{
    std::uint64_t id = m_nextMessageId.fetch_add(1);
    if (id == 0)
        id = m_nextMessageId.fetch_add(1);
    return id;
}

void ProtocolControlDispatcher::terminate(std::string error) noexcept
{
    {
        std::lock_guard lock(m_mutex);
        if (!m_connected.exchange(false))
            return;
        m_terminalError = std::move(error);
        m_activation.reset();
        m_confirmation.reset();
    }
    if (m_connection)
        m_connection->close();
    m_condition.notify_all();
}

} // namespace mcast::ipc
