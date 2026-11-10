// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace mcast::ipc {

// Little-endian on the wire: 31 53 43 4D == "MCS1".
inline constexpr std::uint32_t kProtocolMagic = 0x3153434D;
inline constexpr std::uint16_t kProtocolVersionMajor = 1;
inline constexpr std::uint16_t kProtocolVersionMinor = 0;
inline constexpr std::size_t kHeaderSize = 64;
inline constexpr std::size_t kHeaderReservedOffset = 58;
inline constexpr std::size_t kHeaderReservedSize = 6;
inline constexpr std::size_t kMaxPayloadSize = 1024 * 1024;
inline constexpr std::uint16_t kMaxFieldCount = 64;
inline constexpr std::uint32_t kMaxFieldSize = 256 * 1024;

enum class ProcessRole : std::uint8_t {
    Unknown = 0,
    Shell = 1,
    Protocol = 2,
    Ui = 3,
};

enum class MessageType : std::uint16_t {
    Hello = 1,
    HelloAck = 2,
    Heartbeat = 3,
    HeartbeatAck = 4,
    ProcessState = 5,
    UiActivateRequest = 6,
    UiActivateDecision = 7,
    UiFirstFrame = 8,
    SessionSnapshotRequest = 9,
    SessionSnapshot = 10,
    Error = 11,
    Shutdown = 12,
    PeerIdentityGrant = 13,
    ShellHandoffRequest = 14,
    ShellHandoffAck = 15,
    ShellHandoffComplete = 16,
    ShellHandoffFinalAck = 17,
    UiOpenRequest = 18,
    UiOpenReady = 19,
    SetReceivingEnabled = 20,
    ReceivingState = 21,
    PlaybackCommand = 22,
    PlaybackCommandResult = 23,
    PlaybackSnapshotRequest = 24,
    PlaybackSnapshotResponse = 25,
    PlaybackStateDelta = 26,
    PlaybackPlayerLost = 27,
    PlaybackReconnected = 28,
};

enum class FieldType : std::uint8_t {
    String = 1,
    UInt64 = 2,
    Bool = 3,
    Bytes = 4,
};

enum class MessageFlags : std::uint16_t {
    None = 0,
    Response = 1u << 0,
    Idempotent = 1u << 1,
    Urgent = 1u << 2,
};

constexpr MessageFlags operator|(MessageFlags left, MessageFlags right) noexcept
{
    return static_cast<MessageFlags>(
        static_cast<std::uint16_t>(left) | static_cast<std::uint16_t>(right));
}

struct MessageHeader {
    std::uint16_t versionMajor = kProtocolVersionMajor;
    std::uint16_t versionMinor = kProtocolVersionMinor;
    MessageType type = MessageType::Error;
    ProcessRole source = ProcessRole::Unknown;
    ProcessRole destination = ProcessRole::Unknown;
    MessageFlags flags = MessageFlags::None;
    std::uint64_t messageId = 0;
    std::uint64_t correlationId = 0;
    std::uint64_t sessionId = 0;
    std::uint64_t stateVersion = 0;
    std::int64_t timestampMs = 0;
};

struct Message {
    MessageHeader header;
    std::vector<std::uint8_t> payload;
};

struct FieldView {
    std::uint16_t id = 0;
    FieldType type = FieldType::Bytes;
    std::span<const std::uint8_t> data;
};

class PayloadBuilder
{
public:
    bool addString(std::uint16_t id, std::string_view value);
    bool addUInt64(std::uint16_t id, std::uint64_t value);
    bool addBool(std::uint16_t id, bool value);
    bool addBytes(std::uint16_t id, std::span<const std::uint8_t> value);

    std::optional<std::vector<std::uint8_t>> finish() &&;
    bool valid() const noexcept { return m_valid; }

private:
    bool beginField(std::uint16_t id, FieldType type, std::uint32_t size);

    std::vector<std::uint8_t> m_bytes;
    std::unordered_set<std::uint16_t> m_ids;
    std::uint16_t m_count = 0;
    bool m_finished = false;
    bool m_valid = true;
};

class PayloadReader
{
public:
    explicit PayloadReader(std::span<const std::uint8_t> payload);

    bool valid() const noexcept { return m_valid; }
    std::optional<std::span<const std::uint8_t>>
    find(std::uint16_t id, FieldType expectedType) const;
    std::optional<std::string> getString(std::uint16_t id) const;
    std::optional<std::uint64_t> getUInt64(std::uint16_t id) const;
    std::optional<bool> getBool(std::uint16_t id) const;

private:
    std::vector<FieldView> m_fields;
    bool m_valid = true;
};

enum class DecodeStatus {
    Ok,
    NeedMoreData,
    InvalidMagic,
    UnsupportedVersion,
    InvalidHeader,
    PayloadTooLarge,
};

struct HeaderDecodeResult {
    DecodeStatus status = DecodeStatus::InvalidHeader;
    MessageHeader header;
};

struct FrameDecodeResult {
    DecodeStatus status = DecodeStatus::InvalidHeader;
    Message message;
    std::size_t consumed = 0;
    std::size_t required = 0;
};

std::vector<std::uint8_t> encodeFrame(const Message &message);
HeaderDecodeResult decodeHeader(std::span<const std::uint8_t> bytes);
FrameDecodeResult decodeFrame(std::span<const std::uint8_t> bytes);

} // namespace mcast::ipc
