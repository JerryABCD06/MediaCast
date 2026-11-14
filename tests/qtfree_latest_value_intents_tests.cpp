// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/LatestValueIntents.h"

#include <cstdlib>
#include <iostream>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main()
{
    using mcast::avalonia::qtfree::LatestValueIntents;

    LatestValueIntents intents;
    check(intents.submit("brightness", 10),
          "first intent schedules one drain");
    check(!intents.submit("brightness", 20),
          "new values do not schedule additional drains");
    check(!intents.submit("brightness", 30),
          "latest value replaces older pending values");
    check(intents.draining(), "mailbox remains owned by one drain");

    auto latest = intents.takeLatest();
    check(latest.size() == 1 && latest["brightness"] == 30,
          "drain receives only the latest value");

    check(!intents.submit("brightness", 40),
          "values arriving during apply stay in the same drain");
    check(intents.finishDrain(),
          "mailbox requests one continuation for newer input");
    latest = intents.takeLatest();
    check(latest.size() == 1 && latest["brightness"] == 40,
          "continuation receives the latest value");
    check(!intents.finishDrain(),
          "drain releases ownership after the final value");
    check(intents.submit("contrast", 5),
          "a new gesture schedules a fresh drain");

    intents.reset();
    check(!intents.draining() && intents.takeLatest().empty(),
          "reset clears pending input and drain ownership");

    std::cout << "Qt-free latest value intent tests passed\n";
    return 0;
}
