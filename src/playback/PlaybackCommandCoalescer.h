// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackTypes.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace mcast::playback {

// Holds commands which have not reached the Ui process yet. It only removes
// commands which are provably superseded by a newer command with the same
// property; media boundaries and relative actions always remain ordered.
class PlaybackCommandCoalescer
{
public:
    struct QueuedCommand {
        std::uint64_t commandId = 0;
        PlaybackCommand command;
        std::vector<std::uint64_t> aliases;
    };

    struct EnqueueResult {
        bool queued = false;
        std::vector<std::uint64_t> supersededCommandIds;
    };

    explicit PlaybackCommandCoalescer(
        std::size_t capacity = 256);

    EnqueueResult enqueue(QueuedCommand command);
    std::optional<QueuedCommand> takeNext();
    std::vector<std::uint64_t> discard(std::uint64_t commandId);
    std::vector<std::uint64_t> clear();
    std::size_t size() const noexcept;
    bool empty() const noexcept;

    static bool isBoundary(PlaybackCommandType type) noexcept;
    static std::optional<std::string> coalescingKey(
        const PlaybackCommand &command);

private:
    std::size_t m_capacity;
    std::deque<QueuedCommand> m_commands;
};

} // namespace mcast::playback
