// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "cast/CastTypes.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcast::cast {

class CastArbiter
{
public:
    explicit CastArbiter(ArbitrationPolicy policy = {});

    ClaimDecision claim(ClaimRequest request, CastTime now);
    ClaimDecision confirmPending(
        ClaimKey key, bool accepted, CastTime now);
    ArbitrationTick tick(CastTime now);

    bool sessionReady(std::uint64_t sessionId);
    bool sessionEnded(std::uint64_t sessionId, EndReason reason);
    bool sessionRetired(std::uint64_t sessionId);

    void setPolicy(ArbitrationPolicy policy);
    const ArbitrationPolicy &policy() const noexcept { return m_policy; }
    const ArbitrationSnapshot &snapshot() const noexcept { return m_snapshot; }

private:
    struct CachedDecision {
        ClaimKey key;
        ClaimDecision decision;
    };

    static ArbitrationPolicy normalized(ArbitrationPolicy policy);
    Session makeSession(const ClaimRequest &request, CastTime now);
    std::optional<ClaimDecision> cachedDecision(ClaimKey key) const;
    ClaimDecision rememberDecision(ClaimDecision decision);
    ClaimDecision expirePending(CastTime now);
    ClaimDecision acceptFirstClaim(
        const ClaimRequest &request, CastTime now);
    ClaimDecision beginHandoff(
        const ClaimRequest &request, CastTime now);
    ClaimDecision busy(ClaimReason reason, const ClaimRequest &request) const;

    ArbitrationPolicy m_policy;
    ArbitrationSnapshot m_snapshot;
    std::uint64_t m_nextSessionId = 1;
    std::vector<CachedDecision> m_recentDecisions;
};

} // namespace mcast::cast
