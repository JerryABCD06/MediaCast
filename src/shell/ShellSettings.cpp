// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "shell/ShellSettings.h"

#include <winrt/Windows.Foundation.Collections.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef GetObject

#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <system_error>
#include <vector>

using winrt::Windows::Data::Json::JsonObject;
using winrt::Windows::Data::Json::JsonValue;

namespace mcast::shell {
namespace {

std::string readFile(const std::filesystem::path &path, std::string &error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot open settings file";
        return {};
    }
    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
}

bool writeFileAtomic(
    const std::filesystem::path &path,
    std::string_view content, std::string &error)
{
    std::filesystem::path temporary = path;
    temporary += L".tmp";
    try {
        std::ofstream file(
            temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot open temporary settings file";
            return false;
        }
        file.write(
            content.data(),
            static_cast<std::streamsize>(content.size()));
        file.flush();
        if (!file) {
            error = "cannot write temporary settings file";
            return false;
        }
    } catch (const winrt::hresult_error &exception) {
        error = "cannot serialize settings: "
            + winrt::to_string(exception.message());
        return false;
    }

    if (!MoveFileExW(
            temporary.c_str(), path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "MoveFileExW(settings) failed: "
            + std::to_string(GetLastError());
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::string stringMember(
    const JsonObject &object, const wchar_t *key,
    std::string_view fallback)
{
    if (object && object.HasKey(key)) {
        const auto value = object.GetNamedValue(key);
        if (value.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::String)
            return winrt::to_string(value.GetString());
    }
    return std::string(fallback);
}

bool validLanguageSetting(std::string_view value)
{
    if (value == "System")
        return true;
    if (value.empty() || value.size() > 32)
        return false;
    for (const char ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (!std::isalnum(byte) && ch != '_' && ch != '-')
            return false;
    }
    return true;
}

bool validThemeSetting(std::string_view value)
{
    return value == "System" || value == "Light" || value == "Dark";
}

bool validAccentSetting(std::string_view value)
{
    return value == "System" || value == "WindowsBlue";
}

bool validDeviceName(std::string_view value)
{
    if (value.empty() || value.size() > 256)
        return false;
    for (const char ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7F || ch == '"')
            return false;
    }
    return true;
}

bool validBoundedInteger(
    double value, int minimum, int maximum, int &result)
{
    if (!std::isfinite(value) || value < minimum || value > maximum
        || std::floor(value) != value)
        return false;
    result = static_cast<int>(value);
    return true;
}

bool validPictureNumber(double value, int &result)
{
    return validBoundedInteger(value, -1000, 1000, result);
}

bool boolMember(
    const JsonObject &object, const wchar_t *key, bool fallback)
{
    if (object && object.HasKey(key)) {
        const auto value = object.GetNamedValue(key);
        if (value.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::Boolean)
            return value.GetBoolean();
    }
    return fallback;
}

int intMember(
    const JsonObject &object, const wchar_t *key, int fallback)
{
    if (object && object.HasKey(key)) {
        const auto value = object.GetNamedValue(key);
        if (value.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::Number)
            return static_cast<int>(value.GetNumber());
    }
    return fallback;
}

JsonObject objectMember(const JsonObject &object, const wchar_t *key)
{
    if (object && object.HasKey(key)) {
        const auto value = object.GetNamedValue(key);
        if (value.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::Object)
            return value.GetObject();
    }
    return JsonObject{};
}

std::string hostName()
{
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD size = static_cast<DWORD>(std::size(name));
    if (GetComputerNameW(name, &size) && size > 0)
        return winrt::to_string(winrt::hstring(name, size));
    return "Windows PC";
}

std::string legacyUuidFrom(
    const std::filesystem::path &path)
{
    wchar_t value[128]{};
    GetPrivateProfileStringW(
        L"device", L"uuid", L"", value,
        static_cast<DWORD>(std::size(value)), path.c_str());
    return value[0] ? winrt::to_string(winrt::hstring(value)) : "";
}

std::string legacyUuid(
    const std::filesystem::path &settingsPath)
{
    std::string uuid;
    wchar_t localAppData[MAX_PATH + 1]{};
    const DWORD size = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localAppData,
        static_cast<DWORD>(std::size(localAppData)));
    if (size > 0 && size < std::size(localAppData)) {
        std::filesystem::path path(localAppData);
        path /= L"MCast";
        path /= L"renderer.ini";
        uuid = legacyUuidFrom(path);
        if (!uuid.empty())
            return uuid;
    }

    // 旧实现写不进标准位置时会退到程序目录。Shell 与旧 MCast.exe
    // 通常同目录，因此这里也应从设置文件旁边再找一次。
    std::filesystem::path fallback =
        settingsPath.parent_path() / L"renderer.ini";
    return legacyUuidFrom(fallback);
}

std::string newUuid()
{
    std::random_device random;
    std::array<unsigned char, 16> bytes{};
    for (auto &byte : bytes)
        byte = static_cast<unsigned char>(random());
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3Fu) | 0x80u);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(36);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
            result.push_back('-');
        result.push_back(hex[bytes[index] >> 4]);
        result.push_back(hex[bytes[index] & 0x0Fu]);
    }
    return result;
}

bool validUdn(const std::string &value)
{
    if (value.size() != 41 || !value.starts_with("uuid:"))
        return false;
    for (std::size_t index = 5; index < value.size(); ++index) {
        const char ch = value[index];
        if (index == 13 || index == 18 || index == 23 || index == 28) {
            if (ch != '-')
                return false;
        } else if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    return true;
}

} // namespace

ShellSettings::ShellSettings(std::filesystem::path path)
    : m_path(std::move(path))
    , m_statePath(
        m_path.parent_path()
        / (m_path.stem().wstring() + L".Shell.json"))
{
}

bool ShellSettings::load(std::string &error)
{
    m_root = JsonObject{};
    m_mainSaveAllowed = true;
    if (!std::filesystem::exists(m_path)) {
        ensureDefaults();
    } else {
        const std::string content = readFile(m_path, error);
        if (!error.empty()) {
            m_mainSaveAllowed = false;
            ensureDefaults();
        } else if (content.empty()) {
            ensureDefaults();
        } else {
            try {
                m_root = JsonObject::Parse(winrt::to_hstring(content));
            } catch (const winrt::hresult_error &exception) {
                error = "invalid settings JSON: "
                    + winrt::to_string(exception.message());
                m_root = JsonObject{};
                m_mainSaveAllowed = false;
            }
        }
    }
    if (!ensureDefaults()) {
        m_root = JsonObject{};
        ensureDefaults();
        m_mainSaveAllowed = false;
        if (error.empty())
            error = "settings structure is invalid";
    }

    m_stateRoot = JsonObject{};
    m_stateSaveAllowed = true;
    if (std::filesystem::exists(m_statePath)) {
        std::string stateError;
        const std::string stateContent = readFile(m_statePath, stateError);
        if (!stateError.empty()) {
            m_stateSaveAllowed = false;
            if (error.empty())
                error = stateError;
        } else {
            try {
                m_stateRoot = JsonObject::Parse(
                    winrt::to_hstring(stateContent));
            } catch (const winrt::hresult_error &exception) {
                m_stateSaveAllowed = false;
                if (error.empty()) {
                    error = "invalid Shell state JSON: "
                        + winrt::to_string(exception.message());
                }
            }
        }
    }
    if (!ensureShellState())
        m_stateSaveAllowed = false;
    try {
        JsonObject shell = objectMember(m_root, L"shell");
        const bool imported =
            shell && shell.HasKey(L"legacy_receiving_imported")
            && shell.GetNamedValue(L"legacy_receiving_imported")
                   .ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Boolean
            && shell.GetNamedValue(L"legacy_receiving_imported")
                   .GetBoolean();
        if (!imported && m_stateRoot
            && m_stateRoot.HasKey(L"receiving_enabled")) {
            const auto value =
                m_stateRoot.GetNamedValue(L"receiving_enabled");
            if (value.ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Boolean) {
                JsonObject cast = objectMember(m_root, L"cast");
                if (!cast) {
                    cast = JsonObject{};
                }
                cast.SetNamedValue(L"newcast", value);
                m_root.SetNamedValue(L"cast", cast);
            }
            if (!shell)
                shell = JsonObject{};
            shell.SetNamedValue(
                L"legacy_receiving_imported",
                JsonValue::CreateBooleanValue(true));
            m_root.SetNamedValue(L"shell", shell);
        }
    } catch (const winrt::hresult_error &) {
        if (error.empty())
            error = "legacy receiving migration failed";
    }
    return error.empty();
}

bool ShellSettings::save(std::string &error) const
{
    if (!m_root || !m_mainSaveAllowed) {
        if (error.empty())
            error = "Main settings are not writable";
        return false;
    }
    std::string mainContent;
    try {
        mainContent = winrt::to_string(m_root.Stringify());
    } catch (const winrt::hresult_error &exception) {
        if (error.empty())
            error = "cannot serialize settings: "
                + winrt::to_string(exception.message());
        return false;
    }
    if (!writeFileAtomic(m_path, mainContent, error))
        return false;
    return true;
}

std::string ShellSettings::language() const
{
    return stringMember(objectMember(m_root, L"ui"), L"language", "System");
}

std::string ShellSettings::darkMode() const
{
    return stringMember(
        objectMember(m_root, L"ui"), L"darkmode", "System");
}

std::string ShellSettings::accentMode() const
{
    return stringMember(
        objectMember(m_root, L"ui"), L"accentmode", "System");
}

std::string ShellSettings::deviceName() const
{
    return stringMember(
        objectMember(m_root, L"device"), L"name", hostName());
}

std::string ShellSettings::deviceUdn() const
{
    return stringMember(
        objectMember(m_root, L"device"), L"udn", "uuid:");
}

bool ShellSettings::acceptNewCast() const
{
    return boolMember(
        objectMember(m_root, L"cast"), L"newcast", true);
}

bool ShellSettings::broadcast() const
{
    return boolMember(
        objectMember(m_root, L"cast"), L"broadcast", true);
}

int ShellSettings::broadcastIntervalMs() const
{
    return intMember(
        objectMember(m_root, L"cast"),
        L"broadcast_interval", 10000);
}

bool ShellSettings::mica() const
{
    return boolMember(objectMember(m_root, L"ui"), L"mica", true);
}

std::map<std::string, int> ShellSettings::pictureValues() const
{
    std::map<std::string, int> values;
    const JsonObject picture = objectMember(m_root, L"picture");
    if (!picture)
        return values;
    for (const auto &[key, value] : picture) {
        if (value.ValueType()
            != winrt::Windows::Data::Json::JsonValueType::Number)
            continue;
        int number = 0;
        if (!validPictureNumber(value.GetNumber(), number))
            continue;
        const std::string name = winrt::to_string(key);
        if (!name.empty())
            values[name] = number;
    }
    return values;
}

void ShellSettings::setAcceptNewCast(bool enabled)
{
    if (!m_root)
        m_root = JsonObject{};
    JsonObject cast = objectMember(m_root, L"cast");
    if (!cast) {
        cast = JsonObject{};
        m_root.SetNamedValue(L"cast", cast);
    }
    cast.SetNamedValue(
        L"newcast", JsonValue::CreateBooleanValue(enabled));
}

bool ShellSettings::setAcceptNewCastAndSave(
    bool enabled, std::string &error)
{
    JsonObject patch{};
    JsonObject cast{};
    cast.SetNamedValue(
        L"newcast", JsonValue::CreateBooleanValue(enabled));
    patch.SetNamedValue(L"cast", cast);
    try {
        return applyJsonPatchAndSave(
            winrt::to_string(patch.Stringify()), error);
    } catch (const winrt::hresult_error &exception) {
        error = "cannot serialize receiving setting: "
            + winrt::to_string(exception.message());
        return false;
    }
}

bool ShellSettings::applyJsonPatch(
    std::string_view patch, std::string &error)
{
    error.clear();
    try {
        JsonObject candidate = JsonObject::Parse(m_root.Stringify());
        const JsonObject object = JsonObject::Parse(
            winrt::to_hstring(std::string(patch)));
        const auto requireObjectSection =
            [&object](const wchar_t *key, std::string &sectionError) {
                if (!object.HasKey(key))
                    return true;
                if (object.GetNamedValue(key).ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::Object)
                    return true;
                sectionError = winrt::to_string(key)
                    + " must be an object";
                return false;
            };
        if (!requireObjectSection(L"ui", error)
            || !requireObjectSection(L"cast", error)
            || !requireObjectSection(L"picture", error))
            return false;
        JsonObject ui = objectMember(candidate, L"ui");
        JsonObject cast = objectMember(candidate, L"cast");
        JsonObject picture = objectMember(candidate, L"picture");

        const JsonObject patchUi = objectMember(object, L"ui");
        if (patchUi) {
            if (patchUi.HasKey(L"language")) {
                const auto value = patchUi.GetNamedValue(L"language");
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::String) {
                    error = "ui.language must be a string";
                    return false;
                }
                if (!validLanguageSetting(
                        winrt::to_string(value.GetString()))) {
                    error = "ui.language is invalid";
                    return false;
                }
                ui.SetNamedValue(L"language", value);
            }
            if (patchUi.HasKey(L"darkmode")) {
                const auto value = patchUi.GetNamedValue(L"darkmode");
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::String) {
                    error = "ui.darkmode must be a string";
                    return false;
                }
                if (!validThemeSetting(
                        winrt::to_string(value.GetString()))) {
                    error = "ui.darkmode is invalid";
                    return false;
                }
                ui.SetNamedValue(L"darkmode", value);
            }
            if (patchUi.HasKey(L"accentmode")) {
                const auto value = patchUi.GetNamedValue(L"accentmode");
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::String) {
                    error = "ui.accentmode must be a string";
                    return false;
                }
                if (!validAccentSetting(
                        winrt::to_string(value.GetString()))) {
                    error = "ui.accentmode is invalid";
                    return false;
                }
                ui.SetNamedValue(L"accentmode", value);
            }
            if (patchUi.HasKey(L"mica")) {
                const auto value = patchUi.GetNamedValue(L"mica");
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::Boolean) {
                    error = "ui.mica must be a boolean";
                    return false;
                }
                ui.SetNamedValue(L"mica", value);
            }
        }

        const JsonObject patchCast = objectMember(object, L"cast");
        if (patchCast) {
            if (patchCast.HasKey(L"newcast")) {
                const auto value = patchCast.GetNamedValue(L"newcast");
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::Boolean) {
                    error = "cast.newcast must be a boolean";
                    return false;
                }
                cast.SetNamedValue(L"newcast", value);
            }
            if (patchCast.HasKey(L"broadcast")) {
                const auto value = patchCast.GetNamedValue(L"broadcast");
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::Boolean) {
                    error = "cast.broadcast must be a boolean";
                    return false;
                }
                cast.SetNamedValue(L"broadcast", value);
            }
            if (patchCast.HasKey(L"broadcast_interval")) {
                const auto value =
                    patchCast.GetNamedValue(L"broadcast_interval");
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::Number) {
                    error = "cast.broadcast_interval must be a number";
                    return false;
                }
                int interval = 0;
                if (!validBoundedInteger(
                        value.GetNumber(),
                        1000, 3600000, interval)) {
                    error = "cast.broadcast_interval is out of range";
                    return false;
                }
                cast.SetNamedValue(
                    L"broadcast_interval",
                    JsonValue::CreateNumberValue(interval));
            }
        }

        const JsonObject patchPicture = objectMember(object, L"picture");
        if (patchPicture) {
            for (const auto &[key, value] : patchPicture) {
                if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::Number) {
                    error = "picture values must be numbers";
                    return false;
                }
                int number = 0;
                if (!validPictureNumber(
                        value.GetNumber(), number)) {
                    error = "picture value is out of range";
                    return false;
                }
                picture.SetNamedValue(
                    key, JsonValue::CreateNumberValue(number));
            }
        }

        if (!ui)
            ui = JsonObject{};
        if (!cast)
            cast = JsonObject{};
        candidate.SetNamedValue(L"ui", ui);
        candidate.SetNamedValue(L"cast", cast);
        if (picture)
            candidate.SetNamedValue(L"picture", picture);
        m_root = std::move(candidate);
        return true;
    } catch (const winrt::hresult_error &exception) {
        error = "invalid settings patch: "
            + winrt::to_string(exception.message());
        return false;
    }
}

bool ShellSettings::applyJsonPatchAndSave(
    std::string_view patch, std::string &error)
{
    JsonObject previous;
    try {
        previous = JsonObject::Parse(m_root.Stringify());
    } catch (const winrt::hresult_error &exception) {
        error = "cannot snapshot settings: "
            + winrt::to_string(exception.message());
        return false;
    }
    if (!applyJsonPatch(patch, error))
        return false;
    if (!save(error)) {
        m_root = std::move(previous);
        return false;
    }
    return true;
}

std::string ShellSettings::snapshotJson() const
{
    return winrt::to_string(m_root.Stringify());
}

bool ShellSettings::ensureDefaults()
{
    try {
        if (!m_root)
            m_root = JsonObject{};
        JsonObject ui{};
        if (m_root.HasKey(L"ui")) {
            const auto value = m_root.GetNamedValue(L"ui");
            if (value.ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Object)
                ui = value.GetObject();
        }
        if (!ui) {
            ui = JsonObject{};
            m_root.SetNamedValue(L"ui", ui);
        }
        JsonObject cast{};
        if (m_root.HasKey(L"cast")) {
            const auto value = m_root.GetNamedValue(L"cast");
            if (value.ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Object)
                cast = value.GetObject();
        }
        if (!cast) {
            cast = JsonObject{};
            m_root.SetNamedValue(L"cast", cast);
        }
        JsonObject device{};
        if (m_root.HasKey(L"device")) {
            const auto value = m_root.GetNamedValue(L"device");
            if (value.ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Object)
                device = value.GetObject();
        }
        if (!device) {
            device = JsonObject{};
            m_root.SetNamedValue(L"device", device);
        }

        const auto ensureString = [](JsonObject &object,
                                      const wchar_t *key,
                                      const wchar_t *fallback,
                                      bool (*validator)(
                                          std::string_view)) {
            bool valid = false;
            if (object.HasKey(key)) {
                const auto value = object.GetNamedValue(key);
                valid = value.ValueType()
                        == winrt::Windows::Data::Json::JsonValueType::String
                    && validator(winrt::to_string(value.GetString()));
            }
            if (!valid)
                object.SetNamedValue(
                    key, JsonValue::CreateStringValue(fallback));
        };
        const auto ensureBool = [](JsonObject &object,
                                   const wchar_t *key, bool fallback) {
            bool valid = false;
            if (object.HasKey(key)) {
                const auto value = object.GetNamedValue(key);
                valid = value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::Boolean;
            }
            if (!valid)
                object.SetNamedValue(
                    key, JsonValue::CreateBooleanValue(fallback));
        };
        ensureString(
            ui, L"language", L"System", validLanguageSetting);
        ensureString(
            ui, L"darkmode", L"System", validThemeSetting);
        ensureString(
            ui, L"accentmode", L"System", validAccentSetting);
        ensureBool(ui, L"mica", true);
        ensureBool(cast, L"newcast", true);
        ensureBool(cast, L"broadcast", true);
        bool intervalValid = false;
        if (cast.HasKey(L"broadcast_interval")) {
            const auto value = cast.GetNamedValue(L"broadcast_interval");
            int interval = 0;
            intervalValid = value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::Number
                && validBoundedInteger(
                    value.GetNumber(), 1000, 3600000, interval);
        }
        if (!intervalValid) {
            cast.SetNamedValue(
                L"broadcast_interval", JsonValue::CreateNumberValue(10000));
        }
        bool nameValid = false;
        if (device.HasKey(L"name")) {
            const auto value = device.GetNamedValue(L"name");
            nameValid = value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::String
                && validDeviceName(
                    winrt::to_string(value.GetString()));
        }
        if (!nameValid) {
            device.SetNamedValue(
                L"name",
                JsonValue::CreateStringValue(
                    winrt::to_hstring(hostName())));
        }
        bool udnValid = false;
        if (device.HasKey(L"udn")) {
            const auto value = device.GetNamedValue(L"udn");
            udnValid = value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::String
                && validUdn(winrt::to_string(value.GetString()));
        }
        if (!udnValid) {
            std::string uuid = legacyUuid(m_path);
            if (uuid.empty() || !validUdn("uuid:" + uuid))
                uuid = newUuid();
            device.SetNamedValue(
                L"udn",
                JsonValue::CreateStringValue(
                    winrt::to_hstring("uuid:" + uuid)));
        }
        JsonObject picture{};
        if (m_root.HasKey(L"picture")) {
            const auto value = m_root.GetNamedValue(L"picture");
            if (value.ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Object)
                picture = value.GetObject();
        }
        if (!picture) {
            picture = JsonObject{};
            m_root.SetNamedValue(L"picture", picture);
        }
        std::vector<winrt::hstring> invalidPictureKeys;
        for (const auto &[key, value] : picture) {
            int number = 0;
            if (value.ValueType()
                    != winrt::Windows::Data::Json::JsonValueType::Number
                || !validPictureNumber(value.GetNumber(), number)) {
                invalidPictureKeys.push_back(key);
                continue;
            }
            picture.SetNamedValue(
                key, JsonValue::CreateNumberValue(number));
        }
        for (const auto &key : invalidPictureKeys)
            picture.Remove(key);
        m_root.SetNamedValue(L"ui", ui);
        m_root.SetNamedValue(L"cast", cast);
        m_root.SetNamedValue(L"device", device);
        m_root.SetNamedValue(L"picture", picture);
        return true;
    } catch (const winrt::hresult_error &exception) {
        std::cerr << "Settings structure repair failed: "
                  << winrt::to_string(exception.message()) << '\n';
        return false;
    }
}

bool ShellSettings::ensureShellState()
{
    try {
        if (!m_stateRoot)
            m_stateRoot = JsonObject{};
        if (m_stateRoot.HasKey(L"receiving_enabled")) {
            const auto value =
                m_stateRoot.GetNamedValue(L"receiving_enabled");
            if (value.ValueType()
                != winrt::Windows::Data::Json::JsonValueType::Boolean) {
                m_stateRoot.SetNamedValue(
                    L"receiving_enabled",
                    JsonValue::CreateBooleanValue(true));
            }
        }
        return true;
    } catch (const winrt::hresult_error &) {
        return false;
    }
}

} // namespace mcast::shell
