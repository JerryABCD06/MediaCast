// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/PlaybackCodec.h"

#include "ipc/Messages.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <type_traits>

namespace mcast::playback::wire {
namespace {

using mcast::ipc::ConnectionChannel;
using mcast::ipc::FieldId;
using mcast::ipc::FieldType;
using mcast::ipc::Message;
using mcast::ipc::MessageFlags;
using mcast::ipc::MessageType;
using mcast::ipc::PayloadBuilder;
using mcast::ipc::PayloadReader;
using mcast::ipc::ProcessRole;

constexpr std::uint16_t kBlobVersion = 1;
constexpr std::uint32_t kMaxTextSize = 256 * 1024;
constexpr std::uint32_t kMaxMediaBlobSize = 512 * 1024;
constexpr std::uint32_t kMaxPictureControls = 64;
constexpr std::uint32_t kSnapshotWireOverhead = 64 * 1024;
constexpr std::uint32_t kMaxSnapshotBlobSize =
    mcast::ipc::kMaxPayloadSize - kSnapshotWireOverhead;

template <typename Enum>
bool enumAtMost(Enum value, Enum maximum)
{
    const auto underlying =
        static_cast<std::underlying_type_t<Enum>>(value);
    return underlying >= 0
        && underlying
            <= static_cast<std::underlying_type_t<Enum>>(maximum);
}

bool validateMediaRequest(const MediaRequest &request, std::string &error)
{
    if (!enumAtMost(request.kind, MediaKind::Image)
        || !enumAtMost(request.source, MediaSource::Local)) {
        error = "invalid media request enum";
        return false;
    }
    return true;
}

bool validatePlaybackCommand(
    const PlaybackCommand &command, std::string &error)
{
    if (command.expectedGeneration == 0
        || !enumAtMost(
            command.type, PlaybackCommandType::SetPeerConnected)
        || !enumAtMost(command.playMode, PlayMode::Direct)
        || !std::isfinite(command.doubleValue)) {
        error = "invalid playback command value";
        return false;
    }
    return validateMediaRequest(command.media, error);
}

bool validatePictureControl(
    const PictureControl &control, std::string &error)
{
    if (control.name.empty()
        || control.minimum > control.maximum
        || control.neutral < control.minimum
        || control.neutral > control.maximum
        || control.value < control.minimum
        || control.value > control.maximum) {
        error = "invalid picture control range";
        return false;
    }
    return true;
}

bool validatePlaybackSnapshot(
    const PlaybackSnapshot &snapshot, std::string &error)
{
    if (!enumAtMost(snapshot.state, PlaybackState::Paused)
        || !enumAtMost(snapshot.loadStatus, LoadStatus::Failed)
        || !enumAtMost(snapshot.playMode, PlayMode::Direct)
        || snapshot.generation == 0
        || snapshot.stateVersion == 0
        || !std::isfinite(snapshot.positionSeconds)
        || !std::isfinite(snapshot.durationSeconds)
        || snapshot.positionSeconds < 0.0
        || snapshot.durationSeconds < 0.0
        || snapshot.volumePercent < 0
        || snapshot.volumePercent > 100
        || snapshot.pictureControls.size() > kMaxPictureControls
        || !validateMediaRequest(snapshot.current, error)
        || !validateMediaRequest(snapshot.next, error)) {
        if (error.empty())
            error = "invalid playback snapshot value";
        return false;
    }
    for (const PictureControl &control : snapshot.pictureControls) {
        if (!validatePictureControl(control, error))
            return false;
    }
    return true;
}

class Writer
{
public:
    void u8(std::uint8_t value) { m_bytes.push_back(value); }
    void u16(std::uint16_t value)
    {
        for (int shift = 0; shift < 16; shift += 8)
            m_bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
    void u32(std::uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
            m_bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
    void u64(std::uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8)
            m_bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
    void i32(std::int32_t value)
    {
        u32(static_cast<std::uint32_t>(value));
    }
    void doubleValue(double value)
    {
        u64(std::bit_cast<std::uint64_t>(value));
    }
    bool string(std::string_view value, std::string &error)
    {
        if (value.size() > kMaxTextSize) {
            error = "playback string exceeds limit";
            return false;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        return true;
    }
    bool bytes(std::span<const std::uint8_t> value, std::string &error)
    {
        if (value.size() > kMaxMediaBlobSize) {
            error = "nested playback blob exceeds limit";
            return false;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
        return true;
    }
    std::vector<std::uint8_t> finish() { return std::move(m_bytes); }

private:
    std::vector<std::uint8_t> m_bytes;
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes)
        : m_bytes(bytes)
    {
    }

    bool u8(std::uint8_t &value)
    {
        if (m_offset + 1 > m_bytes.size())
            return false;
        value = m_bytes[m_offset++];
        return true;
    }
    bool u16(std::uint16_t &value)
    {
        if (m_offset + 2 > m_bytes.size())
            return false;
        value = 0;
        for (int shift = 0; shift < 16; shift += 8)
            value |= static_cast<std::uint16_t>(m_bytes[m_offset++]) << shift;
        return true;
    }
    bool u32(std::uint32_t &value)
    {
        if (m_offset + 4 > m_bytes.size())
            return false;
        value = 0;
        for (int shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(m_bytes[m_offset++]) << shift;
        return true;
    }
    bool u64(std::uint64_t &value)
    {
        if (m_offset + 8 > m_bytes.size())
            return false;
        value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(m_bytes[m_offset++]) << shift;
        return true;
    }
    bool i32(std::int32_t &value)
    {
        std::uint32_t raw = 0;
        if (!u32(raw))
            return false;
        value = static_cast<std::int32_t>(raw);
        return true;
    }
    bool doubleValue(double &value)
    {
        std::uint64_t raw = 0;
        if (!u64(raw))
            return false;
        value = std::bit_cast<double>(raw);
        return true;
    }
    bool string(std::string &value, std::string &error)
    {
        std::uint32_t size = 0;
        if (!u32(size) || size > kMaxTextSize
            || m_offset + size > m_bytes.size()) {
            error = "invalid playback string";
            return false;
        }
        value.assign(
            reinterpret_cast<const char *>(m_bytes.data() + m_offset),
            size);
        m_offset += size;
        return true;
    }
    bool bytes(
        std::span<const std::uint8_t> &value, std::string &error)
    {
        std::uint32_t size = 0;
        if (!u32(size) || size > kMaxMediaBlobSize
            || m_offset + size > m_bytes.size()) {
            error = "invalid nested playback blob";
            return false;
        }
        value = m_bytes.subspan(m_offset, size);
        m_offset += size;
        return true;
    }
    bool done() const { return m_offset == m_bytes.size(); }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_offset = 0;
};

bool encodeMediaRequestBlob(
    const MediaRequest &request, Writer &writer, std::string &error)
{
    if (!validateMediaRequest(request, error))
        return false;
    writer.u16(kBlobVersion);
    if (!writer.string(request.uri, error)
        || !writer.string(request.metadata, error)
        || !writer.string(request.title, error)
        || !writer.string(request.artist, error)
        || !writer.string(request.album, error)
        || !writer.string(request.description, error)
        || !writer.string(request.protocol, error))
        return false;
    writer.u8(static_cast<std::uint8_t>(request.kind));
    writer.u8(static_cast<std::uint8_t>(request.source));
    writer.u8(request.mediaSizeBytes ? 1 : 0);
    if (request.mediaSizeBytes)
        writer.u64(*request.mediaSizeBytes);
    return true;
}

bool decodeMediaRequestBlob(
    std::span<const std::uint8_t> bytes, MediaRequest &request,
    std::string &error)
{
    Reader reader(bytes);
    std::uint16_t version = 0;
    std::uint8_t kind = 0;
    std::uint8_t source = 0;
    std::uint8_t hasSize = 0;
    if (!reader.u16(version) || version != kBlobVersion
        || !reader.string(request.uri, error)
        || !reader.string(request.metadata, error)
        || !reader.string(request.title, error)
        || !reader.string(request.artist, error)
        || !reader.string(request.album, error)
        || !reader.string(request.description, error)
        || !reader.string(request.protocol, error)
        || !reader.u8(kind)
        || !reader.u8(source)
        || !reader.u8(hasSize)) {
        if (error.empty())
            error = "invalid media request blob";
        return false;
    }
    if (!enumAtMost(static_cast<MediaKind>(kind), MediaKind::Image)
        || !enumAtMost(static_cast<MediaSource>(source), MediaSource::Local)
        || hasSize > 1) {
        error = "invalid media request enum";
        return false;
    }
    request.kind = static_cast<MediaKind>(kind);
    request.source = static_cast<MediaSource>(source);
    if (hasSize) {
        std::uint64_t size = 0;
        if (!reader.u64(size)) {
            error = "invalid media size";
            return false;
        }
        request.mediaSizeBytes = size;
    } else {
        request.mediaSizeBytes.reset();
    }
    if (!reader.done()) {
        error = "media request has trailing bytes";
        return false;
    }
    return validateMediaRequest(request, error);
}

std::uint64_t secondsToMs(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return 0;
    const long double ms = static_cast<long double>(seconds) * 1000.0L;
    if (ms >= static_cast<long double>(
            std::numeric_limits<std::uint64_t>::max()))
        return std::numeric_limits<std::uint64_t>::max();
    return static_cast<std::uint64_t>(ms);
}

bool messageDirection(
    const Message &message, MessageType type, ProcessRole source,
    ConnectionChannel channel, std::string &error)
{
    return message.header.type == type
        && message.header.source == source
        && mcast::ipc::validateMessageDirection(
            channel, message.header, error);
}

} // namespace

std::optional<std::vector<std::uint8_t>>
encodeMediaRequest(const MediaRequest &request, std::string &error)
{
    Writer writer;
    if (!encodeMediaRequestBlob(request, writer, error))
        return std::nullopt;
    return writer.finish();
}

bool decodeMediaRequest(
    std::span<const std::uint8_t> bytes, MediaRequest &request,
    std::string &error)
{
    return decodeMediaRequestBlob(bytes, request, error);
}

std::optional<std::vector<std::uint8_t>>
encodePlaybackCommand(const PlaybackCommand &command, std::string &error)
{
    if (!validatePlaybackCommand(command, error))
        return std::nullopt;
    Writer writer;
    writer.u16(kBlobVersion);
    writer.u8(static_cast<std::uint8_t>(command.type));
    writer.u64(command.expectedGeneration);
    writer.u8(static_cast<std::uint8_t>(command.playMode));
    if (!writer.string(command.text, error))
        return std::nullopt;
    writer.i32(command.intValue);
    writer.doubleValue(command.doubleValue);
    writer.u8(command.boolValue ? 1 : 0);
    std::vector<std::uint8_t> media;
    if (!command.media.uri.empty()
        || !command.media.metadata.empty()) {
        auto encoded = encodeMediaRequest(command.media, error);
        if (!encoded)
            return std::nullopt;
        media = std::move(*encoded);
    }
    if (!writer.bytes(media, error))
        return std::nullopt;
    return writer.finish();
}

bool decodePlaybackCommand(
    std::span<const std::uint8_t> bytes, PlaybackCommand &command,
    std::string &error)
{
    Reader reader(bytes);
    std::uint16_t version = 0;
    std::uint8_t type = 0;
    std::uint8_t playMode = 0;
    std::uint8_t boolValue = 0;
    std::span<const std::uint8_t> mediaBytes;
    if (!reader.u16(version) || version != kBlobVersion
        || !reader.u8(type)
        || !reader.u64(command.expectedGeneration)
        || !reader.u8(playMode)
        || !reader.string(command.text, error)
        || !reader.i32(command.intValue)
        || !reader.doubleValue(command.doubleValue)
        || !reader.u8(boolValue)
        || !reader.bytes(mediaBytes, error)
        || !reader.done()) {
        if (error.empty())
            error = "invalid playback command blob";
        return false;
    }
    if (command.expectedGeneration == 0
        || !enumAtMost(
            static_cast<PlaybackCommandType>(type),
            PlaybackCommandType::SetPeerConnected)
        || !enumAtMost(
            static_cast<PlayMode>(playMode), PlayMode::Direct)
        || boolValue > 1
        || !std::isfinite(command.doubleValue)) {
        error = "invalid playback command value";
        return false;
    }
    command.type = static_cast<PlaybackCommandType>(type);
    command.playMode = static_cast<PlayMode>(playMode);
    command.boolValue = boolValue != 0;
    if (!mediaBytes.empty()
        && !decodeMediaRequestBlob(mediaBytes, command.media, error))
        return false;
    return validatePlaybackCommand(command, error);
}

std::optional<std::vector<std::uint8_t>>
encodePlaybackSnapshot(const PlaybackSnapshot &snapshot, std::string &error)
{
    if (!validatePlaybackSnapshot(snapshot, error))
        return std::nullopt;
    auto current = encodeMediaRequest(snapshot.current, error);
    auto next = encodeMediaRequest(snapshot.next, error);
    if (!current || !next)
        return std::nullopt;

    Writer controls;
    controls.u16(kBlobVersion);
    controls.u16(
        static_cast<std::uint16_t>(snapshot.pictureControls.size()));
    for (const PictureControl &control : snapshot.pictureControls) {
        if (!controls.string(control.name, error))
            return std::nullopt;
        controls.i32(control.minimum);
        controls.i32(control.maximum);
        controls.i32(control.neutral);
        controls.i32(control.value);
    }

    Writer writer;
    writer.u16(kBlobVersion);
    writer.u8(static_cast<std::uint8_t>(snapshot.state));
    writer.u8(static_cast<std::uint8_t>(snapshot.loadStatus));
    writer.u8(static_cast<std::uint8_t>(snapshot.playMode));
    std::uint8_t flags = 0;
    flags |= snapshot.hasNext ? 1u << 0 : 0;
    flags |= snapshot.hasPrevious ? 1u << 1 : 0;
    flags |= snapshot.peerConnected ? 1u << 2 : 0;
    flags |= snapshot.playerAvailable ? 1u << 3 : 0;
    flags |= snapshot.muted ? 1u << 4 : 0;
    writer.u8(flags);
    writer.u64(snapshot.generation);
    writer.u64(snapshot.stateVersion);
    writer.u64(secondsToMs(snapshot.positionSeconds));
    writer.u64(secondsToMs(snapshot.durationSeconds));
    writer.i32(snapshot.volumePercent);
    if (!writer.bytes(*current, error)
        || !writer.bytes(*next, error)
        || !writer.bytes(controls.finish(), error))
        return std::nullopt;
    auto result = writer.finish();
    if (result.size() > kMaxSnapshotBlobSize) {
        error = "playback snapshot exceeds limit";
        return std::nullopt;
    }
    return result;
}

bool decodePlaybackSnapshot(
    std::span<const std::uint8_t> bytes, PlaybackSnapshot &snapshot,
    std::string &error)
{
    if (bytes.size() > kMaxSnapshotBlobSize) {
        error = "playback snapshot exceeds limit";
        return false;
    }
    Reader reader(bytes);
    std::uint16_t version = 0;
    std::uint8_t state = 0;
    std::uint8_t loadStatus = 0;
    std::uint8_t playMode = 0;
    std::uint8_t flags = 0;
    std::uint64_t positionMs = 0;
    std::uint64_t durationMs = 0;
    std::span<const std::uint8_t> currentBytes;
    std::span<const std::uint8_t> nextBytes;
    std::span<const std::uint8_t> controlsBytes;
    if (!reader.u16(version) || version != kBlobVersion
        || !reader.u8(state)
        || !reader.u8(loadStatus)
        || !reader.u8(playMode)
        || !reader.u8(flags)
        || !reader.u64(snapshot.generation)
        || !reader.u64(snapshot.stateVersion)
        || !reader.u64(positionMs)
        || !reader.u64(durationMs)
        || !reader.i32(snapshot.volumePercent)
        || !reader.bytes(currentBytes, error)
        || !reader.bytes(nextBytes, error)
        || !reader.bytes(controlsBytes, error)
        || !reader.done()) {
        if (error.empty())
            error = "invalid playback snapshot blob";
        return false;
    }
    if (!enumAtMost(
            static_cast<PlaybackState>(state),
            PlaybackState::Paused)
        || !enumAtMost(
            static_cast<LoadStatus>(loadStatus), LoadStatus::Failed)
        || !enumAtMost(
            static_cast<PlayMode>(playMode), PlayMode::Direct)
        || (flags & ~0x1Fu) != 0
        || snapshot.generation == 0
        || snapshot.stateVersion == 0
        || snapshot.volumePercent < 0
        || snapshot.volumePercent > 100) {
        error = "invalid playback snapshot value";
        return false;
    }
    snapshot.state = static_cast<PlaybackState>(state);
    snapshot.loadStatus = static_cast<LoadStatus>(loadStatus);
    snapshot.playMode = static_cast<PlayMode>(playMode);
    snapshot.hasNext = (flags & (1u << 0)) != 0;
    snapshot.hasPrevious = (flags & (1u << 1)) != 0;
    snapshot.peerConnected = (flags & (1u << 2)) != 0;
    snapshot.playerAvailable = (flags & (1u << 3)) != 0;
    snapshot.muted = (flags & (1u << 4)) != 0;
    snapshot.positionSeconds = static_cast<double>(positionMs) / 1000.0;
    snapshot.durationSeconds = static_cast<double>(durationMs) / 1000.0;
    if (!decodeMediaRequestBlob(currentBytes, snapshot.current, error)
        || !decodeMediaRequestBlob(nextBytes, snapshot.next, error))
        return false;

    Reader controls(controlsBytes);
    std::uint16_t controlVersion = 0;
    std::uint16_t count = 0;
    if (!controls.u16(controlVersion)
        || controlVersion != kBlobVersion
        || !controls.u16(count)
        || count > kMaxPictureControls) {
        error = "invalid picture controls blob";
        return false;
    }
    snapshot.pictureControls.clear();
    snapshot.pictureControls.reserve(count);
    for (std::uint16_t index = 0; index < count; ++index) {
        PictureControl control;
        if (!controls.string(control.name, error)
            || !controls.i32(control.minimum)
            || !controls.i32(control.maximum)
            || !controls.i32(control.neutral)
            || !controls.i32(control.value)) {
            if (error.empty())
                error = "invalid picture control";
            return false;
        }
        if (control.name.empty()
            || control.minimum > control.maximum
            || control.neutral < control.minimum
            || control.neutral > control.maximum
            || control.value < control.minimum
            || control.value > control.maximum) {
            error = "invalid picture control range";
            return false;
        }
        snapshot.pictureControls.push_back(std::move(control));
    }
    if (!controls.done()) {
        error = "picture controls have trailing bytes";
        return false;
    }
    return validatePlaybackSnapshot(snapshot, error);
}

std::optional<Message> makePlaybackCommandMessage(
    std::uint64_t commandId, const PlaybackCommand &command,
    std::string &error)
{
    if (commandId == 0) {
        error = "playback command ID is zero";
        return std::nullopt;
    }
    auto blob = encodePlaybackCommand(command, error);
    if (!blob)
        return std::nullopt;
    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackGeneration),
            command.expectedGeneration)
        || !builder.addBytes(
            static_cast<std::uint16_t>(FieldId::PlaybackCommandBlob),
            *blob)) {
        error = "cannot build playback command payload";
        return std::nullopt;
    }
    Message message;
    message.header.type = MessageType::PlaybackCommand;
    message.header.source = ProcessRole::Protocol;
    message.header.destination = ProcessRole::Ui;
    message.header.messageId = commandId;
    message.header.sessionId = 1;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    return message;
}

bool decodePlaybackCommandMessage(
    const Message &message, std::uint64_t &commandId,
    PlaybackCommand &command, std::string &error)
{
    if (!messageDirection(
            message, MessageType::PlaybackCommand,
            ProcessRole::Protocol, ConnectionChannel::Playback, error))
        return false;
    if (message.header.messageId == 0) {
        error = "playback command ID is zero";
        return false;
    }
    const PayloadReader reader(message.payload);
    const auto generation = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PlaybackGeneration));
    const auto blob = reader.find(
        static_cast<std::uint16_t>(FieldId::PlaybackCommandBlob),
        FieldType::Bytes);
    if (!reader.valid() || !generation || !blob
        || !decodePlaybackCommand(*blob, command, error))
        return false;
    if (command.expectedGeneration != *generation) {
        error = "playback generation field mismatch";
        return false;
    }
    commandId = message.header.messageId;
    return true;
}

std::optional<Message> makePlaybackCommandResultMessage(
    const PlaybackCommandResultWire &result, std::string &error)
{
    if (result.commandId == 0 || result.generation == 0
        || result.stateVersion == 0
        || !enumAtMost(
            result.submissionStatus,
            PlaybackSubmissionStatus::SessionEnding)
        || !enumAtMost(
            result.outcome, PlaybackOutcomeStatus::StaleGeneration)) {
        error = "invalid playback result";
        return std::nullopt;
    }
    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackGeneration),
            result.generation)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackSubmissionStatus),
            static_cast<std::uint64_t>(result.submissionStatus))
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackOutcomeStatus),
            static_cast<std::uint64_t>(result.outcome))
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackStateVersion),
            result.stateVersion)
        || (!result.error.empty()
            && !builder.addString(
                static_cast<std::uint16_t>(FieldId::ErrorText),
                result.error))) {
        error = "cannot build playback result payload";
        return std::nullopt;
    }
    Message message;
    message.header.type = MessageType::PlaybackCommandResult;
    message.header.source = ProcessRole::Ui;
    message.header.destination = ProcessRole::Protocol;
    message.header.flags = MessageFlags::Response;
    message.header.messageId = 1;
    message.header.correlationId = result.commandId;
    message.header.sessionId = 1;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    return message;
}

bool decodePlaybackCommandResultMessage(
    const Message &message, PlaybackCommandResultWire &result,
    std::string &error)
{
    if (!messageDirection(
            message, MessageType::PlaybackCommandResult,
            ProcessRole::Ui, ConnectionChannel::Playback, error)
        || message.header.messageId == 0
        || message.header.correlationId == 0
        || (static_cast<std::uint16_t>(message.header.flags)
            & static_cast<std::uint16_t>(MessageFlags::Response))
            == 0) {
        if (error.empty())
            error = "invalid playback result envelope";
        return false;
    }
    const PayloadReader reader(message.payload);
    const auto generation = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PlaybackGeneration));
    const auto submission = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PlaybackSubmissionStatus));
    const auto outcome = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PlaybackOutcomeStatus));
    const auto stateVersion = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PlaybackStateVersion));
    const auto text = reader.getString(
        static_cast<std::uint16_t>(FieldId::ErrorText));
    const auto maxSubmission = static_cast<std::uint64_t>(
        static_cast<std::underlying_type_t<PlaybackSubmissionStatus>>(
            PlaybackSubmissionStatus::SessionEnding));
    const auto maxOutcome = static_cast<std::uint64_t>(
        static_cast<std::underlying_type_t<PlaybackOutcomeStatus>>(
            PlaybackOutcomeStatus::StaleGeneration));
    if (!reader.valid() || !generation || !submission || !outcome
        || !stateVersion || *generation == 0 || *stateVersion == 0
        || *submission > maxSubmission || *outcome > maxOutcome) {
        error = "invalid playback result payload";
        return false;
    }
    result.commandId = message.header.correlationId;
    result.generation = *generation;
    result.submissionStatus =
        static_cast<PlaybackSubmissionStatus>(*submission);
    result.outcome = static_cast<PlaybackOutcomeStatus>(*outcome);
    result.stateVersion = *stateVersion;
    result.error = text.value_or("");
    return true;
}

std::optional<Message> makePlaybackSnapshotRequestMessage(
    std::uint64_t messageId)
{
    if (messageId == 0)
        return std::nullopt;
    Message message;
    message.header.type = MessageType::PlaybackSnapshotRequest;
    message.header.source = ProcessRole::Protocol;
    message.header.destination = ProcessRole::Ui;
    message.header.messageId = messageId;
    message.header.sessionId = 1;
    return message;
}

bool decodePlaybackSnapshotRequestMessage(
    const Message &message, std::uint64_t &messageId,
    std::string &error)
{
    if (!messageDirection(
            message, MessageType::PlaybackSnapshotRequest,
            ProcessRole::Protocol, ConnectionChannel::Playback, error))
        return false;
    if (message.header.messageId == 0) {
        error = "snapshot request ID is zero";
        return false;
    }
    if (!message.payload.empty()) {
        error = "snapshot request payload must be empty";
        return false;
    }
    messageId = message.header.messageId;
    return true;
}

std::optional<Message> makePlaybackSnapshotResponseMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const PlaybackSnapshot &snapshot, std::string &error)
{
    if (messageId == 0 || correlationId == 0) {
        error = "playback snapshot IDs are zero";
        return std::nullopt;
    }
    auto blob = encodePlaybackSnapshot(snapshot, error);
    if (!blob)
        return std::nullopt;
    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackGeneration),
            snapshot.generation)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackStateVersion),
            snapshot.stateVersion)
        || !builder.addBytes(
            static_cast<std::uint16_t>(FieldId::PlaybackSnapshotBlob),
            *blob)) {
        error = "cannot build playback snapshot payload";
        return std::nullopt;
    }
    Message message;
    message.header.type = MessageType::PlaybackSnapshotResponse;
    message.header.source = ProcessRole::Ui;
    message.header.destination = ProcessRole::Protocol;
    message.header.flags = MessageFlags::Response;
    message.header.messageId = messageId;
    message.header.correlationId = correlationId;
    message.header.sessionId = 1;
    message.header.stateVersion = snapshot.stateVersion;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    return message;
}

std::optional<Message> makePlaybackStateMessage(
    MessageType type, std::uint64_t messageId,
    const PlaybackSnapshot &snapshot, std::string &error)
{
    if (messageId == 0) {
        error = "playback state message ID is zero";
        return std::nullopt;
    }
    if (type != MessageType::PlaybackStateDelta
        && type != MessageType::PlaybackPlayerLost
        && type != MessageType::PlaybackReconnected) {
        error = "invalid playback state message type";
        return std::nullopt;
    }
    auto blob = encodePlaybackSnapshot(snapshot, error);
    if (!blob)
        return std::nullopt;
    PayloadBuilder builder;
    if (!builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackGeneration),
            snapshot.generation)
        || !builder.addUInt64(
            static_cast<std::uint16_t>(FieldId::PlaybackStateVersion),
            snapshot.stateVersion)
        || !builder.addBytes(
            static_cast<std::uint16_t>(FieldId::PlaybackSnapshotBlob),
            *blob)) {
        error = "cannot build playback state payload";
        return std::nullopt;
    }
    Message message;
    message.header.type = type;
    message.header.source = ProcessRole::Ui;
    message.header.destination = ProcessRole::Protocol;
    message.header.messageId = messageId;
    message.header.sessionId = 1;
    message.header.stateVersion = snapshot.stateVersion;
    message.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    return message;
}

bool decodePlaybackSnapshotMessage(
    const Message &message, PlaybackSnapshot &snapshot,
    std::string &error)
{
    if (message.header.type != MessageType::PlaybackSnapshotResponse
        && message.header.type != MessageType::PlaybackStateDelta
        && message.header.type != MessageType::PlaybackPlayerLost
        && message.header.type != MessageType::PlaybackReconnected) {
        error = "message is not a playback snapshot";
        return false;
    }
    if (message.header.messageId == 0) {
        error = "playback state message ID is zero";
        return false;
    }
    const auto source = message.header.type
            == MessageType::PlaybackSnapshotResponse
        ? ProcessRole::Ui : ProcessRole::Ui;
    if (!messageDirection(
            message, message.header.type, source,
            ConnectionChannel::Playback, error))
        return false;
    if (message.header.type == MessageType::PlaybackSnapshotResponse
        && message.header.correlationId == 0) {
        error = "snapshot response has no correlation";
        return false;
    }
    const PayloadReader reader(message.payload);
    const auto generation = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PlaybackGeneration));
    const auto stateVersion = reader.getUInt64(
        static_cast<std::uint16_t>(FieldId::PlaybackStateVersion));
    const auto blob = reader.find(
        static_cast<std::uint16_t>(FieldId::PlaybackSnapshotBlob),
        FieldType::Bytes);
    if (!reader.valid() || !generation || !stateVersion || !blob
        || !decodePlaybackSnapshot(*blob, snapshot, error))
        return false;
    if (snapshot.generation != *generation
        || snapshot.stateVersion != *stateVersion) {
        error = "playback snapshot version mismatch";
        return false;
    }
    return true;
}

} // namespace mcast::playback::wire
