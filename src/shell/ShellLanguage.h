// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace mcast::shell {

struct TrayTexts {
    std::string openMain;
    std::string pauseReceiving;
    std::string resumeReceiving;
    std::string quit;
    std::string tooltipReceiving;
    std::string tooltipPaused;

    bool operator==(const TrayTexts &) const = default;
};

struct Terminology {
    std::string appFull;
    std::string appShort;
    std::map<std::string, std::string> strings;

    std::string resolve(
        std::string_view key, std::string_view fallback) const;
};

class ShellLanguage
{
public:
    using Logger = std::function<void(const std::string &)>;

    explicit ShellLanguage(
        std::filesystem::path languageDirectory,
        Logger logger = {});

    bool scan(std::string &error);
    std::string resolve(
        std::string_view requested, std::string &error) const;
    std::string resolveForLocales(
        std::string_view requested,
        const std::vector<std::string> &systemLocales,
        std::string &error) const;
    bool loadTrayTexts(
        std::string_view language, TrayTexts &texts,
        std::string &error) const;
    bool loadTerms(
        std::string_view language, Terminology &terms,
        std::string &error) const;
    std::string catalogJson(
        std::string_view resolvedLanguage,
        std::string &error) const;

    bool contains(std::string_view code) const;
    const std::filesystem::path &directory() const noexcept
    {
        return m_directory;
    }
    const TrayTexts &builtInEnglish() const noexcept
    {
        return m_builtInEnglish;
    }
    const Terminology &builtInTerms() const noexcept
    {
        return m_builtInTerms;
    }
    static std::vector<std::string> preferredSystemLocales();

private:
    struct Entry {
        std::string code;
        std::string name;
        std::filesystem::path root;
        std::filesystem::path uiFile;
        std::vector<std::string> aliases;
        bool builtIn = false;
    };

    const Entry *find(std::string_view code) const;
    void log(const std::string &message) const;

    std::filesystem::path m_directory;
    Logger m_logger;
    std::vector<Entry> m_entries;
    std::vector<std::string> m_conflictedAliases;
    TrayTexts m_builtInEnglish;
    Terminology m_builtInTerms;
};

} // namespace mcast::shell
