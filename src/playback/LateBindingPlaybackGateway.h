// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "playback/PlaybackGateway.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace mcast::playback {

// Protocol 常驻，但 Ui/mpv 只在需要时存在。此类在 Ui 缺席时返回
// PlayerUnavailable，在 PlaybackPipe 建立后透明挂接真实网关，并转发状态事件。
class LateBindingPlaybackGateway final : public PlaybackGateway
{
public:
    LateBindingPlaybackGateway();
    ~LateBindingPlaybackGateway() override;

    LateBindingPlaybackGateway(
        const LateBindingPlaybackGateway &) = delete;
    LateBindingPlaybackGateway &operator=(
        const LateBindingPlaybackGateway &) = delete;

    bool attach(
        std::shared_ptr<PlaybackGateway> gateway,
        std::string &error);
    void detach(std::string reason);
    bool attached() const noexcept;

    PlaybackSnapshot snapshot() const override;
    PlaybackCommandSubmission submit(
        const PlaybackCommand &command) override;
    std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) override;
    void removeEventListener(std::uint64_t listenerId) override;

private:
    struct ListenerEntry {
        std::function<void(const PlaybackEvent &)> callback;
        std::size_t activeCallbacks = 0;
        bool removing = false;
    };

    void forwardEvent(
        std::uint64_t attachmentToken,
        const PlaybackEvent &event);
    void notify(const PlaybackEvent &event);

    std::mutex m_attachmentMutex;
    mutable std::mutex m_mutex;
    std::shared_ptr<PlaybackGateway> m_current;
    std::uint64_t m_currentListener = 0;
    std::uint64_t m_attachmentToken = 0;
    PlaybackSnapshot m_lastSnapshot;
    bool m_stopping = false;
    std::map<std::uint64_t, ListenerEntry> m_listeners;
    std::condition_variable m_listenerIdle;
    std::uint64_t m_nextListenerId = 1;
};

} // namespace mcast::playback
