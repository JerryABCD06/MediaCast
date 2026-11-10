// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mcast::supervisor {

std::string currentBuildId();
std::filesystem::path currentExecutablePath();
std::optional<std::filesystem::path>
queryProcessPath(std::uint32_t processId);
std::optional<std::string>
sha256File(const std::filesystem::path &path);
std::optional<std::string> randomHexToken(std::uint32_t bytes);

} // namespace mcast::supervisor
