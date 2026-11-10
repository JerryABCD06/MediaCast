// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/core/HttpRequest.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>

namespace mcast::dlna::core {
namespace {

std::string lower(std::string_view value)
{
    std::string result(value);
    std::transform(
        result.begin(), result.end(), result.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
    return result;
}

std::string trim(std::string_view value)
{
    while (!value.empty()
           && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    while (!value.empty()
           && (value.back() == ' ' || value.back() == '\t'))
        value.remove_suffix(1);
    return std::string(value);
}

bool token(std::string_view value)
{
    return !value.empty()
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return std::isalnum(ch) || std::string_view("!#$%&'*+-.^_`|~")
                .find(static_cast<char>(ch)) != std::string_view::npos;
        });
}

bool requestTarget(std::string_view value)
{
    return !value.empty()
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return ch >= 0x21 && ch <= 0x7e;
        });
}

bool headerValue(std::string_view value)
{
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch == '\t' || ch == ' ' || (ch >= 0x21 && ch <= 0x7e)
            || ch >= 0x80;
    });
}

enum class ContentLengthStatus {
    Valid,
    Invalid,
    TooLarge,
};

ContentLengthStatus parseContentLength(
    std::string_view value, std::size_t maximum, std::size_t &length)
{
    if (value.empty())
        return ContentLengthStatus::Invalid;
    std::uint64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed, 10);
    if (result.ec != std::errc{}
        || result.ptr != value.data() + value.size())
        return ContentLengthStatus::Invalid;
    if (parsed > maximum)
        return ContentLengthStatus::TooLarge;
    length = static_cast<std::size_t>(parsed);
    return ContentLengthStatus::Valid;
}

} // namespace

std::optional<std::string> HttpRequest::header(std::string_view name) const
{
    const std::string wanted = lower(name);
    for (const auto &[headerName, value] : headers) {
        if (headerName == wanted)
            return value;
    }
    return std::nullopt;
}

HttpParseStatus parseHttpRequest(
    std::span<const std::uint8_t> bytes, HttpRequest &request,
    std::size_t &consumed, std::string &error,
    const HttpParseLimits &limits)
{
    consumed = 0;
    request = {};
    error.clear();
    if (limits.maxHeaderBytes == 0)
        return HttpParseStatus::Error;

    const std::string_view input(
        reinterpret_cast<const char *>(bytes.data()), bytes.size());
    const std::size_t headerEnd = input.find("\r\n\r\n");
    if (headerEnd == std::string_view::npos) {
        if (input.size() > limits.maxHeaderBytes) {
            error = "HTTP header exceeds limit";
            return HttpParseStatus::Error;
        }
        return HttpParseStatus::NeedMoreData;
    }
    if (headerEnd > limits.maxHeaderBytes) {
        error = "HTTP header exceeds limit";
        return HttpParseStatus::Error;
    }

    const std::string_view head = input.substr(0, headerEnd);
    const std::size_t requestLineEnd = head.find("\r\n");
    const std::string_view requestLine = requestLineEnd == std::string_view::npos
        ? head : head.substr(0, requestLineEnd);
    const std::size_t firstSpace = requestLine.find(' ');
    const std::size_t secondSpace = firstSpace == std::string_view::npos
        ? std::string_view::npos : requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos
        || secondSpace == std::string_view::npos
        || requestLine.find(' ', secondSpace + 1) != std::string_view::npos) {
        error = "invalid HTTP request line";
        return HttpParseStatus::Error;
    }
    request.method = std::string(requestLine.substr(0, firstSpace));
    request.target = std::string(requestLine.substr(
        firstSpace + 1, secondSpace - firstSpace - 1));
    request.version = std::string(requestLine.substr(secondSpace + 1));
    if (!token(request.method) || !requestTarget(request.target)
        || (request.version != "HTTP/1.0"
            && request.version != "HTTP/1.1")) {
        error = "invalid HTTP request line";
        return HttpParseStatus::Error;
    }

    std::size_t contentLength = 0;
    std::optional<std::size_t> parsedContentLength;
    if (requestLineEnd != std::string_view::npos) {
        std::size_t offset = requestLineEnd + 2;
        while (offset <= head.size()) {
            const std::size_t lineEnd = head.find("\r\n", offset);
            const std::string_view line = head.substr(
                offset, lineEnd == std::string_view::npos
                    ? head.size() - offset : lineEnd - offset);
            if (!line.empty()) {
                const std::size_t colon = line.find(':');
                if (colon == std::string_view::npos || colon == 0) {
                    error = "invalid HTTP header";
                    return HttpParseStatus::Error;
                }
                const std::string_view rawName = line.substr(0, colon);
                if (!token(rawName)) {
                    error = "invalid HTTP header name";
                    return HttpParseStatus::Error;
                }
                const std::string name = lower(rawName);
                const std::string value = trim(line.substr(colon + 1));
                if (!headerValue(value)) {
                    error = "invalid HTTP header value";
                    return HttpParseStatus::Error;
                }
                if (request.headers.size() >= limits.maxHeaderCount) {
                    error = "too many HTTP headers";
                    return HttpParseStatus::Error;
                }
                if (name == "content-length") {
                    const auto lengthStatus = parseContentLength(
                        value, limits.maxBodyBytes, contentLength);
                    if (lengthStatus == ContentLengthStatus::TooLarge) {
                        error = "HTTP body exceeds limit";
                        return HttpParseStatus::Error;
                    }
                    if (lengthStatus == ContentLengthStatus::Invalid
                        || (parsedContentLength
                            && *parsedContentLength != contentLength)) {
                        error = "invalid HTTP Content-Length";
                        return HttpParseStatus::Error;
                    }
                    parsedContentLength = contentLength;
                } else if (name == "transfer-encoding") {
                    error = "HTTP transfer encoding is not supported";
                    return HttpParseStatus::Error;
                }
                request.headers.emplace_back(name, value);
            }
            if (lineEnd == std::string_view::npos)
                break;
            offset = lineEnd + 2;
        }
    }

    const std::size_t bodyOffset = headerEnd + 4;
    if (contentLength > limits.maxBodyBytes
        || bodyOffset > std::numeric_limits<std::size_t>::max()
            - contentLength) {
        error = "HTTP body exceeds limit";
        return HttpParseStatus::Error;
    }
    const std::size_t total = bodyOffset + contentLength;
    if (input.size() < total)
        return HttpParseStatus::NeedMoreData;
    request.body.assign(input.substr(bodyOffset, contentLength));
    consumed = total;
    return HttpParseStatus::Complete;
}

} // namespace mcast::dlna::core
