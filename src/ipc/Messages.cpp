// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/Messages.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <utility>

namespace mcast::ipc {
namespace {

bool validToken(const std::string &value, std::size_t maxSize)
{
    if (value.empty() || value.size() > maxSize)
        return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-'
            || ch == ':';
    });
}

bool validHash(const std::string &value)
{
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

bool validCastProtocol(cast::ProtocolKind protocol)
{
    return protocol != cast::ProtocolKind::None
        && protocol <= cast::ProtocolKind::Vendor;
}

bool validUtf8(std::string_view value)
{
    std::size_t index = 0;
    while (index < value.size()) {
        const auto lead = static_cast<unsigned char>(value[index]);
        if (lead <= 0x7f) {
            ++index;
            continue;
        }

        std::size_t continuationCount = 0;
        std::uint32_t codePoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xe0) == 0xc0) {
            continuationCount = 1;
            codePoint = lead & 0x1f;
            minimum = 0x80;
        } else if ((lead & 0xf0) == 0xe0) {
            continuationCount = 2;
            codePoint = lead & 0x0f;
            minimum = 0x800;
        } else if ((lead & 0xf8) == 0xf0) {
            continuationCount = 3;
            codePoint = lead & 0x07;
            minimum = 0x10000;
        } else {
            return false;
        }
        if (index + continuationCount >= value.size())
            return false;
        for (std::size_t offset = 1;
             offset <= continuationCount; ++offset) {
            const auto next =
                static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80)
                return false;
            codePoint = (codePoint << 6) | (next & 0x3f);
        }
        if (codePoint < minimum || codePoint > 0x10ffff
            || (codePoint >= 0xd800 && codePoint <= 0xdfff)) {
            return false;
        }
        index += continuationCount + 1;
    }
    return true;
}

bool validCastText(const std::string &value, std::size_t maxSize)
{
    if (value.size() > maxSize || !validUtf8(value))
        return false;
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f)
            return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto first = static_cast<unsigned char>(value[index]);
        const auto second = index + 1 < value.size()
            ? static_cast<unsigned char>(value[index + 1]) : 0;
        if (first == 0xc2 && second >= 0x80 && second <= 0x9f)
            return false;
        if (first == 0xd8 && second == 0x9c)
            return false;
        if (first == 0xe2 && index + 2 < value.size()) {
            const auto third =
                static_cast<unsigned char>(value[index + 2]);
            if (second == 0x80
                && (third == 0x8e || third == 0x8f
                    || (third >= 0xa8 && third <= 0xae))) {
                return false;
            }
            if (second == 0x81
                && third >= 0xa6 && third <= 0xa9) {
                return false;
            }
        }
    }
    return true;
}

bool validCastPeerId(const std::string &value)
{
    return !value.empty() && validCastText(value, 256);
}

constexpr std::uint64_t kMaxCastConfirmationTimeoutMs = 300000;

} // namespace

std::optional<std::vector<std::uint8_t>>
encodeHelloPayload(const HelloInfo &hello, std::string &error)
{
    if (hello.processId == 0) {
        error = "Hello process id is zero";
        return std::nullopt;
    }
    if (!validToken(hello.buildId, 128)) {
        error = "Hello build id is empty or too long";
        return std::nullopt;
    }
    if (!validToken(hello.instanceNonce, 128)) {
        error = "Hello instance nonce is empty or too long";
        return std::nullopt;
    }
    if (!validHash(hello.executableHash)) {
        error = "Hello executable hash is empty or too long";
        return std::nullopt;
    }

    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::HelloProcessId),
            hello.processId)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::HelloBuildId),
            hello.buildId)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::HelloInstanceNonce),
            hello.instanceNonce)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::HelloExecutableHash),
            hello.executableHash)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::HelloCapabilities),
            hello.capabilities)) {
        error = "Hello payload exceeds protocol limits";
        return std::nullopt;
    }
    return std::move(builder).finish();
}

std::optional<HelloInfo>
decodeHelloPayload(std::span<const std::uint8_t> payload, std::string &error)
{
    PayloadReader reader(payload);
    if (!reader.valid()) {
        error = "Hello payload is malformed";
        return std::nullopt;
    }

    const auto processId = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::HelloProcessId));
    const auto buildId = reader.getString(
        static_cast<std::uint16_t>(FieldId::HelloBuildId));
    const auto instanceNonce = reader.getString(
        static_cast<std::uint16_t>(FieldId::HelloInstanceNonce));
    const auto executableHash = reader.getString(
        static_cast<std::uint16_t>(FieldId::HelloExecutableHash));
    const auto capabilities = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::HelloCapabilities));

    if (!processId || *processId == 0
        || *processId > std::numeric_limits<std::uint32_t>::max()) {
        error = "Hello has invalid process id";
        return std::nullopt;
    }
    if (!buildId || !validToken(*buildId, 128)) {
        error = "Hello has invalid build id";
        return std::nullopt;
    }
    if (!instanceNonce || !validToken(*instanceNonce, 128)) {
        error = "Hello has invalid instance nonce";
        return std::nullopt;
    }
    if (!executableHash || !validHash(*executableHash)) {
        error = "Hello has invalid executable hash";
        return std::nullopt;
    }
    if (!capabilities) {
        error = "Hello has no capabilities";
        return std::nullopt;
    }

    return HelloInfo{
        static_cast<std::uint32_t>(*processId),
        *buildId,
        *instanceNonce,
        *executableHash,
        *capabilities,
    };
}

std::optional<Message> makeHelloMessage(
    ConnectionChannel channel, ProcessRole source, ProcessRole destination,
    std::uint64_t messageId, const HelloInfo &hello, std::string &error)
{
    if (source == ProcessRole::Unknown || destination == ProcessRole::Unknown
        || source == destination) {
        error = "Hello has invalid source/destination roles";
        return std::nullopt;
    }
    auto payload = encodeHelloPayload(hello, error);
    if (!payload)
        return std::nullopt;

    Message message;
    message.header.type = MessageType::Hello;
    message.header.source = source;
    message.header.destination = destination;
    message.header.messageId = messageId;
    message.header.timestampMs = 0;
    message.payload = std::move(*payload);
    if (!validateMessageDirection(channel, message.header, error))
        return std::nullopt;
    return message;
}

std::optional<HelloInfo>
decodeHelloMessage(
    const Message &message, ConnectionChannel channel, std::string &error)
{
    if (message.header.type != MessageType::Hello) {
        error = "message is not Hello";
        return std::nullopt;
    }
    if (message.header.source == ProcessRole::Unknown
        || message.header.destination == ProcessRole::Unknown
        || message.header.source == message.header.destination) {
        error = "Hello has invalid source/destination roles";
        return std::nullopt;
    }
    if (!validateMessageDirection(channel, message.header, error))
        return std::nullopt;
    return decodeHelloPayload(message.payload, error);
}

bool validateHello(
    const HelloInfo &hello, const HelloExpectation &expectation,
    std::string &error)
{
    if (expectation.peerProcessId == 0
        || hello.processId != expectation.peerProcessId) {
        error = "Hello PID does not match the pipe peer";
        return false;
    }
    if (expectation.buildId.empty()
        || hello.buildId != expectation.buildId) {
        error = "Hello build id does not match this supervisor";
        return false;
    }
    if (expectation.instanceNonce.empty()
        || hello.instanceNonce != expectation.instanceNonce) {
        error = "Hello instance nonce does not match";
        return false;
    }
    if (expectation.executableHash.empty()
        || hello.executableHash != expectation.executableHash) {
        error = "Hello executable hash does not match";
        return false;
    }
    if ((hello.capabilities & ~expectation.allowedCapabilities) != 0) {
        error = "Hello advertises unknown capabilities";
        return false;
    }
    return true;
}

std::optional<std::vector<std::uint8_t>>
encodeHelloAckPayload(const HelloAckInfo &ack, std::string &error)
{
    if (ack.versionMajor != kProtocolVersionMajor
        || ack.versionMinor > kProtocolVersionMinor) {
        error = "HelloAck contains unsupported protocol version";
        return std::nullopt;
    }
    if (!validToken(ack.instanceNonce, 128)) {
        error = "HelloAck has invalid instance nonce";
        return std::nullopt;
    }
    if (ack.reason.size() > 512) {
        error = "HelloAck reason is too long";
        return std::nullopt;
    }

    PayloadBuilder builder;
    if (!builder.addBool(
            static_cast<std::uint16_t>(FieldId::HelloAckAccepted),
            ack.accepted)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::HelloAckVersionMajor),
            ack.versionMajor)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::HelloAckVersionMinor),
            ack.versionMinor)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::HelloAckInstanceNonce),
            ack.instanceNonce)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::HelloAckCapabilities),
            ack.capabilities)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::HelloAckReason),
            ack.reason)) {
        error = "HelloAck payload exceeds protocol limits";
        return std::nullopt;
    }
    return std::move(builder).finish();
}

std::optional<HelloAckInfo>
decodeHelloAckPayload(std::span<const std::uint8_t> payload, std::string &error)
{
    PayloadReader reader(payload);
    if (!reader.valid()) {
        error = "HelloAck payload is malformed";
        return std::nullopt;
    }

    const auto accepted = reader.getBool(
        static_cast<std::uint16_t>(FieldId::HelloAckAccepted));
    const auto major = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::HelloAckVersionMajor));
    const auto minor = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::HelloAckVersionMinor));
    const auto nonce = reader.getString(
        static_cast<std::uint16_t>(FieldId::HelloAckInstanceNonce));
    const auto capabilities = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::HelloAckCapabilities));
    const auto reason = reader.getString(
        static_cast<std::uint16_t>(FieldId::HelloAckReason));

    if (!accepted || !major || !minor || !nonce || !capabilities || !reason
        || *major != kProtocolVersionMajor
        || *minor > kProtocolVersionMinor
        || !validToken(*nonce, 128)
        || reason->size() > 512) {
        error = "HelloAck has invalid required fields";
        return std::nullopt;
    }

    return HelloAckInfo{
        *accepted,
        static_cast<std::uint16_t>(*major),
        static_cast<std::uint16_t>(*minor),
        *nonce,
        *capabilities,
        *reason,
    };
}

std::optional<Message> makeHelloAckMessage(
    ConnectionChannel channel, ProcessRole source, ProcessRole destination,
    std::uint64_t messageId, std::uint64_t correlationId,
    const HelloAckInfo &ack, std::string &error)
{
    if (source == ProcessRole::Unknown || destination == ProcessRole::Unknown
        || source == destination || correlationId == 0) {
        error = "HelloAck has invalid roles or correlation id";
        return std::nullopt;
    }
    auto payload = encodeHelloAckPayload(ack, error);
    if (!payload)
        return std::nullopt;

    Message message;
    message.header.type = MessageType::HelloAck;
    message.header.source = source;
    message.header.destination = destination;
    message.header.flags = MessageFlags::Response;
    message.header.messageId = messageId;
    message.header.correlationId = correlationId;
    message.payload = std::move(*payload);
    if (!validateMessageDirection(channel, message.header, error))
        return std::nullopt;
    return message;
}

std::optional<HelloAckInfo>
decodeHelloAckMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    ConnectionChannel channel, std::string &error)
{
    if (message.header.type != MessageType::HelloAck
        || expectedCorrelationId == 0
        || message.header.correlationId != expectedCorrelationId
        || !(static_cast<std::uint16_t>(message.header.flags)
             & static_cast<std::uint16_t>(MessageFlags::Response))) {
        error = "message is not a correlated HelloAck";
        return std::nullopt;
    }
    if (!validateMessageDirection(channel, message.header, error))
        return std::nullopt;
    return decodeHelloAckPayload(message.payload, error);
}

bool validateMessageDirection(
    ConnectionChannel channel, const MessageHeader &header,
    std::string &error)
{
    if (channel == ConnectionChannel::Control
        && header.source == ProcessRole::Shell
        && header.destination == ProcessRole::Shell
        && (header.type == MessageType::ShellHandoffRequest
            || header.type == MessageType::ShellHandoffAck
            || header.type == MessageType::ShellHandoffComplete
            || header.type == MessageType::ShellHandoffFinalAck))
        return true;

    if (header.source == ProcessRole::Unknown
        || header.destination == ProcessRole::Unknown
        || header.source == header.destination) {
        error = "invalid source/destination roles";
        return false;
    }

    if (channel == ConnectionChannel::Playback
        && (header.source == ProcessRole::Shell
            || header.destination == ProcessRole::Shell)) {
        error = "Shell is not allowed on the playback pipe";
        return false;
    }

    switch (header.type) {
    case MessageType::PlaybackCommand:
        if (channel == ConnectionChannel::Playback
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Ui)
            return true;
        break;
    case MessageType::PlaybackCommandResult:
    case MessageType::PlaybackStateDelta:
    case MessageType::PlaybackPlayerLost:
    case MessageType::PlaybackReconnected:
        if (channel == ConnectionChannel::Playback
            && header.source == ProcessRole::Ui
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::PlaybackSnapshotRequest:
        if (channel == ConnectionChannel::Playback
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Ui)
            return true;
        break;
    case MessageType::PlaybackSnapshotResponse:
        if (channel == ConnectionChannel::Playback
            && header.source == ProcessRole::Ui
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::SetReceivingEnabled:
        if (channel == ConnectionChannel::Command
            && header.source == ProcessRole::Shell
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::ReceivingState:
        if (channel == ConnectionChannel::Command
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::CastConfirmationRequest:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::CastConfirmationDecision:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::CastConfirmationCancel:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::PlaybackReconnectRequest:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::UiPlaybackReconnect:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && header.destination == ProcessRole::Ui)
            return true;
        break;
    case MessageType::UiCastConfirmationRequest:
    case MessageType::UiCastConfirmationCancel:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && header.destination == ProcessRole::Ui)
            return true;
        break;
    case MessageType::UiCastConfirmationDecision:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Ui
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::UiOpenReady:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::UiOpenRequest:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::UiActivateRequest:
        if (header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::UiActivateDecision:
        if (header.source == ProcessRole::Shell
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::PeerIdentityGrant:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && (header.destination == ProcessRole::Protocol
                || header.destination == ProcessRole::Ui))
            return true;
        break;
    case MessageType::UiFirstFrame:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Ui
            && header.destination == ProcessRole::Shell)
            return true;
        break;
    case MessageType::SessionSnapshotRequest:
        if (header.source == ProcessRole::Ui
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::SessionSnapshot:
        if (header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Ui)
            return true;
        break;
    case MessageType::ShellHandoffRequest:
    case MessageType::ShellHandoffAck:
    case MessageType::ShellHandoffComplete:
    case MessageType::ShellHandoffFinalAck:
        break;
    case MessageType::Hello:
        if (channel == ConnectionChannel::Control
            && header.destination == ProcessRole::Shell
            && (header.source == ProcessRole::Protocol
                || header.source == ProcessRole::Ui))
            return true;
        if (channel == ConnectionChannel::Playback
            && header.source == ProcessRole::Ui
            && header.destination == ProcessRole::Protocol)
            return true;
        break;
    case MessageType::HelloAck:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && (header.destination == ProcessRole::Protocol
                || header.destination == ProcessRole::Ui))
            return true;
        if (channel == ConnectionChannel::Playback
            && header.source == ProcessRole::Protocol
            && header.destination == ProcessRole::Ui)
            return true;
        break;
    case MessageType::Heartbeat:
        if (channel == ConnectionChannel::Control
            && header.destination == ProcessRole::Shell
            && header.source != ProcessRole::Shell)
            return true;
        if (channel == ConnectionChannel::Playback
            && ((header.source == ProcessRole::Ui
                 && header.destination == ProcessRole::Protocol)
                || (header.source == ProcessRole::Protocol
                    && header.destination == ProcessRole::Ui)))
            return true;
        break;
    case MessageType::HeartbeatAck:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && header.destination != ProcessRole::Shell)
            return true;
        if (channel == ConnectionChannel::Playback
            && ((header.source == ProcessRole::Ui
                 && header.destination == ProcessRole::Protocol)
                || (header.source == ProcessRole::Protocol
                    && header.destination == ProcessRole::Ui)))
            return true;
        break;
    case MessageType::ProcessState:
    case MessageType::Shutdown:
        if (channel == ConnectionChannel::Control
            && header.source == ProcessRole::Shell
            && header.destination != ProcessRole::Shell)
            return true;
        break;
    case MessageType::Error:
        if (channel == ConnectionChannel::Control
            && (header.source == ProcessRole::Shell
                || header.destination == ProcessRole::Shell))
            return true;
        if (channel == ConnectionChannel::Playback
            && ((header.source == ProcessRole::Ui
                 && header.destination == ProcessRole::Protocol)
                || (header.source == ProcessRole::Protocol
                    && header.destination == ProcessRole::Ui)))
            return true;
        break;
    default:
        if (channel == ConnectionChannel::Playback)
            return true;
        break;
    }

    error = "message direction is not allowed on this channel";
    return false;
}

std::optional<Message> makePeerIdentityGrantMessage(
    ProcessRole destination, std::uint64_t messageId,
    std::uint64_t activationId, const PeerIdentityGrant &grant,
    std::string &error)
{
    if (destination == ProcessRole::Unknown
        || grant.role == ProcessRole::Unknown
        || destination == grant.role
        || grant.processId == 0 || activationId == 0
        || !validToken(grant.buildId, 128)
        || !validToken(grant.instanceNonce, 128)
        || !validHash(grant.executableHash)) {
        error = "peer identity grant contains invalid fields";
        return std::nullopt;
    }

    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PeerRole),
            static_cast<std::uint8_t>(grant.role))
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PeerProcessId),
            grant.processId)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::PeerBuildId),
            grant.buildId)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::PeerInstanceNonce),
            grant.instanceNonce)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::PeerExecutableHash),
            grant.executableHash)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PeerActivationId),
            activationId)) {
        error = "peer identity grant exceeds protocol limits";
        return std::nullopt;
    }

    Message message;
    message.header.type = MessageType::PeerIdentityGrant;
    message.header.source = ProcessRole::Shell;
    message.header.destination = destination;
    message.header.messageId = messageId;
    message.header.sessionId = 1;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    if (message.payload.empty()) {
        error = "peer identity grant payload is empty";
        return std::nullopt;
    }
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error))
        return std::nullopt;
    return message;
}

std::optional<PeerIdentityGrant>
decodePeerIdentityGrantMessage(const Message &message, std::string &error)
{
    if (message.header.type != MessageType::PeerIdentityGrant
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid peer identity grant";
        return std::nullopt;
    }
    PayloadReader reader(message.payload);
    const auto role = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PeerRole));
    const auto processId = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PeerProcessId));
    const auto buildId = reader.getString(
        static_cast<std::uint16_t>(FieldId::PeerBuildId));
    const auto nonce = reader.getString(
        static_cast<std::uint16_t>(FieldId::PeerInstanceNonce));
    const auto hash = reader.getString(
        static_cast<std::uint16_t>(FieldId::PeerExecutableHash));
    const auto activationId = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PeerActivationId));
    if (!reader.valid() || !role || !processId || !buildId || !nonce
        || !hash || !activationId
        || (*role != static_cast<std::uint8_t>(ProcessRole::Protocol)
            && *role != static_cast<std::uint8_t>(ProcessRole::Ui))
        || static_cast<ProcessRole>(*role) == message.header.destination
        || *processId == 0
        || *processId > std::numeric_limits<std::uint32_t>::max()
        || !validToken(*buildId, 128)
        || !validToken(*nonce, 128)
        || !validHash(*hash)
        || *activationId == 0) {
        error = "peer identity grant has invalid fields";
        return std::nullopt;
    }
    return PeerIdentityGrant{
        static_cast<ProcessRole>(*role),
        static_cast<std::uint32_t>(*processId),
        *buildId,
        *nonce,
        *hash,
        *activationId,
    };
}

std::optional<Message> makeUiActivateDecisionMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    std::uint64_t sessionId, const UiActivateDecisionInfo &decision,
    std::string &error)
{
    if (messageId == 0 || correlationId == 0 || sessionId == 0
        || decision.activationId == 0) {
        error = "Ui activation decision contains invalid fields";
        return std::nullopt;
    }

    PayloadBuilder builder;
    if (!builder.addBool(
            static_cast<std::uint16_t>(FieldId::UiActivationStarted),
            decision.started)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::UiActivationId),
            decision.activationId)) {
        error = "Ui activation decision exceeds protocol limits";
        return std::nullopt;
    }

    Message message;
    message.header.type = MessageType::UiActivateDecision;
    message.header.source = ProcessRole::Shell;
    message.header.destination = ProcessRole::Protocol;
    message.header.flags = MessageFlags::Response;
    message.header.messageId = messageId;
    message.header.correlationId = correlationId;
    message.header.sessionId = sessionId;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    if (message.payload.empty()) {
        error = "Ui activation decision payload is empty";
        return std::nullopt;
    }
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error))
        return std::nullopt;
    return message;
}

std::optional<UiActivateDecisionInfo>
decodeUiActivateDecisionMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    std::string &error)
{
    if (message.header.type != MessageType::UiActivateDecision
        || message.header.correlationId != expectedCorrelationId
        || message.header.sessionId == 0
        || (static_cast<std::uint16_t>(message.header.flags)
            & static_cast<std::uint16_t>(MessageFlags::Response))
            == 0
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid Ui activation decision";
        return std::nullopt;
    }

    PayloadReader reader(message.payload);
    const auto started = reader.getBool(
        static_cast<std::uint16_t>(FieldId::UiActivationStarted));
    const auto activationId = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::UiActivationId));
    if (!reader.valid() || !started || !activationId
        || *activationId == 0) {
        error = "Ui activation decision has invalid fields";
        return std::nullopt;
    }
    return UiActivateDecisionInfo{*started, *activationId};
}

std::optional<Message> makeCastConfirmationRequestMessage(
    std::uint64_t messageId,
    const CastConfirmationRequestInfo &request,
    std::string &error)
{
    if (messageId == 0 || !validCastProtocol(request.protocol)
        || request.claimId == 0
        || request.remainingTimeoutMs == 0
        || request.remainingTimeoutMs > kMaxCastConfirmationTimeoutMs
        || !validCastPeerId(request.peerId)
        || !validCastText(request.peerDisplayName, 512)
        || !validCastText(request.peerAddress, 256)
        || !validCastText(request.mediaTitle, 1024)) {
        error = "cast confirmation request contains invalid fields";
        return std::nullopt;
    }

    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::CastProtocol),
            static_cast<std::uint8_t>(request.protocol))
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::CastClaimId),
            request.claimId)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::CastPeerId),
            request.peerId)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::CastPeerDisplayName),
            request.peerDisplayName)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::CastPeerAddress),
            request.peerAddress)
        || !builder.addString(
            static_cast<std::uint16_t>(FieldId::CastMediaTitle),
            request.mediaTitle)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::CastConfirmationTimeoutMs),
            request.remainingTimeoutMs)) {
        error = "cast confirmation request exceeds protocol limits";
        return std::nullopt;
    }

    Message message;
    message.header.type = MessageType::CastConfirmationRequest;
    message.header.source = ProcessRole::Protocol;
    message.header.destination = ProcessRole::Shell;
    message.header.messageId = messageId;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    if (message.payload.empty()) {
        error = "cast confirmation request payload is empty";
        return std::nullopt;
    }
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error))
        return std::nullopt;
    return message;
}

std::optional<CastConfirmationRequestInfo>
decodeCastConfirmationRequestMessage(
    const Message &message, std::string &error)
{
    if (message.header.type != MessageType::CastConfirmationRequest
        || message.header.messageId == 0
        || message.header.versionMinor < 1
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid cast confirmation request";
        return std::nullopt;
    }

    PayloadReader reader(message.payload);
    const auto protocol = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::CastProtocol));
    const auto claimId = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::CastClaimId));
    const auto peerId = reader.getString(
        static_cast<std::uint16_t>(FieldId::CastPeerId));
    const auto peerDisplayName = reader.getString(
        static_cast<std::uint16_t>(FieldId::CastPeerDisplayName));
    const auto peerAddress = reader.getString(
        static_cast<std::uint16_t>(FieldId::CastPeerAddress));
    const auto mediaTitle = reader.getString(
        static_cast<std::uint16_t>(FieldId::CastMediaTitle));
    const auto timeout = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::CastConfirmationTimeoutMs));
    if (!reader.valid() || !protocol || !claimId || !peerId
        || !peerDisplayName || !peerAddress || !mediaTitle || !timeout
        || *protocol > static_cast<std::uint8_t>(cast::ProtocolKind::Vendor)
        || *protocol == static_cast<std::uint8_t>(cast::ProtocolKind::None)
        || *claimId == 0 || *timeout == 0
        || *timeout > kMaxCastConfirmationTimeoutMs
        || !validCastPeerId(*peerId)
        || !validCastText(*peerDisplayName, 512)
        || !validCastText(*peerAddress, 256)
        || !validCastText(*mediaTitle, 1024)) {
        error = "cast confirmation request has invalid fields";
        return std::nullopt;
    }

    return CastConfirmationRequestInfo{
        static_cast<cast::ProtocolKind>(*protocol),
        *claimId,
        *peerId,
        *peerDisplayName,
        *peerAddress,
        *mediaTitle,
        *timeout,
    };
}

std::optional<Message> makeCastConfirmationDecisionMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const CastConfirmationDecisionInfo &decision,
    std::string &error)
{
    if (messageId == 0 || correlationId == 0
        || !validCastProtocol(decision.protocol)
        || decision.claimId == 0) {
        error = "cast confirmation decision contains invalid fields";
        return std::nullopt;
    }

    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::CastProtocol),
            static_cast<std::uint8_t>(decision.protocol))
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::CastClaimId),
            decision.claimId)
        || !builder.addBool(
            static_cast<std::uint16_t>(FieldId::CastConfirmationAccepted),
            decision.accepted)) {
        error = "cast confirmation decision exceeds protocol limits";
        return std::nullopt;
    }

    Message message;
    message.header.type = MessageType::CastConfirmationDecision;
    message.header.source = ProcessRole::Shell;
    message.header.destination = ProcessRole::Protocol;
    message.header.flags = MessageFlags::Response;
    message.header.messageId = messageId;
    message.header.correlationId = correlationId;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    if (message.payload.empty()) {
        error = "cast confirmation decision payload is empty";
        return std::nullopt;
    }
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error))
        return std::nullopt;
    return message;
}

std::optional<CastConfirmationDecisionInfo>
decodeCastConfirmationDecisionMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    std::string &error)
{
    if (message.header.type != MessageType::CastConfirmationDecision
        || message.header.messageId == 0
        || message.header.versionMinor < 1
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || message.header.correlationId != expectedCorrelationId
        || expectedCorrelationId == 0
        || (static_cast<std::uint16_t>(message.header.flags)
            & static_cast<std::uint16_t>(MessageFlags::Response))
            == 0
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid cast confirmation decision";
        return std::nullopt;
    }

    PayloadReader reader(message.payload);
    const auto protocol = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::CastProtocol));
    const auto claimId = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::CastClaimId));
    const auto accepted = reader.getBool(
        static_cast<std::uint16_t>(FieldId::CastConfirmationAccepted));
    if (!reader.valid() || !protocol || !claimId || !accepted
        || *protocol > static_cast<std::uint8_t>(cast::ProtocolKind::Vendor)
        || *protocol == static_cast<std::uint8_t>(cast::ProtocolKind::None)
        || *claimId == 0) {
        error = "cast confirmation decision has invalid fields";
        return std::nullopt;
    }
    return CastConfirmationDecisionInfo{
        static_cast<cast::ProtocolKind>(*protocol),
        *claimId,
        *accepted,
    };
}

std::optional<Message> makeCastConfirmationCancelMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    std::string &error)
{
    if (messageId == 0 || correlationId == 0) {
        error = "cast confirmation cancel contains invalid ids";
        return std::nullopt;
    }
    Message message;
    message.header.type = MessageType::CastConfirmationCancel;
    message.header.source = ProcessRole::Protocol;
    message.header.destination = ProcessRole::Shell;
    message.header.messageId = messageId;
    message.header.correlationId = correlationId;
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        return std::nullopt;
    }
    return message;
}

std::optional<std::uint64_t>
decodeCastConfirmationCancelMessage(
    const Message &message, std::string &error)
{
    if (message.header.type != MessageType::CastConfirmationCancel
        || message.header.versionMinor < 1
        || message.header.messageId == 0
        || message.header.correlationId == 0
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || !message.payload.empty()
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid cast confirmation cancel";
        return std::nullopt;
    }
    return message.header.correlationId;
}

std::optional<Message> makePlaybackReconnectRequestMessage(
    std::uint64_t messageId, std::string &error)
{
    if (messageId == 0) {
        error = "playback reconnect request has zero message id";
        return std::nullopt;
    }
    Message message;
    message.header.type = MessageType::PlaybackReconnectRequest;
    message.header.source = ProcessRole::Protocol;
    message.header.destination = ProcessRole::Shell;
    message.header.messageId = messageId;
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        return std::nullopt;
    }
    return message;
}

std::optional<std::uint64_t>
decodePlaybackReconnectRequestMessage(
    const Message &message, std::string &error)
{
    if (message.header.type
            != MessageType::PlaybackReconnectRequest
        || message.header.versionMinor < 1
        || message.header.messageId == 0
        || message.header.correlationId != 0
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || !message.payload.empty()
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid playback reconnect request";
        return std::nullopt;
    }
    return message.header.messageId;
}

std::optional<Message> makeUiPlaybackReconnectMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    std::string &error)
{
    if (messageId == 0 || correlationId == 0) {
        error = "UI playback reconnect contains invalid ids";
        return std::nullopt;
    }
    Message message;
    message.header.type = MessageType::UiPlaybackReconnect;
    message.header.source = ProcessRole::Shell;
    message.header.destination = ProcessRole::Ui;
    message.header.messageId = messageId;
    message.header.correlationId = correlationId;
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        return std::nullopt;
    }
    return message;
}

std::optional<std::uint64_t>
decodeUiPlaybackReconnectMessage(
    const Message &message, std::string &error)
{
    if (message.header.type != MessageType::UiPlaybackReconnect
        || message.header.versionMinor < 1
        || message.header.messageId == 0
        || message.header.correlationId == 0
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || !message.payload.empty()
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid UI playback reconnect";
        return std::nullopt;
    }
    return message.header.correlationId;
}

std::optional<Message> makeUiCastConfirmationRequestMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const CastConfirmationRequestInfo &request, std::string &error)
{
    if (correlationId == 0) {
        error = "UI cast confirmation request correlation is zero";
        return std::nullopt;
    }
    auto message = makeCastConfirmationRequestMessage(
        messageId, request, error);
    if (!message)
        return std::nullopt;
    message->header.type = MessageType::UiCastConfirmationRequest;
    message->header.source = ProcessRole::Shell;
    message->header.destination = ProcessRole::Ui;
    message->header.correlationId = correlationId;
    if (!validateMessageDirection(
            ConnectionChannel::Control, message->header, error)) {
        return std::nullopt;
    }
    return message;
}

std::optional<CastConfirmationRequestInfo>
decodeUiCastConfirmationRequestMessage(
    const Message &message, std::uint64_t expectedCorrelationId,
    std::string &error)
{
    if (message.header.type != MessageType::UiCastConfirmationRequest
        || message.header.versionMinor < 2
        || message.header.messageId == 0
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || message.header.correlationId != expectedCorrelationId
        || expectedCorrelationId == 0
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid UI cast confirmation request";
        return std::nullopt;
    }
    Message canonical = message;
    canonical.header.type = MessageType::CastConfirmationRequest;
    canonical.header.source = ProcessRole::Protocol;
    canonical.header.destination = ProcessRole::Shell;
    return decodeCastConfirmationRequestMessage(canonical, error);
}

std::optional<Message> makeUiCastConfirmationDecisionMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const CastConfirmationDecisionInfo &decision, std::string &error)
{
    auto message = makeCastConfirmationDecisionMessage(
        messageId, correlationId, decision, error);
    if (!message)
        return std::nullopt;
    message->header.type = MessageType::UiCastConfirmationDecision;
    message->header.source = ProcessRole::Ui;
    message->header.destination = ProcessRole::Shell;
    if (!validateMessageDirection(
            ConnectionChannel::Control, message->header, error)) {
        return std::nullopt;
    }
    return message;
}

std::optional<CastConfirmationDecisionInfo>
decodeUiCastConfirmationDecisionMessage(
    const Message &message, std::uint64_t expectedRequestCorrelationId,
    std::string &error)
{
    if (message.header.type != MessageType::UiCastConfirmationDecision
        || message.header.versionMinor < 2
        || message.header.messageId == 0
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || message.header.correlationId != expectedRequestCorrelationId
        || expectedRequestCorrelationId == 0
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid UI cast confirmation decision";
        return std::nullopt;
    }
    Message canonical = message;
    canonical.header.type = MessageType::CastConfirmationDecision;
    canonical.header.source = ProcessRole::Shell;
    canonical.header.destination = ProcessRole::Protocol;
    return decodeCastConfirmationDecisionMessage(
        canonical, expectedRequestCorrelationId, error);
}

std::optional<Message> makeUiCastConfirmationCancelMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    std::string &error)
{
    if (messageId == 0 || correlationId == 0) {
        error = "UI cast confirmation cancel contains invalid fields";
        return std::nullopt;
    }
    Message message;
    message.header.type = MessageType::UiCastConfirmationCancel;
    message.header.source = ProcessRole::Shell;
    message.header.destination = ProcessRole::Ui;
    message.header.messageId = messageId;
    message.header.correlationId = correlationId;
    if (!validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        return std::nullopt;
    }
    return message;
}

std::optional<std::uint64_t> decodeUiCastConfirmationCancelMessage(
    const Message &message, std::string &error)
{
    if (message.header.type != MessageType::UiCastConfirmationCancel
        || message.header.versionMinor < 2
        || message.header.messageId == 0
        || message.header.correlationId == 0
        || message.header.sessionId != 0
        || message.header.stateVersion != 0
        || !message.payload.empty()
        || !validateMessageDirection(
            ConnectionChannel::Control, message.header, error)) {
        error = "message is not a valid UI cast confirmation cancel";
        return std::nullopt;
    }
    return message.header.correlationId;
}

} // namespace mcast::ipc
