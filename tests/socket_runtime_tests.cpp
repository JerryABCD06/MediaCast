// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/net/SocketRuntime.h"

#include <iphlpapi.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

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

} // namespace

int main()
{
    std::string error;
    WinsockRuntime runtime;
    check(runtime.initialize(error), "Winsock initializes");

    SocketHandle receiver;
    check(receiver.createUdp(error), "UDP receiver creates");
    check(receiver.setNonBlocking(error), "receiver is nonblocking");
    check(receiver.bindLoopback(0, error), "receiver binds loopback");
    const auto port = receiver.localPort();
    check(port.has_value() && *port != 0, "ephemeral port assigned");

    SocketEventLoop loop;
    check(loop.initialize(error), "event loop initializes");
    check(loop.add(receiver.get(), FD_READ, error),
          "receiver registers for read");

    SocketHandle sender;
    check(sender.createUdp(error), "UDP sender creates");
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(*port);
    const char payload[] = "mcast";
    constexpr int payloadSize = sizeof(payload) - 1;
    check(
        sendto(
            sender.get(), payload, payloadSize, 0,
            reinterpret_cast<const sockaddr *>(&destination),
            sizeof(destination)) != SOCKET_ERROR,
        "UDP payload sends");

    std::vector<SocketActivation> activations;
    bool woken = false;
    check(loop.poll(2s, activations, woken, error),
          "event loop polls");
    check(!woken && !activations.empty(), "read activation arrives");
    check(
        (activations.front().events & FD_READ) != 0,
        "read event bit is set");

    char received[16]{};
    sockaddr_in senderAddress{};
    int senderSize = sizeof(senderAddress);
    const int bytes = recvfrom(
        receiver.get(), received, sizeof(received), 0,
        reinterpret_cast<sockaddr *>(&senderAddress), &senderSize);
    check(bytes == payloadSize,
          "UDP payload receives");
    check(std::string(received, bytes) == "mcast",
          "UDP payload matches");

    loop.wake();
    check(loop.poll(2s, activations, woken, error) && woken,
          "wake event interrupts poll");

    SocketHandle tcp;
    check(tcp.createTcp(error), "TCP socket creates");
    check(tcp.setNonBlocking(error), "TCP socket is nonblocking");
    check(loop.add(tcp.get(), FD_CONNECT | FD_CLOSE, error),
          "TCP connect event registers");
    const int connectResult = connect(
        tcp.get(), reinterpret_cast<const sockaddr *>(&destination),
        sizeof(destination));
    check(
        connectResult == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK,
        "nonblocking TCP connect starts");
    const auto tcpDeadline = std::chrono::steady_clock::now() + 5s;
    do {
        check(loop.poll(250ms, activations, woken, error),
              "TCP failure event polls");
    } while (activations.empty()
             && std::chrono::steady_clock::now() < tcpDeadline);
    const auto tcpActivation = std::find_if(
        activations.begin(), activations.end(),
        [&](const SocketActivation &activation) {
            return activation.socket == tcp.get();
        });
    check(tcpActivation != activations.end(), "TCP activation is present");
    check((tcpActivation->events & FD_CONNECT) != 0,
          "TCP FD_CONNECT is present");
    check(tcpActivation->errorCodes[FD_CONNECT_BIT] != 0,
          "TCP connect error code is preserved");

    TimerQueue timers;
    const auto now = std::chrono::steady_clock::now();
    const auto first = timers.schedule(now - 1ms, 11);
    const auto second = timers.schedule(now + 100ms, 12);
    const auto duplicate = timers.schedule(now + 200ms, 12);
    check(first == 11 && second == 12, "timer IDs are stable");
    check(duplicate == 0, "duplicate timer ID is rejected");
    check(timers.cancel(12), "timer cancel accepts");
    const auto due = timers.takeDue(now);
    check(due.size() == 1 && due.front() == 11, "due timer is returned");
    check(!timers.nextDeadline().has_value(),
          "cancelled future timer is not returned");

    TimerQueue collisionTimers;
    const auto automatic = collisionTimers.schedule(now + 1s);
    check(collisionTimers.schedule(now + 2s, automatic) == 0,
          "explicit ID cannot collide with an automatic timer");
    check(collisionTimers.cancel(automatic), "automatic ID remains cancellable");

    error.clear();
    const auto interfaces = enumerateIpv4Interfaces(error);
    check(error.empty(), "IPv4 adapter enumeration succeeds");
    for (const auto &interfaceValue : interfaces) {
        check(!interfaceValue.address.empty(), "adapter address is present");
        check(
            interfaceValue.canMulticast
                == ((interfaceValue.adapterFlags & IP_ADAPTER_NO_MULTICAST) == 0),
            "multicast capability is reported");
    }

    loop.remove(receiver.get());
    loop.remove(tcp.get());
    std::cout << "Socket runtime tests passed\n";
    return 0;
}
