// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/UiSettingsModel.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

const JsonValue *sectionValue(
    const JsonValue &root, std::string_view section,
    std::string_view key)
{
    const JsonValue *object = root.find(section);
    return object && object->isObject() ? object->find(key) : nullptr;
}

bool hasObject(const JsonValue &root, std::string_view section)
{
    const JsonValue *value = root.find(section);
    return value && value->isObject() && !value->members().empty();
}

bool validSnapshot(const JsonValue &root)
{
    const JsonValue *ui = root.find("ui");
    const JsonValue *cast = root.find("cast");
    const JsonValue *device = root.find("device");
    return hasObject(root, "ui") && hasObject(root, "cast")
        && hasObject(root, "device")
        && ui->at("language").isString()
        && ui->at("darkmode").isString()
        && ui->at("accentmode").isString()
        && ui->at("mica").isBool()
        && cast->at("newcast").isBool()
        && cast->at("broadcast").isBool()
        && cast->at("broadcast_interval").isNumber()
        && device->at("name").isString()
        && device->at("udn").isString();
}

} // namespace

UiSettingsModel::UiSettingsModel(EventLoop &loop)
    : m_loop(loop)
{
}

UiSettingsModel::~UiSettingsModel()
{
    if (m_patchTimer != 0)
        m_loop.cancelTimer(m_patchTimer);
}

bool UiSettingsModel::mica() const
{
    const JsonValue *value = sectionValue(m_root, "ui", "mica");
    return value && value->isBool() ? value->asBool() : true;
}

bool UiSettingsModel::acceptNewCast() const
{
    const JsonValue *value = sectionValue(m_root, "cast", "newcast");
    return value && value->isBool() ? value->asBool() : true;
}

bool UiSettingsModel::broadcast() const
{
    const JsonValue *value = sectionValue(m_root, "cast", "broadcast");
    return value && value->isBool() ? value->asBool() : true;
}

int UiSettingsModel::broadcastIntervalMs() const
{
    const JsonValue *value =
        sectionValue(m_root, "cast", "broadcast_interval");
    return value && value->isNumber()
        ? value->asInt(10000) : 10000;
}

std::string UiSettingsModel::deviceName() const
{
    const JsonValue *value = sectionValue(m_root, "device", "name");
    return value ? value->asString() : std::string{};
}

std::string UiSettingsModel::deviceUdn() const
{
    const JsonValue *value = sectionValue(m_root, "device", "udn");
    return value ? value->asString() : std::string{};
}

std::map<std::string, int, std::less<>>
UiSettingsModel::pictureValues() const
{
    std::map<std::string, int, std::less<>> values;
    const JsonValue *picture = m_root.find("picture");
    if (!picture || !picture->isObject())
        return values;
    for (const auto &[key, value] : picture->members()) {
        if (value.isNumber())
            values.emplace(key, value.asInt());
    }
    return values;
}

void UiSettingsModel::setMica(bool value)
{
    if (mica() == value)
        return;
    setSectionValue("ui", "mica", JsonValue(value));
    queueValue("ui", "mica", JsonValue(value));
    micaChanged.emit();
}

void UiSettingsModel::setAcceptNewCast(bool value)
{
    if (acceptNewCast() == value)
        return;
    setSectionValue("cast", "newcast", JsonValue(value));
    queueValue("cast", "newcast", JsonValue(value));
    acceptNewCastChanged.emit();
}

void UiSettingsModel::setBroadcast(bool value)
{
    if (broadcast() == value)
        return;
    setSectionValue("cast", "broadcast", JsonValue(value));
    queueValue("cast", "broadcast", JsonValue(value));
    broadcastChanged.emit();
}

void UiSettingsModel::setBroadcastIntervalMs(int value)
{
    if (value <= 0 || broadcastIntervalMs() == value)
        return;
    const JsonValue number(static_cast<double>(value));
    setSectionValue("cast", "broadcast_interval", number);
    queueValue("cast", "broadcast_interval", number);
    broadcastIntervalChanged.emit();
}

void UiSettingsModel::setPictureValue(
    std::string_view name, int value)
{
    if (name.empty())
        return;
    value = std::clamp(value, -1000, 1000);
    JsonValue *picture = m_root.find("picture");
    if (!picture || !picture->isObject()) {
        m_root.set("picture", JsonValue::object());
        picture = m_root.find("picture");
    }
    const JsonValue *existing = picture->find(name);
    if (existing && existing->isNumber()
        && existing->asInt(value) == value) {
        return;
    }
    const JsonValue number(static_cast<double>(value));
    picture->set(std::string(name), number);
    queueValue("picture", name, number);
}

void UiSettingsModel::queueUiSetting(
    std::string key, JsonValue value)
{
    setSectionValue("ui", key, value);
    queueValue("ui", key, std::move(value));
}

bool UiSettingsModel::applySnapshot(
    std::string_view json, std::uint64_t version,
    std::string *error)
{
    JsonError parseError;
    const auto parsed = JsonValue::parse(json, &parseError);
    if (!parsed || !parsed->isObject()) {
        if (error) {
            *error = parsed
                ? "settings snapshot is not an object"
                : "invalid settings JSON at "
                    + std::to_string(parseError.offset)
                    + ": " + parseError.message;
        }
        return false;
    }
    if (!validSnapshot(*parsed)) {
        if (error)
            *error = "incomplete settings snapshot";
        return false;
    }
    if (version < m_version) {
        if (error)
            *error = "stale settings snapshot";
        return false;
    }

    const bool oldMica = mica();
    const bool oldAccept = acceptNewCast();
    const bool oldBroadcast = broadcast();
    const int oldInterval = broadcastIntervalMs();
    m_authoritativeRoot = *parsed;
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
    m_inFlightPatch = JsonValue::object();
    m_version = std::max(m_version, version);
    m_root = m_authoritativeRoot;
    mergePatch(m_root, m_patch);
    if (!m_patch.members().empty())
        scheduleFlush();
    emitChanges(oldMica, oldAccept, oldBroadcast, oldInterval);
}

void UiSettingsModel::rejectPendingPatch()
{
    if (m_patchTimer != 0)
        m_loop.cancelTimer(m_patchTimer);
    m_patchTimer = 0;
    const bool oldMica = mica();
    const bool oldAccept = acceptNewCast();
    const bool oldBroadcast = broadcast();
    const int oldInterval = broadcastIntervalMs();
    m_inFlightPatch = JsonValue::object();
    m_root = m_authoritativeRoot;
    mergePatch(m_root, m_patch);
    if (!m_patch.members().empty())
        scheduleFlush();
    emitChanges(oldMica, oldAccept, oldBroadcast, oldInterval);
}

std::string UiSettingsModel::snapshotJson() const
{
    return m_root.dump();
}

void UiSettingsModel::emitChanges(
    bool oldMica, bool oldAccept,
    bool oldBroadcast, int oldInterval)
{
    if (oldMica != mica())
        micaChanged.emit();
    if (oldAccept != acceptNewCast())
        acceptNewCastChanged.emit();
    if (oldBroadcast != broadcast())
        broadcastChanged.emit();
    if (oldInterval != broadcastIntervalMs())
        broadcastIntervalChanged.emit();
}

void UiSettingsModel::setSectionValue(
    std::string_view sectionName, std::string_view key,
    JsonValue value)
{
    JsonValue *section = m_root.find(sectionName);
    if (!section || !section->isObject()) {
        m_root.set(std::string(sectionName), JsonValue::object());
        section = m_root.find(sectionName);
    }
    section->set(std::string(key), std::move(value));
}

void UiSettingsModel::mergePatch(
    JsonValue &target, const JsonValue &patch)
{
    if (!patch.isObject())
        return;
    for (const auto &[sectionName, sectionValue] : patch.members()) {
        if (!sectionValue.isObject())
            continue;
        JsonValue *targetSection = target.find(sectionName);
        if (!targetSection || !targetSection->isObject()) {
            target.set(sectionName, JsonValue::object());
            targetSection = target.find(sectionName);
        }
        for (const auto &[key, value] : sectionValue.members())
            targetSection->set(key, value);
    }
}

void UiSettingsModel::queueValue(
    std::string_view sectionName, std::string_view key,
    JsonValue value)
{
    JsonValue *patchSection = m_patch.find(sectionName);
    if (!patchSection || !patchSection->isObject()) {
        m_patch.set(std::string(sectionName), JsonValue::object());
        patchSection = m_patch.find(sectionName);
    }
    patchSection->set(std::string(key), std::move(value));
    scheduleFlush();
}

void UiSettingsModel::scheduleFlush()
{
    if (m_patchTimer != 0)
        m_loop.cancelTimer(m_patchTimer);
    m_patchTimer = m_loop.scheduleAfter(
        std::chrono::milliseconds(250), [this] {
            m_patchTimer = 0;
            flushPatch();
        });
}

void UiSettingsModel::flushPatch()
{
    if (m_patch.members().empty())
        return;
    if (!m_inFlightPatch.members().empty()) {
        scheduleFlush();
        return;
    }
    const std::string json = m_patch.dump();
    m_inFlightPatch = m_patch;
    m_patch = JsonValue::object();
    settingsPatchReady.emit(json);
}

} // namespace mcast::avalonia::qtfree

