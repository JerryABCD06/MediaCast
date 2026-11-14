// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/EventLoop.h"
#include "QtFree/MediaTypes.h"
#include "QtFree/Signal.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mcast::avalonia::qtfree {

class ImageSequenceStrategy
{
public:
    using LoadSink = std::function<bool(
        const std::string &mfUri,
        const std::string &listPath,
        double fps, bool play)>;

    explicit ImageSequenceStrategy(EventLoop &loop);
    ~ImageSequenceStrategy();

    bool start(
        const std::vector<MediaLoadFrame> &segments,
        LoadSink loadSink);
    void markLoaded();
    void play();
    void pause();
    void stop();
    void seek(double seconds);
    void reset();

    bool isActive() const { return m_active; }
    bool isPaused() const { return m_paused; }
    bool isWaitingForLoad() const { return m_waitingForLoad; }
    double positionSeconds() const { return m_position; }
    double durationSeconds() const { return m_duration; }

    Signal<double> positionChanged;
    Signal<double> durationChanged;
    Signal<> ended;
    Signal<const std::string &> loadFailed;
    Signal<const std::string &> logMessage;

private:
    bool reload(double seconds, bool play);
    void scheduleTick();
    void cancelTick();
    void tick();

    EventLoop &m_loop;
    std::uint64_t m_timer = 0;
    std::vector<MediaLoadFrame> m_segments;
    LoadSink m_loadSink;
    std::string m_listPath;
    double m_duration = 0;
    double m_position = 0;
    double m_baseSeconds = 0;
    bool m_active = false;
    bool m_paused = false;
    bool m_ended = false;
    bool m_waitingForLoad = false;
    std::chrono::steady_clock::time_point m_clockStart;
};

} // namespace mcast::avalonia::qtfree

