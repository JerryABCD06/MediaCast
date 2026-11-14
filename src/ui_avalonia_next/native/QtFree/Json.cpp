// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/Json.h"

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

constexpr int kMaxJsonDepth = 128;

bool validUtf8(std::string_view text)
{
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto first =
            static_cast<unsigned char>(text[offset]);
        if (first < 0x80) {
            ++offset;
            continue;
        }

        int length = 0;
        std::uint32_t scalar = 0;
        if ((first & 0xe0) == 0xc0) {
            length = 2;
            scalar = first & 0x1f;
        } else if ((first & 0xf0) == 0xe0) {
            length = 3;
            scalar = first & 0x0f;
        } else if ((first & 0xf8) == 0xf0) {
            length = 4;
            scalar = first & 0x07;
        } else {
            return false;
        }
        if (offset + static_cast<std::size_t>(length) > text.size())
            return false;
        for (int index = 1; index < length; ++index) {
            const auto next =
                static_cast<unsigned char>(text[offset + index]);
            if ((next & 0xc0) != 0x80)
                return false;
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if ((length == 2 && scalar < 0x80)
            || (length == 3 && scalar < 0x800)
            || (length == 4 && scalar < 0x10000)
            || scalar > 0x10ffff
            || (scalar >= 0xd800 && scalar <= 0xdfff)) {
            return false;
        }
        offset += static_cast<std::size_t>(length);
    }
    return true;
}

void setError(JsonError *error, std::size_t offset, std::string message)
{
    if (!error)
        return;
    error->offset = offset;
    error->message = std::move(message);
}

void appendUtf8(std::string &target, std::uint32_t scalar)
{
    if (scalar <= 0x7f) {
        target.push_back(static_cast<char>(scalar));
    } else if (scalar <= 0x7ff) {
        target.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
        target.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
    } else if (scalar <= 0xffff) {
        target.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
        target.push_back(
            static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
    } else {
        target.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
        target.push_back(
            static_cast<char>(0x80 | ((scalar >> 12) & 0x3f)));
        target.push_back(
            static_cast<char>(0x80 | ((scalar >> 6) & 0x3f)));
        target.push_back(static_cast<char>(0x80 | (scalar & 0x3f)));
    }
}

class Parser
{
public:
    explicit Parser(std::string_view text)
        : m_text(text)
    {
    }

    std::optional<JsonValue> parse(JsonError *error)
    {
        skipWhitespace();
        auto value = parseValue(0);
        if (!value) {
            if (error)
                *error = m_error;
            return std::nullopt;
        }
        skipWhitespace();
        if (m_offset != m_text.size()) {
            fail("trailing JSON data");
            if (error)
                *error = m_error;
            return std::nullopt;
        }
        if (error)
            *error = {};
        return value;
    }

private:
    std::optional<JsonValue> parseValue(int depth)
    {
        if (depth > kMaxJsonDepth)
            return fail("JSON nesting depth exceeded");
        if (m_offset >= m_text.size())
            return fail("expected JSON value");

        switch (m_text[m_offset]) {
        case '{':
            return parseObject(depth + 1);
        case '[':
            return parseArray(depth + 1);
        case '"': {
            auto string = parseString();
            if (!string)
                return std::nullopt;
            return JsonValue(std::move(*string));
        }
        case 't':
            return parseLiteral("true", JsonValue(true));
        case 'f':
            return parseLiteral("false", JsonValue(false));
        case 'n':
            return parseLiteral("null", JsonValue());
        default:
            if (m_text[m_offset] == '-'
                || (m_text[m_offset] >= '0'
                    && m_text[m_offset] <= '9')) {
                return parseNumber();
            }
            return fail("unexpected JSON token");
        }
    }

    std::optional<JsonValue> parseObject(int depth)
    {
        JsonValue result = JsonValue::object();
        ++m_offset;
        skipWhitespace();
        if (consume('}'))
            return result;

        while (true) {
            skipWhitespace();
            auto key = parseString();
            if (!key)
                return std::nullopt;
            skipWhitespace();
            if (!consume(':'))
                return fail("expected ':' after object key");
            skipWhitespace();
            auto value = parseValue(depth);
            if (!value)
                return std::nullopt;
            result.set(std::move(*key), std::move(*value));
            skipWhitespace();
            if (consume('}'))
                return result;
            if (!consume(','))
                return fail("expected ',' or '}' in object");
        }
    }

    std::optional<JsonValue> parseArray(int depth)
    {
        JsonValue result = JsonValue::array();
        ++m_offset;
        skipWhitespace();
        if (consume(']'))
            return result;

        while (true) {
            skipWhitespace();
            auto value = parseValue(depth);
            if (!value)
                return std::nullopt;
            result.push(std::move(*value));
            skipWhitespace();
            if (consume(']'))
                return result;
            if (!consume(','))
                return fail("expected ',' or ']' in array");
        }
    }

    std::optional<std::string> parseString()
    {
        if (!consume('"'))
            return fail("expected JSON string");

        std::string result;
        while (m_offset < m_text.size()) {
            const auto ch =
                static_cast<unsigned char>(m_text[m_offset++]);
            if (ch == '"')
                return result;
            if (ch < 0x20)
                return fail("control character in JSON string");
            if (ch != '\\') {
                result.push_back(static_cast<char>(ch));
                continue;
            }
            if (m_offset >= m_text.size())
                return fail("unterminated JSON escape");

            switch (m_text[m_offset++]) {
            case '"':
                result.push_back('"');
                break;
            case '\\':
                result.push_back('\\');
                break;
            case '/':
                result.push_back('/');
                break;
            case 'b':
                result.push_back('\b');
                break;
            case 'f':
                result.push_back('\f');
                break;
            case 'n':
                result.push_back('\n');
                break;
            case 'r':
                result.push_back('\r');
                break;
            case 't':
                result.push_back('\t');
                break;
            case 'u': {
                auto scalar = parseHexCodeUnit();
                if (!scalar)
                    return std::nullopt;
                if (*scalar >= 0xd800 && *scalar <= 0xdbff) {
                    if (m_offset + 2 > m_text.size()
                        || m_text[m_offset] != '\\'
                        || m_text[m_offset + 1] != 'u') {
                        return fail(
                            "unpaired high surrogate in JSON string");
                    }
                    m_offset += 2;
                    const auto low = parseHexCodeUnit();
                    if (!low || *low < 0xdc00 || *low > 0xdfff)
                        return fail(
                            "invalid low surrogate in JSON string");
                    *scalar = 0x10000
                        + ((*scalar - 0xd800) << 10)
                        + (*low - 0xdc00);
                } else if (*scalar >= 0xdc00 && *scalar <= 0xdfff) {
                    return fail(
                        "unpaired low surrogate in JSON string");
                }
                appendUtf8(result, *scalar);
                break;
            }
            default:
                return fail("invalid JSON escape");
            }
        }
        return fail("unterminated JSON string");
    }

    std::optional<std::uint32_t> parseHexCodeUnit()
    {
        if (m_offset + 4 > m_text.size())
            return fail("short unicode escape");
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char ch = m_text[m_offset++];
            value <<= 4;
            if (ch >= '0' && ch <= '9')
                value |= static_cast<std::uint32_t>(ch - '0');
            else if (ch >= 'a' && ch <= 'f')
                value |= static_cast<std::uint32_t>(ch - 'a' + 10);
            else if (ch >= 'A' && ch <= 'F')
                value |= static_cast<std::uint32_t>(ch - 'A' + 10);
            else
                return fail("invalid unicode escape");
        }
        return value;
    }

    std::optional<JsonValue> parseNumber()
    {
        const std::size_t begin = m_offset;
        if (m_text[m_offset] == '-')
            ++m_offset;
        if (m_offset >= m_text.size())
            return fail("short JSON number");
        if (m_text[m_offset] == '0') {
            ++m_offset;
        } else {
            if (m_text[m_offset] < '1' || m_text[m_offset] > '9')
                return fail("invalid JSON number");
            while (m_offset < m_text.size()
                   && m_text[m_offset] >= '0'
                   && m_text[m_offset] <= '9') {
                ++m_offset;
            }
        }
        if (m_offset < m_text.size() && m_text[m_offset] == '.') {
            ++m_offset;
            const std::size_t fraction = m_offset;
            while (m_offset < m_text.size()
                   && m_text[m_offset] >= '0'
                   && m_text[m_offset] <= '9') {
                ++m_offset;
            }
            if (fraction == m_offset)
                return fail("invalid JSON fraction");
        }
        if (m_offset < m_text.size()
            && (m_text[m_offset] == 'e'
                || m_text[m_offset] == 'E')) {
            ++m_offset;
            if (m_offset < m_text.size()
                && (m_text[m_offset] == '+'
                    || m_text[m_offset] == '-')) {
                ++m_offset;
            }
            const std::size_t exponent = m_offset;
            while (m_offset < m_text.size()
                   && m_text[m_offset] >= '0'
                   && m_text[m_offset] <= '9') {
                ++m_offset;
            }
            if (exponent == m_offset)
                return fail("invalid JSON exponent");
        }

        const std::string token(
            m_text.substr(begin, m_offset - begin));
        char *end = nullptr;
        const double number = std::strtod(token.c_str(), &end);
        if (end != token.c_str() + token.size()
            || !std::isfinite(number)) {
            return fail("invalid JSON number value");
        }
        return JsonValue(number);
    }

    std::optional<JsonValue> parseLiteral(
        std::string_view literal, JsonValue value)
    {
        if (m_text.substr(m_offset, literal.size()) != literal)
            return fail("invalid JSON literal");
        m_offset += literal.size();
        return value;
    }

    void skipWhitespace()
    {
        while (m_offset < m_text.size()) {
            const char ch = m_text[m_offset];
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n')
                break;
            ++m_offset;
        }
    }

    bool consume(char expected)
    {
        if (m_offset >= m_text.size() || m_text[m_offset] != expected)
            return false;
        ++m_offset;
        return true;
    }

    std::nullopt_t fail(std::string message)
    {
        if (m_error.message.empty())
            setError(&m_error, m_offset, std::move(message));
        return std::nullopt;
    }

    std::string_view m_text;
    std::size_t m_offset = 0;
    JsonError m_error;
};

void appendIndent(std::string &output, int depth)
{
    output.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void dumpValue(const JsonValue &value, std::string &output, int depth)
{
    switch (value.type()) {
    case JsonValue::Type::Null:
        output += "null";
        break;
    case JsonValue::Type::Bool:
        output += value.asBool() ? "true" : "false";
        break;
    case JsonValue::Type::Number: {
        std::ostringstream stream;
        stream << std::setprecision(17) << value.asNumber();
        output += stream.str();
        break;
    }
    case JsonValue::Type::String:
        output.push_back('"');
        output += jsonEscape(value.asString());
        output.push_back('"');
        break;
    case JsonValue::Type::Array:
        output.push_back('[');
        if (!value.items().empty()) {
            output.push_back('\n');
            for (std::size_t index = 0;
                 index < value.items().size(); ++index) {
                appendIndent(output, depth + 1);
                dumpValue(value.items()[index], output, depth + 1);
                if (index + 1 != value.items().size())
                    output.push_back(',');
                output.push_back('\n');
            }
            appendIndent(output, depth);
        }
        output.push_back(']');
        break;
    case JsonValue::Type::Object: {
        output.push_back('{');
        if (!value.members().empty()) {
            output.push_back('\n');
            std::size_t count = 0;
            for (const auto &[key, member] : value.members()) {
                appendIndent(output, depth + 1);
                output.push_back('"');
                output += jsonEscape(key);
                output += "\": ";
                dumpValue(member, output, depth + 1);
                if (++count != value.members().size())
                    output.push_back(',');
                output.push_back('\n');
            }
            appendIndent(output, depth);
        }
        output.push_back('}');
        break;
    }
    }
}

} // namespace

JsonValue::JsonValue(bool value)
    : m_type(Type::Bool)
    , m_bool(value)
{
}

JsonValue::JsonValue(double value)
    : m_type(Type::Number)
    , m_number(value)
{
}

JsonValue::JsonValue(const char *value)
    : m_type(Type::String)
    , m_string(value ? value : "")
{
}

JsonValue::JsonValue(std::string value)
    : m_type(Type::String)
    , m_string(std::move(value))
{
}

JsonValue JsonValue::array()
{
    JsonValue value;
    value.m_type = Type::Array;
    return value;
}

JsonValue JsonValue::object()
{
    JsonValue value;
    value.m_type = Type::Object;
    return value;
}

bool JsonValue::asBool(bool fallback) const noexcept
{
    return m_type == Type::Bool ? m_bool : fallback;
}

double JsonValue::asNumber(double fallback) const noexcept
{
    return m_type == Type::Number ? m_number : fallback;
}

int JsonValue::asInt(int fallback) const noexcept
{
    return m_type == Type::Number
            && m_number >= std::numeric_limits<int>::min()
            && m_number <= std::numeric_limits<int>::max()
        ? static_cast<int>(m_number)
        : fallback;
}

std::string JsonValue::asString(std::string fallback) const
{
    return m_type == Type::String ? m_string : std::move(fallback);
}

const JsonValue *JsonValue::find(std::string_view key) const noexcept
{
    if (m_type != Type::Object)
        return nullptr;
    const auto found = m_object.find(key);
    return found == m_object.end() ? nullptr : &found->second;
}

JsonValue *JsonValue::find(std::string_view key) noexcept
{
    if (m_type != Type::Object)
        return nullptr;
    const auto found = m_object.find(key);
    return found == m_object.end() ? nullptr : &found->second;
}

const JsonValue &JsonValue::at(std::string_view key) const
{
    static const JsonValue null;
    const JsonValue *value = find(key);
    return value ? *value : null;
}

JsonValue &JsonValue::operator[](std::string key)
{
    if (m_type == Type::Null)
        m_type = Type::Object;
    return m_object[std::move(key)];
}

void JsonValue::set(std::string key, JsonValue value)
{
    m_type = Type::Object;
    m_object.insert_or_assign(std::move(key), std::move(value));
}

void JsonValue::push(JsonValue value)
{
    m_type = Type::Array;
    m_array.push_back(std::move(value));
}

std::string JsonValue::dump() const
{
    std::string output;
    dumpValue(*this, output, 0);
    return output;
}

std::optional<JsonValue> JsonValue::parse(
    std::string_view text, JsonError *error)
{
    if (error)
        *error = {};
    if (!validUtf8(text)) {
        setError(error, 0, "invalid UTF-8 in JSON");
        return std::nullopt;
    }
    Parser parser(text);
    return parser.parse(error);
}

std::string jsonEscape(std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\b':
            output += "\\b";
            break;
        case '\f':
            output += "\\f";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20) {
                output += "\\u00";
                output.push_back(hex[ch >> 4]);
                output.push_back(hex[ch & 0x0f]);
            } else {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return output;
}

} // namespace mcast::avalonia::qtfree

