// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "cast/CastTypes.h"
#include "ipc/IpcProtocol.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mcast::ipc {

enum class FieldId : std::uint16_t {
    HelloProcessId = 1,
    HelloBuildId = 2,
    HelloInstanceNonce = 3,
    HelloExecutableHash = 4,
    HelloCapabilities = 5,
    HelloAckAccepted = 32,
    HelloAckVersionMajor = 33,
    HelloAckVersionMinor = 34,
    HelloAckInstanceNonce = 35,
    HelloAckCapabilities = 36,
    HelloAckReason = 37,
    PeerRole = 48,
    PeerProcessId = 49,
    PeerBuildId = 50,
    PeerInstanceNonce = 51,
    PeerExecutableHash = 52,
    PeerActivationId = 53,
    UiActivationStarted = 64,
    UiActivationId = 65,
    ShellHandoffArguments = 66,
    ShellHandoffWorkingDirectory = 67,
    ShellHandoffAccepted = 68,
    ReceivingEnabled = 69,
    PlaybackGeneration = 70,
    PlaybackCommandBlob = 71,
    PlaybackSnapshotBlob = 72,
    PlaybackSubmissionStatus = 73,
    PlaybackOutcomeStatus = 74,
    PlaybackStateVersion = 75,
    CastProtocol = 80,
    CastClaimId = 81,
    CastPeerId = 82,
    CastPeerDisplayName = 83,
    CastPeerAddress = 84,
    CastMediaTitle = 85,
    CastConfirmationTimeoutMs = 86,
    CastConfirmationAccepted = 87,
    ErrorCode = 16,
    ErrorText = 17,
};

struct HelloInfo {
    std::uint32_t processId = 0;
    std::string buildId;
    std::string instanceNonce;
    std::string executableHash;
    std::uint64_t capabilities = 0;
};

struct HelloAckInfo {
    bool accepted = false;
    std::uint16_t versionMajor = kProtocolVersionMajor;
    std::uint16_t versionMinor = kProtocolVersionMinor;
    std::string instanceNonce;
    std::uint64_t capabilities = 0;
    std::string reason;
};

enum class ConnectionChannel {
    Control,
    Playback,
    Command,
};

struct HelloExpectation {
    std::uint32_t peerProcessId = 0;
    std::string buildId;
    std::string instanceNonce;
    std::string executableHash;
    std::uint64_t allowedCapabilities = 0;
};

struct PeerIdentityGrant {
    ProcessRole role = ProcessRole::Unknown;
    std::uint32_t processId = 0;
    std::string buildId;
    std::string instanceNonce;
    std::string executableHash;
    std::uint64_t activationId = 0;
};

struct UiActivateDecisionInfo {
    bool started = false;
    std::uint64_t activationId = 0;
};

struct CastConfirmationRequestInfo {
    cast::ProtocolKind protocol = cast::ProtocolKind::None;
    std::uint64_t claimId = 0;
    std::string peerId;
    std::string peerDisplayName;
    std::string peerAddress;
    std::string mediaTitle;
    std::uint64_t remainingTimeoutMs = 0;
};

struct CastConfirmationDecisionInfo {
    cast::ProtocolKind protocol = cast::ProtocolKind::None;
    std::uint64_t claimId = 0;
    bool accepted = false;
};

std::optional<std::vector<std::uint8_t>>
encodeHelloPayload(const HelloInfo &hello, std::string &error);

std::optional<HelloInfo>
decodeHelloPayload(std::span<const std::uint8_t> payload, std::string &error);

std::optional<Message> makeHelloMessage(
    ConnectionChannel channel, ProcessRole source, ProcessRole destination,
    std::uint64_t messageId, const HelloInfo &hello, std::string &error);

std::optional<HelloInfo>
decodeHelloMessage(
    const Message &message, ConnectionChannel channel, std::string &error);

bool validateHello(
    const HelloInfo &hello, const HelloExpectation &expectation,
    std::string &error);

std::optional<std::vector<std::uint8_t>>
encodeHelloAckPayload(const HelloAckInfo &ack, std::string &error);

std::optional<HelloAckInfo>
decodeHelloAckPayload(std::span<const std::uint8_t> payload, std::string &error);

std::optional<Message> makeHelloAckMessage(
    ConnectionChannel channel, ProcessRole source, ProcessRole destination,
    std::uint64_t messageId, std::uint64_t correlationId,
    const HelloAckInfo &ack, std::string &error);

std::optional<HelloAckInfo>
decodeHelloAckMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    ConnectionChannel channel, std::string &error);

bool validateMessageDirection(
    ConnectionChannel channel, const MessageHeader &header,
    std::string &error);

std::optional<Message> makePeerIdentityGrantMessage(
    ProcessRole destination, std::uint64_t messageId,
    std::uint64_t activationId, const PeerIdentityGrant &grant,
    std::string &error);

std::optional<PeerIdentityGrant>
decodePeerIdentityGrantMessage(const Message &message, std::string &error);

std::optional<Message> makeUiActivateDecisionMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    std::uint64_t sessionId, const UiActivateDecisionInfo &decision,
    std::string &error);

std::optional<UiActivateDecisionInfo>
decodeUiActivateDecisionMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    std::string &error);

std::optional<Message> makeCastConfirmationRequestMessage(
    std::uint64_t messageId,
    const CastConfirmationRequestInfo &request,
    std::string &error);

std::optional<CastConfirmationRequestInfo>
decodeCastConfirmationRequestMessage(
    const Message &message, std::string &error);

std::optional<Message> makeCastConfirmationDecisionMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const CastConfirmationDecisionInfo &decision,
    std::string &error);

std::optional<CastConfirmationDecisionInfo>
decodeCastConfirmationDecisionMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    std::string &error);

std::optional<Message> makeCastConfirmationCancelMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    std::string &error);

std::optional<std::uint64_t> decodeCastConfirmationCancelMessage(
    const Message &message, std::string &error);

std::optional<Message> makeUiCastConfirmationRequestMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const CastConfirmationRequestInfo &request, std::string &error);

std::optional<CastConfirmationRequestInfo>
decodeUiCastConfirmationRequestMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    std::string &error);

std::optional<Message> makeUiCastConfirmationDecisionMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const CastConfirmationDecisionInfo &decision, std::string &error);

std::optional<CastConfirmationDecisionInfo>
decodeUiCastConfirmationDecisionMessage(
    const Message &message, std::uint64_t expectedRequestCorrelationId,
    std::string &error);

std::optional<Message> makeUiCastConfirmationCancelMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    std::string &error);

std::optional<std::uint64_t> decodeUiCastConfirmationCancelMessage(
    const Message &message, std::string &error);

} // namespace mcast::ipc
