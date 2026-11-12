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
    if (const char *probePath =
            std::getenv("MCAST_SETTINGS_TEST_PATH")) {
        std::string probeError;
        mcast::shell::ShellSettings probe(probePath);
        const bool loaded = probe.load(probeError);
        std::cout << "probe load=" << loaded
                  << " error=" << probeError << '\n';
        probeError.clear();
        std::cout << "probe save=" << probe.save(probeError)
                  << " error=" << probeError << '\n';
        return loaded ? 0 : 1;
    }
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
    check(!settings.deviceName().empty()
              && settings.deviceUdn().starts_with("uuid:")
              && settings.deviceUdn().size() == 41,
          "default device identity is generated");
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
    check(custom.acceptNewCast(), "custom newcast loaded");
    check(!custom.broadcast(), "custom broadcast loaded");
    custom.setAcceptNewCast(false);
    error.clear();
    check(custom.save(error), "Shell settings save");
    check(readAll(path) != mainBefore
              && readAll(path).find("\"newcast\":false")
                  != std::string::npos
              && readAll(path).find("\"keep\":7")
                  != std::string::npos,
          "Shell settings save updates the main file and preserves unknown fields");
    check(!std::filesystem::exists(statePath),
          "new Shell writes receiving state only to the main JSON");

    mcast::shell::ShellSettings reloaded(path);
    error.clear();
    check(reloaded.load(error), "reloaded settings load");
    check(!reloaded.acceptNewCast(), "main receiving state survives reload");
    check(reloaded.language() == "zh_CN",
          "main language still loads");

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << R"({"ui":{"language":"zh_CN"},"cast":{"newcast":false},"picture":{"brightness":25,"contrast":-10,"bad":"value"}})";
    }
    mcast::shell::ShellSettings pictureSettings(path);
    error.clear();
    check(
        pictureSettings.load(error),
        "non-empty picture settings are repaired without iterator mutation");
    check(
        pictureSettings.pictureValues().at("brightness") == 25
            && pictureSettings.pictureValues().at("contrast") == -10
            && !pictureSettings.pictureValues().contains("bad"),
        "picture value repair preserves valid entries and drops invalid ones");
    error.clear();
    check(
        pictureSettings.save(error),
        "repaired picture settings remain writable");

    {
        std::ofstream file(statePath, std::ios::binary | std::ios::trunc);
        file << R"({"receiving_enabled": broken})";
    }
    const std::string invalidState = readAll(statePath);
    mcast::shell::ShellSettings corruptState(path);
    error.clear();
    check(!corruptState.load(error), "invalid Shell state is rejected");
    check(!corruptState.acceptNewCast(),
          "invalid Shell state falls back to current main newcast");
    corruptState.setAcceptNewCast(false);
    error.clear();
    check(corruptState.save(error),
          "main settings remain writable with invalid legacy Shell state");
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
          "wrong Shell state type is repaired to a safe value");
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
    check(!invalid.save(error),
          "invalid main settings cannot be overwritten");

    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << R"({"ui":1,"cast":{"newcast":"false"}})";
    }
    mcast::shell::ShellSettings wrongTypes(path);
    error.clear();
    check(wrongTypes.load(error), "wrong field types are repaired in memory");
    check(wrongTypes.broadcast(), "wrong cast fields fall back");

    error.clear();
    const std::string beforeRejected = wrongTypes.snapshotJson();
    check(
        !wrongTypes.applyJsonPatch(
            R"({"ui":{"language":"zh_CN","darkmode":17}})",
            error),
        "invalid settings patch is rejected");
    check(wrongTypes.snapshotJson() == beforeRejected,
          "rejected patch leaves in-memory settings unchanged");

    error.clear();
    check(
        !wrongTypes.applyJsonPatch(
            R"({"ui":{"darkmode":"Automatic"}})", error),
        "invalid theme value is rejected");
    error.clear();
    check(
        !wrongTypes.applyJsonPatch(
            R"({"picture":{"brightness":25.5}})", error),
        "fractional picture value is rejected");
    error.clear();
    check(
        !wrongTypes.applyJsonPatch(
            R"({"cast":"disabled"})", error),
        "wrong patch section type is rejected");
    error.clear();
    check(
        !wrongTypes.applyJsonPatch(
            R"({"cast":{"broadcast_interval":1000.5}})", error),
        "fractional broadcast interval is rejected");

    error.clear();
    check(
        wrongTypes.applyJsonPatch(
            R"({"ui":{"language":"en_US","darkmode":"Dark","accentmode":"WindowsBlue","mica":false},"cast":{"broadcast":false,"broadcast_interval":30000},"picture":{"brightness":25}})",
            error),
        "settings JSON patch applies");
    check(wrongTypes.save(error), "patched settings save");
    check(wrongTypes.language() == "en_US"
              && wrongTypes.darkMode() == "Dark"
              && wrongTypes.accentMode() == "WindowsBlue"
              && !wrongTypes.mica()
              && !wrongTypes.broadcast()
              && wrongTypes.broadcastIntervalMs() == 30000
              && wrongTypes.pictureValues().at("brightness") == 25,
          "settings JSON patch round-trips");
    const std::string snapshot = wrongTypes.snapshotJson();
    check(snapshot.find("\"language\":\"en_US\"")
              != std::string::npos
              && snapshot.find("\"brightness\":25")
                  != std::string::npos,
          "settings snapshot contains UI and picture values");

    const auto blockedPath = std::filesystem::temp_directory_path()
        / ("mcast-settings-blocked-"
           + std::to_string(GetCurrentProcessId()) + ".json");
    const auto blockedTemp = std::filesystem::path(
        blockedPath.wstring() + L".tmp");
    std::filesystem::remove(blockedPath, ignored);
    std::filesystem::remove_all(blockedTemp, ignored);
    {
        std::ofstream file(
            blockedPath, std::ios::binary | std::ios::trunc);
        file << R"({"ui":{"language":"en_US"},"cast":{"newcast":true}})";
    }
    mcast::shell::ShellSettings blocked(blockedPath);
    error.clear();
    check(blocked.load(error), "blocked-save settings load");
    const std::string blockedBefore = blocked.snapshotJson();
    check(std::filesystem::create_directory(blockedTemp),
          "blocked-save temporary directory is created");
    error.clear();
    check(!blocked.applyJsonPatchAndSave(
              R"({"ui":{"language":"zh_CN"}})", error),
          "failed atomic save is reported");
    check(blocked.snapshotJson() == blockedBefore,
          "failed atomic save restores in-memory settings");
    std::filesystem::remove_all(blockedTemp, ignored);

    std::filesystem::remove(statePath, ignored);
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(blockedPath, ignored);
    std::cout << "Shell settings tests passed\n";
    return 0;
}
