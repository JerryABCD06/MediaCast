// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/MediaControls.h"

#include "core/PlaybackController.h"
#include "core/ProductIdentity.h"
#include "platform/windows/WindowsMediaControls.h"

#include <QCoreApplication>
#include <QWidget>

namespace {

QString appShortName()
{
    const QString translated = QCoreApplication::translate(
        "WindowsMediaControlsService", "term_app_short");
    if (translated != QStringLiteral("term_app_short"))
        return translated;
    return QString::fromLatin1(
        mcast::identity::appShortEnglish.data(),
        static_cast<int>(mcast::identity::appShortEnglish.size()));
}

class WindowsMediaControlsService final : public MediaControls
{
public:
    explicit WindowsMediaControlsService(PlaybackController &playback)
        : m_playback(playback)
    {
    }

    ~WindowsMediaControlsService() override
    {
        shutdown();
    }

    bool initialize(const LogSink &logSink) override
    {
        if (m_initialized)
            return true;

        m_logSink = logSink;
        m_hostWindow = std::make_unique<QWidget>();
        m_hostWindow->setWindowTitle(appShortName());
        m_hostWindow->setWindowFlags(Qt::Tool);
        m_hostWindow->resize(1, 1);
        m_hostWindow->show();
        m_hostWindow->hide();

        m_controls = std::make_unique<WindowsMediaControls>();
        QObject::connect(
            m_controls.get(), &WindowsMediaControls::logMessage,
            m_controls.get(),
            [this](const QString &text) {
                if (m_logSink)
                    m_logSink(text);
            });

        if (!m_controls->attachToWindow(
                static_cast<quintptr>(m_hostWindow->winId()))) {
            shutdown();
            return false;
        }

        m_controls->setTransportState(
            QStringLiteral("NO_MEDIA_PRESENT"));
        m_controls->setQueueAvailability(false, false);

        QObject::connect(
            &m_playback, &PlaybackController::nowPlayingChanged,
            m_controls.get(), &WindowsMediaControls::setNowPlaying);
        QObject::connect(
            &m_playback, &PlaybackController::stateChanged,
            m_controls.get(),
            [controls = m_controls.get()](
                PlaybackController::State state) {
                switch (state) {
                case PlaybackController::State::NoMedia:
                    controls->setTransportState(
                        QStringLiteral("NO_MEDIA_PRESENT"));
                    break;
                case PlaybackController::State::Stopped:
                    controls->setTransportState(
                        QStringLiteral("STOPPED"));
                    break;
                case PlaybackController::State::Preparing:
                    controls->setTransportState(
                        QStringLiteral("TRANSITIONING"));
                    break;
                case PlaybackController::State::Playing:
                    controls->setTransportState(
                        QStringLiteral("PLAYING"));
                    break;
                case PlaybackController::State::Paused:
                    controls->setTransportState(
                        QStringLiteral("PAUSED_PLAYBACK"));
                    break;
                }
            });
        QObject::connect(
            &m_playback, &PlaybackController::positionChanged,
            m_controls.get(), &WindowsMediaControls::setPosition);
        QObject::connect(
            &m_playback, &PlaybackController::durationChanged,
            m_controls.get(), &WindowsMediaControls::setDuration);
        QObject::connect(
            &m_playback, &PlaybackController::queueChanged,
            m_controls.get(),
            [controls = m_controls.get()](
                bool hasNext, bool hasPrevious, const QString &,
                PlaybackController::PlayMode) {
                controls->setQueueAvailability(
                    hasNext, hasPrevious);
            });

        QObject::connect(
            m_controls.get(), &WindowsMediaControls::playRequested,
            &m_playback, &PlaybackController::play);
        QObject::connect(
            m_controls.get(), &WindowsMediaControls::pauseRequested,
            &m_playback, &PlaybackController::pause);
        QObject::connect(
            m_controls.get(), &WindowsMediaControls::stopRequested,
            &m_playback, &PlaybackController::stop);
        QObject::connect(
            m_controls.get(), &WindowsMediaControls::nextRequested,
            &m_playback, &PlaybackController::next);
        QObject::connect(
            m_controls.get(), &WindowsMediaControls::previousRequested,
            &m_playback, &PlaybackController::previous);
        QObject::connect(
            m_controls.get(), &WindowsMediaControls::seekRequested,
            &m_playback, &PlaybackController::seekTo);
        m_initialized = true;
        return true;
    }

    void shutdown() override
    {
        m_initialized = false;
        m_controls.reset();
        m_hostWindow.reset();
        m_logSink = {};
    }

private:
    PlaybackController &m_playback;
    LogSink m_logSink;
    bool m_initialized = false;
    std::unique_ptr<QWidget> m_hostWindow;
    std::unique_ptr<WindowsMediaControls> m_controls;
};

} // namespace

std::unique_ptr<MediaControls> createPlatformMediaControls(
    PlaybackController &playback)
{
    return std::make_unique<WindowsMediaControlsService>(playback);
}
