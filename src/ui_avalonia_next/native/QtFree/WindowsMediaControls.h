// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/EventLoop.h"
#include "QtFree/MediaTypes.h"
#include "QtFree/PlaybackController.h"
#include "QtFree/Signal.h"

#include <memory>

namespace mcast::avalonia::qtfree {

class WindowsMediaControls
    : public std::enable_shared_from_this<WindowsMediaControls>
{
public:
    explicit WindowsMediaControls(EventLoop &loop);
    ~WindowsMediaControls();

    WindowsMediaControls(const WindowsMediaControls &) = delete;
    WindowsMediaControls &operator=(const WindowsMediaControls &) = delete;

    bool attach();
    void detach();
    void setNowPlaying(const NowPlaying &info);
    void setTransportState(PlaybackController::State state);
    void setPosition(double seconds);
    void setDuration(double seconds);
    void setQueueAvailability(bool hasNext, bool hasPrevious);

    Signal<> playRequested;
    Signal<> pauseRequested;
    Signal<> stopRequested;
    Signal<> nextRequested;
    Signal<> previousRequested;
    Signal<double> seekRequested;
    Signal<const std::string &> logMessage;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace mcast::avalonia::qtfree

