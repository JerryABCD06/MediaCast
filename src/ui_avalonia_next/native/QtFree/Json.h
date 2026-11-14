// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mcast::avalonia::qtfree {

struct JsonError
{
    std::size_t offset = 0;
    std::string message;
};

class JsonValue
{
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    JsonValue() = default;
    explicit JsonValue(bool value);
    explicit JsonValue(double value);
    explicit JsonValue(std::string value);

    static JsonValue array();
    static JsonValue object();

    Type type() const noexcept { return m_type; }
    bool isNull() const noexcept { return m_type == Type::Null; }
    bool isBool() const noexcept { return m_type == Type::Bool; }
    bool isNumber() const noexcept { return m_type == Type::Number; }
    bool isString() const noexcept { return m_type == Type::String; }
    bool isArray() const noexcept { return m_type == Type::Array; }
    bool isObject() const noexcept { return m_type == Type::Object; }

    bool asBool(bool fallback = false) const noexcept;
    double asNumber(double fallback = 0) const noexcept;
    int asInt(int fallback = 0) const noexcept;
    std::string asString(std::string fallback = {}) const;

    const JsonValue *find(std::string_view key) const noexcept;
    JsonValue *find(std::string_view key) noexcept;
    const JsonValue &at(std::string_view key) const;
    JsonValue &operator[](std::string key);
    void set(std::string key, JsonValue value);

    const std::vector<JsonValue> &items() const noexcept { return m_array; }
    std::vector<JsonValue> &items() noexcept { return m_array; }
    const std::map<std::string, JsonValue, std::less<>> &members()
        const noexcept
    {
        return m_object;
    }

    void push(JsonValue value);
    std::string dump() const;

    static std::optional<JsonValue> parse(
        std::string_view text, JsonError *error = nullptr);

private:
    Type m_type = Type::Null;
    bool m_bool = false;
    double m_number = 0;
    std::string m_string;
    std::vector<JsonValue> m_array;
    std::map<std::string, JsonValue, std::less<>> m_object;
};

std::string jsonEscape(std::string_view value);

} // namespace mcast::avalonia::qtfree

