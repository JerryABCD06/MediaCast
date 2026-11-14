// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/Json.h"

#include <cstdlib>
#include <iostream>
#include <string>

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
    using mcast::avalonia::qtfree::JsonError;
    using mcast::avalonia::qtfree::JsonValue;

    JsonError error;
    const auto root = JsonValue::parse(
        R"({"ui":{"language":"zh_CN","mica":true},"items":[1,2.5,false,null],"text":"a\nb"})",
        &error);
    check(root.has_value(), error.message.c_str());
    check(root->at("ui").at("language").asString() == "zh_CN",
          "UTF-8 string round trip");
    check(root->at("ui").at("mica").asBool(),
          "boolean round trip");
    check(root->at("items").items().size() == 4,
          "array round trip");
    check(root->at("items").items()[1].asNumber() == 2.5,
          "number round trip");
    check(root->at("text").asString() == "a\nb",
          "escape round trip");
    check(
        JsonValue("0.1.0").isString()
            && JsonValue("0.1.0").asString() == "0.1.0",
        "string literals do not fall through to bool");

    const auto reparsed = JsonValue::parse(root->dump(), &error);
    check(reparsed.has_value(), error.message.c_str());
    check(reparsed->at("ui").at("mica").asBool(),
          "serialized JSON parses");

    check(!JsonValue::parse(R"({"a":})", &error).has_value(),
          "malformed JSON rejected");
    check(!JsonValue::parse(R"({"a":1} trailing)", &error).has_value(),
          "trailing JSON rejected");
    check(!JsonValue::parse(R"("\u12")", &error).has_value(),
          "short Unicode escape rejected");

    const auto emoji = JsonValue::parse(
        R"({"text":"\uD83D\uDE00"})", &error);
    check(
        emoji.has_value()
            && emoji->at("text").asString() == "😀",
        "surrogate pair becomes one UTF-8 scalar");
    check(
        !JsonValue::parse(R"("\uD83D")", &error).has_value(),
        "unpaired surrogate rejected");

    std::string deep;
    for (int index = 0; index < 130; ++index)
        deep.push_back('[');
    deep.push_back('0');
    for (int index = 0; index < 130; ++index)
        deep.push_back(']');
    check(!JsonValue::parse(deep, &error).has_value(),
          "JSON depth rejected");

    std::string invalidUtf8 = "\"";
    invalidUtf8.push_back(static_cast<char>(0xc0));
    invalidUtf8.push_back(static_cast<char>(0xaf));
    invalidUtf8.push_back('"');
    check(!JsonValue::parse(invalidUtf8, &error).has_value(),
          "invalid UTF-8 rejected");

    std::cout << "Qt-free JSON tests passed\n";
    return 0;
}
