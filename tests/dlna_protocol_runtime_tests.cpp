// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "playback/FakePlaybackGateway.h"
#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/UpnpXml.h"
#include "protocols/dlna/net/SsdpSocket.h"
#include "protocols/dlna/runtime/DlnaProtocolRuntime.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
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
    const std::string &address, const std::string &body,
    std::string action = "SetAVTransportURI")
{
    return "POST /AVTransport/control HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "User-Agent: Runtime-Test-" + address + "\r\n"
        "SOAPACTION: \"urn:schemas-upnp-org:service:AVTransport:1#"
            + action + "\"\r\n"
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

std::optional<std::string> responseHeader(
    const std::string &response, std::string_view name)
{
    std::size_t offset = 0;
    while (offset < response.size()) {
        const std::size_t end = response.find("\r\n", offset);
        if (end == std::string::npos)
            break;
        const std::string line = response.substr(offset, end - offset);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::transform(
                key.begin(), key.end(), key.begin(),
                [](unsigned char ch) {
                    return static_cast<char>(std::tolower(ch));
                });
            if (key == name) {
                std::size_t begin = colon + 1;
                while (begin < line.size()
                       && (line[begin] == ' ' || line[begin] == '\t')) {
                    ++begin;
                }
                return line.substr(begin);
            }
        }
        offset = end + 2;
    }
    return std::nullopt;
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
            1900,
            true,
            true,
            true,
            true,
            10s,
            [&] {
                ++activationRequests;
                return true;
            },
            [](const std::string &) {},
        });
    const bool started = runtime.start(error);
    if (!started)
        std::cerr << "runtime start error: " << error << '\n';
    check(started, "DLNA runtime starts: " + error);
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
            1900,
            true,
            true,
            true,
            true,
            10s,
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

void testNewOwnerCancelsPendingOpen()
{
    WinsockRuntime winsock;
    std::string error;
    check(winsock.initialize(error), "new-owner test Winsock initializes");

    FakePlaybackGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "New Owner Test",
            "uuid:new-owner-test",
            0,
            1900,
            true,
            true,
            true,
            true,
            10s,
            [] { return true; },
            [](const std::string &) {},
        });
    check(runtime.start(error),
          "new-owner runtime starts: " + error);

    std::string initialResponse;
    std::atomic_bool initialDone{false};
    std::jthread initialClient([&] {
        initialResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.90",
                setUriBody(
                    "http://example.test/old-owner-current.mp4",
                    "192.0.2.90")));
        initialDone = true;
    });
    processPlaybackUntil(
        playback, [&] { return initialDone.load(); });
    initialClient.join();
    check(initialResponse.starts_with("HTTP/1.1 200 OK"),
          "old owner establishes the active session");

    std::string pendingResponse;
    std::atomic_bool pendingDone{false};
    std::jthread pendingClient([&] {
        pendingResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.90",
                setUriBody(
                    "http://example.test/old-owner-pending.mp4",
                    "192.0.2.90")));
        pendingDone = true;
    });
    const auto pendingDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (!pendingDone.load()
           && playback.snapshot().state == PlaybackState::NoMedia
           && std::chrono::steady_clock::now() < pendingDeadline) {
        static_cast<void>(playback.processNext());
        std::this_thread::sleep_for(2ms);
    }
    while (!pendingDone.load()
           && playback.snapshot().current.uri
               != "http://example.test/old-owner-pending.mp4"
           && std::chrono::steady_clock::now() < pendingDeadline) {
        static_cast<void>(playback.processNext());
        std::this_thread::sleep_for(2ms);
    }
    check(!pendingDone.load()
              && playback.snapshot().state
                  == PlaybackState::Preparing,
          "old owner Open remains awaiting readiness");

    std::string secondResponse;
    std::atomic_bool secondDone{false};
    std::jthread secondClient([&] {
        secondResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.91",
                setUriBody(
                    "http://example.test/new-owner.mp4",
                    "192.0.2.91")));
        secondDone = true;
    });
    check(port.waitForPrompt().has_value(),
          "new owner reaches confirmation");
    port.decide(true);

    const auto secondDeadline =
        std::chrono::steady_clock::now() + 3s;
    while (!secondDone.load()
           && std::chrono::steady_clock::now() < secondDeadline) {
        static_cast<void>(playback.processNext());
        if (playback.snapshot().state == PlaybackState::Preparing)
            playback.markReady();
        std::this_thread::sleep_for(2ms);
    }
    pendingClient.join();
    secondClient.join();

    check(!pendingResponse.starts_with("HTTP/1.1 200 OK"),
          "old owner's pending Open receives a definite failure");
    check(secondResponse.starts_with("HTTP/1.1 200 OK")
              && playback.snapshot().current.uri
                  == "http://example.test/new-owner.mp4",
          "new owner replaces the stale pending Open");
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
            1900,
            true,
            true,
            true,
            true,
            10s,
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

void testReceivingSurfaceToggle()
{
    FakePlaybackGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "Receiving Toggle Test",
            "uuid:receiving-toggle",
            0,
            1900,
            true,
            true,
            true,
            true,
            10s,
            [] { return true; },
            [](const std::string &) {},
        });
    std::string error;
    check(runtime.start(error),
          "receiving toggle runtime starts: " + error);
    check(runtime.localPort() != 0,
          "receiving toggle starts HTTP");
    const auto initialStateVersion =
        runtime.receivingStateVersion();

    PlaybackCommand open;
    open.type = PlaybackCommandType::Open;
    open.media.uri = "http://example.test/active.mp4";
    check(playback.submit(open).queued,
          "receiving toggle opens active media");
    playback.processAll();
    playback.markReady();

    check(runtime.setReceivingEnabled(false, error),
          "receiving can pause: " + error);
    check(runtime.receivingStateVersion() > initialStateVersion,
          "pausing advances the receiving state version");
    check(runtime.localPort() == 0,
          "paused receiving closes HTTP listener");
    check(!runtime.setReceivingEnabled(true, error),
          "receiving rejects resume while pause is finishing");
    check(runtime.localPort() == 0,
          "rejected resume leaves HTTP listener closed");

    playback.processAll();
    check(playback.snapshot().state == PlaybackState::NoMedia,
          "pausing receiving clears active playback");
    check(runtime.setReceivingEnabled(true, error),
          "receiving can resume after pause finishes: " + error);
    check(runtime.receivingStateVersion() > initialStateVersion,
          "resuming preserves a monotonic receiving state version");
    check(runtime.localPort() != 0,
          "resumed receiving restarts HTTP listener");
    const std::string response = exchange(
        runtime.localPort(),
        "GET /description.xml HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Connection: close\r\n\r\n");
    check(response.starts_with("HTTP/1.1 200 OK"),
          "resumed receiving serves HTTP again");
    runtime.stop();
}

void testReceivingStartsPaused()
{
    FakePlaybackGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "Paused Start Test",
            "uuid:paused-start",
            0,
            1900,
            true,
            true,
            false,
            true,
            10s,
            [] { return true; },
            [](const std::string &) {},
        });
    std::string error;
    check(runtime.start(error),
          "runtime starts with receiving disabled: " + error);
    check(runtime.localPort() == 0,
          "persisted pause does not open the HTTP surface");
    check(runtime.setReceivingEnabled(true, error),
          "paused runtime can resume: " + error);
    check(runtime.localPort() != 0,
          "resuming a paused runtime opens HTTP");
    runtime.stop();
    check(runtime.start(error),
          "runtime can restart with receiving still disabled: " + error);
    check(runtime.localPort() == 0,
          "restarted runtime preserves the paused initial state");
    check(runtime.setReceivingEnabled(true, error),
          "restarted runtime resumes after reset: " + error);
    std::string response;
    std::atomic_bool done{false};
    std::jthread client([&] {
        response = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.90",
                setUriBody(
                    "http://example.test/after-restart.mp4",
                    "192.0.2.90")));
        done = true;
    });
    processPlaybackUntil(
        playback, [&] { return done.load(); });
    client.join();
    check(response.starts_with("HTTP/1.1 200 OK"),
          "restarted paused runtime accepts new media without stale end");
    runtime.stop();
}

void testPauseCancelsPendingClaim()
{
    FakePlaybackGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "Paused Claim Test",
            "uuid:paused-claim",
            0,
            1900,
            true,
            true,
            true,
            true,
            10s,
            [] { return true; },
            [](const std::string &) {},
        });
    std::string error;
    check(runtime.start(error),
          "claim pause runtime starts: " + error);

    std::atomic_bool firstDone{false};
    std::jthread firstClient([&] {
        static_cast<void>(exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.50",
                setUriBody(
                    "http://example.test/owner.mp4",
                    "192.0.2.50"))));
        firstDone = true;
    });
    processPlaybackUntil(
        playback, [&] { return firstDone.load(); });
    firstClient.join();

    std::atomic_bool candidateDone{false};
    std::jthread candidateClient([&] {
        static_cast<void>(exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.60",
                setUriBody(
                    "http://example.test/candidate.mp4",
                    "192.0.2.60"))));
        candidateDone = true;
    });
    check(port.waitForPrompt().has_value(),
          "pause test reaches confirmation port");

    check(runtime.setReceivingEnabled(false, error),
          "pausing cancels a pending claim: " + error);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!candidateDone.load()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    candidateClient.join();
    check(candidateDone.load(),
          "pending claim connection closes when receiving pauses");

    playback.processAll();
    check(playback.snapshot().state == PlaybackState::Playing
              && playback.snapshot().current.uri
                  == "http://example.test/owner.mp4",
          "pausing cancels the candidate without replacing current media");
    port.decide(true);
    std::this_thread::sleep_for(20ms);
    check(playback.snapshot().current.uri
              == "http://example.test/owner.mp4",
          "late confirmation cannot revive a paused session");
    runtime.stop();
}

void testSubscriptionControlsPeerConnection()
{
    FakePlaybackGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "GENA Runtime Test",
            "uuid:gena-runtime",
            0,
            1900,
            true,
            true,
            true,
            true,
            10s,
            [] { return true; },
            [](const std::string &) {},
        });
    std::string error;
    check(runtime.start(error),
          "GENA runtime starts: " + error);

    const std::string subscribe =
        "SUBSCRIBE /AVTransport/event HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Callback: <http://127.0.0.1:1/notify>\r\n"
        "NT: upnp:event\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const std::string subscribed =
        exchange(runtime.localPort(), subscribe);
    check(subscribed.starts_with("HTTP/1.1 200 OK"),
          "runtime accepts SUBSCRIBE");
    const auto sid = responseHeader(subscribed, "sid");
    check(sid.has_value(), "runtime returns a GENA SID");

    playback.processAll();
    check(playback.snapshot().peerConnected,
          "a GENA subscription marks the peer connected");

    std::string mediaResponse;
    std::atomic_bool mediaDone{false};
    std::jthread mediaClient([&] {
        mediaResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.80",
                setUriBody(
                    "http://example.test/current.mp4",
                    "192.0.2.80")));
        mediaDone = true;
    });
    processPlaybackUntil(
        playback, [&] { return mediaDone.load(); });
    mediaClient.join();
    check(mediaResponse.starts_with("HTTP/1.1 200 OK")
              && playback.snapshot().state == PlaybackState::Playing,
          "connected peer can start the current media");

    check(runtime.setReceivingEnabled(false, error),
          "pausing with a connected peer succeeds: " + error);
    check(runtime.localPort() != 0,
          "pausing keeps HTTP alive for the current session");
    check(playback.snapshot().state == PlaybackState::Playing,
          "pausing does not stop the current media");
    const std::string blockedMedia = exchange(
        runtime.localPort(),
        httpRequest(
            "192.0.2.81",
            setUriBody(
                "http://example.test/blocked.mp4",
                "192.0.2.81")));
    check(blockedMedia.find("<errorCode>705</errorCode>")
              != std::string::npos,
          "paused receiving rejects a new media session");
    check(runtime.setReceivingEnabled(true, error),
          "resuming with a kept surface succeeds: " + error);
    check(playback.snapshot().state == PlaybackState::Playing,
          "resuming does not disturb the current media");
    check(runtime.requestDisconnect(error),
          "an explicit UI disconnect requests a session end: " + error);
    check(runtime.localPort() != 0,
          "explicit disconnect keeps HTTP/GENA alive while EndSession drains");
    playback.processAll();
    check(playback.snapshot().state == PlaybackState::NoMedia
              && !playback.snapshot().peerConnected,
          "explicit disconnect clears media and the peer session");
    const auto teardownDeadline =
        std::chrono::steady_clock::now() + 2s;
    while (runtime.localPort() != 0
           && std::chrono::steady_clock::now() < teardownDeadline) {
        std::this_thread::sleep_for(20ms);
    }
    check(runtime.localPort() == 0,
          "explicit disconnect stops the network only after the final drain");
    const auto restartDeadline =
        std::chrono::steady_clock::now() + 4s;
    while (runtime.localPort() == 0
           && std::chrono::steady_clock::now() < restartDeadline) {
        std::this_thread::sleep_for(20ms);
    }
    check(runtime.localPort() != 0,
          "DLNA service comes back after the disconnect bounce");
    const std::string transportInfo = exchange(
        runtime.localPort(),
        httpRequest(
            "192.0.2.80",
            soapBody(
                "GetTransportInfo",
                "<InstanceID>0</InstanceID>"),
            "GetTransportInfo"));
    playback.processAll();
    check(transportInfo.starts_with("HTTP/1.1 200 OK")
              && !playback.snapshot().peerConnected,
          "queries after disconnect do not revive the peer session");

    std::atomic_bool reconnectDone{false};
    std::jthread reconnectClient([&] {
        static_cast<void>(exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.80",
                setUriBody(
                    "http://example.test/next-session.mp4",
                    "192.0.2.80"))));
        reconnectDone = true;
    });
    processPlaybackUntil(
        playback, [&] { return reconnectDone.load(); });
    reconnectClient.join();
    check(playback.snapshot().state == PlaybackState::Playing
              && playback.snapshot().peerConnected,
          "a new SetAVTransportURI starts a new peer session");

    const std::string unsubscribe =
        "UNSUBSCRIBE /AVTransport/event HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "SID: " + *sid + "\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n";
    const std::string unsubscribed =
        exchange(runtime.localPort(), unsubscribe);
    check(
        unsubscribed.starts_with("HTTP/1.1 412"),
        "network bounce invalidates the old GENA subscription; response="
            + unsubscribed.substr(
                  0, std::min<std::size_t>(unsubscribed.size(), 64)));

    playback.processAll();
    check(playback.snapshot().peerConnected,
          "the stale old SID does not close the newly started session");

    std::string stopResponse;
    std::atomic_bool stopDone{false};
    std::jthread stopClient([&] {
        stopResponse = exchange(
            runtime.localPort(),
            httpRequest(
                "192.0.2.80",
                soapBody(
                    "Stop",
                    "<InstanceID>0</InstanceID>"),
                "Stop"));
        stopDone = true;
    });
    processPlaybackUntil(
        playback, [&] { return stopDone.load(); });
    stopClient.join();
    const bool stopSucceeded =
        stopResponse.starts_with("HTTP/1.1 200 OK")
        && playback.snapshot().state == PlaybackState::Stopped
        && playback.snapshot().peerConnected;
    if (!stopSucceeded) {
        std::cerr << "stop response: "
                  << stopResponse.substr(
                         0, std::min<std::size_t>(
                                stopResponse.size(), 800))
                  << " state="
                  << static_cast<int>(playback.snapshot().state)
                  << " peer=" << playback.snapshot().peerConnected
                  << '\n';
    }
    check(stopSucceeded,
          "control-point Stop enters the idle grace window");

    const auto idleDeadline =
        std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < idleDeadline
           && (playback.snapshot().peerConnected
               || playback.snapshot().state != PlaybackState::NoMedia)) {
        playback.processAll();
        std::this_thread::sleep_for(20ms);
    }
    check(!playback.snapshot().peerConnected
              && playback.snapshot().state == PlaybackState::NoMedia,
          "an idle control point after Stop ends the session");
    runtime.stop();
}

void testEphemeralSsdpSurface()
{
    WinsockRuntime winsock;
    std::string error;
    check(winsock.initialize(error),
          "ephemeral SSDP test Winsock initializes");

    FakePlaybackGateway playback;
    TestConfirmationPort port;
    DlnaProtocolRuntime runtime(
        playback, port,
        DlnaProtocolRuntimeOptions{
            "SSDP Runtime Test",
            "uuid:ssdp-runtime",
            0,
            0,
            false,
            true,
            true,
            true,
            10s,
            [] { return true; },
            [](const std::string &) {},
        });
    check(runtime.start(error),
          "ephemeral SSDP runtime starts: " + error);
    const std::uint16_t httpPort = runtime.localPort();
    const std::uint16_t ssdpPort = runtime.localSsdpPort();
    check(httpPort != 0 && ssdpPort != 0,
          "runtime exposes dynamic HTTP and SSDP ports");

    mcast::dlna::net::SsdpSocket client;
    mcast::platform::net::SocketEventLoop loop;
    check(client.open(0, error),
          "ephemeral SSDP client opens: " + error);
    check(loop.initialize(error),
          "ephemeral SSDP event loop initializes: " + error);
    check(loop.add(client.handle(), FD_READ, error),
          "ephemeral SSDP client registers: " + error);

    const std::string search =
        "M-SEARCH * HTTP/1.1\r\n"
        "HOST: 239.255.255.250:1900\r\n"
        "MAN: \"ssdp:discover\"\r\n"
        "MX: 1\r\n"
        "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n"
        "\r\n";
    const std::span<const std::uint8_t> bytes(
        reinterpret_cast<const std::uint8_t *>(search.data()),
        search.size());
    check(client.sendDatagram(
              bytes, "127.0.0.1", ssdpPort, error)
              == mcast::dlna::net::SsdpSendStatus::Sent,
          "ephemeral SSDP M-SEARCH sends: " + error);

    std::string response;
    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    while (response.empty()
           && std::chrono::steady_clock::now() < deadline) {
        std::vector<mcast::platform::net::SocketActivation>
            activations;
        bool woken = false;
        if (!loop.poll(100ms, activations, woken, error))
            break;
        while (true) {
            mcast::dlna::net::SsdpDatagram datagram;
            const auto status =
                client.receiveDatagram(datagram, error);
            if (status
                == mcast::dlna::net::SsdpReceiveStatus::WouldBlock)
                break;
            if (status
                != mcast::dlna::net::SsdpReceiveStatus::Received)
                break;
            response.assign(
                datagram.bytes.begin(), datagram.bytes.end());
            break;
        }
    }
    check(response.starts_with("HTTP/1.1 200 OK"),
          "ephemeral SSDP surface responds to M-SEARCH: " + response);
    check(response.find(
              "ST: urn:schemas-upnp-org:device:MediaRenderer:1\r\n")
              != std::string::npos,
          "ephemeral SSDP response preserves the search target");
    check(response.find(
              "USN: uuid:ssdp-runtime::"
              "urn:schemas-upnp-org:device:MediaRenderer:1\r\n")
              != std::string::npos,
          "ephemeral SSDP response uses the runtime UDN");
    check(response.find(
              "LOCATION: http://") != std::string::npos
              && response.find(
                  ":" + std::to_string(httpPort)
                      + "/description.xml")
                  != std::string::npos,
          "ephemeral SSDP response points at the runtime HTTP port");

    loop.remove(client.handle());
    client.close();
    runtime.stop();
}

} // namespace

int main()
{
    std::cerr << "[ RUN ] initial media and confirmation\n";
    testEndToEndInitialMediaAndConfirmation();
    std::cerr << "[ RUN ] media load failure\n";
    testMediaLoadFailureFailsHttp();
    std::cerr << "[ RUN ] new owner cancels pending open\n";
    testNewOwnerCancelsPendingOpen();
    std::cerr << "[ RUN ] early command completion\n";
    testEarlyCommandCompletionDoesNotLoseWakeup();
    std::cerr << "[ RUN ] receiving surface toggle\n";
    testReceivingSurfaceToggle();
    std::cerr << "[ RUN ] receiving starts paused\n";
    testReceivingStartsPaused();
    std::cerr << "[ RUN ] pause cancels pending claim\n";
    testPauseCancelsPendingClaim();
    std::cerr << "[ RUN ] subscription controls peer\n";
    testSubscriptionControlsPeerConnection();
    std::cerr << "[ RUN ] ephemeral SSDP surface\n";
    testEphemeralSsdpSurface();
    std::cout << "DLNA protocol runtime tests passed\n";
    return 0;
}
