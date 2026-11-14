// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"
#include "QtFree/ImageSequenceStrategy.h"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>

namespace {

using namespace std::chrono_literals;
using mcast::avalonia::qtfree::EventLoop;
using mcast::avalonia::qtfree::ImageSequenceStrategy;
using mcast::avalonia::qtfree::MediaLoadFrame;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    EventLoop loop;
    check(loop.start(), "event loop starts");
    ImageSequenceStrategy strategy(loop);
    std::mutex mutex;
    std::condition_variable condition;
    double duration = 0;
    double position = 0;
    int loads = 0;
    auto durationConnection = strategy.durationChanged.connect(
        [&](double value) {
            const std::scoped_lock lock(mutex);
            duration = value;
            condition.notify_all();
        });
    auto positionConnection = strategy.positionChanged.connect(
        [&](double value) {
            const std::scoped_lock lock(mutex);
            position = value;
            condition.notify_all();
        });

    const bool started = strategy.start(
        {
            MediaLoadFrame{"file:///one.jpg", 0.4},
            MediaLoadFrame{"file:///two.jpg", 0.6},
        },
        [&](const std::string &mfUri,
            const std::string &listPath,
            double fps, bool play) {
            bool firstLoad = false;
            {
                const std::scoped_lock lock(mutex);
                firstLoad = loads == 0;
                ++loads;
            }
            check(mfUri.rfind("mf://@", 0) == 0,
                  "MF URI is built");
            check(std::filesystem::exists(listPath),
                  "sequence list exists");
            check(fps > 0, "positive fps reaches sink");
            if (firstLoad) {
                check(play, "initial load receives play intent");
                std::ifstream stream(listPath);
                std::string first;
                std::string second;
                std::getline(stream, first);
                std::getline(stream, second);
                check(first == "file:///one.jpg",
                      "first frame is listed");
                check(second == "file:///two.jpg",
                      "second frame is listed");
            }
            return true;
        });
    check(started && strategy.isActive(), "sequence starts");
    check(strategy.isWaitingForLoad(), "sequence waits for load");
    check(duration > 0.99 && duration < 1.01,
          "virtual duration is reported");

    strategy.markLoaded();
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 1s, [&] { return position >= 0.12; }),
              "timeline advances");
    }
    strategy.pause();
    const double paused = strategy.positionSeconds();
    std::this_thread::sleep_for(150ms);
    check(std::abs(strategy.positionSeconds() - paused) < 0.02,
          "pause freezes timeline");

    strategy.seek(0.75);
    check(strategy.positionSeconds() >= 0.39
              && strategy.positionSeconds() <= 0.41,
          "seek selects correct image segment");
    strategy.stop();
    check(strategy.positionSeconds() == 0,
          "stop resets virtual timeline");
    strategy.reset();
    check(!strategy.isActive(), "reset deactivates strategy");

    durationConnection.disconnect();
    positionConnection.disconnect();
    loop.stop();
    std::cout << "Qt-free image sequence tests passed\n";
    return 0;
}
