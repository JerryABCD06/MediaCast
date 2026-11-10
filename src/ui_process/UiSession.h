// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

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
        std::string instance;
        std::string nonce;
        std::uint32_t shellPid = 0;
        std::uint64_t activationId = 0;
    };

    static Options parseArguments(int argc, char **argv, std::string &error);

    UiSession();
    ~UiSession();

    bool connectToPeers(const Options &options, std::string &error);
    bool sendFirstFrame(std::string &error);

private:
    Options m_options;
    std::unique_ptr<mcast::ipc::PipeConnection> m_control;
};
