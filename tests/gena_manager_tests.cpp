// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "platform/windows/net/SocketRuntime.h"
#include "protocols/dlna/runtime/GenaManager.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace mcast::dlna;
using namespace mcast::dlna::core;
using namespace mcast::dlna::net;
using namespace mcast::dlna::runtime;
using namespace mcast::platform::net;
using namespace mcast::playback;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const std::string &message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::optional<std::string> header(
    const HttpResponse &response, std::string_view name)
{
    for (const auto &[key, value] : response.headers) {
        if (key == name)
            return value;
    }
    return std::nullopt;
}

HttpRequest subscribeRequest(
    std::uint64_t id, std::string callback)
{
    HttpRequest request;
    request.requestId = id;
    request.method = "SUBSCRIBE";
    request.target = "/AVTransport/event";
    request.version = "HTTP/1.1";
    request.headers.emplace_back(
        "callback", "<" + std::move(callback) + ">");
    request.headers.emplace_back("nt", "upnp:event");
    return request;
}

HttpRequest unsubscribeRequest(
    std::uint64_t id, std::string sid)
{
    HttpRequest request;
    request.requestId = id;
    request.method = "UNSUBSCRIBE";
    request.target = "/AVTransport/event";
    request.version = "HTTP/1.1";
    request.headers.emplace_back("sid", std::move(sid));
    return request;
}

struct Notification {
    HttpRequest request;
};

class NotificationSink
{
public:
    bool start(std::uint16_t port, std::string &error)
    {
        return m_server.listenLoopback(
            port,
            [this](const HttpRequest &request) {
                {
                    std::lock_guard lock(m_mutex);
                    m_requests.push_back(request);
                }
                m_changed.notify_all();
                HttpResponse response;
                response.status = 200;
                return response;
            },
            HttpServerOptions{}, error);
    }

    void run()
    {
        while (!m_stop.stop_requested()) {
            std::string error;
            if (!m_server.pollOnce(100ms, error))
                return;
        }
    }

    std::optional<HttpRequest> waitForNotify(
        std::size_t index, std::chrono::milliseconds timeout = 4s)
    {
        std::unique_lock lock(m_mutex);
        if (!m_changed.wait_for(lock, timeout, [&] {
                return m_requests.size() > index;
            })) {
            return std::nullopt;
        }
        return m_requests[index];
    }

    void stop()
    {
        m_stop.request_stop();
        m_server.wake();
        if (m_thread.joinable())
            m_thread.join();
        m_server.stop();
    }

    void startPoller()
    {
        m_thread = std::jthread(
            [this](std::stop_token) { run(); });
    }

private:
    HttpServer m_server;
    std::stop_source m_stop;
    std::jthread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_changed;
    std::vector<HttpRequest> m_requests;
};

std::uint16_t reserveUnusedPort()
{
    std::string error;
    SocketHandle socket;
    check(socket.createTcp(error), "reserve TCP socket: " + error);
    check(socket.bindLoopback(0, error), "reserve TCP port: " + error);
    return socket.localPort().value_or(0);
}

} // namespace

int main()
{
    std::string error;
    WinsockRuntime winsock;
    check(winsock.initialize(error), "Winsock initializes");

    std::mutex callbackMutex;
    std::vector<std::pair<std::size_t, SubscriptionChangeReason>>
        counts;
    int activities = 0;
    GenaManager manager(
        GenaManagerCallbacks{
            [&](std::size_t count, SubscriptionChangeReason reason) {
                std::lock_guard lock(callbackMutex);
                counts.emplace_back(count, reason);
            },
            [&] {
                std::lock_guard lock(callbackMutex);
                ++activities;
            },
            [](const std::string &message) {
                std::cerr << message << '\n';
            },
        },
        GenaManagerOptions{
            32,
            4,
            std::chrono::seconds{1800},
            100ms,
            500ms,
        });
    check(manager.start(error), "GENA manager starts: " + error);

    const std::uint16_t callbackPort = reserveUnusedPort();
    const auto subscription = manager.handleSubscribe(
        subscribeRequest(
            1, "http://127.0.0.1:" + std::to_string(callbackPort)
                + "/notify"));
    check(subscription.status == 200,
          "GENA SUBSCRIBE succeeds before callback server exists");
    const auto sid = header(subscription, "sid");
    check(sid.has_value() && sid->starts_with("uuid:"),
          "GENA SUBSCRIBE returns a SID");
    check(manager.subscriptionCount() == 1,
          "GENA manager tracks the subscription");

    std::this_thread::sleep_for(650ms);
    NotificationSink sink;
    check(sink.start(callbackPort, error),
          "callback server starts after transient failures: " + error);
    sink.startPoller();

    const auto initial = sink.waitForNotify(0, 5s);
    check(initial.has_value(), "GENA retries and reaches recovered callback");
    check(initial->method == "NOTIFY",
          "GENA recovery uses NOTIFY");
    check(initial->header("seq").value_or("") == "0",
          "connect failures do not consume the GENA sequence");
    check(initial->body.find("NO_MEDIA_PRESENT") != std::string::npos,
          "initial event carries the full current transport state");

    mcast::playback::PlaybackSnapshot playing;
    playing.state = mcast::playback::PlaybackState::Playing;
    playing.durationSeconds = 75.0;
    manager.updateState(playing);
    const auto next = sink.waitForNotify(1, 4s);
    check(next.has_value(), "state change produces the next GENA event");
    check(next->header("seq").value_or("") == "1",
          "successful delivery advances GENA sequence");
    check(next->body.find("PLAYING") != std::string::npos,
          "recovered event contains the latest complete state");

    const auto unsubscribed = manager.handleUnsubscribe(
        unsubscribeRequest(2, *sid));
    check(unsubscribed.status == 200,
          "GENA UNSUBSCRIBE succeeds");
    check(manager.subscriptionCount() == 0,
          "unsubscribe removes the subscription");
    {
        std::lock_guard lock(callbackMutex);
        check(counts.size() >= 2
                  && counts.back().first == 0
                  && counts.back().second
                      == SubscriptionChangeReason::ExplicitRemoval,
              "explicit unsubscribe reports an immediate disconnect");
        check(activities >= 1,
              "new subscription reports peer activity");
    }

    const auto invalid = manager.handleSubscribe(HttpRequest{
        3, {}, "SUBSCRIBE", "/AVTransport/event", "HTTP/1.1", {}, {}});
    check(invalid.status == 412,
          "SUBSCRIBE without callback is rejected");

    const auto hostnameSubscription = manager.handleSubscribe(
        subscribeRequest(5, "http://localhost:1/notify"));
    check(hostnameSubscription.status == 200,
          "GENA accepts a DNS callback host");
    const auto hostnameSid = header(hostnameSubscription, "sid");
    check(hostnameSid.has_value(),
          "DNS callback subscription receives a SID");
    check(manager.handleUnsubscribe(
              unsubscribeRequest(6, *hostnameSid)).status == 200,
          "DNS callback subscription can be removed");

    sink.stop();
    const std::uint16_t deadPort = reserveUnusedPort();
    const auto deadSubscription = manager.handleSubscribe(
        subscribeRequest(
            4, "http://127.0.0.1:" + std::to_string(deadPort)
                + "/notify"));
    check(deadSubscription.status == 200,
          "unreachable callback subscription is accepted initially");
    const auto deadDeadline = std::chrono::steady_clock::now() + 7s;
    bool unreachableReported = false;
    while (std::chrono::steady_clock::now() < deadDeadline) {
        {
            std::lock_guard lock(callbackMutex);
            unreachableReported = !counts.empty()
                && counts.back().first == 0
                && counts.back().second
                    == SubscriptionChangeReason::Unreachable;
        }
        if (unreachableReported)
            break;
        std::this_thread::sleep_for(20ms);
    }
    check(unreachableReported,
          "five connect failures report an unreachable subscription");
    check(manager.subscriptionCount() == 0,
          "unreachable subscription is eventually removed");

    manager.stop();
    std::cout << "GENA manager tests passed\n";
    return 0;
}
