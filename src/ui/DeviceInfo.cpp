// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "DeviceInfo.h"

#include <QCoreApplication>
#include <QSysInfo>

DeviceInfo::DeviceInfo(QObject *parent)
    : QObject(parent)
    , m_deviceName(QSysInfo::machineHostName())
{
}

void DeviceInfo::applyIdentity(
    const QString &deviceName, const QString &udn,
    const QString &address, const QString &locationUrl)
{
    if (!deviceName.isEmpty() && m_deviceName != deviceName) {
        m_deviceName = deviceName;
        emit deviceNameChanged();
    }
    if (m_udn != udn) {
        m_udn = udn;
        emit udnChanged();
    }
    if (m_address != address) {
        m_address = address;
        emit addressChanged();
    }
    if (m_locationUrl != locationUrl) {
        m_locationUrl = locationUrl;
        emit locationUrlChanged();
    }
}

QString DeviceInfo::appVersion() const
{
    return QCoreApplication::applicationVersion();
}
