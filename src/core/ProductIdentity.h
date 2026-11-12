// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <string_view>

namespace mcast::identity {

// Canonical English fallbacks. User-facing titles use terms.json when a
// language pack is available; protocol identity and broken-pack fallback use
// these values.
inline constexpr std::string_view appFullEnglish = "Media Cast Receiver";
inline constexpr std::string_view appShortEnglish = "Media Cast";
inline constexpr std::wstring_view appFullEnglishWide = L"Media Cast Receiver";
inline constexpr std::wstring_view appShortEnglishWide = L"Media Cast";

} // namespace mcast::identity
