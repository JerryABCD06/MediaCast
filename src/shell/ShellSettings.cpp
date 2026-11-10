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

#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>

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
    if (!std::filesystem::exists(m_path)) {
        ensureDefaults();
    } else {
        const std::string content = readFile(m_path, error);
        if (!error.empty()) {
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
            }
        }
    }
    if (!ensureDefaults()) {
        m_root = JsonObject{};
        ensureDefaults();
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
    return error.empty();
}

bool ShellSettings::save(std::string &error) const
{
    if (!m_stateRoot || !m_stateSaveAllowed) {
        if (error.empty())
            error = "Shell state is not writable";
        return false;
    }
    std::filesystem::path temporary = m_statePath;
    temporary += L".tmp";
    try {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) {
            error = "cannot open temporary settings file";
            return false;
        }
        const std::string content =
            winrt::to_string(m_stateRoot.Stringify());
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
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
            temporary.c_str(), m_statePath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "MoveFileExW(settings) failed: "
            + std::to_string(GetLastError());
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool ShellSettings::chineseUi() const
{
    winrt::hstring language(L"System");
    if (m_root && m_root.HasKey(L"ui")) {
        const auto uiValue = m_root.GetNamedValue(L"ui");
        if (uiValue.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::Object) {
            const auto ui = uiValue.GetObject();
            if (ui.HasKey(L"language")) {
                const auto value = ui.GetNamedValue(L"language");
                if (value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::String)
                    language = value.GetString();
            }
        }
    }
    if (language == L"zh_CN")
        return true;
    if (language == L"en_US")
        return false;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
}

bool ShellSettings::acceptNewCast() const
{
    if (m_stateRoot && m_stateRoot.HasKey(L"receiving_enabled")) {
        const auto value = m_stateRoot.GetNamedValue(L"receiving_enabled");
        if (value.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::Boolean)
            return value.GetBoolean();
    }
    if (m_root && m_root.HasKey(L"cast")) {
        const auto castValue = m_root.GetNamedValue(L"cast");
        if (castValue.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::Object) {
            const auto cast = castValue.GetObject();
            if (cast.HasKey(L"newcast")) {
                const auto value = cast.GetNamedValue(L"newcast");
                if (value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::Boolean)
                    return value.GetBoolean();
            }
        }
    }
    return true;
}

bool ShellSettings::broadcast() const
{
    if (m_root && m_root.HasKey(L"cast")) {
        const auto castValue = m_root.GetNamedValue(L"cast");
        if (castValue.ValueType()
            == winrt::Windows::Data::Json::JsonValueType::Object) {
            const auto cast = castValue.GetObject();
            if (cast.HasKey(L"broadcast")) {
                const auto value = cast.GetNamedValue(L"broadcast");
                if (value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::Boolean)
                    return value.GetBoolean();
            }
        }
    }
    return true;
}

void ShellSettings::setAcceptNewCast(bool enabled)
{
    if (!m_stateRoot)
        m_stateRoot = JsonObject{};
    m_stateRoot.SetNamedValue(
        L"receiving_enabled", JsonValue::CreateBooleanValue(enabled));
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

        const auto ensureString = [](JsonObject &object,
                                      const wchar_t *key,
                                      const wchar_t *fallback) {
            bool valid = false;
            if (object.HasKey(key)) {
                const auto value = object.GetNamedValue(key);
                valid = value.ValueType()
                    == winrt::Windows::Data::Json::JsonValueType::String;
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
        ensureString(ui, L"language", L"System");
        ensureString(ui, L"darkmode", L"System");
        ensureString(ui, L"accentmode", L"System");
        ensureBool(ui, L"mica", true);
        ensureBool(cast, L"newcast", true);
        ensureBool(cast, L"broadcast", true);
        bool intervalValid = false;
        if (cast.HasKey(L"broadcast_interval")) {
            const auto value = cast.GetNamedValue(L"broadcast_interval");
            intervalValid = value.ValueType()
                == winrt::Windows::Data::Json::JsonValueType::Number;
        }
        if (!intervalValid) {
            cast.SetNamedValue(
                L"broadcast_interval", JsonValue::CreateNumberValue(10000));
        }
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
