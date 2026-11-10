// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/net/SsdpInterfaceSelector.h"
#include "protocols/dlna/net/SsdpSocket.h"

#include <iphlpapi.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
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

Ipv4Interface makeInterface(
    std::string name, std::string address, unsigned long index,
    unsigned long ifType, unsigned long metric)
{
    Ipv4Interface interface;
    interface.name = std::move(name);
    interface.address = std::move(address);
    interface.index = index;
    interface.ifType = ifType;
    interface.ipv4Metric = metric;
    interface.prefixLength = 24;
    interface.up = true;
    interface.canMulticast = true;
    return interface;
}

bool waitForActivation(
    SocketEventLoop &loop, std::vector<SocketActivation> &activations,
    bool &woken, std::string &error)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    do {
        if (!loop.poll(250ms, activations, woken, error))
            return false;
        if (!activations.empty())
            return true;
    } while (std::chrono::steady_clock::now() < deadline);
    error = "timed out waiting for socket activation";
    return false;
}

} // namespace

int main()
{
    std::vector<Ipv4Interface> sample{
        makeInterface("Ethernet", "192.168.1.20", 12,
                      IF_TYPE_ETHERNET_CSMACD, 5),
        makeInterface("Wi-Fi", "192.168.1.10", 15,
                      IF_TYPE_IEEE80211, 50),
        makeInterface("Tunnel", "10.0.0.2", 4, IF_TYPE_TUNNEL, 1),
    };

    auto selection = selectSsdpInterface(sample);
    check(selection.selected.has_value(), "sample interface is selected");
    check(selection.selected->interface.name == "Wi-Fi",
          "Wi-Fi outranks Ethernet and tunnel adapters");
    check(selection.selected->score == 100,
          "Wi-Fi receives the preferred score");
    check(selection.selected->kind == SsdpInterfaceKind::Wifi,
          "Wi-Fi interface kind is reported");
    check(selection.candidates.size() == 2,
          "all usable interface samples are collected");
    check(selection.rejected.size() == 1,
          "tunnel adapters are rejected");

    auto ethernetOnly = sample;
    ethernetOnly.erase(ethernetOnly.begin() + 1);
    selection = selectSsdpInterface(ethernetOnly);
    check(selection.selected.has_value()
              && selection.selected->interface.name == "Ethernet",
          "Ethernet outranks tunnel adapters");

    auto metricTie = sample;
    metricTie.erase(metricTie.begin() + 1);
    metricTie.erase(metricTie.begin() + 1);
    metricTie.push_back(makeInterface(
        "Ethernet 2", "192.168.1.30", 13,
        IF_TYPE_ETHERNET_CSMACD, 2));
    metricTie[0].ipv4Metric = 10;
    selection = selectSsdpInterface(metricTie);
    check(selection.selected.has_value()
              && selection.selected->interface.name == "Ethernet 2",
          "lower IPv4 metric wins within the same interface class");

    std::vector<Ipv4Interface> invalid{
        makeInterface("Down", "10.0.0.3", 1, IF_TYPE_ETHERNET_CSMACD, 1),
        makeInterface("NoMulticast", "10.0.0.4", 2,
                      IF_TYPE_ETHERNET_CSMACD, 1),
        makeInterface("Loopback", "127.0.0.1", 3,
                      IF_TYPE_SOFTWARE_LOOPBACK, 1),
        makeInterface("LinkLocal", "169.254.1.1", 4,
                      IF_TYPE_ETHERNET_CSMACD, 1),
        makeInterface("Invalid", "not-an-ip", 5,
                      IF_TYPE_ETHERNET_CSMACD, 1),
        makeInterface("Virtual Hardware", "10.0.0.6", 6,
                      IF_TYPE_ETHERNET_CSMACD, 1),
        makeInterface("vEthernet (Default Switch)", "10.0.0.7", 7,
                      IF_TYPE_ETHERNET_CSMACD, 1),
    };
    invalid[0].up = false;
    invalid[1].canMulticast = false;
    invalid[2].isLoopback = true;
    invalid[3].isLinkLocal = true;
    invalid[5].hardwareInterface = false;
    selection = selectSsdpInterface(invalid);
    check(!selection.selected.has_value(),
          "invalid interfaces are not selected");
    check(selection.rejected.size() == invalid.size(),
          "every invalid interface is reported");

    std::string error;
    WinsockRuntime runtime;
    check(runtime.initialize(error), "Winsock initializes");

    SsdpSocket receiver;
    check(receiver.open(0, error), "SSDP socket opens on an ephemeral port");
    const std::uint16_t receiverPort = receiver.localPort();
    check(receiverPort != 0, "SSDP socket reports its bound port");

    SsdpSocket shared;
    check(shared.open(receiverPort, error),
          "SSDP socket can share a reused UDP port");
    shared.close();

    SocketEventLoop loop;
    check(loop.initialize(error), "SSDP event loop initializes");
    check(loop.add(receiver.handle(), FD_READ, error),
          "SSDP receiver registers for read");

    SsdpSocket sender;
    check(sender.open(0, error), "SSDP sender opens");
    const std::string request =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: ssdp:all\r\n"
        "\r\n";
    const std::span<const std::uint8_t> requestBytes(
        reinterpret_cast<const std::uint8_t *>(request.data()),
        request.size());
    check(sender.sendDatagram(
              requestBytes, "127.0.0.1", receiverPort, error)
              == SsdpSendStatus::Sent,
          "SSDP datagram sends to loopback");

    std::vector<SocketActivation> activations;
    bool woken = false;
    check(waitForActivation(loop, activations, woken, error),
          "SSDP read activation arrives");
    SsdpDatagram datagram;
    check(receiver.receiveDatagram(datagram, error)
              == SsdpReceiveStatus::Received,
          "SSDP datagram receives");
    check(std::string(datagram.bytes.begin(), datagram.bytes.end())
              == request,
          "SSDP datagram matches");
    check(datagram.sourceAddress == "127.0.0.1"
              && datagram.sourcePort != 0,
          "SSDP sender endpoint is reported");
    check(receiver.receiveDatagram(datagram, error)
              == SsdpReceiveStatus::WouldBlock,
          "empty SSDP socket reports would-block");

    const std::span<const std::uint8_t> emptyDatagram;
    check(sender.sendDatagram(
              emptyDatagram, "127.0.0.1", receiverPort, error)
              == SsdpSendStatus::Sent,
          "zero-length UDP datagram sends");
    check(waitForActivation(loop, activations, woken, error),
          "zero-length SSDP event polls");
    check(receiver.receiveDatagram(datagram, error)
              == SsdpReceiveStatus::Received
              && datagram.bytes.empty(),
          "zero-length UDP datagram receives");
    check(sender.sendDatagram(
              requestBytes, "not-an-ip", receiverPort, error)
              == SsdpSendStatus::Error,
          "invalid SSDP destination address is rejected");
    check(sender.sendDatagram(
              requestBytes, "127.0.0.1", 0, error)
              == SsdpSendStatus::Error,
          "zero SSDP destination port is rejected");

    std::vector<std::uint8_t> oversized(
        SsdpSocket::MaxDatagramBytes + 1, 0x5a);
    check(sender.sendDatagram(
              oversized, "127.0.0.1", receiverPort, error)
              == SsdpSendStatus::Error,
          "oversized SSDP send is rejected");
    sockaddr_in rawDestination{};
    rawDestination.sin_family = AF_INET;
    rawDestination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    rawDestination.sin_port = htons(receiverPort);
    oversized.resize(SsdpSocket::MaxDatagramBytes + 2);
    check(sendto(
              sender.handle(),
              reinterpret_cast<const char *>(oversized.data()),
              static_cast<int>(oversized.size()), 0,
              reinterpret_cast<const sockaddr *>(&rawDestination),
              sizeof(rawDestination)) != SOCKET_ERROR,
          "raw oversized UDP datagram sends");
    check(waitForActivation(loop, activations, woken, error),
          "oversized SSDP datagram activation arrives");
    check(receiver.receiveDatagram(datagram, error)
              == SsdpReceiveStatus::Oversize,
          "oversized SSDP datagram is reported as recoverable");
    oversized.resize(SsdpSocket::MaxDatagramBytes + 1);
    check(sendto(
              sender.handle(),
              reinterpret_cast<const char *>(oversized.data()),
              static_cast<int>(oversized.size()), 0,
              reinterpret_cast<const sockaddr *>(&rawDestination),
              sizeof(rawDestination)) != SOCKET_ERROR,
          "raw Max+1 UDP datagram sends");
    check(waitForActivation(loop, activations, woken, error),
          "Max+1 SSDP datagram activation arrives");
    check(receiver.receiveDatagram(datagram, error)
              == SsdpReceiveStatus::Oversize,
          "Max+1 SSDP datagram is reported as oversize");

    check(!receiver.setMulticastInterface("not-an-ip", error),
          "invalid multicast interface is rejected");
    check(!receiver.joinMulticastGroup(
              "224.0.0.1", "0.0.0.0", error),
          "unspecified multicast interface is rejected");
    check(!receiver.joinMulticastGroup(
              "192.168.1.1", "127.0.0.1", error),
          "unicast group address is rejected");

    error.clear();
    const auto liveInterfaces = enumerateIpv4Interfaces(error);
    check(error.empty(), "live IPv4 interfaces enumerate");
    const auto liveSelection = selectSsdpInterface(liveInterfaces);
    const bool expectedPhysicalCandidate = std::any_of(
        liveInterfaces.begin(), liveInterfaces.end(),
        [](const Ipv4Interface &interface) {
            const bool physicalType =
                interface.ifType == IF_TYPE_IEEE80211
                || interface.ifType == IF_TYPE_ETHERNET_CSMACD
                || interface.ifType == IF_TYPE_FASTETHER
                || interface.ifType == IF_TYPE_GIGABITETHERNET;
            return physicalType && interface.up
                && interface.hardwareInterface
                && !interface.filterInterface
                && !interface.isLoopback
                && !interface.isLinkLocal
                && interface.canMulticast;
        });
    check(
        !expectedPhysicalCandidate || liveSelection.selected,
        "physical multicast adapter is not filtered out");
    if (liveSelection.selected) {
        const std::string &address =
            liveSelection.selected->interface.address;
        check(receiver.setMulticastInterface(address, error),
              "live multicast interface configures");
        check(receiver.joinMulticastGroup(
                  SsdpSocket::MulticastGroup, address, error),
              "live SSDP multicast group joins");
        check(sender.setMulticastInterface(address, error),
              "live multicast sender interface configures");
        const std::string multicastRequest =
            "NOTIFY * HTTP/1.1\r\n"
            "NTS: ssdp:alive\r\n"
            "\r\n";
        const std::span<const std::uint8_t> multicastBytes(
            reinterpret_cast<const std::uint8_t *>(
                multicastRequest.data()),
            multicastRequest.size());
        check(sender.sendDatagram(
                  multicastBytes, SsdpSocket::MulticastGroup,
                  receiverPort, error)
                  == SsdpSendStatus::Sent,
              "live SSDP multicast datagram sends");
        check(waitForActivation(loop, activations, woken, error),
              "live SSDP multicast activation arrives");
        check(receiver.receiveDatagram(datagram, error)
                  == SsdpReceiveStatus::Received
                  && std::string(
                         datagram.bytes.begin(), datagram.bytes.end())
                      == multicastRequest,
              "live SSDP multicast datagram receives");
        check(receiver.leaveMulticastGroup(
                  SsdpSocket::MulticastGroup, address, error),
              "live SSDP multicast group leaves");
    } else {
        std::cout
            << "LIVE_MULTICAST_TESTS=SKIP no reliable adapter\n";
    }

    loop.remove(receiver.handle());
    receiver.close();
    sender.close();
    std::cout << "DLNA network tests passed\n";
    return 0;
}
