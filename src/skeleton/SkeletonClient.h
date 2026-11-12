// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "core/ProductIdentity.h"

#include "ipc/IpcProtocol.h"

#include <string>

namespace mcast::skeleton {

struct ClientOptions {
    ipc::ProcessRole role = ipc::ProcessRole::Unknown;
    std::string instance;
    std::string nonce;
    std::string friendlyName{
        mcast::identity::appFullEnglish};
    std::string udn = "uuid:mcast-default";
    std::uint32_t shellPid = 0;
    std::uint64_t activationId = 0;
    bool requestUi = false;
    bool persistent = false;
    bool disableSsdp = false;
    bool initialReceivingEnabled = true;
    bool initialBroadcasting = true;
    std::uint32_t broadcastIntervalMs = 10000;
    std::uint16_t dlnaHttpPort = 0;
    std::uint16_t dlnaSsdpPort = 1900;
    std::string protocolLogPath;
    std::uint32_t uiRequestRounds = 1;
};

bool parseClientOptions(
    int argc, wchar_t **argv, ipc::ProcessRole role,
    ClientOptions &options, std::string &error);

int runClient(const ClientOptions &options);

} // namespace mcast::skeleton
