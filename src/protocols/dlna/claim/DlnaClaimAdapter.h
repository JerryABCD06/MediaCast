// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "cast/CastClaimCoordinator.h"
#include "playback/PlaybackTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace mcast::dlna::claim {

enum class DlnaClaimDisposition : std::uint8_t {
    Execute,
    Pending,
    Rejected,
    Ignored,
};

struct DlnaClaimInvalidation {
    cast::ClaimDecision decision;
    std::optional<playback::MediaRequest> media;
};

struct DlnaClaimOutcome {
    DlnaClaimDisposition disposition = DlnaClaimDisposition::Rejected;
    cast::ClaimReason reason = cast::ClaimReason::InvalidClaim;
    std::uint64_t transactionId = 0;
    std::optional<std::uint64_t> claimedSessionId;
    std::optional<std::uint64_t> ownerSessionId;
    std::uint64_t ownerEpoch = 0;
    std::uint64_t mediaGeneration = 0;
    cast::CastTime deadline{0};
    std::optional<cast::ConfirmationPrompt> confirmation;
    std::optional<playback::MediaRequest> media;
    std::optional<DlnaClaimInvalidation> invalidatedPending;
};

struct DlnaClaimResolution {
    cast::ClaimDecision decision;
    std::optional<playback::MediaRequest> media;
    bool executeMedia = false;
    std::optional<std::uint64_t> ownerSessionId;
    std::uint64_t ownerEpoch = 0;
    std::uint64_t mediaGeneration = 0;
    std::optional<DlnaClaimInvalidation> invalidatedPending;
};

struct DlnaClaimTick {
    cast::ArbitrationTick arbitration;
    std::optional<DlnaClaimInvalidation> pendingInvalidation;
};

// DLNA 的协议内适配层。跨请求事务只在这一层保存；主 owning session 仍由
// CastClaimCoordinator 决定，调用方负责把 HTTP requestId 与这里的事务关联。
// 所有方法必须在 Protocol 的单一仲裁执行上下文中串行调用。
class DlnaClaimAdapter
{
public:
    explicit DlnaClaimAdapter(cast::CastClaimCoordinator &coordinator);

    DlnaClaimOutcome beginMedia(
        cast::PeerIdentity peer, playback::MediaRequest media,
        cast::CastTime now);
    DlnaClaimOutcome requestPlay(
        cast::PeerIdentity peer, cast::CastTime now);

    // 未知或候选控制点的 Stop 只能被忽略。它不能穿透到当前 owner，
    // 否则用户还没确认，旧投送就会先被停掉。
    DlnaClaimOutcome requestStop(
        cast::PeerIdentity peer, cast::CastTime now);

    DlnaClaimResolution resolveConfirmation(
        cast::ClaimKey key, bool accepted, cast::CastTime now);
    DlnaClaimResolution cancelConfirmation(
        cast::ClaimKey key, cast::CastTime now);
    DlnaClaimResolution cancelAllPending(cast::CastTime now);
    DlnaClaimTick tick(cast::CastTime now);

    bool ownsPeer(const cast::PeerIdentity &peer) const;
    // These two are projection queries. Call tick(now) before using them as a
    // deadline-sensitive source of truth.
    bool hasPending() const noexcept { return m_pending.has_value(); }
    std::optional<cast::ClaimKey> pendingKey() const;

private:
    struct PendingMedia {
        std::uint64_t claimId = 0;
        cast::PeerIdentity peer;
        playback::MediaRequest media;
        cast::CastTime deadline{0};
        bool playRequested = false;
    };

    struct OwnerMedia {
        std::uint64_t sessionId = 0;
        playback::MediaRequest media;
    };

    bool ownerMatches(const cast::PeerIdentity &peer) const;
    bool pendingMatches(const cast::PeerIdentity &peer) const;
    bool pendingProjectionMatches() const;
    std::optional<DlnaClaimInvalidation> expirePending(
        cast::CastTime now);
    void publishOwner(
        DlnaClaimOutcome &outcome, const cast::Session &owner,
        bool newMedia);
    void publishOwner(
        DlnaClaimResolution &result, const cast::Session &owner,
        bool newMedia);
    void invalidatePending();
    std::uint64_t nextClaimId();
    static cast::ClaimDecision rejected(
        cast::ClaimReason reason, std::uint64_t revision,
        std::uint64_t claimId = 0);
    static bool sameMedia(
        const playback::MediaRequest &left,
        const playback::MediaRequest &right);

    cast::CastClaimCoordinator &m_coordinator;
    std::optional<PendingMedia> m_pending;
    std::optional<OwnerMedia> m_ownerMedia;
    std::uint64_t m_nextClaimId = 1;
    std::uint64_t m_mediaGeneration = 0;
};

} // namespace mcast::dlna::claim
