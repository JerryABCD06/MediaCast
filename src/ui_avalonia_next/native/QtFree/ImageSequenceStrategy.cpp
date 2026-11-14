// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/ImageSequenceStrategy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace mcast::avalonia::qtfree {
namespace {

constexpr double kMinFrameSeconds = 0.05;
constexpr double kMaxFrameSeconds = 3600.0;
std::atomic_uint64_t g_sequenceId{1};

double frameSeconds(const MediaLoadFrame &segment)
{
    return std::clamp(
        segment.durationSeconds,
        kMinFrameSeconds, kMaxFrameSeconds);
}

std::filesystem::path sequenceDirectory()
{
    return std::filesystem::temp_directory_path()
        / "MediaCast" / "image-sequences";
}

std::filesystem::path nextListPath()
{
    const auto stamp =
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    return sequenceDirectory()
        / ("sequence-" + std::to_string(stamp) + "-"
           + std::to_string(g_sequenceId.fetch_add(1)) + ".txt");
}

} // namespace

ImageSequenceStrategy::ImageSequenceStrategy(EventLoop &loop)
    : m_loop(loop)
{
}

ImageSequenceStrategy::~ImageSequenceStrategy()
{
    reset();
}

bool ImageSequenceStrategy::start(
    const std::vector<MediaLoadFrame> &segments,
    LoadSink loadSink)
{
    reset();
    if (segments.empty() || !loadSink) {
        const std::string reason = "图片序列为空或没有加载出口";
        logMessage.emit(reason);
        loadFailed.emit(reason);
        return false;
    }
    for (const auto &segment : segments) {
        if (segment.uri.empty() || !(segment.durationSeconds > 0)) {
            const std::string reason = "图片序列包含无效帧";
            logMessage.emit(reason);
            loadFailed.emit(reason);
            return false;
        }
    }

    m_segments = segments;
    m_loadSink = std::move(loadSink);
    m_duration = 0;
    for (const auto &segment : segments)
        m_duration += frameSeconds(segment);
    m_active = true;
    m_paused = false;
    m_ended = false;
    m_waitingForLoad = false;
    m_position = 0;
    m_baseSeconds = 0;
    durationChanged.emit(m_duration);
    positionChanged.emit(0);
    if (!reload(0, true)) {
        reset();
        loadFailed.emit("图片序列加载失败");
        return false;
    }
    return true;
}

void ImageSequenceStrategy::markLoaded()
{
    if (!m_active || !m_waitingForLoad)
        return;
    m_waitingForLoad = false;
    if (m_paused)
        return;
    m_baseSeconds = m_position;
    m_clockStart = std::chrono::steady_clock::now();
    scheduleTick();
}

void ImageSequenceStrategy::play()
{
    if (!m_active)
        return;
    if (m_duration > 0 && m_position >= m_duration) {
        m_position = 0;
        if (!reload(0, true)) {
            reset();
            loadFailed.emit("图片序列重新加载失败");
            return;
        }
    }
    m_paused = false;
    if (m_waitingForLoad)
        return;
    m_baseSeconds = m_position;
    m_clockStart = std::chrono::steady_clock::now();
    scheduleTick();
}

void ImageSequenceStrategy::pause()
{
    if (!m_active)
        return;
    if (!m_waitingForLoad)
        tick();
    m_baseSeconds = m_position;
    m_paused = true;
    cancelTick();
}

void ImageSequenceStrategy::stop()
{
    if (!m_active)
        return;
    m_paused = true;
    cancelTick();
    if (!reload(0, false)) {
        reset();
        loadFailed.emit("图片序列停止失败");
        return;
    }
    m_position = 0;
    m_baseSeconds = 0;
    m_ended = false;
    positionChanged.emit(0);
}

void ImageSequenceStrategy::seek(double seconds)
{
    if (!m_active)
        return;
    const double target = std::clamp(seconds, 0.0, m_duration);
    if (!reload(target, !m_paused)) {
        reset();
        loadFailed.emit("图片序列跳转失败");
        return;
    }
    m_baseSeconds = m_position;
    m_clockStart = std::chrono::steady_clock::now();
    m_ended = false;
}

void ImageSequenceStrategy::reset()
{
    cancelTick();
    if (!m_listPath.empty()) {
        std::error_code ignored;
        std::filesystem::remove(m_listPath, ignored);
        m_listPath.clear();
    }
    m_segments.clear();
    m_loadSink = {};
    m_duration = 0;
    m_position = 0;
    m_baseSeconds = 0;
    m_active = false;
    m_paused = false;
    m_ended = false;
    m_waitingForLoad = false;
}

bool ImageSequenceStrategy::reload(double seconds, bool play)
{
    if (m_segments.empty() || !m_loadSink)
        return false;

    double cursor = 0;
    std::size_t first = m_segments.size() - 1;
    double timelinePosition = 0;
    for (std::size_t index = 0; index < m_segments.size(); ++index) {
        const double duration = frameSeconds(m_segments[index]);
        if (seconds < cursor + duration) {
            first = index;
            timelinePosition = cursor;
            break;
        }
        cursor += duration;
    }
    if (seconds >= m_duration) {
        first = m_segments.size() - 1;
        timelinePosition =
            m_duration - frameSeconds(m_segments.back());
    }

    std::error_code error;
    std::filesystem::create_directories(sequenceDirectory(), error);
    if (error) {
        logMessage.emit("无法创建图片序列缓存目录");
        return false;
    }

    const std::filesystem::path path = nextListPath();
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        logMessage.emit("无法写入图片序列清单");
        return false;
    }
    for (std::size_t index = first; index < m_segments.size(); ++index)
        stream << m_segments[index].uri << '\n';
    stream.close();
    if (!stream) {
        std::filesystem::remove(path, error);
        logMessage.emit("无法写入图片序列清单");
        return false;
    }

    double remaining = m_duration;
    for (std::size_t index = 0; index < first; ++index)
        remaining -= frameSeconds(m_segments[index]);
    const double fps = std::clamp(
        static_cast<double>(m_segments.size() - first)
            / std::max(remaining, kMinFrameSeconds),
        0.01, 60.0);
    const std::string listPath = path.string();
    const std::string mfUri = "mf://@" + listPath;
    if (!m_loadSink(mfUri, listPath, fps, play)) {
        std::filesystem::remove(path, error);
        return false;
    }

    if (!m_listPath.empty() && m_listPath != listPath)
        std::filesystem::remove(m_listPath, error);
    m_listPath = listPath;
    m_position = timelinePosition;
    m_waitingForLoad = true;
    positionChanged.emit(m_position);
    return true;
}

void ImageSequenceStrategy::scheduleTick()
{
    cancelTick();
    m_timer = m_loop.scheduleAfter(
        std::chrono::milliseconds(100), [this] {
            m_timer = 0;
            tick();
        });
}

void ImageSequenceStrategy::cancelTick()
{
    if (m_timer != 0) {
        m_loop.cancelTimer(m_timer);
        m_timer = 0;
    }
}

void ImageSequenceStrategy::tick()
{
    if (!m_active || m_paused || m_ended)
        return;
    const double elapsed =
        std::chrono::duration<double>(
            std::chrono::steady_clock::now() - m_clockStart)
            .count();
    const double position =
        std::min(m_duration, m_baseSeconds + elapsed);
    if (!std::isfinite(position))
        return;
    m_position = position;
    positionChanged.emit(position);
    if (m_duration > 0 && position >= m_duration) {
        m_ended = true;
        cancelTick();
        ended.emit();
        return;
    }
    scheduleTick();
}

} // namespace mcast::avalonia::qtfree

