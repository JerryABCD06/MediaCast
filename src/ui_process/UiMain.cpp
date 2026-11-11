// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

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
#include "ui_process/UiSettingsModel.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QJsonObject>
#include <QMetaObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalBlocker>
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

    SystemAccentColors systemColors;
    systemColors.refresh();
    qmlRegisterSingletonInstance(
        "MediaCast", 1, 0, "SystemColors", &systemColors);

    Tr tr;
    QObject::connect(&tr, &Tr::logMessage, writeLog);

    UiSession uiSession;
    if (!uiSession.connectToPeers(uiSessionOptions, uiSessionError)) {
        writeLog(QStringLiteral("IPC 连接失败：%1")
                     .arg(QString::fromStdString(uiSessionError)));
        return 4;
    }
    QString languageError;
    if (!tr.applyCatalog(
            QByteArray::fromStdString(
                uiSession.languageCatalogJson()),
            &languageError)) {
        writeLog(QStringLiteral("语言目录解析失败：%1")
                     .arg(languageError));
        return 5;
    }
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Tr", &tr);
    QString resolvedLanguage =
        QString::fromStdString(uiSession.resolvedLanguage());

    mcast::ui_process::UiSettingsModel settings;
    QString settingsError;
    if (!settings.applySnapshot(
            QByteArray::fromStdString(uiSession.settingsJson()),
            uiSession.settingsVersion(),
            &settingsError)) {
        writeLog(QStringLiteral("设置快照解析失败：%1")
                     .arg(settingsError));
        return 5;
    }
    UiState uiState(&tr);
    if (!uiState.applySettingsJson(
            QByteArray::fromStdString(uiSession.settingsJson()),
            resolvedLanguage,
            &settingsError)) {
        writeLog(QStringLiteral("外观快照解析失败：%1")
                     .arg(settingsError));
    }
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
    device.applyIdentity(
        settings.deviceName(), settings.deviceUdn());
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Device", &device);

    MainWindow mainWindow(nullptr);
    QObject::connect(&mainWindow, &MainWindow::logMessage, writeLog);
    const auto applyAuthoritativeSettings = [&] {
        QString error;
        if (!uiState.applySettingsJson(
                settings.snapshotJson(),
                resolvedLanguage, &error)) {
            writeLog(
                QStringLiteral("外观设置回滚失败：%1").arg(error));
        }
        device.applyIdentity(
            settings.deviceName(), settings.deviceUdn());
        const QSignalBlocker pictureSignalBlocker(&playback);
        playback.resetPictureControls();
        const QVariantMap picture = settings.pictureValues();
        for (auto it = picture.constBegin();
             it != picture.constEnd(); ++it) {
            playback.setPictureControl(
                it.key(), it.value().toInt());
        }
    };
    QObject::connect(
        &playback, &PlaybackController::logMessage,
        &mainWindow, [&writeLog](const QString &text) {
            writeLog(QStringLiteral("Playback  %1").arg(text));
        });
    QObject::connect(
        &playback, &PlaybackController::pictureControlChanged,
        &settings,
        [&settings](const QString &name, int value) {
            settings.setPictureValue(name, value);
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

    QObject::connect(
        &uiState, &UiState::themeModeChangeRequested,
        &settings, [&settings](int mode) {
            const QString value =
                mode == UiState::Dark
                ? QStringLiteral("Dark")
                : (mode == UiState::Light
                       ? QStringLiteral("Light")
                       : QStringLiteral("System"));
            settings.queueUiSetting(
                QStringLiteral("darkmode"), value);
        });
    QObject::connect(
        &uiState, &UiState::accentModeChangeRequested,
        &settings, [&settings](int mode) {
            settings.queueUiSetting(
                QStringLiteral("accentmode"),
                mode == UiState::SystemAccent
                    ? QStringLiteral("System")
                    : QStringLiteral("WindowsBlue"));
        });
    QObject::connect(
        &uiState, &UiState::languageChangeRequested,
        &settings, [&settings](const QString &language) {
            settings.queueUiSetting(
                QStringLiteral("language"),
                language.isEmpty()
                    ? QStringLiteral("System") : language);
        });

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
            &settings,
            &mcast::ui_process::UiSettingsModel::settingsPatchReady,
            confirmationBridge.get(),
            [&](const QByteArray &json) {
                std::string error;
                if (!confirmationBridge->submitSettings(
                        json, settings.version(), error)) {
                    writeLog(
                        QStringLiteral("设置更新发送失败：%1")
                            .arg(QString::fromStdString(error)));
                }
            });
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::settingsReceived,
            &mainWindow, [&](const QByteArray &json, qulonglong version,
                              const QString &newResolvedLanguage) {
                QString error;
                if (!settings.applySnapshot(
                        json, version, &error)) {
                    writeLog(
                        QStringLiteral("设置快照更新失败：%1")
                            .arg(error));
                    return;
                }
                resolvedLanguage = newResolvedLanguage;
                applyAuthoritativeSettings();
            });
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::
                settingsSubmitFailed,
            &mainWindow, [&](const QString &reason) {
                settings.rejectPendingPatch();
                applyAuthoritativeSettings();
                writeLog(
                    QStringLiteral("设置更新发送失败：%1")
                        .arg(reason));
            });
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::
                settingsUpdateRejected,
            &mainWindow, [&](const QString &reason) {
                settings.rejectPendingPatch();
                applyAuthoritativeSettings();
                writeLog(
                    QStringLiteral("设置更新被 Shell 拒绝：%1")
                        .arg(reason));
            });
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::
                settingsRuntimeApplyFailed,
            &mainWindow, [&](const QString &reason) {
                writeLog(
                    QStringLiteral("设置已保存但运行态应用失败：%1")
                        .arg(reason));
            });
        QObject::connect(
            confirmationBridge.get(),
            &mcast::ui_process::UiConfirmationBridge::
                settingsCommitted,
            &mainWindow, [&](qulonglong version,
                              const QString &newResolvedLanguage) {
                settings.acceptPendingPatch(
                    static_cast<std::uint64_t>(version));
                if (newResolvedLanguage == resolvedLanguage)
                    return;
                resolvedLanguage = newResolvedLanguage;
                QString error;
                if (!uiState.applySettingsJson(
                        settings.snapshotJson(),
                        resolvedLanguage, &error)) {
                    writeLog(
                        QStringLiteral("语言切换失败：%1")
                            .arg(error));
                }
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
