// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/IpcProtocol.h"
#include "playback/PlaybackTypes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mcast::playback::wire {

struct PlaybackCommandResultWire {
    std::uint64_t commandId = 0;
    std::uint64_t generation = 0;
    std::uint64_t stateVersion = 0;
    PlaybackSubmissionStatus submissionStatus =
        PlaybackSubmissionStatus::InvalidCommand;
    PlaybackOutcomeStatus outcome =
        PlaybackOutcomeStatus::Rejected;
    std::string error;
};

std::optional<std::vector<std::uint8_t>>
encodeMediaRequest(const MediaRequest &request, std::string &error);
bool decodeMediaRequest(
    std::span<const std::uint8_t> bytes, MediaRequest &request,
    std::string &error);

std::optional<std::vector<std::uint8_t>>
encodePlaybackCommand(
    const PlaybackCommand &command, std::string &error);
bool decodePlaybackCommand(
    std::span<const std::uint8_t> bytes, PlaybackCommand &command,
    std::string &error);

std::optional<std::vector<std::uint8_t>>
encodePlaybackSnapshot(
    const PlaybackSnapshot &snapshot, std::string &error);
bool decodePlaybackSnapshot(
    std::span<const std::uint8_t> bytes, PlaybackSnapshot &snapshot,
    std::string &error);

std::optional<ipc::Message> makePlaybackCommandMessage(
    std::uint64_t commandId, const PlaybackCommand &command,
    std::string &error);
bool decodePlaybackCommandMessage(
    const ipc::Message &message, std::uint64_t &commandId,
    PlaybackCommand &command, std::string &error);

std::optional<ipc::Message> makePlaybackCommandResultMessage(
    const PlaybackCommandResultWire &result, std::string &error);
bool decodePlaybackCommandResultMessage(
    const ipc::Message &message, PlaybackCommandResultWire &result,
    std::string &error);

std::optional<ipc::Message> makePlaybackSnapshotRequestMessage(
    std::uint64_t messageId);
bool decodePlaybackSnapshotRequestMessage(
    const ipc::Message &message, std::uint64_t &messageId,
    std::string &error);
std::optional<ipc::Message> makePlaybackSnapshotResponseMessage(
    std::uint64_t messageId, std::uint64_t correlationId,
    const PlaybackSnapshot &snapshot, std::string &error);
std::optional<ipc::Message> makePlaybackStateMessage(
    ipc::MessageType type, std::uint64_t messageId,
    const PlaybackSnapshot &snapshot, std::string &error);
bool decodePlaybackSnapshotMessage(
    const ipc::Message &message, PlaybackSnapshot &snapshot,
    std::string &error);

} // namespace mcast::playback::wire
