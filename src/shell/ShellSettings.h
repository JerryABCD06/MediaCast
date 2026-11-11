// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <winrt/Windows.Data.Json.h>

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

namespace mcast::shell {

class ShellSettings
{
public:
    explicit ShellSettings(std::filesystem::path path);

    bool load(std::string &error);
    bool save(std::string &error) const;

    const std::filesystem::path &path() const noexcept { return m_path; }
    std::string language() const;
    std::string darkMode() const;
    std::string accentMode() const;
    std::string deviceName() const;
    std::string deviceUdn() const;
    bool chineseUi() const;
    bool acceptNewCast() const;
    bool broadcast() const;
    int broadcastIntervalMs() const;
    bool mica() const;
    std::map<std::string, int> pictureValues() const;

    void setAcceptNewCast(bool enabled);
    bool setAcceptNewCastAndSave(
        bool enabled, std::string &error);
    bool applyJsonPatch(std::string_view patch, std::string &error);
    bool applyJsonPatchAndSave(
        std::string_view patch, std::string &error);
    std::string snapshotJson() const;

private:
    bool ensureDefaults();
    bool ensureShellState();

    std::filesystem::path m_path;
    std::filesystem::path m_statePath;
    winrt::Windows::Data::Json::JsonObject m_root = nullptr;
    winrt::Windows::Data::Json::JsonObject m_stateRoot = nullptr;
    bool m_mainSaveAllowed = true;
    bool m_stateSaveAllowed = true;
};

} // namespace mcast::shell
