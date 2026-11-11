// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/UpnpXml.h"
#include "protocols/dlna/runtime/DlnaProtocolRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace mcast::cast;
using namespace mcast::dlna;
using namespace mcast::dlna::runtime;
using namespace mcast::playback;
using namespace mcast::platform::net;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::string soapBody(
    std::string action, std::string arguments = {})
{
    return "<?xml version=\"1.0\"?>"
        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
        "<s:Body><u:" + action
        + " xmlns:u=\"urn:schemas-upnp-org:service:AVTransport:1\">"
        + arguments + "</u:" + action + "></s:Body></s:Envelope>";
}

std::string setUriBody(const std::string &uri, const std::string &address)
{
    const std::string didl =
        "<DIDL-Lite><item><dc:title xmlns:dc=\"urn:test\">"
        "Runtime Test</dc:title></item></DIDL-Lite>";
    return soapBody(
        "SetAVTransportURI",
        "<InstanceID>0</InstanceID><CurrentURI>"
            + UpnpXml::escapeText(uri)
            + "</CurrentURI><CurrentURIMetaData>"
            + UpnpXml::escapeText(didl)
            + "</CurrentURIMetaData>");
}

std::string httpRequest(
    const std::string &address, const std::string &body)
{
    return "POST /AVTransport/control HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: Runtime-Test-" + address + "\r\n"
        "SOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#"
            "SetAVTransportURI\"\r\n"
        "Content-Type: text/xml; charset=utf-8\r\n"
        "Content-Length: " + std::to_string(body.size()) + "\r\n"
        "Connection: close\r\n\r\n" + body;
}

std::string exchange(
    std::uint16_t port, const std::string &request)
{
    std::string error;
    SocketHandle socket;
    check(socket.createTcp(error), "HTTP client socket creates: " + error);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    destination.sin_port = htons(port);
    check(connect(
              socket.get(),
              reinterpret_cast<const sockaddr *>(&destination),
              sizeof(destination)) != SOCKET_ERROR,
          "HTTP client connects: "
              + std::to_string(WSAGetLastError()));
    std::size_t offset = 0;
    while (offset < request.size()) {
        const int sent = send(
            socket.get(), request.data() + offset,
            static_cast<int>(request.size() - offset), 0);
        check(sent > 0, "HTTP client sends request");
        offset += static_cast<std::size_t>(sent);
    }

    std::string response;
    char buffer[4096];
    while (true) {
        const int received = recv(socket.get(), buffer, sizeof(buffer), 0);
        if (received > 0) {
            response.append(buffer, static_cast<std::size_t>(received));
            continue;
        }
        if (received == 0)
            break;
        const int code = WSAGetLastError();
        check(
            code == WSAECONNRESET || code == WSAECONNABORTED
                || code == WSAENETRESET || code == WSAESHUTDOWN,
            "HTTP client receive completes: " + std::to_string(code));
        break;
    }
    return response;
}

class TestConfirmationPort final : public DlnaClaimConfirmationPort
{
public:
    bool waitForDecision(
        const ConfirmationPrompt &prompt,
        bool &accepted, std::string &error) override
    {
        std::unique_lock lock(m_mutex);
        m_prompt = prompt;
        m_ready.notify_all();
        m_decided.wait(lock, [this] {
            return m_cancelled || m_decision.has_value();
        });
        if (m_cancelled) {
            accepted = false;
            error = "cancelled";
            return false;
        }
        accepted = *m_decision;
        m_decision.reset();
        error.clear();
        return true;
    }

    void cancel() noexcept override
    {
        {
            std::lock_guard lock(m_mutex);
            m_cancelled = true;
        }
        m_decided.notify_all();
    }

    std::optional<ConfirmationPrompt> waitForPrompt()
    {
        std::unique_lock lock(m_mutex);
        if (!m_ready.wait_for(lock, 2s, [this] {
                return m_prompt.has_value();
            })) {
            return std::nullopt;
        }
        auto prompt = std::move(m_prompt);
        m_prompt.reset();
        return prompt;
    }

    void decide(bool accepted)
    {
        {
            std::lock_guard lock(m_mutex);
            m_decision = accepted;
        }
        m_decided.notify_all();
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_ready;
    std::condition_variable m_decided;
    std::optional<ConfirmationPrompt> m_prompt;
    std::optional<bool> m_decision;
    bool m_cancelled = false;
};

class EarlyCompletionGateway final : public PlaybackGateway
{
public:
    PlaybackSnapshot snapshot() const override
    {
        std::lock_guard lock(m_mutex);
        return m_snapshot;
    }

    PlaybackCommandSubmission submit(
        const PlaybackCommand &command) override
    {
        std::vector<std::function<void(const PlaybackEvent &)>>
            listeners;
        PlaybackSnapshot snapshot;
        std::uint64_t commandId = 0;
        {
            std::lock_guard lock(m_mutex);
            commandId = ++m_nextCommandId;
            if (command.type == PlaybackCommandType::Open) {
                m_snapshot.current = command.media;
                m_snapshot.state = PlaybackState::Preparing;
                m_snapshot.loadStatus = LoadStatus::Ok;
            } else if (
                command.type
                == PlaybackCommandType::SetPeerConnected) {
                m_snapshot.peerConnected = command.boolValue;
            }
            ++m_snapshot.stateVersion;
            snapshot = m_snapshot;
            for (const auto &[id, listener] : m_listeners) {
                (void)id;
                listeners.push_back(listener);
            }
        }
        PlaybackEvent completed;
        completed.kind = PlaybackEventKind::CommandCompleted;
        completed.commandId = commandId;
        completed.outcome = PlaybackOutcomeStatus::Applied;
        completed.snapshot = snapshot;
        for (const auto &listener : listeners)
            listener(completed);
        PlaybackEvent changed;
        changed.kind = PlaybackEventKind::SnapshotChanged;
        changed.snapshot = snapshot;
        for (const auto &listener : listeners)
            listener(changed);
        return {
            true,
            PlaybackSubmissionStatus::Queued,
            commandId,
            snapshot.generation,
            {},
            snapshot.stateVersion,
        };
    }

    std::uint64_t addEventListener(
        std::function<void(const PlaybackEvent &)> listener) override
    {
        std::lock_guard lock(m_mutex);
        const std::uint64_t id = ++m_nextListenerId;
        m_listeners.emplace(id, std::move(listener));
        return id;
    }

    void removeEventListener(std::uint64_t listenerId) override
    {
        std::lock_guard lock(m_mutex);
        m_listeners.erase(listenerId);
    }

    void markReady()
    {
        std::vector<std::function<void(const PlaybackEvent &)>>
            listeners;
        PlaybackSnapshot snapshot;
        {
            std::lock_guard lock(m_mutex);
            m_snapshot.state = PlaybackState::Playing;
            ++m_snapshot.stateVersion;
            snapshot = m_snapshot;
            for (const auto &[id, listener] : m_listeners) {
                (void)id;
                listeners.push_back(listener);
            }
        }
        PlaybackEvent changed;
        changed.kind = PlaybackEventKind::SnapshotChanged;
        changed.snapshot = snapshot;
        for (const auto &listener : listeners)
            listener(changed);
    }

private:
    mutable std::mutex m_mutex;
    PlaybackSnapshot m_snapshot;
    std::map<
        std::uint64_t,
        std::function<void(const PlaybackEvent &)>> m_listeners;
    std::uint64_t m_nextCommandId = 0;
    std::uint64_t m_nextListenerId = 0;
};

void processPlaybackUntil(
    FakePlaybackGateway &playback,
    const std::function<bool()> &done,
    std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done() && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(playback.processNext());
        if (playback.snapshot().state == PlaybackState::Preparing)
            playback.markReady();
        std::this_thread::sleep_for(2ms);
    }
    check(done(), "runtime HTTP request completes");
}

void testEndToEndInitialMediaAndConfirmation()
{
    WinsockRuntime winsock;
    std::string error;
    check(winsock.initialize(error), "runtime test Winsock initializes");

    FakePlaybackGateway playback;
    TestConfirmationPort port;
    std::atomic_int activationRequests{0};
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "Runtime Test",
            "uuid:runtime-test",
            0,
            true,
            [&] {
                ++activationRequests;
                return true;
            },
            [](const std::string &) {},
        });
    check(runtime.start(error), "DLNA runtime starts: " + error);
    check(runtime.localPort() != 0,
          "DLNA runtime allocates an HTTP port");

    std::string firstResponse;
    std::atomic_bool firstDone{false};
    const std::string uri = "http://example.test/runtime.mp4";
    std::jthread firstClient([&] {
        firstResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.10",
                setUriBody(uri, "192.0.2.10")));
        firstDone = true;
    });
    processPlaybackUntil(
        playback, [&] { return firstDone.load(); });
    firstClient.join();
    check(firstResponse.starts_with("HTTP/1.1 200 OK")
              && firstResponse.find("SetAVTransportURIResponse")
                  != std::string::npos,
          "initial SetURI completes only after media readiness");
    check(activationRequests.load() == 1,
          "initial Open requests Ui activation");

    std::string candidateResponse;
    std::atomic_bool candidateDone{false};
    std::jthread candidateClient([&] {
        candidateResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.20",
                setUriBody(
                    "http://example.test/candidate.mp4",
                    "192.0.2.20")));
        candidateDone = true;
    });
    check(port.waitForPrompt().has_value(),
          "candidate request reaches confirmation Port");
    port.decide(true);
    processPlaybackUntil(
        playback, [&] { return candidateDone.load(); });
    candidateClient.join();
    check(candidateResponse.starts_with("HTTP/1.1 200 OK"),
          "accepted candidate completes over HTTP");

    std::string rejectedResponse;
    std::atomic_bool rejectedDone{false};
    std::jthread rejectedClient([&] {
        rejectedResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.30",
                setUriBody(
                    "http://example.test/rejected.mp4",
                    "192.0.2.30")));
        rejectedDone = true;
    });
    check(port.waitForPrompt().has_value(),
          "rejected candidate reaches confirmation Port");
    port.decide(false);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!rejectedDone.load()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    rejectedClient.join();
    check(rejectedDone.load()
              && rejectedResponse.find("<errorCode>701</errorCode>")
                  != std::string::npos,
          "rejected candidate fails closed over HTTP");

    runtime.stop();
    check(runtime.localPort() == 0,
          "runtime stops its HTTP listener");

    check(runtime.start(error),
          "DLNA runtime can restart after stop: " + error);
    const std::string description = exchange(
        runtime.localPort(),
        "GET /description.xml HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    check(description.starts_with("HTTP/1.1 200 OK")
              && description.find("Runtime Test") != std::string::npos,
          "restarted runtime serves device description");
    runtime.stop();
}

void testMediaLoadFailureFailsHttp()
{
    FakePlaybackGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "Runtime Failure Test",
            "uuid:runtime-failure",
            0,
            true,
            [] { return true; },
            [](const std::string &) {},
        });
    std::string error;
    check(runtime.start(error),
          "failure runtime starts: " + error);

    std::string response;
    std::atomic_bool done{false};
    std::jthread client([&] {
        response = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.60",
                setUriBody(
                    "http://example.test/failure.mp4",
                    "192.0.2.60")));
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done.load()
           && playback.snapshot().state != PlaybackState::Preparing
           && std::chrono::steady_clock::now() < deadline) {
        static_cast<void>(playback.processNext());
        std::this_thread::sleep_for(2ms);
    }
    check(playback.snapshot().state == PlaybackState::Preparing,
          "failed Open reaches Preparing");
    playback.markReady(false);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    client.join();
    check(done.load()
              && response.find("<errorCode>501</errorCode>")
                  != std::string::npos,
          "load failure fails the held HTTP request");
    runtime.stop();
}

void testEarlyCommandCompletionDoesNotLoseWakeup()
{
    EarlyCompletionGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "Early Completion Test",
            "uuid:early-completion",
            0,
            true,
            [] { return true; },
            [](const std::string &) {},
        });
    std::string error;
    check(runtime.start(error),
          "early-completion runtime starts: " + error);

    std::string response;
    std::atomic_bool done{false};
    std::jthread client([&] {
        response = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.70",
                setUriBody(
                    "http://example.test/early.mp4",
                    "192.0.2.70")));
        done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!done.load()
           && playback.snapshot().state != PlaybackState::Preparing
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    check(playback.snapshot().state == PlaybackState::Preparing,
          "early completion reached preparing state");
    std::this_thread::sleep_for(20ms);
    playback.markReady();
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    client.join();
    check(done.load()
              && response.starts_with("HTTP/1.1 200 OK"),
          "command completion before submit return does not lose readiness");
    runtime.stop();
}

} // namespace

int main()
{
    testEndToEndInitialMediaAndConfirmation();
    testMediaLoadFailureFailsHttp();
    testEarlyCommandCompletionDoesNotLoseWakeup();
    std::cout << "DLNA protocol runtime tests passed\n";
    return 0;
}
