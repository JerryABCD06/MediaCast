// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "shell/ShellLanguage.h"

#include "core/ProductIdentity.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef GetObject

#include <algorithm>
#include <array>
#include <cctype>
#include <cwchar>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <set>

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;
using winrt::Windows::Data::Json::JsonValueType;

namespace mcast::shell {
namespace {

constexpr std::string_view kPackType = "MediaCast.LanguagePack";
constexpr int kPackSchemaVersion = 1;

std::string readFile(
    const std::filesystem::path &path, std::string &error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open " + path.filename().string();
        return {};
    }
    std::string content{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
    if (!file.eof() && file.fail()) {
        error = "cannot read " + path.filename().string();
        return {};
    }
    return content;
}

std::string pathUtf8(const std::filesystem::path &path)
{
    return winrt::to_string(winrt::hstring(path.wstring()));
}

std::string trim(std::string value)
{
    const auto first = std::find_if_not(
        value.begin(), value.end(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        });
    const auto last = std::find_if_not(
        value.rbegin(), value.rend(), [](unsigned char ch) {
            return std::isspace(ch) != 0;
        }).base();
    if (first >= last)
        return {};
    return std::string(first, last);
}

std::string normalizeLocale(std::string_view value)
{
    std::string result(value);
    for (char &ch : result) {
        if (ch == '-')
            ch = '_';
        else
            ch = static_cast<char>(
                std::tolower(static_cast<unsigned char>(ch)));
    }
    return result;
}

bool validLanguageCode(std::string_view code)
{
    if (code.empty() || code.size() > 64)
        return false;
    if (!std::isalpha(static_cast<unsigned char>(code.front())))
        return false;
    for (const char ch : code) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_')
            return false;
    }
    return true;
}

std::string stringMember(
    const JsonObject &object, const wchar_t *key)
{
    if (!object || !object.HasKey(key))
        return {};
    const auto value = object.GetNamedValue(key);
    if (value.ValueType() != JsonValueType::String)
        return {};
    return winrt::to_string(value.GetString());
}

bool objectParses(
    const std::filesystem::path &path, std::string &error)
{
    const std::string content = readFile(path, error);
    if (!error.empty())
        return false;
    try {
        const JsonObject object =
            JsonObject::Parse(winrt::to_hstring(content));
        if (!object) {
            error = path.filename().string() + " is not a JSON object";
            return false;
        }
        return true;
    } catch (const winrt::hresult_error &exception) {
        error = path.filename().string() + ": "
            + winrt::to_string(exception.message());
        return false;
    }
}

bool validatesUiStrings(
    const std::filesystem::path &path, std::string &error)
{
    const std::string content = readFile(path, error);
    if (!error.empty())
        return false;
    try {
        const JsonObject root =
            JsonObject::Parse(winrt::to_hstring(content));
        if (!root.HasKey(L"strings")) {
            error = "ui.json has no strings object";
            return false;
        }
        const auto stringsValue = root.GetNamedValue(L"strings");
        if (stringsValue.ValueType() != JsonValueType::Object) {
            error = "ui.json strings is not an object";
            return false;
        }
        const JsonObject strings = stringsValue.GetObject();
        for (const auto &[key, value] : strings) {
            if (key.empty()
                || value.ValueType() != JsonValueType::String
                || winrt::to_string(value.GetString()).empty()) {
                error = "ui.json contains an invalid string entry";
                return false;
            }
        }
        return true;
    } catch (const winrt::hresult_error &exception) {
        error = "ui.json: " + winrt::to_string(exception.message());
        return false;
    }
}

std::optional<Terminology> readTerms(
    const std::filesystem::path &path, std::string &error)
{
    const std::string content = readFile(path, error);
    if (!error.empty())
        return std::nullopt;
    try {
        const JsonObject root =
            JsonObject::Parse(winrt::to_hstring(content));
        if (!root.HasKey(L"strings")) {
            error = "terms.json has no strings object";
            return std::nullopt;
        }
        const auto stringsValue = root.GetNamedValue(L"strings");
        if (stringsValue.ValueType() != JsonValueType::Object) {
            error = "terms.json strings is not an object";
            return std::nullopt;
        }
        Terminology terms;
        const JsonObject strings = stringsValue.GetObject();
        for (const auto &[key, value] : strings) {
            if (key.empty()
                || value.ValueType() != JsonValueType::String) {
                error = "terms.json contains an invalid string entry";
                return std::nullopt;
            }
            terms.strings.emplace(
                winrt::to_string(key),
                winrt::to_string(value.GetString()));
        }
        terms.appFull = terms.resolve(
            "term_app_full", mcast::identity::appFullEnglish);
        terms.appShort = terms.resolve(
            "term_app_short", mcast::identity::appShortEnglish);
        return terms;
    } catch (const winrt::hresult_error &exception) {
        error = "terms.json: " + winrt::to_string(exception.message());
        return std::nullopt;
    }
}

void replaceAll(
    std::string &text, std::string_view needle, std::string_view value)
{
    if (needle.empty())
        return;
    std::size_t offset = 0;
    while ((offset = text.find(needle, offset)) != std::string::npos) {
        text.replace(offset, needle.size(), value);
        offset += value.size();
    }
}

std::optional<TrayTexts> readTrayTexts(
    const std::filesystem::path &path, std::string &error)
{
    const std::string content = readFile(path, error);
    if (!error.empty())
        return std::nullopt;
    try {
        const JsonObject object =
            JsonObject::Parse(winrt::to_hstring(content));
        if (!object) {
            error = "shell.json is not a JSON object";
            return std::nullopt;
        }
        TrayTexts texts{
            stringMember(object, L"tray_open_main"),
            stringMember(object, L"tray_pause_receiving"),
            stringMember(object, L"tray_resume_receiving"),
            stringMember(object, L"tray_quit"),
            stringMember(object, L"tray_tooltip_receiving"),
            stringMember(object, L"tray_tooltip_paused"),
        };
        if (texts.openMain.empty() || texts.pauseReceiving.empty()
            || texts.resumeReceiving.empty() || texts.quit.empty()
            || texts.tooltipReceiving.empty()
            || texts.tooltipPaused.empty()) {
            error = "shell.json is missing one or more tray strings";
            return std::nullopt;
        }
        return texts;
    } catch (const winrt::hresult_error &exception) {
        error = "shell.json: " + winrt::to_string(exception.message());
        return std::nullopt;
    }
}

std::vector<std::string> readAliases(
    const std::filesystem::path &path, std::string &error)
{
    const std::string content = readFile(path, error);
    if (!error.empty())
        return {};
    std::vector<std::string> aliases;
    std::size_t begin = 0;
    while (begin <= content.size()) {
        const std::size_t end = content.find('\n', begin);
        std::string line = trim(content.substr(
            begin,
            end == std::string::npos
                ? std::string::npos : end - begin));
        if (!line.empty() && line.front() != '#') {
            const std::string alias = normalizeLocale(line);
            if (!validLanguageCode(alias)) {
                error = "invalid locale alias: " + line;
                return {};
            }
            aliases.push_back(alias);
        }
        if (end == std::string::npos)
            break;
        begin = end + 1;
    }
    return aliases;
}

} // namespace

std::string Terminology::resolve(
    std::string_view key, std::string_view fallback) const
{
    const auto value = strings.find(std::string(key));
    return value == strings.end() || value->second.empty()
        ? std::string(fallback) : value->second;
}

ShellLanguage::ShellLanguage(
    std::filesystem::path languageDirectory, Logger logger)
    : m_directory(std::move(languageDirectory))
    , m_logger(std::move(logger))
    , m_builtInEnglish{
          "Open main window",
          "Pause receiving",
          "Resume receiving",
          "Quit",
          std::string(mcast::identity::appShortEnglish),
          std::string(mcast::identity::appShortEnglish)
              + " (receiving paused)",
      }
    , m_builtInTerms{
          std::string(mcast::identity::appFullEnglish),
          std::string(mcast::identity::appShortEnglish),
          {
              {"term_app_full",
               std::string(mcast::identity::appFullEnglish)},
              {"term_app_short",
               std::string(mcast::identity::appShortEnglish)},
              {"term_protocol_dlna_full",
               "Digital Living Network Alliance (DLNA)"},
              {"term_protocol_dlna_short", "DLNA"},
          }}
{
}

bool ShellLanguage::scan(std::string &error)
{
    error.clear();
    m_entries.clear();
    m_conflictedAliases.clear();

    std::error_code filesystemError;
    std::vector<std::filesystem::path> directories;
    if (!std::filesystem::exists(m_directory, filesystemError)) {
        if (filesystemError) {
            error = "cannot inspect language directory: "
                + filesystemError.message();
            return false;
        }
        log("Language directory missing; using built-in English");
        m_entries.push_back(Entry{
            "en_US",
            "English",
            {},
            std::filesystem::path(L":/lang/en_US/ui.json"),
            {"en_us", "en_gb", "en_au", "en_ca", "en"},
            true});
        return true;
    }
    if (std::filesystem::is_directory(m_directory, filesystemError)) {
        for (std::filesystem::directory_iterator iterator(
                 m_directory, filesystemError),
             end;
             !filesystemError && iterator != end; iterator.increment(
                 filesystemError)) {
            if (iterator->is_directory(filesystemError))
                directories.push_back(iterator->path());
        }
    }
    if (filesystemError) {
        log("Cannot enumerate language directory: "
            + filesystemError.message()
            + "; using built-in English");
        filesystemError.clear();
        m_entries.push_back(Entry{
            "en_US",
            "English",
            {},
            std::filesystem::path(L":/lang/en_US/ui.json"),
            {"en_us", "en_gb", "en_au", "en_ca", "en"},
            true});
        error.clear();
        return true;
    }
    std::sort(
        directories.begin(), directories.end(),
        [](const auto &left, const auto &right) {
            return pathUtf8(left.filename())
                < pathUtf8(right.filename());
        });

    for (const auto &directory : directories) {
        const std::string code = pathUtf8(directory.filename());
        if (!validLanguageCode(code)) {
            log("Language pack skipped: invalid directory name '"
                + code + "'");
            continue;
        }

        const auto manifestPath = directory / L"manifest.json";
        const auto aliasesPath = directory / L"locale-aliases.txt";
        const auto shellPath = directory / L"shell.json";
        const auto uiPath = directory / L"ui.json";
        const auto termsPath = directory / L"terms.json";
        const std::array required{
            manifestPath, aliasesPath, shellPath, uiPath, termsPath};
        bool missing = false;
        for (const auto &path : required) {
            if (!std::filesystem::is_regular_file(
                    path, filesystemError)
                || filesystemError) {
                log("Language pack skipped: missing "
                    + path.filename().string() + " in " + code);
                missing = true;
                filesystemError.clear();
                break;
            }
        }
        if (missing)
            continue;

        std::string packError;
        const std::string manifestContent =
            readFile(manifestPath, packError);
        if (!packError.empty()) {
            log("Language pack skipped: " + code + ": " + packError);
            continue;
        }
        JsonObject manifest;
        try {
            manifest =
                JsonObject::Parse(winrt::to_hstring(manifestContent));
        } catch (const winrt::hresult_error &exception) {
            log(
                "Language pack skipped: invalid manifest in " + code
                + ": " + winrt::to_string(exception.message()));
            continue;
        }
        const std::string type = stringMember(manifest, L"type");
        const std::string declaredCode =
            stringMember(manifest, L"language");
        const std::string name = trim(stringMember(manifest, L"name"));
        bool schemaVersionValid = false;
        if (manifest.HasKey(L"schemaVersion")) {
            const auto value = manifest.GetNamedValue(L"schemaVersion");
            if (value.ValueType() == JsonValueType::Number)
                schemaVersionValid =
                    value.GetNumber() == kPackSchemaVersion;
        }
        if (type != kPackType
            || !schemaVersionValid
            || declaredCode != code || name.empty()) {
            log("Language pack skipped: invalid manifest values in "
                + code);
            continue;
        }
        if (!objectParses(shellPath, packError)
            || !validatesUiStrings(uiPath, packError)
            || !readTerms(termsPath, packError)) {
            log("Language pack skipped: " + code + ": " + packError);
            continue;
        }
        TrayTexts trayTexts;
        if (!readTrayTexts(shellPath, packError)) {
            log("Language pack skipped: " + code + ": " + packError);
            continue;
        }
        (void)trayTexts;
        auto aliases = readAliases(aliasesPath, packError);
        if (!packError.empty()) {
            log("Language pack skipped: " + code + ": " + packError);
            continue;
        }

        m_entries.push_back(Entry{
            code, name, directory,
            std::filesystem::absolute(uiPath),
            std::move(aliases), false});
    }

    if (!find("en_US")) {
        m_entries.push_back(Entry{
            "en_US",
            "English",
            {},
            std::filesystem::path(L":/lang/en_US/ui.json"),
            {"en_us", "en_gb", "en_au", "en_ca", "en"},
            true});
    }
    std::sort(
        m_entries.begin(), m_entries.end(),
        [](const Entry &left, const Entry &right) {
            return left.code < right.code;
        });

    std::map<std::string, std::string> owners;
    std::set<std::string> conflicts;
    for (const Entry &entry : m_entries) {
        for (const std::string &alias : entry.aliases) {
            const auto [iterator, inserted] =
                owners.emplace(alias, entry.code);
            if (!inserted && iterator->second != entry.code) {
                conflicts.insert(alias);
                log("Locale alias conflict '" + alias + "': "
                    + iterator->second + " and " + entry.code
                    + "; alias ignored");
            }
        }
    }
    m_conflictedAliases.assign(conflicts.begin(), conflicts.end());
    log("Language packs: " + std::to_string(m_entries.size())
        + " available");
    return true;
}

std::string ShellLanguage::resolve(
    std::string_view requested, std::string &error) const
{
    return resolveForLocales(
        requested, preferredSystemLocales(), error);
}

std::string ShellLanguage::resolveForLocales(
    std::string_view requested,
    const std::vector<std::string> &systemLocales,
    std::string &error) const
{
    error.clear();
    if (!requested.empty() && requested != "System") {
        if (find(requested))
            return std::string(requested);
        error = "language pack not found: " + std::string(requested);
        return "en_US";
    }

    for (const std::string &systemLocale : systemLocales) {
        const std::string normalized = normalizeLocale(systemLocale);
        if (std::find(
                m_conflictedAliases.begin(),
                m_conflictedAliases.end(), normalized)
            != m_conflictedAliases.end()) {
            continue;
        }
        for (const Entry &entry : m_entries) {
            if (std::find(
                    entry.aliases.begin(),
                    entry.aliases.end(), normalized)
                != entry.aliases.end()) {
                return entry.code;
            }
        }
    }

    error = "no system language matched";
    return "en_US";
}

bool ShellLanguage::loadTrayTexts(
    std::string_view language, TrayTexts &texts,
    std::string &error) const
{
    error.clear();
    const Entry *entry = find(language);
    if (!entry) {
        error = "language pack not found: " + std::string(language);
        return false;
    }
    if (entry->builtIn) {
        texts = m_builtInEnglish;
        return true;
    }
    Terminology terms;
    if (!loadTerms(language, terms, error))
        return false;
    const auto loaded =
        readTrayTexts(entry->root / L"shell.json", error);
    if (!loaded)
        return false;
    texts = *loaded;
    replaceAll(texts.tooltipReceiving, "{app_short}", terms.appShort);
    replaceAll(texts.tooltipPaused, "{app_short}", terms.appShort);
    return true;
}

bool ShellLanguage::loadTerms(
    std::string_view language, Terminology &terms,
    std::string &error) const
{
    error.clear();
    const Entry *entry = find(language);
    if (!entry) {
        error = "language pack not found: " + std::string(language);
        return false;
    }
    if (entry->builtIn) {
        terms = m_builtInTerms;
        return true;
    }
    const auto loaded = readTerms(entry->root / L"terms.json", error);
    if (!loaded)
        return false;
    terms = *loaded;
    return true;
}

std::string ShellLanguage::catalogJson(
    std::string_view resolvedLanguage, std::string &error) const
{
    error.clear();
    const std::string resolved =
        find(resolvedLanguage) ? std::string(resolvedLanguage)
                               : std::string("en_US");
    try {
        JsonObject root{};
        root.SetNamedValue(
            L"baseLanguage",
            JsonValue::CreateStringValue(L"en_US"));
        root.SetNamedValue(
            L"resolvedLanguage",
            JsonValue::CreateStringValue(
                winrt::to_hstring(resolved)));
        winrt::Windows::Data::Json::JsonArray languages{};
        bool hasEnglish = false;
        for (const Entry &entry : m_entries) {
            if (entry.code == "en_US")
                hasEnglish = true;
            JsonObject item{};
            item.SetNamedValue(
                L"code",
                JsonValue::CreateStringValue(
                    winrt::to_hstring(entry.code)));
            item.SetNamedValue(
                L"name",
                JsonValue::CreateStringValue(
                    winrt::to_hstring(entry.name)));
            item.SetNamedValue(
                L"uiFile",
                JsonValue::CreateStringValue(
                    winrt::hstring(entry.uiFile.wstring())));
            languages.Append(item);
        }
        if (!hasEnglish) {
            JsonObject item{};
            item.SetNamedValue(
                L"code",
                JsonValue::CreateStringValue(L"en_US"));
            item.SetNamedValue(
                L"name",
                JsonValue::CreateStringValue(L"English"));
            item.SetNamedValue(
                L"uiFile",
                JsonValue::CreateStringValue(
                    L":/lang/en_US/ui.json"));
            languages.Append(item);
        }
        root.SetNamedValue(L"languages", languages);
        return winrt::to_string(root.Stringify());
    } catch (const winrt::hresult_error &exception) {
        error = "cannot serialize language catalog: "
            + winrt::to_string(exception.message());
        return {};
    }
}

bool ShellLanguage::contains(std::string_view code) const
{
    return find(code) != nullptr;
}

const ShellLanguage::Entry *ShellLanguage::find(
    std::string_view code) const
{
    for (const Entry &entry : m_entries) {
        if (entry.code == code)
            return &entry;
    }
    return nullptr;
}

void ShellLanguage::log(const std::string &message) const
{
    if (m_logger) {
        try {
            m_logger(message);
        } catch (...) {
        }
    }
}

std::vector<std::string> ShellLanguage::preferredSystemLocales()
{
    ULONG count = 0;
    ULONG size = 0;
    if (!GetUserPreferredUILanguages(
            MUI_LANGUAGE_NAME, &count, nullptr, &size)
        || size == 0)
        return {};
    std::vector<wchar_t> buffer(size + 1, L'\0');
    if (!GetUserPreferredUILanguages(
            MUI_LANGUAGE_NAME, &count, buffer.data(), &size))
        return {};

    std::vector<std::string> locales;
    const wchar_t *current = buffer.data();
    while (*current) {
        locales.push_back(winrt::to_string(current));
        current += wcslen(current) + 1;
    }
    return locales;
}

} // namespace mcast::shell
