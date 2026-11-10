// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/IpcProtocol.h"

#include <string>

namespace mcast::skeleton {

struct ClientOptions {
    ipc::ProcessRole role = ipc::ProcessRole::Unknown;
    std::string instance;
    std::string nonce;
    std::uint32_t shellPid = 0;
    std::uint64_t activationId = 0;
    bool requestUi = false;
    bool persistent = false;
    std::uint32_t uiRequestRounds = 1;
};

bool parseClientOptions(
    int argc, wchar_t **argv, ipc::ProcessRole role,
    ClientOptions &options, std::string &error);

int runClient(const ClientOptions &options);

} // namespace mcast::skeleton
