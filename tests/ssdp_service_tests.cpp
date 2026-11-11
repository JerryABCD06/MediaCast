// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/net/SsdpService.h"
#include "protocols/dlna/net/SsdpSocket.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace mcast::dlna::net;
using namespace mcast::platform::net;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const char *message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

bool receiveMatching(
    SsdpSocket &socket, SocketEventLoop &loop,
    const std::string &needle, std::string &response)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        std::vector<SocketActivation> activations;
        bool woken = false;
        std::string error;
        if (!loop.poll(100ms, activations, woken, error))
            return false;
        if (activations.empty())
            continue;

        while (true) {
            SsdpDatagram datagram;
            const auto status =
                socket.receiveDatagram(datagram, error);
            if (status == SsdpReceiveStatus::WouldBlock)
                break;
            if (status != SsdpReceiveStatus::Received)
                return false;
            response.assign(
                datagram.bytes.begin(), datagram.bytes.end());
            if (response.find(needle) != std::string::npos)
                return true;
        }
    }
    return false;
}

} // namespace

int main()
{
    std::string error;
    WinsockRuntime winsock;
    check(winsock.initialize(error), "Winsock initializes");

    SsdpService service;
    const SsdpServiceOptions options{
        "uuid:ssdp-service-test",
        8200,
        0,
        "127.0.0.1",
        false,
        false,
        50ms,
    };
    check(service.start(options, {}, error),
          ("SSDP service starts: " + error).c_str());
    check(service.running() && service.localPort() != 0,
          "SSDP service reports its listener");
    check(service.locationUrl()
              == "http://127.0.0.1:8200/description.xml",
          "SSDP service builds the advertised location");

    SsdpSocket client;
    check(client.open(0, error), "SSDP test client opens");
    SocketEventLoop loop;
    check(loop.initialize(error), "SSDP test event loop initializes");
    check(loop.add(client.handle(), FD_READ, error),
          "SSDP test client registers");

    const std::string search =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
        "\r\n";
    const std::span<const std::uint8_t> searchBytes(
        reinterpret_cast<const std::uint8_t *>(search.data()),
        search.size());
    check(client.sendDatagram(
              searchBytes, "127.0.0.1", service.localPort(), error)
              == SsdpSendStatus::Sent,
          "SSDP M-SEARCH sends");

    std::string response;
    check(receiveMatching(
              client, loop, "HTTP/1.1 200 OK", response),
          "SSDP M-SEARCH receives a response");
    check(response.find(
              "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n")
              != std::string::npos,
          "SSDP response preserves the search target");
    check(response.find(
              "LOCATION: http://127.0.0.1:8200/description.xml\r\n")
              != std::string::npos,
          "SSDP response advertises the HTTP description");
    check(response.find(
              "USN: uuid:ssdp-service-test::"
              "urn:schemas-upnp-org:device:MediaRenderer:1\r\n")
              != std::string::npos,
          "SSDP response uses the configured UDN");

    const std::string unsupported =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:example:unsupported\r\n"
        "\r\n";
    const std::span<const std::uint8_t> unsupportedBytes(
        reinterpret_cast<const std::uint8_t *>(unsupported.data()),
        unsupported.size());
    check(client.sendDatagram(
              unsupportedBytes, "127.0.0.1", service.localPort(), error)
              == SsdpSendStatus::Sent,
          "unsupported SSDP search sends");
    response.clear();
    check(!receiveMatching(
              client, loop, "HTTP/1.1 200 OK", response),
          "unsupported SSDP search receives no response");

    loop.remove(client.handle());
    client.close();
    service.stop();
    check(!service.running() && service.localPort() == 0,
          "SSDP service stops and closes its socket");

    std::cout << "SSDP service tests passed\n";
    return 0;
}
