// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcast::cast {

using CastTime = std::chrono::milliseconds;

enum class ProtocolKind : std::uint8_t {
    None,
    Dlna,
    GoogleCast,
    AirPlay,
    Miracast,
    LeLink,
    Vendor,
};

enum class ArbitrationPhase : std::uint8_t {
    Idle,
    Preparing,
    Active,
    ActiveWithPendingClaim,
    IdleWithPendingClaim,
    Handoff,
    Faulted,
};

enum class EndReason : std::uint8_t {
    Completed,
    Failed,
    UserDisconnected,
    Preempted,
    ProtocolLost,
    Shutdown,
};

enum class ClaimStatus : std::uint8_t {
    Accepted,
    AwaitingConfirmation,
    Rejected,
    Busy,
    NotFound,
};

enum class ClaimReason : std::uint8_t {
    None,
    FirstClaim,
    Preempted,
    ConfirmationRequired,
    ConfirmationAccepted,
    ConfirmationRejected,
    ConfirmationTimedOut,
    ActiveClaimProtected,
    PendingClaimExists,
    SessionTransitionPending,
    InvalidClaim,
    DuplicateClaim,
    UnknownClaim,
};

struct PeerIdentity {
    std::string id;
    std::string displayName;
    std::string address;
};

struct ClaimKey {
    ProtocolKind protocol = ProtocolKind::None;
    std::uint64_t id = 0;
};

struct ClaimRequest {
    std::uint64_t claimId = 0;
    ProtocolKind protocol = ProtocolKind::None;
    PeerIdentity peer;
    std::string mediaTitle;
};

struct Session {
    std::uint64_t id = 0;
    std::uint64_t epoch = 0;
    std::uint64_t claimId = 0;
    ProtocolKind protocol = ProtocolKind::None;
    PeerIdentity peer;
    std::string mediaTitle;
};

struct PendingClaim {
    std::uint64_t claimId = 0;
    ProtocolKind protocol = ProtocolKind::None;
    PeerIdentity peer;
    std::string mediaTitle;
    CastTime deadline{0};
};

struct RetiringSession {
    Session session;
    EndReason reason = EndReason::Preempted;
    CastTime deadline{0};
};

struct ArbitrationPolicy {
    bool allowPreemption = false;
    bool confirmPreemption = true;
    CastTime confirmationTimeout{10000};
    CastTime retirementTimeout{5000};
    std::size_t maxRetiringSessions = 8;
};

struct ArbitrationSnapshot {
    std::uint64_t revision = 0;
    ArbitrationPhase phase = ArbitrationPhase::Idle;
    std::optional<Session> owner;
    std::optional<PendingClaim> pendingClaim;
    std::vector<RetiringSession> retiringSessions;
    std::uint64_t lastEpoch = 0;
    std::optional<EndReason> lastFailure;
};

struct ClaimDecision {
    ClaimStatus status = ClaimStatus::Rejected;
    ClaimReason reason = ClaimReason::None;
    ProtocolKind protocol = ProtocolKind::None;
    std::uint64_t claimId = 0;
    std::uint64_t revision = 0;
    std::optional<Session> owner;
    std::optional<Session> displaced;
    bool duplicate = false;
};

struct ArbitrationTick {
    std::uint64_t revision = 0;
    std::optional<ClaimDecision> pendingTimeout;
    std::vector<RetiringSession> expiredRetirements;
};

} // namespace mcast::cast
