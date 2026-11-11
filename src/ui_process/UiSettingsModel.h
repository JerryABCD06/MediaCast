// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

namespace mcast::ui_process {

// UI 侧的用户设置镜像。文件读写和最终裁决都归 Shell；本类只负责把
// Shell 快照映射到 QML，并把用户修改合并成有界 JSON patch 发回去。
class UiSettingsModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool uiMica READ mica WRITE setMica NOTIFY micaChanged)
    Q_PROPERTY(bool castNewCast READ acceptNewCast WRITE setAcceptNewCast
                   NOTIFY acceptNewCastChanged)
    Q_PROPERTY(bool castBroadcast READ broadcast WRITE setBroadcast
                   NOTIFY broadcastChanged)
    Q_PROPERTY(int castBroadcastInterval READ broadcastIntervalMs
                   WRITE setBroadcastIntervalMs
                   NOTIFY broadcastIntervalChanged)

public:
    explicit UiSettingsModel(QObject *parent = nullptr);

    bool mica() const;
    bool acceptNewCast() const;
    bool broadcast() const;
    int broadcastIntervalMs() const;
    QString deviceName() const;
    QString deviceUdn() const;
    QVariantMap pictureValues() const;

    void setMica(bool value);
    void setAcceptNewCast(bool value);
    void setBroadcast(bool value);
    void setBroadcastIntervalMs(int value);
    void setPictureValue(const QString &name, int value);
    void queueUiSetting(
        const QString &key, const QJsonValue &value);

    bool applySnapshot(
        const QByteArray &json, std::uint64_t version = 0,
        QString *error = nullptr);
    void acceptPendingPatch(std::uint64_t version);
    void rejectPendingPatch();
    QByteArray snapshotJson() const;
    std::uint64_t version() const noexcept { return m_version; }

signals:
    void micaChanged();
    void acceptNewCastChanged();
    void broadcastChanged();
    void broadcastIntervalChanged();
    void settingsPatchReady(const QByteArray &json);

private:
    void setSectionValue(
        const QString &section, const QString &key,
        const QJsonValue &value);
    static void mergePatch(
        QJsonObject &target, const QJsonObject &patch);
    void queueValue(
        const QString &section, const QString &key,
        const QJsonValue &value);
    void flushPatch();
    void emitChanges(
        bool oldMica, bool oldAccept,
        bool oldBroadcast, int oldInterval);

    QJsonObject m_root;
    QJsonObject m_authoritativeRoot;
    QJsonObject m_patch;
    QJsonObject m_inFlightPatch;
    QTimer m_patchTimer;
    std::uint64_t m_version = 0;
};

} // namespace mcast::ui_process
