// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"
#include "QtFree/Json.h"
#include "QtFree/UiSettingsModel.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

namespace {

using mcast::avalonia::qtfree::Connection;
using mcast::avalonia::qtfree::EventLoop;
using mcast::avalonia::qtfree::JsonValue;
using mcast::avalonia::qtfree::UiSettingsModel;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

struct PatchWaiter
{
    std::mutex mutex;
    std::condition_variable condition;
    std::string patch;
    Connection connection;

    void attach(UiSettingsModel &model)
    {
        connection = model.settingsPatchReady.connect(
            [this](const std::string &json) {
                const std::scoped_lock lock(mutex);
                patch = json;
                condition.notify_all();
            });
    }

    std::string wait()
    {
        std::unique_lock lock(mutex);
        const bool ready = condition.wait_for(
            lock, std::chrono::seconds(2),
            [this] { return !patch.empty(); });
        check(ready, "settings patch was emitted");
        return patch;
    }
};

} // namespace

int main()
{
    constexpr const char *initial = R"({
        "ui":{"language":"en_US","darkmode":"Dark","accentmode":"WindowsBlue","mica":true},
        "cast":{"newcast":true,"broadcast":true,"broadcast_interval":10000},
        "device":{"name":"Living Room PC","udn":"uuid:11111111-2222-4333-8444-555555555555"},
        "picture":{"brightness":10}
    })";

    EventLoop loop;
    check(loop.start(), "event loop starts");
    {
        UiSettingsModel model(loop);
        std::string error;
        check(model.applySnapshot(initial, 5, &error),
              error.c_str());
        check(model.mica() && model.broadcast()
                  && model.broadcastIntervalMs() == 10000
                  && model.pictureValues().at("brightness") == 10
                  && model.deviceName() == "Living Room PC",
              "settings snapshot values");

        PatchWaiter waiter;
        waiter.attach(model);
        model.setMica(false);
        model.setBroadcast(false);
        model.setBroadcastIntervalMs(30000);
        model.setPictureValue("contrast", 25);
        const std::string patch = waiter.wait();
        const auto object = JsonValue::parse(patch);
        check(object.has_value(), "patch JSON parses");
        check(!object->at("ui").at("mica").asBool(true),
              "mica patch is queued");
        check(!object->at("cast").at("broadcast").asBool(true)
                  && object->at("cast")
                         .at("broadcast_interval").asInt()
                      == 30000,
              "broadcast patch is coalesced");
        check(object->at("picture").at("contrast").asInt() == 25,
              "picture patch is coalesced");

        error.clear();
        check(model.applySnapshot(initial, 5, &error),
              "same-version snapshot is accepted");
        check(model.mica() && model.broadcast()
                  && model.pictureValues().count("contrast") == 0,
              "same-version snapshot rolls back optimistic patch");
        model.rejectPendingPatch();
        check(model.mica() && model.broadcast()
                  && model.broadcastIntervalMs() == 10000,
              "rejected patch restores authority");

        const std::string beforeStale = model.snapshotJson();
        error.clear();
        check(!model.applySnapshot(initial, 4, &error)
                  && model.snapshotJson() == beforeStale,
              "stale snapshot is ignored");
        check(error == "stale settings snapshot",
              "stale snapshot is reported");

        model.setBroadcast(false);
        model.rejectPendingPatch();
        check(model.mica() && !model.broadcast()
                  && model.broadcastIntervalMs() == 10000,
              "rejection preserves newer unsent edits");
        model.rejectPendingPatch();
    }

    {
        UiSettingsModel accepted(loop);
        std::string error;
        check(accepted.applySnapshot(initial, 5, &error),
              "accepted model loads snapshot");
        PatchWaiter waiter;
        waiter.attach(accepted);
        accepted.setMica(false);
        (void)waiter.wait();
        accepted.acceptPendingPatch(6);
        check(accepted.version() == 6 && !accepted.mica(),
              "accepted patch advances authority");

        accepted.setPictureValue("brightness", 5000);
        check(accepted.pictureValues().at("brightness") == 1000,
              "picture value remains inside protocol-safe range");
    }

    loop.stop();
    std::cout << "Qt-free UI settings model tests passed\n";
    return 0;
}
