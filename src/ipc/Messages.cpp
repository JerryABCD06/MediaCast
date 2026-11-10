// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/Messages.h"

#include <algorithm>
#include <cctype>
#include <limits>

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

} // namespace mcast::ipc
