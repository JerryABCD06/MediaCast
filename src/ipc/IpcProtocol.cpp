// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/IpcProtocol.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_set>

namespace mcast::ipc {
namespace {

void appendU16(std::vector<std::uint8_t> &out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

void appendU64(std::vector<std::uint8_t> &out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    return static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(bytes[offset + 1]) << 8;
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index)
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8);
    return value;
}

std::uint64_t readU64(std::span<const std::uint8_t> bytes, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int index = 0; index < 8; ++index)
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8);
    return value;
}

bool isKnownFieldType(FieldType type)
{
    return type == FieldType::String || type == FieldType::UInt64
        || type == FieldType::Bool || type == FieldType::Bytes;
}

} // namespace

bool PayloadBuilder::beginField(std::uint16_t id, FieldType type, std::uint32_t size)
{
    if (!m_valid || m_finished || m_count >= kMaxFieldCount || size > kMaxFieldSize)
        return m_valid = false;
    if (!m_ids.insert(id).second)
        return m_valid = false;
    const std::size_t nextSize = 4 + m_bytes.size() + 8 + size;
    if (nextSize > kMaxPayloadSize)
        return m_valid = false;

    appendU16(m_bytes, id);
    m_bytes.push_back(static_cast<std::uint8_t>(type));
    m_bytes.push_back(0);
    appendU32(m_bytes, size);
    ++m_count;
    return true;
}

bool PayloadBuilder::addString(std::uint16_t id, std::string_view value)
{
    if (value.size() > kMaxFieldSize)
        return m_valid = false;
    if (!beginField(id, FieldType::String, static_cast<std::uint32_t>(value.size())))
        return false;
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
    return true;
}

bool PayloadBuilder::addUInt64(std::uint16_t id, std::uint64_t value)
{
    if (!beginField(id, FieldType::UInt64, 8))
        return false;
    appendU64(m_bytes, value);
    return true;
}

bool PayloadBuilder::addBool(std::uint16_t id, bool value)
{
    if (!beginField(id, FieldType::Bool, 1))
        return false;
    m_bytes.push_back(value ? 1 : 0);
    return true;
}

bool PayloadBuilder::addBytes(
    std::uint16_t id, std::span<const std::uint8_t> value)
{
    if (value.size() > kMaxFieldSize)
        return m_valid = false;
    if (!beginField(id, FieldType::Bytes,
                    static_cast<std::uint32_t>(value.size())))
        return false;
    m_bytes.insert(m_bytes.end(), value.begin(), value.end());
    return true;
}

std::optional<std::vector<std::uint8_t>> PayloadBuilder::finish() &&
{
    if (m_finished || !m_valid)
        return std::nullopt;
    m_finished = true;
    std::vector<std::uint8_t> result;
    result.reserve(4 + m_bytes.size());
    appendU16(result, m_count);
    appendU16(result, 0);
    result.insert(result.end(), m_bytes.begin(), m_bytes.end());
    return result;
}

PayloadReader::PayloadReader(std::span<const std::uint8_t> payload)
{
    if (payload.size() < 4) {
        m_valid = false;
        return;
    }

    const std::uint16_t count = readU16(payload, 0);
    if (count > kMaxFieldCount || readU16(payload, 2) != 0) {
        m_valid = false;
        return;
    }

    std::size_t offset = 4;
    std::unordered_set<std::uint16_t> ids;
    m_fields.reserve(count);

    for (std::uint16_t index = 0; index < count; ++index) {
        constexpr std::size_t fieldHeaderSize = 8;
        if (offset + fieldHeaderSize > payload.size()) {
            m_valid = false;
            return;
        }

        const std::uint16_t id = readU16(payload, offset);
        const auto type = static_cast<FieldType>(payload[offset + 2]);
        const std::uint8_t reserved = payload[offset + 3];
        const std::uint32_t size = readU32(payload, offset + 4);
        offset += fieldHeaderSize;
        if (!ids.insert(id).second || !isKnownFieldType(type)
            || reserved != 0 || size > kMaxFieldSize
            || offset + size > payload.size()) {
            m_valid = false;
            return;
        }

        m_fields.push_back(
            { id, type, payload.subspan(offset, size) });
        offset += size;
    }

    if (offset != payload.size())
        m_valid = false;
}

std::optional<std::span<const std::uint8_t>>
PayloadReader::find(std::uint16_t id, FieldType expectedType) const
{
    if (!m_valid)
        return std::nullopt;
    for (const FieldView &field : m_fields) {
        if (field.id == id)
            return field.type == expectedType
                ? std::optional<std::span<const std::uint8_t>>(field.data)
                : std::nullopt;
    }
    return std::nullopt;
}

std::optional<std::string> PayloadReader::getString(std::uint16_t id) const
{
    const auto value = find(id, FieldType::String);
    if (!value)
        return std::nullopt;
    return std::string(reinterpret_cast<const char *>(value->data()),
                       value->size());
}

std::optional<std::uint64_t> PayloadReader::getUInt64(std::uint16_t id) const
{
    const auto value = find(id, FieldType::UInt64);
    if (!value || value->size() != 8)
        return std::nullopt;
    return readU64(*value, 0);
}

std::optional<bool> PayloadReader::getBool(std::uint16_t id) const
{
    const auto value = find(id, FieldType::Bool);
    if (!value || value->size() != 1
        || ((*value)[0] != 0 && (*value)[0] != 1))
        return std::nullopt;
    return (*value)[0] != 0;
}

std::vector<std::uint8_t> encodeFrame(const Message &message)
{
    if (message.payload.size() > kMaxPayloadSize)
        return {};

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + message.payload.size());
    appendU32(out, kProtocolMagic);
    appendU16(out, message.header.versionMajor);
    appendU16(out, message.header.versionMinor);
    appendU16(out, static_cast<std::uint16_t>(message.header.type));
    out.push_back(static_cast<std::uint8_t>(message.header.source));
    out.push_back(static_cast<std::uint8_t>(message.header.destination));
    appendU16(out, static_cast<std::uint16_t>(message.header.flags));
    appendU64(out, message.header.messageId);
    appendU64(out, message.header.correlationId);
    appendU64(out, message.header.sessionId);
    appendU64(out, message.header.stateVersion);
    appendU64(out, static_cast<std::uint64_t>(message.header.timestampMs));
    appendU32(out, static_cast<std::uint32_t>(message.payload.size()));
    out.resize(kHeaderSize, 0);
    out.insert(out.end(), message.payload.begin(), message.payload.end());
    return out;
}

HeaderDecodeResult decodeHeader(std::span<const std::uint8_t> bytes)
{
    HeaderDecodeResult result;
    if (bytes.size() < kHeaderSize) {
        result.status = DecodeStatus::NeedMoreData;
        return result;
    }
    if (readU32(bytes, 0) != kProtocolMagic) {
        result.status = DecodeStatus::InvalidMagic;
        return result;
    }

    result.header.versionMajor = readU16(bytes, 4);
    result.header.versionMinor = readU16(bytes, 6);
    if (result.header.versionMajor != kProtocolVersionMajor
        || result.header.versionMinor > kProtocolVersionMinor) {
        result.status = DecodeStatus::UnsupportedVersion;
        return result;
    }

    result.header.type = static_cast<MessageType>(readU16(bytes, 8));
    result.header.source = static_cast<ProcessRole>(bytes[10]);
    result.header.destination = static_cast<ProcessRole>(bytes[11]);
    result.header.flags = static_cast<MessageFlags>(readU16(bytes, 12));
    const auto messageType = static_cast<std::uint16_t>(result.header.type);
    const auto sourceRole = static_cast<std::uint8_t>(result.header.source);
    const auto destinationRole =
        static_cast<std::uint8_t>(result.header.destination);
    const auto flags = static_cast<std::uint16_t>(result.header.flags);
    if (messageType < static_cast<std::uint16_t>(MessageType::Hello)
        || messageType
            > static_cast<std::uint16_t>(
                MessageType::SettingsUpdateResult)
        || sourceRole > static_cast<std::uint8_t>(ProcessRole::Ui)
        || destinationRole > static_cast<std::uint8_t>(ProcessRole::Ui)
        || (flags & ~0x7u) != 0) {
        result.status = DecodeStatus::InvalidHeader;
        return result;
    }
    for (std::size_t index = kHeaderReservedOffset;
         index < kHeaderReservedOffset + kHeaderReservedSize; ++index) {
        if (bytes[index] != 0) {
            result.status = DecodeStatus::InvalidHeader;
            return result;
        }
    }
    result.header.messageId = readU64(bytes, 14);
    result.header.correlationId = readU64(bytes, 22);
    result.header.sessionId = readU64(bytes, 30);
    result.header.stateVersion = readU64(bytes, 38);
    result.header.timestampMs = static_cast<std::int64_t>(readU64(bytes, 46));

    const std::uint32_t payloadSize = readU32(bytes, 54);
    if (payloadSize > kMaxPayloadSize) {
        result.status = DecodeStatus::PayloadTooLarge;
        return result;
    }

    result.status = DecodeStatus::Ok;
    return result;
}

FrameDecodeResult decodeFrame(std::span<const std::uint8_t> bytes)
{
    FrameDecodeResult result;
    if (bytes.size() < kHeaderSize) {
        result.status = DecodeStatus::NeedMoreData;
        result.required = kHeaderSize;
        return result;
    }

    HeaderDecodeResult header = decodeHeader(bytes.first(kHeaderSize));
    result.status = header.status;
    result.required = kHeaderSize;
    if (header.status != DecodeStatus::Ok)
        return result;

    const std::uint32_t payloadSize = readU32(bytes, 54);
    result.required = kHeaderSize + payloadSize;
    if (bytes.size() < result.required) {
        result.status = DecodeStatus::NeedMoreData;
        return result;
    }

    result.message.header = header.header;
    result.message.payload.assign(
        bytes.begin() + kHeaderSize, bytes.begin() + result.required);
    result.consumed = result.required;
    return result;
}

} // namespace mcast::ipc
