// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "DeviceInfo.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>

DeviceInfo::DeviceInfo(QObject *parent)
    : QObject(parent)
    , m_deviceName(QSysInfo::machineHostName())
{
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
        + QStringLiteral("/renderer.ini");
    QSettings settings(path, QSettings::IniFormat);
    const QString uuid = settings.value(QStringLiteral("device/uuid")).toString();
    if (!uuid.isEmpty())
        m_udn = QStringLiteral("uuid:") + uuid;
}

QString DeviceInfo::appVersion() const
{
    return QCoreApplication::applicationVersion();
}
