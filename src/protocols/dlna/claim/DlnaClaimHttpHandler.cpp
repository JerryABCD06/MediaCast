// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/claim/DlnaClaimHttpHandler.h"

#include "ipc/PlaybackCodec.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string_view>
#include <utility>

namespace mcast::dlna::claim {
namespace {

cast::CastTime steadyNow()
{
    return std::chrono::duration_cast<cast::CastTime>(
        std::chrono::steady_clock::now().time_since_epoch());
}

bool validPlaySpeed(const soap::SoapRequestXml &body)
{
    const auto speed = body.value("Speed");
    return speed
        && (*speed == "1" || *speed == "1.0" || *speed == "1/1");
}

std::string trim(std::string value)
{
    const auto whitespace = [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    };
    while (!value.empty() && whitespace(value.front()))
        value.erase(value.begin());
    while (!value.empty() && whitespace(value.back()))
        value.pop_back();
    return value;
}

bool sameAddressControlAction(std::string_view action)
{
    return action == "Pause" || action == "Seek";
}

bool ownerOnlyMutation(std::string_view action)
{
    return action == "SetNextAVTransportURI"
        || action == "Next" || action == "Previous"
        || action == "SetPlayMode";
}

} // namespace

DlnaClaimHttpHandler::DlnaClaimHttpHandler(
    soap::SoapRouter &router, DlnaClaimRuntime &runtime,
    std::function<cast::CastTime()> now,
    DlnaClaimHttpCallbacks callbacks)
    : m_router(router)
    , m_runtime(runtime)
    , m_now(now ? std::move(now) : steadyNow)
    , m_callbacks(std::move(callbacks))
{
}

net::HttpDispatchResult DlnaClaimHttpHandler::handle(
    const core::HttpRequest &request)
{
    if (!m_callbacks.completeHttp
        || !m_callbacks.requestConfirmation
        || !m_callbacks.executeMedia
        || !m_callbacks.playNow
        || !m_callbacks.stopNow) {
        return {
            net::HttpDispatchStatus::Completed,
            soap::SoapRouter::faultResponse(501, "Action Failed"),
        };
    }

    net::HttpResponse parseError;
    const auto parsed = m_router.parseRequest(request, parseError);
    if (!parsed)
        return {net::HttpDispatchStatus::Completed, parseError};
    if (request.requestId == 0) {
        return {
            net::HttpDispatchStatus::Completed,
            soap::SoapRouter::faultResponse(501, "Action Failed"),
        };
    }
    std::unique_lock operationLock(m_operationMutex);
    tick(m_now());

    if (parsed->service != "AVTransport") {
        const std::uint64_t ownerEpoch =
            m_router.ownerEpoch();
        operationLock.unlock();
        return {
            net::HttpDispatchStatus::Completed,
            m_router.handleParsed(*parsed, ownerEpoch),
        };
    }

    DlnaClaimRuntimeActions actions;
    if (parsed->action == "SetAVTransportURI") {
        if (m_callbacks.acceptNewMedia
            && !m_callbacks.acceptNewMedia()) {
            return {
                net::HttpDispatchStatus::Completed,
                soap::SoapRouter::faultResponse(
                    705, "Transport is locked"),
            };
        }
        const auto uri = parsed->body.value("CurrentURI");
        const auto metadata =
            parsed->body.value("CurrentURIMetaData");
        if (!uri || !metadata || uri->empty()) {
            return {
                net::HttpDispatchStatus::Completed,
                soap::SoapRouter::faultResponse(402, "Invalid Args"),
            };
        }
        const cast::PeerIdentity peer = peerIdentity(request);
        auto media = soap::SoapRouter::mediaRequest(*uri, *metadata);
        std::string mediaError;
        if (!mcast::playback::wire::encodeMediaRequest(
                media, mediaError)) {
            return {
                net::HttpDispatchStatus::Completed,
                soap::SoapRouter::faultResponse(
                    402, "Invalid Args"),
            };
        }
        {
            std::lock_guard lock(m_mutex);
            m_requests[request.requestId] = ResponseKind::SetUri;
            actions = m_runtime.beginSetUri(
                request.requestId, peer,
                std::move(media),
                m_now());
        }
    } else if (parsed->action == "Play") {
        if (!validPlaySpeed(parsed->body)) {
            return {
                net::HttpDispatchStatus::Completed,
                soap::SoapRouter::faultResponse(
                    717, "Play speed not supported"),
            };
        }
        m_router.notePeerActivity(request.peerAddress);
        const cast::PeerIdentity peer = peerIdentity(request);
        if (m_runtime.canControl(peer)
            && !m_runtime.mediaOpening()
            && !m_router.hasTransportMedia()) {
            return {
                net::HttpDispatchStatus::Completed,
                soap::SoapRouter::faultResponse(
                    701, "Transition not available"),
            };
        }
        {
            std::lock_guard lock(m_mutex);
            m_requests[request.requestId] = ResponseKind::Play;
            actions = m_runtime.attachPlay(
                request.requestId, peer, m_now());
        }
    } else if (parsed->action == "Stop") {
        const cast::PeerIdentity peer = peerIdentity(request);
        if (m_runtime.ownsPeer(peer)
            && !m_runtime.mediaOpening()
            && !m_router.hasTransportMedia()) {
            return {
                net::HttpDispatchStatus::Completed,
                soap::SoapRouter::faultResponse(
                    701, "Transition not available"),
            };
        }
        {
            std::lock_guard lock(m_mutex);
            m_requests[request.requestId] = ResponseKind::Stop;
            actions = m_runtime.requestStop(
                request.requestId, peer, m_now());
        }
        if (actions.stopNow)
            m_router.notePeerActivity(request.peerAddress);
        else if (std::any_of(
                     actions.http.begin(), actions.http.end(),
                     [](const DlnaHttpCompletion &completion) {
                         return completion.success;
                     })) {
            log(
                "DLNA non-owner Stop acknowledged without playback "
                "change from " + peer.address
                + ": peer " + peer.id);
        }
    } else if (sameAddressControlAction(parsed->action)) {
        m_router.notePeerActivity(request.peerAddress);
        const cast::PeerIdentity peer = peerIdentity(request);
        DlnaClaimDisposition disposition;
        {
            std::lock_guard lock(m_mutex);
            disposition = m_runtime.requestControl(
                peer, m_now());
        }
        if (disposition == DlnaClaimDisposition::Execute) {
            const std::uint64_t ownerEpoch =
                m_router.ownerEpoch();
            log(
                "DLNA control accepted for " + parsed->action
                + " from " + peer.address);
            operationLock.unlock();
            return {
                net::HttpDispatchStatus::Completed,
                m_router.handleParsed(*parsed, ownerEpoch),
            };
        }
        log(
            "DLNA control rejected for " + parsed->action
            + " from " + peer.address + ": peer " + peer.id);
        return {
            net::HttpDispatchStatus::Completed,
            soap::SoapRouter::faultResponse(
                701, "Transition not available"),
        };
    } else if (ownerOnlyMutation(parsed->action)) {
        m_router.notePeerActivity(request.peerAddress);
        const cast::PeerIdentity peer = peerIdentity(request);
        bool ownsPeer = false;
        {
            std::lock_guard lock(m_mutex);
            ownsPeer = m_runtime.ownsPeer(peer);
        }
        if (ownsPeer) {
            const std::uint64_t ownerEpoch =
                m_router.ownerEpoch();
            log(
                "DLNA owner mutation accepted for " + parsed->action
                + " from " + peer.address);
            operationLock.unlock();
            return {
                net::HttpDispatchStatus::Completed,
                m_router.handleParsed(*parsed, ownerEpoch),
            };
        }
        log(
            "DLNA owner mutation rejected for " + parsed->action
            + " from " + peer.address + ": peer " + peer.id);
        return {
            net::HttpDispatchStatus::Completed,
            soap::SoapRouter::faultResponse(
                701, "Transition not available"),
        };
    } else {
        operationLock.unlock();
        return {
            net::HttpDispatchStatus::Completed,
            m_router.handleParsed(*parsed),
        };
    }

    actions.ownerEpoch = m_router.ownerEpoch();
    applyActions(std::move(actions));
    return {net::HttpDispatchStatus::Pending, {}};
}

void DlnaClaimHttpHandler::confirmationDecision(
    cast::ClaimKey key, bool accepted, cast::CastTime now)
{
    std::lock_guard operationLock(m_operationMutex);
    DlnaClaimRuntimeActions actions;
    {
        std::lock_guard lock(m_mutex);
        actions = m_runtime.confirmationDecision(
            key, accepted, now);
    }
    actions.ownerEpoch = m_router.ownerEpoch();
    applyActions(std::move(actions));
}

void DlnaClaimHttpHandler::tick(cast::CastTime now)
{
    std::lock_guard operationLock(m_operationMutex);
    DlnaClaimRuntimeActions actions;
    {
        std::lock_guard lock(m_mutex);
        actions = m_runtime.tick(now);
    }
    actions.ownerEpoch = m_router.ownerEpoch();
    applyActions(std::move(actions));
}

void DlnaClaimHttpHandler::mediaReady(
    std::uint64_t operationId, bool success,
    cast::CastTime now)
{
    std::lock_guard operationLock(m_operationMutex);
    DlnaClaimRuntimeActions actions;
    {
        std::lock_guard lock(m_mutex);
        actions = m_runtime.mediaReady(
            operationId, success, now);
    }
    actions.ownerEpoch = m_router.ownerEpoch();
    applyActions(std::move(actions));
}

void DlnaClaimHttpHandler::controlLost(cast::CastTime now)
{
    std::lock_guard operationLock(m_operationMutex);
    DlnaClaimRuntimeActions actions;
    {
        std::lock_guard lock(m_mutex);
        actions = m_runtime.controlLost(now);
    }
    actions.ownerEpoch = m_router.ownerEpoch();
    applyActions(std::move(actions));
}

cast::PeerIdentity DlnaClaimHttpHandler::peerIdentity(
    const core::HttpRequest &request)
{
    if (m_callbacks.identifyPeer) {
        try {
            const auto identified = m_callbacks.identifyPeer(request);
            if (!identified.id.empty() && !identified.address.empty())
                return identified;
        } catch (const std::exception &exception) {
            log(
                std::string("DLNA peer identification failed: ")
                + exception.what());
        } catch (...) {
            log("DLNA peer identification failed");
        }
    }
    std::string agent = request.header("user-agent").value_or("");
    agent.erase(
        std::remove_if(
            agent.begin(), agent.end(),
            [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; }),
        agent.end());
    std::replace_if(
        agent.begin(), agent.end(),
        [](unsigned char ch) { return ch < 0x20 || ch >= 0x7f; },
        '?');
    agent = trim(agent);
    if (agent.size() > 256)
        agent.resize(256);
    constexpr cast::CastTime kPeerCacheTtl{120000};
    constexpr std::size_t kPeerCacheLimit = 256;
    const cast::CastTime now = m_now();
    for (auto it = m_peerByAddress.begin();
         it != m_peerByAddress.end();) {
        if (now < it->second.lastSeen
            || now - it->second.lastSeen > kPeerCacheTtl) {
            it = m_peerByAddress.erase(it);
        } else {
            ++it;
        }
    }
    if (!m_peerByAddress.contains(request.peerAddress)
        && m_peerByAddress.size() >= kPeerCacheLimit) {
        const auto oldest = std::min_element(
            m_peerByAddress.begin(), m_peerByAddress.end(),
            [](const auto &left, const auto &right) {
                return left.second.lastSeen < right.second.lastSeen;
            });
        if (oldest != m_peerByAddress.end())
            m_peerByAddress.erase(oldest);
    }
    const std::string peerId = request.peerAddress + "|" + agent;
    cast::PeerIdentity peer{
        peerId,
        agent.empty() ? request.peerAddress : agent,
        request.peerAddress,
    };
    auto &cache = m_peerByAddress[request.peerAddress];
    if (agent.empty()) {
        // A missing User-Agent may inherit the sole cached identity on this
        // address during the short TTL. Seeing a second User-Agent marks the
        // address ambiguous and permanently disables that fallback.
        if (!cache.ambiguous && cache.lastSeen != cast::CastTime{0}
            && now >= cache.lastSeen
            && now - cache.lastSeen <= kPeerCacheTtl) {
            cache.lastSeen = now;
            return cache.peer;
        }
        peer.id = request.peerAddress + "|unknown-"
            + std::to_string(m_unknownPeerGeneration++);
        peer.displayName = request.peerAddress;
        cache.ambiguous = true;
        cache.lastSeen = now;
        return peer;
    }

    if (cache.lastSeen != cast::CastTime{0}
        && cache.peer.id != peer.id) {
        cache.ambiguous = true;
    }
    cache.peer = peer;
    cache.lastSeen = now;
    return peer;
}

void DlnaClaimHttpHandler::applyActions(
    DlnaClaimRuntimeActions actions)
{
    bool forceFault = false;
    bool cleanupRuntime = false;
    const auto invoke = [](const auto &callback, const auto &argument) {
        try {
            callback(argument);
            return true;
        } catch (...) {
            return false;
        }
    };
    if (actions.confirmation && m_callbacks.requestConfirmation) {
        if (!invoke(
                m_callbacks.requestConfirmation,
                *actions.confirmation)) {
            log("DLNA confirmation callback failed");
            forceFault = true;
            cleanupRuntime = true;
        }
    }
    const bool takeoverStop = actions.media.has_value();
    if (!forceFault && actions.stopNow && actions.stopPeer
        && m_callbacks.stopNow && m_router.hasTransportMedia()) {
        if (takeoverStop) {
            log(
                "DLNA takeover accepted; stopping displaced owner "
                "before media start");
        }
        try {
            m_callbacks.stopNow(
                *actions.stopPeer, actions.ownerEpoch);
        } catch (const std::exception &exception) {
            log(
                std::string("DLNA stop callback failed: ")
                + exception.what());
            forceFault = true;
            cleanupRuntime = takeoverStop;
        } catch (...) {
            log("DLNA stop callback failed");
            forceFault = true;
            cleanupRuntime = takeoverStop;
        }
    }
    if (!forceFault && actions.media && m_callbacks.executeMedia) {
        if (!invoke(m_callbacks.executeMedia, *actions.media)) {
            log("DLNA media execution callback failed");
            forceFault = true;
            cleanupRuntime = true;
        }
    }
    if (!forceFault && actions.playNow && m_callbacks.playNow) {
        try {
            m_callbacks.playNow(actions.ownerEpoch);
        } catch (const std::exception &exception) {
            log(
                std::string("DLNA play callback failed: ")
                + exception.what());
            forceFault = true;
        } catch (...) {
            log("DLNA play callback failed");
            forceFault = true;
        }
    }

    for (const auto &completion : actions.http) {
        std::optional<ResponseKind> kind;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_requests.find(completion.requestId);
            if (found == m_requests.end())
                continue;
            kind = found->second;
            m_requests.erase(found);
        }
        if (m_callbacks.completeHttp) {
            try {
                if (!m_callbacks.completeHttp(
                    completion.requestId,
                    responseFor(
                        *kind, completion.success && !forceFault,
                        completion.reason))) {
                    log(
                        "DLNA HTTP completion was rejected by the server");
                }
            } catch (const std::exception &exception) {
                log(
                    std::string("DLNA HTTP completion failed: ")
                    + exception.what());
            } catch (...) {
                log("DLNA HTTP completion failed");
            }
        }
    }
    if (cleanupRuntime) {
        DlnaClaimRuntimeActions cleanup;
        {
            std::lock_guard lock(m_mutex);
            cleanup = m_runtime.controlLost(m_now());
        }
        applyActions(std::move(cleanup));
    }
}

net::HttpResponse DlnaClaimHttpHandler::responseFor(
    ResponseKind kind, bool success,
    cast::ClaimReason reason) const
{
    if (!success) {
        const bool transition =
            reason == cast::ClaimReason::ConfirmationRejected
            || reason == cast::ClaimReason::ConfirmationTimedOut
            || reason == cast::ClaimReason::PendingClaimExists
            || reason == cast::ClaimReason::SessionTransitionPending
            || reason == cast::ClaimReason::ActiveClaimProtected;
        if (transition) {
            return soap::SoapRouter::faultResponse(
                701, "Transition not available");
        }
        return soap::SoapRouter::faultResponse(
            501, "Action Failed");
    }
    switch (kind) {
    case ResponseKind::SetUri:
        return soap::SoapRouter::successResponse(
            "AVTransport", "SetAVTransportURI");
    case ResponseKind::Play:
        return soap::SoapRouter::successResponse(
            "AVTransport", "Play");
    case ResponseKind::Stop:
        return soap::SoapRouter::successResponse(
            "AVTransport", "Stop");
    }
    return soap::SoapRouter::faultResponse(501, "Action Failed");
}

void DlnaClaimHttpHandler::log(const std::string &message) const
{
    if (!m_callbacks.log)
        return;
    try {
        m_callbacks.log(message);
    } catch (...) {
    }
}

} // namespace mcast::dlna::claim
