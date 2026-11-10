// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "protocols/dlna/core/HttpRequest.h"
#include "protocols/dlna/core/SsdpMessages.h"

#include <cstdlib>
#include <iostream>
#include <string>

using namespace mcast::dlna::core;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::span<const std::uint8_t> bytes(std::string_view text)
{
    return {
        reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

} // namespace

int main()
{
    const std::string request =
        "POST /AVTransport/control HTTP/1.1\r\n"
        "Host: 127.0.0.1:8200\r\n"
        "Content-Length: 11\r\n"
        "SoapAction: \"urn:test#Play\"\r\n"
        "\r\n"
        "hello world";
    HttpRequest parsed;
    std::size_t consumed = 0;
    std::string error;
    check(
        parseHttpRequest(
            bytes(std::string_view(request).substr(0, 12)),
            parsed, consumed, error)
            == HttpParseStatus::NeedMoreData,
        "fragmented HTTP request needs more data");
    const auto status = parseHttpRequest(
        bytes(request), parsed, consumed, error);
    check(status == HttpParseStatus::Complete,
          "complete HTTP request parses");
    check(consumed == request.size(), "HTTP consumed length");
    check(parsed.method == "POST"
              && parsed.target == "/AVTransport/control",
          "HTTP request line");
    check(parsed.header("host").value_or("") == "127.0.0.1:8200",
          "HTTP headers are case-insensitive");
    check(parsed.header("soapaction")
              == "\"urn:test#Play\"",
          "SOAPAction header");
    check(parsed.body == "hello world", "HTTP body");

    HttpParseLimits bodylessLimits;
    bodylessLimits.maxBodyBytes = 0;
    const std::string bodyless =
        "M-SEARCH * HTTP/1.1\r\n"
        "Host: 239.255.255.250:1900\r\n"
        "\r\n";
    check(
        parseHttpRequest(
            bytes(bodyless), parsed, consumed, error, bodylessLimits)
            == HttpParseStatus::Complete,
        "zero body limit accepts a bodyless request");

    const std::string emptyBody =
        "M-SEARCH * HTTP/1.1\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    check(
        parseHttpRequest(
            bytes(emptyBody), parsed, consumed, error, bodylessLimits)
            == HttpParseStatus::Complete,
        "zero body limit accepts Content-Length zero");
    const std::string forbiddenBody =
        "M-SEARCH * HTTP/1.1\r\n"
        "Content-Length: 1\r\n"
        "\r\n"
        "x";
    check(
        parseHttpRequest(
            bytes(forbiddenBody), parsed, consumed, error, bodylessLimits)
            == HttpParseStatus::Error,
        "zero body limit rejects a non-empty body");

    const std::string repeatedLength =
        "POST / HTTP/1.1\r\n"
        "Content-Length: 1\r\n"
        "Content-Length: 1\r\n"
        "\r\n"
        "x";
    check(
        parseHttpRequest(
            bytes(repeatedLength), parsed, consumed, error)
            == HttpParseStatus::Complete,
        "identical repeated Content-Length accepted");

    const std::string conflicting =
        "POST / HTTP/1.1\r\nContent-Length: 1\r\n"
        "Content-Length: 2\r\n\r\nxx";
    check(
        parseHttpRequest(
            bytes(conflicting), parsed, consumed, error)
            == HttpParseStatus::Error,
        "conflicting Content-Length rejected");

    const std::string chunked =
        "POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    check(
        parseHttpRequest(bytes(chunked), parsed, consumed, error)
            == HttpParseStatus::Error,
        "chunked request rejected");
    const std::string identityEncoding =
        "POST / HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n";
    check(
        parseHttpRequest(
            bytes(identityEncoding), parsed, consumed, error)
            == HttpParseStatus::Error,
        "any Transfer-Encoding is rejected");

    const std::string spacedHeaderName =
        "POST / HTTP/1.1\r\nContent-Length : 5\r\n\r\n";
    check(
        parseHttpRequest(
            bytes(spacedHeaderName), parsed, consumed, error)
            == HttpParseStatus::Error,
        "whitespace before a header colon is rejected");
    const std::string foldedHeader =
        "POST / HTTP/1.1\r\n Content-Length: 5\r\n\r\n";
    check(
        parseHttpRequest(
            bytes(foldedHeader), parsed, consumed, error)
            == HttpParseStatus::Error,
        "obsolete header folding is rejected");
    std::string controlHeader =
        "POST / HTTP/1.1\r\nX-Test: a";
    controlHeader.push_back('\0');
    controlHeader += "b\r\n\r\n";
    check(
        parseHttpRequest(
            bytes(controlHeader), parsed, consumed, error)
            == HttpParseStatus::Error,
        "control characters in a header value are rejected");

    const std::string badVersion = "GET / HTTP/1.1junk\r\n\r\n";
    check(
        parseHttpRequest(
            bytes(badVersion), parsed, consumed, error)
            == HttpParseStatus::Error,
        "suffix after HTTP version is rejected");
    std::string controlTarget = "GET /bad";
    controlTarget.push_back('\0');
    controlTarget += " HTTP/1.1\r\n\r\n";
    check(
        parseHttpRequest(
            bytes(controlTarget), parsed, consumed, error)
            == HttpParseStatus::Error,
        "control characters in request target are rejected");

    HttpParseLimits limits;
    limits.maxHeaderBytes = 32;
    const std::string oversized =
        "POST / HTTP/1.1\r\nX-Long: 12345678901234567890\r\n\r\n";
    check(
        parseHttpRequest(
            bytes(oversized), parsed, consumed, error, limits)
            == HttpParseStatus::Error,
        "oversized header rejected");

    const std::string search =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 2\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";
    SsdpRequest ssdp;
    error.clear();
    check(parseSsdpRequest(bytes(search), ssdp, error),
          "M-SEARCH parses");
    check(ssdp.method == "M-SEARCH"
              && ssdp.header("st").value_or("") == "ssdp:all",
          "M-SEARCH headers");
    check(parseSsdpRequest(bytes(search + "x"), ssdp, error) == false
              && error == "trailing data after SSDP request",
          "SSDP datagram trailing data is rejected");
    const std::string notify =
        "NOTIFY * HTTP/1.1\r\n"
        "NTS: ssdp:alive\r\n"
        "\r\n";
    check(parseSsdpRequest(bytes(notify), ssdp, error) == false
              && error == "not an SSDP M-SEARCH request",
          "non-M-SEARCH SSDP datagram is rejected");

    const std::string udn = "uuid:test-device";
    const std::string location =
        "http://127.0.0.1:8200/description.xml";
    const std::string server =
        "Windows/10.0 UPnP/1.1 MCast/0.1";
    const std::string alive = makeSsdpNotify(
        true, "upnp:rootdevice", udn, location, 1800, server, 123, error);
    const std::string expectedAlive =
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "NT: upnp:rootdevice\r\n"
        "NTS: ssdp:alive\r\n"
        "USN: uuid:test-device::upnp:rootdevice\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "LOCATION: http://127.0.0.1:8200/description.xml\r\n"
        "SERVER: Windows/10.0 UPnP/1.1 MCast/0.1\r\n"
        "BOOTID.UPNP.ORG: 123\r\n"
        "\r\n";
    check(alive == expectedAlive && error.empty(),
          "SSDP alive message is byte-exact");

    const std::string byebye = makeSsdpNotify(
        false, "upnp:rootdevice", udn, "", 0, "", 123, error);
    const std::string expectedByebye =
        "NOTIFY * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "NT: upnp:rootdevice\r\n"
        "NTS: ssdp:byebye\r\n"
        "USN: uuid:test-device::upnp:rootdevice\r\n"
        "\r\n";
    check(byebye == expectedByebye && error.empty(),
          "SSDP byebye message is byte-exact");

    const std::string response = makeSsdpSearchResponse(
        "urn:schemas-upnp-org:device:MediaRenderer:1", udn,
        location, 1800, server, "Tue, 10 Nov 2026 12:00:00 GMT", 123,
        error);
    const std::string expectedResponse =
        "HTTP/1.1 200 OK\r\n"
        "CACHE-CONTROL: max-age=1800\r\n"
        "DATE: Tue, 10 Nov 2026 12:00:00 GMT\r\n"
        "EXT:\r\n"
        "LOCATION: http://127.0.0.1:8200/description.xml\r\n"
        "SERVER: Windows/10.0 UPnP/1.1 MCast/0.1\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
        "USN: uuid:test-device::urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
        "BOOTID.UPNP.ORG: 123\r\n"
        "\r\n";
    check(response == expectedResponse && error.empty(),
          "SSDP search response is byte-exact");

    const std::string injected = makeSsdpNotify(
        true, "upnp:rootdevice\r\nInjected: value", udn,
        location, 1800, "server", 1, error);
    check(injected.empty(), "SSDP header injection rejected");

    std::cout << "DLNA core tests passed\n";
    return 0;
}
