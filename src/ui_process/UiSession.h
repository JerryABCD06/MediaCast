// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/Messages.h"

#include <cstdint>
#include <memory>
#include <string>

namespace mcast::ipc {
class PipeConnection;
}

class UiSession
{
public:
    struct Options {
        bool enabled = false;
        bool standalone = false;
        bool confirmationOnly = false;
        bool testTakeoverWindow = false;
        std::string instance;
        std::string nonce;
        std::uint32_t shellPid = 0;
        std::uint64_t activationId = 0;
    };

    static Options parseArguments(int argc, char **argv, std::string &error);

    UiSession();
    ~UiSession();

    bool connectToPeers(const Options &options, std::string &error);
    std::unique_ptr<mcast::ipc::PipeConnection> reconnectPlayback(
        std::string &error);
    std::unique_ptr<mcast::ipc::PipeConnection> takeControlConnection();
    std::unique_ptr<mcast::ipc::PipeConnection> takePlaybackConnection();
    const std::string &settingsJson() const noexcept
    {
        return m_settingsJson;
    }
    std::uint64_t settingsVersion() const noexcept
    {
        return m_settingsVersion;
    }

private:
    Options m_options;
    std::unique_ptr<mcast::ipc::PipeConnection> m_control;
    std::unique_ptr<mcast::ipc::PipeConnection> m_playback;
    mcast::ipc::PeerIdentityGrant m_protocolGrant;
    bool m_hasProtocolGrant = false;
    std::string m_settingsJson;
    std::uint64_t m_settingsVersion = 0;
};
