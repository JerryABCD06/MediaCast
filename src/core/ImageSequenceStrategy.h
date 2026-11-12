// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ImageSequencePlaylist.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <chrono>
#include <functional>

class QTimer;

namespace mcast::core {

/**
 * Protocol-neutral image-sequence playback strategy.
 *
 * It owns the virtual timeline and the temporary mf:// list. Qt/MpvCore only
 * provides the load sink and renderer; all other commands remain common.
 */
class ImageSequenceStrategy final : public QObject
{
    Q_OBJECT

public:
    using LoadSink = std::function<bool(
        const QString &mfUri, const QString &listPath,
        double fps, bool play)>;

    explicit ImageSequenceStrategy(QObject *parent = nullptr);
    ~ImageSequenceStrategy() override;

    bool start(
        const QVector<ImageSequenceSegment> &segments,
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

signals:
    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void ended();
    void loadFailed(const QString &reason);
    void logMessage(const QString &message);

private:
    bool reload(double seconds, bool play);
    void tick();

    QTimer *m_timer = nullptr;
    QVector<ImageSequenceSegment> m_segments;
    LoadSink m_loadSink;
    QString m_listPath;
    double m_duration = 0.0;
    double m_position = 0.0;
    double m_baseSeconds = 0.0;
    bool m_active = false;
    bool m_paused = false;
    bool m_ended = false;
    bool m_waitingForLoad = false;
    std::chrono::steady_clock::time_point m_clockStart;
};

} // namespace mcast::core
