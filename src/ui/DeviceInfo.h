// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>
#include <QString>

class DeviceInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString deviceName READ deviceName CONSTANT)
    Q_PROPERTY(QString address READ address CONSTANT)
    Q_PROPERTY(QString locationUrl READ locationUrl CONSTANT)
    Q_PROPERTY(QString udn READ udn CONSTANT)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

public:
    explicit DeviceInfo(QObject *parent = nullptr);

    QString deviceName() const { return m_deviceName; }
    QString address() const { return {}; }
    QString locationUrl() const { return {}; }
    QString udn() const { return m_udn; }
    QString appVersion() const;

private:
    QString m_deviceName;
    QString m_udn;
};
