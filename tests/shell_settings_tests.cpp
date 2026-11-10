// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "shell/ShellSettings.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string readAll(const std::filesystem::path &path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

} // namespace

int main()
{
    const auto path = std::filesystem::temp_directory_path()
        / ("mcast-settings-" + std::to_string(GetCurrentProcessId())
           + ".json");
    const auto statePath = path.parent_path()
        / (path.stem().wstring() + L".Shell.json");
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(statePath, ignored);

    std::string error;
    mcast::shell::ShellSettings settings(path);
    check(settings.load(error), "missing settings use defaults");
    check(settings.acceptNewCast(), "default receiving state");
    check(settings.broadcast(), "default broadcast state");
    check(!std::filesystem::exists(path),
          "Shell does not create the main settings file");

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << R"({"ui":{"language":"zh_CN"},"cast":{"newcast":true,"broadcast":false},"custom":{"keep":7}})";
    }
    const std::string mainBefore = readAll(path);
    mcast::shell::ShellSettings custom(path);
    error.clear();
    const bool customLoaded = custom.load(error);
    if (!customLoaded)
        std::cerr << "custom load error: " << error << '\n';
    check(customLoaded, "custom settings load");
    check(custom.chineseUi(), "zh_CN selects Chinese tray");
    check(custom.acceptNewCast(), "custom newcast loaded");
    check(!custom.broadcast(), "custom broadcast loaded");
    custom.setAcceptNewCast(false);
    error.clear();
    check(custom.save(error), "Shell state save");
    check(readAll(path) == mainBefore,
          "Shell state save does not touch the main settings file");
    check(readAll(statePath).find("\"receiving_enabled\":false")
              != std::string::npos,
          "receiving state is persisted separately");

    mcast::shell::ShellSettings reloaded(path);
    error.clear();
    check(reloaded.load(error), "reloaded settings load");
    check(!reloaded.acceptNewCast(), "Shell state overrides main newcast");
    check(reloaded.chineseUi(), "main language still loads");

    {
        std::ofstream file(statePath, std::ios::binary | std::ios::trunc);
        file << R"({"receiving_enabled": broken})";
    }
    const std::string invalidState = readAll(statePath);
    mcast::shell::ShellSettings corruptState(path);
    error.clear();
    check(!corruptState.load(error), "invalid Shell state is rejected");
    check(corruptState.acceptNewCast(),
          "invalid Shell state falls back to main newcast");
    corruptState.setAcceptNewCast(false);
    error.clear();
    check(!corruptState.save(error),
          "invalid Shell state is not overwritten");
    check(readAll(statePath) == invalidState,
          "invalid Shell state file remains intact");

    {
        std::ofstream file(statePath, std::ios::binary | std::ios::trunc);
        file << R"({"receiving_enabled":"false"})";
    }
    mcast::shell::ShellSettings wrongState(path);
    error.clear();
    check(wrongState.load(error), "wrong Shell state types are repaired");
    check(wrongState.acceptNewCast(),
          "wrong Shell state type falls back to main newcast");
    wrongState.setAcceptNewCast(false);
    error.clear();
    check(wrongState.save(error), "repaired Shell state can be saved");

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << R"({"ui": broken})";
    }
    const std::string invalidBefore = readAll(path);
    mcast::shell::ShellSettings invalid(path);
    error.clear();
    check(!invalid.load(error), "invalid settings are rejected");
    check(readAll(path) == invalidBefore,
          "invalid settings file remains intact");

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << R"({"ui":1,"cast":{"newcast":"false"}})";
    }
    mcast::shell::ShellSettings wrongTypes(path);
    error.clear();
    check(wrongTypes.load(error), "wrong field types are repaired in memory");
    check(wrongTypes.broadcast(), "wrong cast fields fall back");

    std::filesystem::remove(statePath, ignored);
    std::filesystem::remove(path, ignored);
    std::cout << "Shell settings tests passed\n";
    return 0;
}
