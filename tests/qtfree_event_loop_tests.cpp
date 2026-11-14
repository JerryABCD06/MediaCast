// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"
#include "QtFree/Signal.h"

#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>

namespace {

using namespace std::chrono_literals;

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
    mcast::avalonia::qtfree::EventLoop loop;
    check(loop.start(), "event loop starts");

    auto ordered = std::make_shared<std::promise<void>>();
    auto orderedFuture = ordered->get_future();
    bool first = false;
    bool second = false;
    check(
        loop.post([&] {
            first = true;
            loop.post([&] {
                second = first;
                ordered->set_value();
            });
        }),
        "task is posted");
    check(
        orderedFuture.wait_for(2s) == std::future_status::ready,
        "nested task executes");
    check(second, "tasks execute serially in FIFO order");

    auto cancelled = std::make_shared<std::promise<void>>();
    auto cancelledFuture = cancelled->get_future();
    const auto timer = loop.scheduleAfter(200ms, [] {});
    check(timer != 0, "timer is scheduled");
    check(loop.cancelTimer(timer), "timer is cancelled");
    loop.scheduleAfter(10ms, [cancelled] { cancelled->set_value(); });
    check(
        cancelledFuture.wait_for(2s) == std::future_status::ready,
        "loop remains usable after timer cancellation");

    mcast::avalonia::qtfree::Signal<int, const std::string &> signal;
    int received = 0;
    std::string text;
    auto connection = signal.connect([&](int value, const std::string &valueText) {
        received = value;
        text = valueText;
    });
    signal.emit(42, "media");
    check(received == 42 && text == "media", "signal delivers arguments");
    connection.disconnect();
    signal.emit(7, "ignored");
    check(received == 42, "disconnected slot is not called");

    loop.stop();
    check(!loop.isRunning(), "event loop stops");
    check(!loop.post([] {}), "stopped loop rejects tasks");

    std::cout << "Qt-free event loop tests passed\n";
    return 0;
}
