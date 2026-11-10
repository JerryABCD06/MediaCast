// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/CastConfirmationDialog.h"

#include <windows.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace {

const char *choiceName(mcast::platform::CastConfirmationChoice choice)
{
    using mcast::platform::CastConfirmationChoice;
    switch (choice) {
    case CastConfirmationChoice::Accepted:
        return "Accepted";
    case CastConfirmationChoice::Declined:
        return "Declined";
    case CastConfirmationChoice::TimedOut:
        return "TimedOut";
    case CastConfirmationChoice::Cancelled:
        return "Cancelled";
    case CastConfirmationChoice::Failed:
        return "Failed";
    }
    return "Unknown";
}

} // namespace

int wmain(int argc, wchar_t **argv)
{
    std::chrono::milliseconds cancelAfter{0};
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        constexpr std::wstring_view prefix = L"--cancel-after=";
        if (argument.starts_with(prefix)) {
            try {
                cancelAfter = std::chrono::milliseconds(
                    std::stoll(argument.substr(prefix.size())));
            } catch (...) {
                std::cerr << "invalid --cancel-after\n";
                return 1;
            }
        }
    }

    HANDLE cancel = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!cancel) {
        std::cerr << "CreateEventW failed\n";
        return 1;
    }

    mcast::platform::CastConfirmationPrompt prompt;
    prompt.protocolName = L"AirPlay";
    prompt.peerName = L"Living Room Phone";
    prompt.mediaTitle = L"Example Movie";
    prompt.timeout = std::chrono::seconds(10);
    prompt.cancelEvent = cancel;
    prompt.chinese =
        PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    std::jthread canceller;
    if (cancelAfter > std::chrono::milliseconds::zero()) {
        canceller = std::jthread([cancel, cancelAfter] {
            std::this_thread::sleep_for(cancelAfter);
            SetEvent(cancel);
        });
    }
    const auto choice =
        mcast::platform::showCastConfirmationDialog(nullptr, prompt);
    canceller.request_stop();
    CloseHandle(cancel);

    std::cout << choiceName(choice) << '\n';
    return choice == mcast::platform::CastConfirmationChoice::Failed ? 2 : 0;
}
