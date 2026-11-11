// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "skeleton/ProtocolDlnaConfirmationPort.h"

namespace mcast::skeleton {

ProtocolDlnaConfirmationPort::ProtocolDlnaConfirmationPort(
    ipc::ProtocolControlDispatcher &dispatcher)
    : m_dispatcher(dispatcher)
{
}

bool ProtocolDlnaConfirmationPort::waitForDecision(
    const cast::ConfirmationPrompt &prompt,
    bool &accepted, std::string &error)
{
    accepted = false;
    if (m_cancelRequested.exchange(false)) {
        error = "cast confirmation cancelled";
        return false;
    }
    if (prompt.remaining <= cast::CastTime::zero()) {
        error = "cast confirmation already expired";
        return false;
    }
    const ipc::CastConfirmationRequestInfo request{
        prompt.key.protocol,
        prompt.key.id,
        prompt.peer.id,
        prompt.peer.displayName,
        prompt.peer.address,
        prompt.mediaTitle,
        static_cast<std::uint64_t>(prompt.remaining.count()),
    };
    m_dispatcher.clearCastConfirmationCancel();
    m_waiting = true;
    if (m_cancelRequested.exchange(false)) {
        m_waiting = false;
        error = "cast confirmation cancelled";
        return false;
    }
    const bool result = m_dispatcher.requestCastConfirmation(
        request, prompt.remaining, accepted, error);
    m_waiting = false;
    return result;
}

void ProtocolDlnaConfirmationPort::cancel() noexcept
{
    m_cancelRequested = true;
    if (m_waiting.load())
        static_cast<void>(m_dispatcher.cancelCastConfirmation());
}

void ProtocolDlnaConfirmationPort::reset() noexcept
{
    m_waiting = false;
    m_cancelRequested = false;
    m_dispatcher.clearCastConfirmationCancel();
}

} // namespace mcast::skeleton
