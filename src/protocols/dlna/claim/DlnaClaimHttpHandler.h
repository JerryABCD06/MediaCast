// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "protocols/dlna/claim/DlnaClaimRuntime.h"
#include "protocols/dlna/net/HttpServer.h"
#include "protocols/dlna/soap/SoapRouter.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mcast::dlna::claim {

struct DlnaClaimHttpCallbacks {
    // These callbacks are serialized across every handler entry point, but
    // may run on either the HTTP owner thread or the Protocol event thread.
    // executeMedia stays asynchronous. playNow/stopNow may wait for the
    // PlaybackPipe acknowledgement so the SOAP success is not sent before the
    // transport state becomes visible. UI creation and media opening do not
    // belong here.
    std::function<bool(
        std::uint64_t requestId, net::HttpResponse response)>
        completeHttp;
    std::function<void(const cast::ConfirmationPrompt &)>
        requestConfirmation;
    std::function<void(const DlnaMediaExecution &)> executeMedia;
    std::function<void(std::optional<std::uint64_t>)> playNow;
    std::function<void(
        const cast::PeerIdentity &, std::optional<std::uint64_t>)>
        stopNow;
    std::function<bool()> acceptNewMedia;
    // Optional replacement for the default IP+User-Agent fingerprint. Future
    // DLNA control subscriptions can provide a stronger application identity.
    std::function<cast::PeerIdentity(const core::HttpRequest &)>
        identifyPeer;
    std::function<void(const std::string &)> log;
};

// SoapRouter 与 DlnaClaimRuntime 之间的异步边界。只拦截会改变当前媒体或
// 所有权的 AVTransport 动作，查询和渲染控制仍走原来的同步 SoapRouter。
// 所有公开方法可被 HttpServer 或 Protocol 事件线程并发调用；入口先取得
// 递归操作锁，保证“授权判断 -> callback”不被另一请求插入。runtime 与
// requestId 映射另有短临界区，外部 callback 不在该短锁内执行。
class DlnaClaimHttpHandler
{
public:
    DlnaClaimHttpHandler(
        soap::SoapRouter &router, DlnaClaimRuntime &runtime,
        std::function<cast::CastTime()> now,
        DlnaClaimHttpCallbacks callbacks = {});

    net::HttpDispatchResult handle(
        const core::HttpRequest &request);
    void confirmationDecision(
        cast::ClaimKey key, bool accepted, cast::CastTime now);
    void tick(cast::CastTime now);
    void mediaReady(
        std::uint64_t operationId, bool success,
        cast::CastTime now);
    void controlLost(cast::CastTime now);

private:
    enum class ResponseKind {
        SetUri,
        Play,
        Stop,
    };

    struct PeerCache {
        cast::PeerIdentity peer;
        cast::CastTime lastSeen{0};
        bool ambiguous = false;
    };

    cast::PeerIdentity peerIdentity(
        const core::HttpRequest &request);
    void applyActions(DlnaClaimRuntimeActions actions);
    net::HttpResponse responseFor(
        ResponseKind kind, bool success,
        cast::ClaimReason reason) const;
    void log(const std::string &message) const;

    soap::SoapRouter &m_router;
    DlnaClaimRuntime &m_runtime;
    std::function<cast::CastTime()> m_now;
    DlnaClaimHttpCallbacks m_callbacks;
    mutable std::mutex m_mutex;
    std::recursive_mutex m_operationMutex;
    std::unordered_map<std::uint64_t, ResponseKind> m_requests;
    std::unordered_map<std::string, PeerCache> m_peerByAddress;
    std::uint64_t m_unknownPeerGeneration = 1;
};

} // namespace mcast::dlna::claim
