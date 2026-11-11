// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

namespace mcast::platform {

struct AccentPair {
    unsigned int light = 0x0067c0;
    unsigned int dark = 0x4cc2ff;
};

// FluentUI Blue uses Blue.dark in light mode and Blue.lighter in dark mode.
inline constexpr AccentPair WindowsBlueAccent{0x0067c0, 0x4cc2ff};

} // namespace mcast::platform
