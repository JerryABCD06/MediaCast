// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/EventLoop.h"
#include "QtFree/Json.h"
#include "QtFree/Signal.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace mcast::avalonia::qtfree {

class UiSettingsModel
{
public:
    explicit UiSettingsModel(EventLoop &loop);
    ~UiSettingsModel();

    UiSettingsModel(const UiSettingsModel &) = delete;
    UiSettingsModel &operator=(const UiSettingsModel &) = delete;

    bool mica() const;
    bool acceptNewCast() const;
    bool broadcast() const;
    int broadcastIntervalMs() const;
    std::string deviceName() const;
    std::string deviceUdn() const;
    std::map<std::string, int, std::less<>> pictureValues() const;

    void setMica(bool value);
    void setAcceptNewCast(bool value);
    void setBroadcast(bool value);
    void setBroadcastIntervalMs(int value);
    void setPictureValue(std::string_view name, int value);
    void queueUiSetting(std::string key, JsonValue value);

    bool applySnapshot(
        std::string_view json, std::uint64_t version = 0,
        std::string *error = nullptr);
    void acceptPendingPatch(std::uint64_t version);
    void rejectPendingPatch();
    std::string snapshotJson() const;
    std::uint64_t version() const noexcept { return m_version; }

    Signal<> micaChanged;
    Signal<> acceptNewCastChanged;
    Signal<> broadcastChanged;
    Signal<> broadcastIntervalChanged;
    Signal<const std::string &> settingsPatchReady;

private:
    void setSectionValue(
        std::string_view section, std::string_view key,
        JsonValue value);
    static void mergePatch(JsonValue &target, const JsonValue &patch);
    void queueValue(
        std::string_view section, std::string_view key,
        JsonValue value);
    void scheduleFlush();
    void flushPatch();
    void emitChanges(
        bool oldMica, bool oldAccept,
        bool oldBroadcast, int oldInterval);

    EventLoop &m_loop;
    JsonValue m_root = JsonValue::object();
    JsonValue m_authoritativeRoot = JsonValue::object();
    JsonValue m_patch = JsonValue::object();
    JsonValue m_inFlightPatch = JsonValue::object();
    std::uint64_t m_patchTimer = 0;
    std::uint64_t m_version = 0;
};

} // namespace mcast::avalonia::qtfree

