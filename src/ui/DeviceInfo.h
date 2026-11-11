// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>
#include <QString>

class DeviceInfo : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY deviceNameChanged)
    Q_PROPERTY(QString address READ address NOTIFY addressChanged)
    Q_PROPERTY(QString locationUrl READ locationUrl NOTIFY locationUrlChanged)
    Q_PROPERTY(QString udn READ udn NOTIFY udnChanged)
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

public:
    explicit DeviceInfo(QObject *parent = nullptr);
    void applyIdentity(
        const QString &deviceName, const QString &udn,
        const QString &address = {}, const QString &locationUrl = {});

    QString deviceName() const { return m_deviceName; }
    QString address() const { return m_address; }
    QString locationUrl() const { return m_locationUrl; }
    QString udn() const { return m_udn; }
    QString appVersion() const;

signals:
    void deviceNameChanged();
    void udnChanged();
    void addressChanged();
    void locationUrlChanged();

private:
    QString m_deviceName;
    QString m_udn;
    QString m_address;
    QString m_locationUrl;
};
