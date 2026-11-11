// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "core/AppSettings.h"
#include "core/LegalDocs.h"
#include "core/MpvCore.h"
#include "core/MpvQmlItem.h"
#include "core/NowPlaying.h"
#include "core/PlaybackController.h"
#include "core/Tr.h"
#include "platform/windows/SystemAccentColors.h"
#include "ui/DeviceInfo.h"
#include "ui/MainWindow.h"
#include "ui/UiState.h"
#include "ui_process/PlaybackBridge.h"
#include "ui_process/UiConfirmationBridge.h"
#include "ui_process/UiSession.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QTime>
#include <QTimer>
#include <QtQml/qqml.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

int main(int argc, char *argv[])
{
    std::string uiSessionError;
    const UiSession::Options uiSessionOptions =
        UiSession::parseArguments(argc, argv, uiSessionError);
    if (!uiSessionError.empty())
        return 3;

    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("MCast"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/mcast-256.png")));

    QFile logFile(
        QCoreApplication::applicationDirPath() + QStringLiteral("/MCast.Ui.log"));
    logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    QTextStream log(&logFile);
    log << "\n===== Ui 进程启动 "
        << QDateTime::currentDateTime().toString(Qt::ISODate) << " =====\n";

    auto writeLog = [&log](const QString &text) {
        log << QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz"))
            << "  Ui  " << text << '\n';
        log.flush();
    };

    UiSession uiSession;

    SystemAccentColors systemColors;
    systemColors.refresh();
    qmlRegisterSingletonInstance(
        "MediaCast", 1, 0, "SystemColors", &systemColors);

    Tr tr;
    QObject::connect(&tr, &Tr::logMessage, writeLog);
    tr.scan();
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Tr", &tr);

    AppSettings settings;
    QObject::connect(&settings, &AppSettings::logMessage, writeLog);
    UiState uiState(&settings, &tr);
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "UiState", &uiState);
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Settings", &settings);

    LegalDocs legal;
    QObject::connect(&legal, &LegalDocs::logMessage, writeLog);
    legal.scan();
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Legal", &legal);

    auto player = std::make_unique<MpvCore>();
    player->setOutputMode(MpvCore::RenderApiOutput);
    qmlRegisterUncreatableType<MpvCore>(
        "MediaCast", 1, 0, "MpvCore",
        QStringLiteral("MpvCore 只能由 C++ 创建"));
    qmlRegisterType<MpvQmlItem>("MediaCast", 1, 0, "MpvQmlItem");
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Player", player.get());

    PlaybackController playback(player.get());
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Playback", &playback);
    qmlRegisterAnonymousType<NowPlaying>("MediaCast", 1);

    DeviceInfo device;
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Device", &device);

    MainWindow mainWindow(nullptr);
    QObject::connect(&mainWindow, &MainWindow::logMessage, writeLog);
    QObject::connect(
        &playback, &PlaybackController::logMessage,
        &mainWindow, [&writeLog](const QString &text) {
            writeLog(QStringLiteral("Playback  %1").arg(text));
        });
    QObject::connect(
        &mainWindow, &MainWindow::windowClosed, &app, &QApplication::quit);
    if (uiSessionOptions.confirmationOnly) {
        QObject::connect(
            &mainWindow, &MainWindow::castConfirmationWindowClosed,
            &app, &QApplication::quit);
    }
    QObject::connect(
        &uiState, &UiState::languageChanged,
        &mainWindow, &MainWindow::retranslate);
    QObject::connect(
        &uiState, &UiState::languageChanged,
        &playback, &PlaybackController::retranslate);
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Shell", &mainWindow);

    if (!uiSession.connectToPeers(uiSessionOptions, uiSessionError)) {
        writeLog(QStringLiteral("IPC 连接失败：%1")
                     .arg(QString::fromStdString(uiSessionError)));
        return 4;
    }
    std::unique_ptr<mcast::ui_process::PlaybackBridge> playbackBridge;
    std::atomic_bool windowFrameReady{false};
    std::atomic_bool mpvRendererReady{false};
    std::atomic_bool playbackUiReadySent{false};
    std::function<void()> notifyPlaybackReady;
    QObject::connect(
        &mainWindow, &MainWindow::castEndRequested,
        &mainWindow, [&] {
            if (playbackBridge) {
                writeLog(QStringLiteral("用户请求断开当前投送"));
                playbackBridge->requestEndSession();
                return;
            }
            writeLog(QStringLiteral(
                "断开连接失败：PlaybackBridge 尚未就绪"));
        });
    QObject::connect(
        &mainWindow, &MainWindow::castPauseRequested,
        &mainWindow, [&] {
            if (playbackBridge) {
                playbackBridge->pauseAndFlushState();
                return;
            }
            writeLog(QStringLiteral(
                "关闭窗口前暂停失败：PlaybackBridge 尚未就绪"));
        });
    std::function<void()> startPlaybackReconnect;
    std::jthread playbackReconnectThread;
    std::atomic_bool playbackReconnectDone{true};
    const auto createPlaybackBridge =
        [&](std::shared_ptr<mcast::ipc::PipeConnection> connection) {
        playbackBridge =
            std::make_unique<mcast::ui_process::PlaybackBridge>(
                std::move(connection),
                playback, uiSessionOptions.activationId);
        QObject::connect(
            playbackBridge.get(),
            &mcast::ui_process::PlaybackBridge::logMessage,
            &mainWindow, [&writeLog](const QString &text) {
                writeLog(QStringLiteral("Playback IPC  %1").arg(text));
            });
        playbackUiReadySent = false;
        if (notifyPlaybackReady)
            notifyPlaybackReady();
    };
    notifyPlaybackReady = [&] {
        if (!playbackBridge || !windowFrameReady
            || !mpvRendererReady || !player->isRunning()
            || playbackUiReadySent.exchange(true)) {
            return;
        }
        playbackBridge->notifyUiReady();
        writeLog(QStringLiteral(
            "Playback UI 就绪：窗口首帧、mpv renderer、PlaybackBridge 均已就绪"));
    };
    if (auto playbackConnection = uiSession.takePlaybackConnection()) {
        createPlaybackBridge(
            std::shared_ptr<mcast::ipc::PipeConnection>(
                std::move(playbackConnection)));
    }
    std::atomic_bool playbackReconnectPending{false};
    startPlaybackReconnect = [&] {
        if (playbackReconnectThread.joinable()) {
            if (!playbackReconnectDone.load()) {
                playbackReconnectPending = true;
                return;
            }
            playbackReconnectThread.join();
        }
        playbackReconnectDone = false;
        playbackReconnectThread = std::jthread(
            [&](std::stop_token stop) {
                std::string reconnectError;
                auto connection =
                    uiSession.reconnectPlayback(reconnectError);
                if (stop.stop_requested())
                    return;
                auto shared = connection
                    ? std::shared_ptr<mcast::ipc::PipeConnection>(
                          std::move(connection))
                    : std::shared_ptr<mcast::ipc::PipeConnection>{};
                QMetaObject::invokeMethod(
                    &mainWindow,
                    [&,
                     shared = std::move(shared),
                     reconnectError = std::move(reconnectError)] {
                        playbackReconnectDone = true;
                        if (shared) {
                            playbackBridge.reset();
                            createPlaybackBridge(shared);
                            writeLog(QStringLiteral(
                                "Playback IPC 已重新连接"));
                        } else {
                            writeLog(
                                QStringLiteral(
                                    "Playback IPC 重连失败：%1")
                                    .arg(QString::fromStdString(
                                        reconnectError)));
                        }
                        if (playbackReconnectPending.exchange(false))
                            startPlaybackReconnect();
                    },
                    Qt::QueuedConnection);
            });
    };

    std::unique_ptr<mcast::ui_process::UiConfirmationBridge>
        confirmationBridge;
    if (auto controlConnection = uiSession.takeControlConnection()) {
        confirmationBridge =
            std::make_unique<mcast::ui_process::UiConfirmationBridge>(
                std::shared_ptr<mcast::ipc::PipeConnection>(
                    std::move(controlConnection)));
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::requestReceived,
            &mainWindow, &MainWindow::showCastConfirmation);
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::cancelReceived,
            &mainWindow, &MainWindow::cancelCastConfirmation);
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::
                playbackReconnectRequested,
            &mainWindow, [&](qulonglong) {
                startPlaybackReconnect();
            });
        QObject::connect(
            &mainWindow, &MainWindow::castConfirmationResponded,
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::submitDecision);
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::logMessage,
            &mainWindow, [&writeLog](const QString &text) {
                writeLog(QStringLiteral("确认 IPC  %1").arg(text));
            });
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::connectionLost,
            &app, [&app, &writeLog](const QString &reason) {
                writeLog(QStringLiteral("Shell ControlPipe 断开：%1")
                             .arg(reason));
                app.quit();
            });
    }
    QObject::connect(
        &mainWindow, &MainWindow::firstFrameReady, &app, [&] {
        windowFrameReady = true;
        notifyPlaybackReady();
        if (!confirmationBridge)
            return;
        std::string error;
        if (!confirmationBridge->sendFirstFrame(
                uiSessionOptions.activationId, error)) {
            writeLog(QStringLiteral("FirstFrame 上报失败：%1")
                         .arg(QString::fromStdString(error)));
        }
    });
    QObject::connect(
        player.get(), &MpvCore::rendererReady,
        &mainWindow, [&] {
            mpvRendererReady = true;
            notifyPlaybackReady();
        });

    if (!uiSessionOptions.confirmationOnly) {
        player->start();
        const QVariantMap savedPicture = settings.pictureValues();
        for (auto it = savedPicture.constBegin();
             it != savedPicture.constEnd(); ++it) {
            playback.setPictureControl(it.key(), it.value().toInt());
        }
    }

    if (!uiSessionOptions.confirmationOnly)
        mainWindow.show();
    if (uiSessionOptions.testTakeoverWindow) {
        const QVariantMap request{
            {QStringLiteral("protocol"), 3},
            {QStringLiteral("peerName"), QStringLiteral("Living Room Phone")},
            {QStringLiteral("mediaTitle"), QStringLiteral("Example Movie")},
            {QStringLiteral("timeoutMs"), 10000},
        };
        QTimer::singleShot(0, &mainWindow, [&mainWindow, request] {
            mainWindow.showCastConfirmation(9001, request);
        });
    }
    return app.exec();
}
