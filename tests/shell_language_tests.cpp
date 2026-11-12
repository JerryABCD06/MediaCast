// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "shell/ShellLanguage.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

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
#include <string>
#include <vector>

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void writeFile(
    const std::filesystem::path &path, const std::string &content)
{
    std::ofstream file(
        path, std::ios::binary | std::ios::trunc);
    file << content;
}

void writePack(
    const std::filesystem::path &root,
    const std::string &code,
    const std::string &name,
    const std::string &openText)
{
    const auto directory = root / code;
    std::filesystem::create_directories(directory);
    writeFile(
        directory / L"manifest.json",
        R"({"type":"MediaCast.LanguagePack","schemaVersion":1,"language":")"
            + code + R"(","name":")" + name + R"("})");
    writeFile(
        directory / L"locale-aliases.txt",
        code == "zh_CN"
            ? "zh-CN\nzh-Hans-CN\nzh_CN\n"
            : "en-US\nen\n");
    writeFile(
        directory / L"ui.json",
        R"({"strings":{"ui_test":"ok"}})");
    const std::string shell =
        "{\"tray_open_main\":\"" + openText
        + "\",\"tray_pause_receiving\":\"pause\","
          "\"tray_resume_receiving\":\"resume\","
          "\"tray_quit\":\"quit\","
          "\"tray_tooltip_receiving\":\"{app_short}\","
          "\"tray_tooltip_paused\":\"{app_short} (paused)\"}";
    writeFile(directory / L"shell.json", shell);
    const std::string terms =
        code == "zh_CN"
        ? R"JSON({"strings":{"term_app_full":"媒体投屏接收器","term_app_short":"媒体投屏","term_protocol_dlna_full":"数字生活网络联盟（DLNA）","term_protocol_dlna_short":"DLNA"}})JSON"
        : R"JSON({"strings":{"term_app_full":"Media Cast Receiver","term_app_short":"Media Cast","term_protocol_dlna_full":"Digital Living Network Alliance (DLNA)","term_protocol_dlna_short":"DLNA"}})JSON";
    writeFile(directory / L"terms.json", terms);
}

} // namespace

int main()
{
    const auto root = std::filesystem::temp_directory_path()
        / ("mcast-language-" + std::to_string(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    writePack(root, "en_US", "English", "Open main window");
    writePack(root, "zh_CN", "简体中文", "打开主界面");
    writePack(root, "ko_KR", "한국어", "Open main window");
    writeFile(root / L"ko_KR" / L"ui.json", "{}");

    const auto incomplete = root / "ja_JP";
    std::filesystem::create_directories(incomplete);
    writeFile(
        incomplete / L"manifest.json",
        R"({"type":"MediaCast.LanguagePack","schemaVersion":1,"language":"ja_JP","name":"日本語"})");

    const auto wrongType = root / "fr_FR";
    std::filesystem::create_directories(wrongType);
    writeFile(
        wrongType / L"manifest.json",
        R"({"type":"SomethingElse","schemaVersion":1,"language":"fr_FR","name":"Français"})");
    writeFile(wrongType / L"locale-aliases.txt", "fr-FR\n");
    writeFile(wrongType / L"shell.json", "{}");
    writeFile(wrongType / L"ui.json", "{}");

    std::vector<std::string> logs;
    mcast::shell::ShellLanguage languages(
        root,
        [&logs](const std::string &message) {
            logs.push_back(message);
        });
    std::string error;
    check(languages.scan(error),
          "language packs scan");
    check(languages.contains("en_US")
              && languages.contains("zh_CN"),
          "valid language packs are available");
    check(!languages.contains("ja_JP")
              && !languages.contains("fr_FR")
              && !languages.contains("ko_KR"),
          "incomplete, misidentified, and invalid UI packs are rejected");

    error.clear();
    check(languages.resolveForLocales(
              "System", {"zh-Hans-CN", "en-US"}, error)
              == "zh_CN",
          "preferred system locale resolves Chinese");
    error.clear();
    check(languages.resolveForLocales(
              "System", {"de-DE", "en-GB"}, error)
              == "en_US",
          "English fallback resolves from aliases");
    error.clear();
    check(languages.resolve("missing", error) == "en_US"
              && !error.empty(),
          "missing explicit language falls back to English");

    mcast::shell::TrayTexts texts;
    error.clear();
    check(languages.loadTrayTexts("zh_CN", texts, error)
              && texts.openMain == "打开主界面"
              && texts.tooltipReceiving == "媒体投屏"
              && texts.tooltipPaused == "媒体投屏 (paused)",
          "Chinese tray strings and terminology load");
    mcast::shell::Terminology terms;
    error.clear();
    check(languages.loadTerms("zh_CN", terms, error)
              && terms.appFull == "媒体投屏接收器"
              && terms.appShort == "媒体投屏",
          "Chinese terminology loads");

    error.clear();
    const std::string catalog =
        languages.catalogJson("zh_CN", error);
    check(!catalog.empty() && error.empty(),
          "language catalog serializes");
    const auto rootJson =
        winrt::Windows::Data::Json::JsonObject::Parse(
            winrt::to_hstring(catalog));
    check(rootJson.GetNamedString(L"resolvedLanguage") == L"zh_CN",
          "catalog contains resolved language");
    check(rootJson.GetNamedArray(L"languages").Size() == 2,
          "catalog contains valid packs only");

    std::filesystem::remove_all(root, ignored);
    std::cout << "Shell language tests passed\n";
    return 0;
}
