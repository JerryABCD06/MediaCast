// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <winrt/Windows.Data.Json.h>

#include <filesystem>
#include <string>

namespace mcast::shell {

class ShellSettings
{
public:
    explicit ShellSettings(std::filesystem::path path);

    bool load(std::string &error);
    bool save(std::string &error) const;

    const std::filesystem::path &path() const noexcept { return m_path; }
    bool chineseUi() const;
    bool acceptNewCast() const;
    bool broadcast() const;

    void setAcceptNewCast(bool enabled);

private:
    bool ensureDefaults();
    bool ensureShellState();

    std::filesystem::path m_path;
    std::filesystem::path m_statePath;
    winrt::Windows::Data::Json::JsonObject m_root = nullptr;
    winrt::Windows::Data::Json::JsonObject m_stateRoot = nullptr;
    bool m_stateSaveAllowed = true;
};

} // namespace mcast::shell
