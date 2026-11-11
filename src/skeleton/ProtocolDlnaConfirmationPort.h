// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/ProtocolControlDispatcher.h"
#include "protocols/dlna/runtime/DlnaClaimConfirmationWorker.h"

#include <atomic>

namespace mcast::skeleton {

// Protocol 进程内的适配器：DLNA runtime 只看到中性确认 Port，
// ControlPipe 只看到现有的 CastConfirmationRequest/Decision 消息。
class ProtocolDlnaConfirmationPort final
    : public dlna::runtime::DlnaClaimConfirmationPort
{
public:
    explicit ProtocolDlnaConfirmationPort(
        ipc::ProtocolControlDispatcher &dispatcher);

    bool waitForDecision(
        const cast::ConfirmationPrompt &prompt,
        bool &accepted, std::string &error) override;
    void cancel() noexcept override;
    void reset() noexcept override;

private:
    ipc::ProtocolControlDispatcher &m_dispatcher;
    std::atomic_bool m_waiting{false};
    std::atomic_bool m_cancelRequested{false};
};

} // namespace mcast::skeleton
