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
#include "ui_process/UiSession.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTextStream>
#include <QTime>
#include <QtQml/qqml.h>

#include <memory>

int main(int argc, char *argv[])
{
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

    std::string uiSessionError;
    const UiSession::Options uiSessionOptions =
        UiSession::parseArguments(argc, argv, uiSessionError);
    if (!uiSessionError.empty()) {
        writeLog(QString::fromStdString(uiSessionError));
        return 3;
    }
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
        &mainWindow, &MainWindow::windowClosed, &app, &QApplication::quit);
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
    QObject::connect(
        &mainWindow, &MainWindow::firstFrameReady, &app, [&] {
        std::string error;
        if (!uiSession.sendFirstFrame(error)) {
            writeLog(QStringLiteral("FirstFrame 上报失败：%1")
                         .arg(QString::fromStdString(error)));
        }
    });

    player->start();
    const QVariantMap savedPicture = settings.pictureValues();
    for (auto it = savedPicture.constBegin(); it != savedPicture.constEnd(); ++it)
        playback.setPictureControl(it.key(), it.value().toInt());

    mainWindow.show();
    return app.exec();
}
