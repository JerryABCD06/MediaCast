// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ui_process/UiSettingsModel.h"

#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>

namespace mcast::ui_process {

UiSettingsModel::UiSettingsModel(QObject *parent)
    : QObject(parent)
{
    m_patchTimer.setSingleShot(true);
    m_patchTimer.setInterval(250);
    connect(
        &m_patchTimer, &QTimer::timeout,
        this, &UiSettingsModel::flushPatch);
}

bool UiSettingsModel::mica() const
{
    return m_root.value(QStringLiteral("ui")).toObject()
        .value(QStringLiteral("mica")).toBool(true);
}

bool UiSettingsModel::acceptNewCast() const
{
    return m_root.value(QStringLiteral("cast")).toObject()
        .value(QStringLiteral("newcast")).toBool(true);
}

bool UiSettingsModel::broadcast() const
{
    return m_root.value(QStringLiteral("cast")).toObject()
        .value(QStringLiteral("broadcast")).toBool(true);
}

int UiSettingsModel::broadcastIntervalMs() const
{
    return m_root.value(QStringLiteral("cast")).toObject()
        .value(QStringLiteral("broadcast_interval")).toInt(10000);
}

QString UiSettingsModel::deviceName() const
{
    return m_root.value(QStringLiteral("device")).toObject()
        .value(QStringLiteral("name")).toString();
}

QString UiSettingsModel::deviceUdn() const
{
    return m_root.value(QStringLiteral("device")).toObject()
        .value(QStringLiteral("udn")).toString();
}

QVariantMap UiSettingsModel::pictureValues() const
{
    QVariantMap values;
    const QJsonObject picture =
        m_root.value(QStringLiteral("picture")).toObject();
    for (auto it = picture.constBegin(); it != picture.constEnd(); ++it) {
        if (it.value().isDouble())
            values.insert(it.key(), it.value().toInt());
    }
    return values;
}

void UiSettingsModel::setMica(bool value)
{
    if (mica() == value)
        return;
    setSectionValue(
        QStringLiteral("ui"), QStringLiteral("mica"), value);
    queueValue(QStringLiteral("ui"), QStringLiteral("mica"), value);
    emit micaChanged();
}

void UiSettingsModel::setAcceptNewCast(bool value)
{
    if (acceptNewCast() == value)
        return;
    setSectionValue(
        QStringLiteral("cast"), QStringLiteral("newcast"), value);
    queueValue(
        QStringLiteral("cast"), QStringLiteral("newcast"), value);
    emit acceptNewCastChanged();
}

void UiSettingsModel::setBroadcast(bool value)
{
    if (broadcast() == value)
        return;
    setSectionValue(
        QStringLiteral("cast"), QStringLiteral("broadcast"), value);
    queueValue(
        QStringLiteral("cast"), QStringLiteral("broadcast"), value);
    emit broadcastChanged();
}

void UiSettingsModel::setBroadcastIntervalMs(int value)
{
    if (value <= 0 || broadcastIntervalMs() == value)
        return;
    setSectionValue(
        QStringLiteral("cast"),
        QStringLiteral("broadcast_interval"), value);
    queueValue(
        QStringLiteral("cast"),
        QStringLiteral("broadcast_interval"), value);
    emit broadcastIntervalChanged();
}

void UiSettingsModel::setPictureValue(const QString &name, int value)
{
    if (name.isEmpty())
        return;
    QJsonObject picture =
        m_root.value(QStringLiteral("picture")).toObject();
    if (picture.value(name).toInt(value) == value
        && picture.contains(name))
        return;
    picture.insert(name, value);
    m_root.insert(QStringLiteral("picture"), picture);
    queueValue(QStringLiteral("picture"), name, value);
}

void UiSettingsModel::queueUiSetting(
    const QString &key, const QJsonValue &value)
{
    setSectionValue(QStringLiteral("ui"), key, value);
    queueValue(QStringLiteral("ui"), key, value);
}

bool UiSettingsModel::applySnapshot(
    const QByteArray &json, std::uint64_t version, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument document =
        QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (error) {
            *error = parseError.errorString();
        }
        return false;
    }
    const QJsonObject candidate = document.object();
    const QJsonObject ui =
        candidate.value(QStringLiteral("ui")).toObject();
    const QJsonObject cast =
        candidate.value(QStringLiteral("cast")).toObject();
    const QJsonObject device =
        candidate.value(QStringLiteral("device")).toObject();
    if (ui.isEmpty() || cast.isEmpty() || device.isEmpty()
        || !ui.value(QStringLiteral("language")).isString()
        || !ui.value(QStringLiteral("darkmode")).isString()
        || !ui.value(QStringLiteral("accentmode")).isString()
        || !ui.value(QStringLiteral("mica")).isBool()
        || !cast.value(QStringLiteral("newcast")).isBool()
        || !cast.value(QStringLiteral("broadcast")).isBool()
        || !cast.value(QStringLiteral("broadcast_interval")).isDouble()
        || !device.value(QStringLiteral("name")).isString()
        || !device.value(QStringLiteral("udn")).isString()) {
        if (error)
            *error = QStringLiteral("incomplete settings snapshot");
        return false;
    }

    if (version < m_version) {
        if (error)
            *error = QStringLiteral("stale settings snapshot");
        return false;
    }
    const bool oldMica = mica();
    const bool oldAccept = acceptNewCast();
    const bool oldBroadcast = broadcast();
    const int oldInterval = broadcastIntervalMs();
    m_authoritativeRoot = candidate;
    m_root = m_authoritativeRoot;
    mergePatch(m_root, m_patch);
    m_version = version;
    emitChanges(oldMica, oldAccept, oldBroadcast, oldInterval);
    return true;
}

void UiSettingsModel::acceptPendingPatch(std::uint64_t version)
{
    const bool oldMica = mica();
    const bool oldAccept = acceptNewCast();
    const bool oldBroadcast = broadcast();
    const int oldInterval = broadcastIntervalMs();
    mergePatch(m_authoritativeRoot, m_inFlightPatch);
    m_inFlightPatch = {};
    m_version = std::max(m_version, version);
    m_root = m_authoritativeRoot;
    mergePatch(m_root, m_patch);
    if (!m_patch.isEmpty())
        m_patchTimer.start();
    emitChanges(oldMica, oldAccept, oldBroadcast, oldInterval);
}

void UiSettingsModel::rejectPendingPatch()
{
    m_patchTimer.stop();
    const bool oldMica = mica();
    const bool oldAccept = acceptNewCast();
    const bool oldBroadcast = broadcast();
    const int oldInterval = broadcastIntervalMs();
    m_inFlightPatch = {};
    m_root = m_authoritativeRoot;
    mergePatch(m_root, m_patch);
    if (!m_patch.isEmpty())
        m_patchTimer.start();
    emitChanges(oldMica, oldAccept, oldBroadcast, oldInterval);
}

QByteArray UiSettingsModel::snapshotJson() const
{
    return QJsonDocument(m_root).toJson(QJsonDocument::Compact);
}

void UiSettingsModel::emitChanges(
    bool oldMica, bool oldAccept,
    bool oldBroadcast, int oldInterval)
{
    if (oldMica != mica())
        emit micaChanged();
    if (oldAccept != acceptNewCast())
        emit acceptNewCastChanged();
    if (oldBroadcast != broadcast())
        emit broadcastChanged();
    if (oldInterval != broadcastIntervalMs())
        emit broadcastIntervalChanged();
}

void UiSettingsModel::setSectionValue(
    const QString &sectionName, const QString &key,
    const QJsonValue &value)
{
    QJsonObject object =
        m_root.value(sectionName).toObject();
    object.insert(key, value);
    m_root.insert(sectionName, object);
}

void UiSettingsModel::mergePatch(
    QJsonObject &target, const QJsonObject &patch)
{
    for (auto section = patch.constBegin();
         section != patch.constEnd(); ++section) {
        if (!section.value().isObject())
            continue;
        QJsonObject object =
            target.value(section.key()).toObject();
        const QJsonObject values = section.value().toObject();
        for (auto value = values.constBegin();
             value != values.constEnd(); ++value) {
            object.insert(value.key(), value.value());
        }
        target.insert(section.key(), object);
    }
}

void UiSettingsModel::queueValue(
    const QString &sectionName, const QString &key,
    const QJsonValue &value)
{
    QJsonObject patchSection =
        m_patch.value(sectionName).toObject();
    patchSection.insert(key, value);
    m_patch.insert(sectionName, patchSection);
    m_patchTimer.start();
}

void UiSettingsModel::flushPatch()
{
    if (m_patch.isEmpty())
        return;
    if (!m_inFlightPatch.isEmpty()) {
        m_patchTimer.start();
        return;
    }
    const QByteArray json =
        QJsonDocument(m_patch).toJson(QJsonDocument::Compact);
    m_inFlightPatch = m_patch;
    m_patch = {};
    emit settingsPatchReady(json);
}

} // namespace mcast::ui_process
