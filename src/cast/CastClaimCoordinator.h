// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "cast/CastArbiter.h"

#include <optional>
#include <string>

namespace mcast::cast {

struct ConfirmationPrompt {
    ClaimKey key;
    PeerIdentity peer;
    std::string mediaTitle;
    CastTime deadline{0};
    CastTime remaining{0};
};

struct ClaimStart {
    ClaimDecision decision;
    std::optional<ConfirmationPrompt> confirmation;
};

// Protocol 进程唯一调用 CastArbiter 的入口。调用方负责串行访问和把
// ConfirmationPrompt 转成 ControlPipe 消息；这里不依赖 IPC、Qt 或具体协议。
class CastClaimCoordinator
{
public:
    explicit CastClaimCoordinator(ArbitrationPolicy policy = {});

    ClaimStart claim(ClaimRequest request, CastTime now);
    ClaimDecision resolveConfirmation(
        ClaimKey key, bool accepted, CastTime now);
    ArbitrationTick tick(CastTime now);

    bool sessionReady(std::uint64_t sessionId);
    bool sessionEnded(std::uint64_t sessionId, EndReason reason);
    bool sessionRetired(std::uint64_t sessionId);

    void setPolicy(ArbitrationPolicy policy);
    const ArbitrationPolicy &policy() const noexcept;
    const ArbitrationSnapshot &snapshot() const noexcept;

private:
    CastArbiter m_arbiter;
};

} // namespace mcast::cast
