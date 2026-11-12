// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <chrono>
#include <string>

namespace mcast::platform {

enum class CastConfirmationChoice {
    Accepted,
    Declined,
    TimedOut,
    Cancelled,
    Failed,
};

struct CastConfirmationPrompt {
    std::wstring appName;
    std::wstring protocolName;
    std::wstring peerName;
    std::wstring mediaTitle;
    std::chrono::milliseconds timeout{10000};
    HANDLE cancelEvent = nullptr;
    bool chinese = false;
};

CastConfirmationChoice showCastConfirmationDialog(
    HWND owner, const CastConfirmationPrompt &prompt);

} // namespace mcast::platform
