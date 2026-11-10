// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "skeleton/SkeletonClient.h"

#include <windows.h>

#include <cstdlib>
#include <iostream>

int runProtocolMain(int argc, wchar_t **argv)
{
    mcast::skeleton::ClientOptions options;
    std::string error;
    if (!mcast::skeleton::parseClientOptions(
            argc, argv, mcast::ipc::ProcessRole::Protocol,
            options, error)) {
        std::cerr << error << '\n';
        return 1;
    }
    return mcast::skeleton::runClient(options);
}

int wmain(int argc, wchar_t **argv)
{
    return runProtocolMain(argc, argv);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return runProtocolMain(__argc, __wargv);
}
