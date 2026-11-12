// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ImageSequenceStrategy.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <cmath>

namespace mcast::core {
namespace {

constexpr double kMinFrameSeconds = 0.05;
constexpr double kMaxFrameSeconds = 3600.0;

double frameSeconds(const ImageSequenceSegment &segment)
{
    return std::clamp(
        segment.durationSeconds, kMinFrameSeconds, kMaxFrameSeconds);
}

QString sequenceDirectory()
{
    return QStandardPaths::writableLocation(
               QStandardPaths::TempLocation)
        + QStringLiteral("/MediaCast/image-sequences");
}

} // namespace

ImageSequenceStrategy::ImageSequenceStrategy(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(100);
    connect(m_timer, &QTimer::timeout, this, &ImageSequenceStrategy::tick);
}

ImageSequenceStrategy::~ImageSequenceStrategy()
{
    reset();
}

bool ImageSequenceStrategy::start(
    const QVector<ImageSequenceSegment> &segments, LoadSink loadSink)
{
    reset();
    if (segments.isEmpty() || !loadSink) {
        emit logMessage(QStringLiteral("图片序列为空或没有加载出口"));
        emit loadFailed(QStringLiteral("图片序列为空或没有加载出口"));
        return false;
    }
    for (const ImageSequenceSegment &segment : segments) {
        if (!segment.uri.isValid() || segment.uri.isEmpty()
            || !(segment.durationSeconds > 0.0)) {
            const QString reason = QStringLiteral("图片序列包含无效帧");
            emit logMessage(reason);
            emit loadFailed(reason);
            return false;
        }
    }

    m_segments = segments;
    m_loadSink = std::move(loadSink);
    m_duration = 0.0;
    for (const ImageSequenceSegment &segment : segments)
        m_duration += frameSeconds(segment);
    m_active = true;
    m_paused = false;
    m_ended = false;
    m_waitingForLoad = false;
    m_position = 0.0;
    m_baseSeconds = 0.0;
    emit durationChanged(m_duration);
    emit positionChanged(0.0);
    if (!reload(0.0, true)) {
        reset();
        emit loadFailed(QStringLiteral("图片序列加载失败"));
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
    m_timer->start();
}

void ImageSequenceStrategy::play()
{
    if (!m_active)
        return;
    if (m_position >= m_duration && m_duration > 0.0) {
        m_position = 0.0;
        if (!reload(0.0, true)) {
            reset();
            emit loadFailed(QStringLiteral("图片序列重新加载失败"));
            return;
        }
    }
    m_paused = false;
    if (m_waitingForLoad)
        return;
    m_baseSeconds = m_position;
    m_clockStart = std::chrono::steady_clock::now();
    m_timer->start();
}

void ImageSequenceStrategy::pause()
{
    if (!m_active)
        return;
    if (!m_waitingForLoad)
        tick();
    m_baseSeconds = m_position;
    m_paused = true;
    m_timer->stop();
}

void ImageSequenceStrategy::stop()
{
    if (!m_active)
        return;
    m_paused = true;
    m_timer->stop();
    if (!reload(0.0, false)) {
        reset();
        emit loadFailed(QStringLiteral("图片序列停止失败"));
        return;
    }
    m_position = 0.0;
    m_baseSeconds = 0.0;
    m_ended = false;
    emit positionChanged(0.0);
}

void ImageSequenceStrategy::seek(double seconds)
{
    if (!m_active)
        return;
    const double target = std::clamp(seconds, 0.0, m_duration);
    if (!reload(target, !m_paused)) {
        reset();
        emit loadFailed(QStringLiteral("图片序列跳转失败"));
        return;
    }
    m_baseSeconds = m_position;
    m_clockStart = std::chrono::steady_clock::now();
    m_ended = false;
}

void ImageSequenceStrategy::reset()
{
    if (m_timer)
        m_timer->stop();
    if (!m_listPath.isEmpty()) {
        QFile::remove(m_listPath);
        m_listPath.clear();
    }
    m_segments.clear();
    m_loadSink = {};
    m_duration = 0.0;
    m_position = 0.0;
    m_baseSeconds = 0.0;
    m_active = false;
    m_paused = false;
    m_ended = false;
    m_waitingForLoad = false;
}

bool ImageSequenceStrategy::reload(double seconds, bool play)
{
    if (m_segments.isEmpty() || !m_loadSink)
        return false;

    double cursor = 0.0;
    qsizetype firstSegment = m_segments.size() - 1;
    double timelinePosition = 0.0;
    for (qsizetype index = 0; index < m_segments.size(); ++index) {
        const double duration = frameSeconds(m_segments[index]);
        if (seconds < cursor + duration) {
            firstSegment = index;
            timelinePosition = cursor;
            break;
        }
        cursor += duration;
    }
    if (seconds >= m_duration) {
        firstSegment = m_segments.size() - 1;
        timelinePosition =
            m_duration - frameSeconds(m_segments.back());
    }

    QDir directory(sequenceDirectory());
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        emit logMessage(QStringLiteral("无法创建图片序列缓存目录"));
        return false;
    }
    const QString path = directory.filePath(
        QStringLiteral("sequence-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QStringLiteral(".txt"));
    QSaveFile file(path);
    const QByteArray list = buildMfImageList(m_segments, firstSegment);
    if (!file.open(QIODevice::WriteOnly)
        || file.write(list) != list.size() || !file.commit()) {
        emit logMessage(QStringLiteral("无法写入图片序列清单"));
        return false;
    }

    double remainingDuration = m_duration;
    for (qsizetype index = 0; index < firstSegment; ++index)
        remainingDuration -= frameSeconds(m_segments[index]);
    const double fps = std::clamp(
        static_cast<double>(m_segments.size() - firstSegment)
            / std::max(remainingDuration, kMinFrameSeconds),
        0.01, 60.0);
    const QString mfUri =
        QStringLiteral("mf://@") + QDir::toNativeSeparators(path);
    if (!m_loadSink(mfUri, path, fps, play)) {
        QFile::remove(path);
        return false;
    }
    if (!m_listPath.isEmpty() && m_listPath != path)
        QFile::remove(m_listPath);
    m_listPath = path;
    m_position = timelinePosition;
    m_waitingForLoad = true;
    emit positionChanged(m_position);
    return true;
}

void ImageSequenceStrategy::tick()
{
    if (!m_active || m_paused || m_ended)
        return;
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - m_clockStart);
    const double position = std::min(
        m_duration, m_baseSeconds + elapsed.count());
    if (!std::isfinite(position))
        return;
    m_position = position;
    emit positionChanged(position);
    if (m_duration > 0.0 && position >= m_duration) {
        m_timer->stop();
        m_ended = true;
        emit ended();
    }
}

} // namespace mcast::core
