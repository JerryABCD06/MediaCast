// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ui_process/UiSettingsModel.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    mcast::ui_process::UiSettingsModel model;

    const QByteArray initial = R"({
        "ui":{"language":"en_US","darkmode":"Dark","accentmode":"WindowsBlue","mica":true},
        "cast":{"newcast":true,"broadcast":true,"broadcast_interval":10000},
        "device":{"name":"Living Room PC","udn":"uuid:11111111-2222-4333-8444-555555555555"},
        "picture":{"brightness":10}
    })";
    QString error;
    check(model.applySnapshot(initial, 5, &error),
          "settings snapshot applies");
    check(model.mica() && model.broadcast()
              && model.broadcastIntervalMs() == 10000
              && model.pictureValues().value(QStringLiteral("brightness")) == 10
              && model.deviceName() == QStringLiteral("Living Room PC"),
          "settings snapshot values");

    QByteArray patch;
    QEventLoop loop;
    QObject::connect(
        &model, &mcast::ui_process::UiSettingsModel::settingsPatchReady,
        &loop, [&](const QByteArray &json) {
            patch = json;
            loop.quit();
        });
    model.setMica(false);
    model.setBroadcast(false);
    model.setBroadcastIntervalMs(30000);
    model.setPictureValue(QStringLiteral("contrast"), 25);
    QTimer::singleShot(2000, &loop, &QEventLoop::quit);
    loop.exec();

    const QJsonObject object =
        QJsonDocument::fromJson(patch).object();
    const QJsonObject ui =
        object.value(QStringLiteral("ui")).toObject();
    const QJsonObject cast =
        object.value(QStringLiteral("cast")).toObject();
    const QJsonObject picture =
        object.value(QStringLiteral("picture")).toObject();
    check(!patch.isEmpty() && !ui.value(QStringLiteral("mica")).toBool(true),
          "mica patch is queued");
    check(!cast.value(QStringLiteral("broadcast")).toBool(true)
              && cast.value(QStringLiteral("broadcast_interval")).toInt()
                  == 30000,
          "broadcast patch is coalesced");
    check(picture.value(QStringLiteral("contrast")).toInt()
              == 25,
          "picture patch is coalesced");
    error.clear();
    check(model.applySnapshot(initial, 5, &error),
          "same-version authoritative snapshot is accepted");
    check(model.mica() && model.broadcast()
              && model.broadcastIntervalMs() == 10000
              && model.pictureValues().value(QStringLiteral("contrast"), -999)
                  == -999,
          "same-version snapshot rolls back an acknowledged optimistic patch");
    model.rejectPendingPatch();
    check(model.mica() && model.broadcast()
              && model.broadcastIntervalMs() == 10000
              && model.pictureValues().value(QStringLiteral("contrast"), -999)
                  == -999,
          "rejected patch restores the last authoritative snapshot");
    const QByteArray beforeStale = model.snapshotJson();
    error.clear();
    check(!model.applySnapshot(initial, 4, &error)
              && model.snapshotJson() == beforeStale,
          "stale settings snapshot is ignored");
    check(error == QStringLiteral("stale settings snapshot"),
          "stale settings snapshot is reported");

    model.setBroadcast(false);
    model.rejectPendingPatch();
    check(model.mica() && !model.broadcast()
              && model.broadcastIntervalMs() == 10000,
          "rejection restores authority while preserving newer unsent edits");

    std::cout << "UI settings model tests passed\n";
    return 0;
}
