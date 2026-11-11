// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackTypes.h"

#include <cstdint>
#include <functional>

namespace mcast::playback {

class PlaybackGateway
{
public:
    virtual ~PlaybackGateway() = default;

    virtual PlaybackSnapshot snapshot() const = 0;
    virtual PlaybackCommandSubmission submit(
        const PlaybackCommand &command) = 0;
    virtual std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) = 0;
    // Implementations must not invoke listenerId again after this returns.
    virtual void removeEventListener(std::uint64_t listenerId) = 0;
};

} // namespace mcast::playback
