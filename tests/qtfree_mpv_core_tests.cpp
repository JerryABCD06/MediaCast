// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/EventLoop.h"
#include "QtFree/Json.h"
#include "QtFree/MpvCore.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace {

using namespace std::chrono_literals;
using mcast::avalonia::qtfree::EventLoop;
using mcast::avalonia::qtfree::JsonValue;
using mcast::avalonia::qtfree::MediaLoadRequest;
using mcast::avalonia::qtfree::MpvCore;

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void writeU16(std::ofstream &stream, std::uint16_t value)
{
    const char bytes[2]{
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
    };
    stream.write(bytes, 2);
}

void writeU32(std::ofstream &stream, std::uint32_t value)
{
    const char bytes[4]{
        static_cast<char>(value & 0xff),
        static_cast<char>((value >> 8) & 0xff),
        static_cast<char>((value >> 16) & 0xff),
        static_cast<char>((value >> 24) & 0xff),
    };
    stream.write(bytes, 4);
}

std::filesystem::path writeWave(
    std::uint32_t durationSeconds, std::string_view tag)
{
    wchar_t directory[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    check(length > 0 && length < MAX_PATH,
          "temporary directory is available");
    const auto path = std::filesystem::path(directory)
        / ("mcast-qtfree-" + std::to_string(GetCurrentProcessId())
           + "-" + std::string(tag) + ".wav");
    std::ofstream stream(path, std::ios::binary);
    constexpr std::uint32_t sampleRate = 8000;
    const std::uint32_t dataSize =
        sampleRate * durationSeconds * 2;
    stream.write("RIFF", 4);
    writeU32(stream, 36 + dataSize);
    stream.write("WAVEfmt ", 8);
    writeU32(stream, 16);
    writeU16(stream, 1);
    writeU16(stream, 1);
    writeU32(stream, sampleRate);
    writeU32(stream, sampleRate * 2);
    writeU16(stream, 2);
    writeU16(stream, 16);
    stream.write("data", 4);
    writeU32(stream, dataSize);
    const std::string silence(dataSize, '\0');
    stream.write(silence.data(), silence.size());
    stream.close();
    return path;
}

} // namespace

int main()
{
    const auto wave = writeWave(2, "first");
    const auto secondWave = writeWave(4, "second");
    EventLoop loop;
    check(loop.start(), "event loop starts");
    MpvCore player(loop);
    check(player.start(), "MPV starts");
    player.setRendererAttached(true);

    std::mutex mutex;
    std::condition_variable condition;
    bool ready = false;
    bool failed = false;
    bool durationSeen = false;
    bool volumeSeen = false;
    std::atomic_int renderUpdates = 0;
    std::string failure;
    auto readyConnection = player.ready.connect([&](std::uint64_t) {
        const std::scoped_lock lock(mutex);
        ready = true;
        condition.notify_all();
    });
    auto failedConnection = player.loadFailed.connect(
        [&](std::uint64_t, const std::string &reason) {
            const std::scoped_lock lock(mutex);
            failed = true;
            failure = reason;
            condition.notify_all();
        });
    auto durationConnection = player.durationChanged.connect(
        [&](double value) {
            if (value <= 1.5)
                return;
            const std::scoped_lock lock(mutex);
            durationSeen = true;
            condition.notify_all();
        });
    auto volumeConnection = player.volumeChanged.connect(
        [&](int value) {
            if (value != 40)
                return;
            const std::scoped_lock lock(mutex);
            volumeSeen = true;
            condition.notify_all();
        });
    auto renderConnection = player.renderUpdate.connect(
        [&] { ++renderUpdates; });

    MediaLoadRequest request;
    request.logicalUri = wave.string();
    request.loadGeneration = 11;
    player.load(request);
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 8s, [&] { return ready || failed; }),
              "MPV reports load result");
    }
    check(!failed, failure.c_str());
    check(ready, "MPV load becomes ready");
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 3s, [&] { return durationSeen; }),
              "MPV duration is reported");
    }
    check(player.durationSeconds() > 1.5, "MPV duration is cached");

    player.pause();
    player.play();
    player.seekTo(1.0);
    player.setVolumePercent(40);
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 3s, [&] { return volumeSeen; }),
              "MPV reports volume");
    }
    player.setMuted(true);
    check(player.volumePercent() == 40,
          "volume command value is preserved");
    check(player.setPictureControl("brightness", 20),
          "picture control is accepted");
    check(player.pictureControlValue("brightness") == 20,
          "picture control value is readable");
    check(renderUpdates.load() == 1,
          "picture control requests a redraw");
    check(player.setPictureControl("sharpen", 25),
          "sharpen filter is accepted");
    check(renderUpdates.load() == 2,
          "sharpen control requests a redraw");

    const auto info = JsonValue::parse(player.mediaInfoJson());
    check(info.has_value() && info->isObject(),
          "media info JSON is valid");
    check(player.mediaSizeBytes() > 0, "media size is available");

    {
        const std::scoped_lock lock(mutex);
        ready = false;
        durationSeen = false;
    }
    MediaLoadRequest second;
    second.logicalUri = secondWave.string();
    second.loadGeneration = 12;
    player.load(second);
    check(player.mediaInfoJson() == "{}",
          "new load clears media summary immediately");
    {
        std::unique_lock lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        while (!ready && !failed
               && std::chrono::steady_clock::now() < deadline) {
            lock.unlock();
            const auto current =
                JsonValue::parse(player.mediaInfoJson());
            if (current.has_value()) {
                const double duration =
                    current->at("duration_seconds").asNumber();
                check(
                    duration < 0.1 || duration > 3.5,
                    "old media summary is not republished");
            }
            std::this_thread::sleep_for(2ms);
            lock.lock();
        }
        check(ready || failed, "second media reports load result");
    }
    check(!failed, failure.c_str());
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 3s, [&] { return durationSeen; }),
              "second duration is reported");
    }
    const auto secondInfo =
        JsonValue::parse(player.mediaInfoJson());
    check(
        secondInfo.has_value()
            && secondInfo->at("duration_seconds").asNumber() > 3.5,
        "media summary belongs to new load");

    {
        const std::scoped_lock lock(mutex);
        ready = false;
        failed = false;
        durationSeen = false;
        failure.clear();
    }
    MediaLoadRequest missing;
    missing.logicalUri =
        (wave.parent_path() / "mcast-missing-media.wav").string();
    missing.loadGeneration = 13;
    player.load(missing);
    check(player.mediaInfoJson() == "{}",
          "failed load clears the previous media summary");
    {
        std::unique_lock lock(mutex);
        check(condition.wait_for(
                  lock, 8s, [&] { return failed; }),
              "missing media reports load failure");
    }
    check(player.mediaInfoJson() == "{}",
          "failed load does not republish an old summary");

    readyConnection.disconnect();
    failedConnection.disconnect();
    durationConnection.disconnect();
    volumeConnection.disconnect();
    player.shutdown();
    loop.stop();
    std::error_code ignored;
    std::filesystem::remove(wave, ignored);
    std::filesystem::remove(secondWave, ignored);
    std::cout << "Qt-free MPV core tests passed\n";
    return 0;
}
