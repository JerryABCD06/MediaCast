// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "protocols/dlna/claim/DlnaClaimAdapter.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace mcast::dlna::claim {

struct DlnaHttpCompletion {
    std::uint64_t requestId = 0;
    bool success = false;
    cast::ClaimReason reason = cast::ClaimReason::InvalidClaim;
};

struct DlnaMediaExecution {
    std::uint64_t operationId = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t epoch = 0;
    std::uint64_t mediaGeneration = 0;
    bool newSession = false;
    bool playRequested = false;
    playback::MediaRequest media;
};

struct DlnaClaimRuntimeActions {
    std::vector<DlnaHttpCompletion> http;
    std::optional<cast::ConfirmationPrompt> confirmation;
    std::optional<DlnaMediaExecution> media;
    bool playNow = false;
    bool staleOperation = false;
};

// Protocol 内的 DLNA 事务层：把 HTTP requestId、确认阶段和媒体打开完成
// 关联起来。它不直接读写 socket 或 NamedPipe；调用方负责发送 confirmation，
// 执行 media，并按返回的 http 列表完成挂起请求。
class DlnaClaimRuntime
{
public:
    explicit DlnaClaimRuntime(DlnaClaimAdapter &adapter);

    DlnaClaimRuntimeActions beginSetUri(
        std::uint64_t requestId, cast::PeerIdentity peer,
        playback::MediaRequest media, cast::CastTime now);
    DlnaClaimRuntimeActions attachPlay(
        std::uint64_t requestId, cast::PeerIdentity peer,
        cast::CastTime now);
    DlnaClaimRuntimeActions confirmationDecision(
        cast::ClaimKey key, bool accepted, cast::CastTime now);
    DlnaClaimRuntimeActions tick(cast::CastTime now);
    DlnaClaimRuntimeActions mediaReady(
        std::uint64_t operationId, bool success,
        cast::CastTime now);

    // ControlPipe 或 Protocol 代次丢失。当前确认候选和正在准备的媒体都
    // fail-closed，所有关联 HTTP 必须拒绝。
    DlnaClaimRuntimeActions controlLost(cast::CastTime now);

    bool mediaOpening() const noexcept { return m_operation.has_value(); }
    std::optional<std::uint64_t> operationId() const noexcept;
    std::optional<cast::ClaimKey> pendingKey() const noexcept;

private:
    struct PendingHttp {
        cast::ClaimKey key;
        std::vector<std::uint64_t> setRequests;
        std::vector<std::uint64_t> playRequests;
    };

    struct Operation {
        std::uint64_t id = 0;
        std::uint64_t sessionId = 0;
        std::uint64_t epoch = 0;
        std::uint64_t mediaGeneration = 0;
        bool newSession = false;
        bool playRequested = false;
        playback::MediaRequest media;
        std::vector<std::uint64_t> setRequests;
        std::vector<std::uint64_t> playRequests;
    };

    DlnaClaimRuntimeActions beginExecute(
        DlnaClaimOutcome &&outcome);
    DlnaClaimRuntimeActions beginResolution(
        DlnaClaimResolution &&resolution);
    void completePending(
        DlnaClaimRuntimeActions &actions,
        const cast::ClaimDecision &decision);
    void completeOperation(
        DlnaClaimRuntimeActions &actions,
        const Operation &operation, bool success,
        cast::ClaimReason reason);
    void replaceOperation(
        DlnaClaimRuntimeActions &actions,
        const cast::ClaimDecision &decision);
    bool requestTracked(std::uint64_t requestId) const;
    std::uint64_t nextOperationId();

    DlnaClaimAdapter &m_adapter;
    std::optional<PendingHttp> m_pending;
    std::optional<Operation> m_operation;
    std::uint64_t m_nextOperationId = 1;
};

} // namespace mcast::dlna::claim
