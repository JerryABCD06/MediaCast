// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "QtFree/EventLoop.h"
#include "QtFree/Signal.h"
#include "ipc/Messages.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace mcast::ipc {
class PipeConnection;
}

namespace mcast::avalonia::qtfree {

struct ConfirmationEvent
{
    std::uint64_t requestId = 0;
    ipc::CastConfirmationRequestInfo request;
    std::string resolvedMediaTitle;
};

struct SettingsSnapshotEvent
{
    std::string json;
    std::uint64_t stateVersion = 0;
    std::string resolvedLanguage;
};

class UiConfirmationBridge
{
public:
    using TitleResolver =
        std::function<std::string(const std::string &)>;

    UiConfirmationBridge(
        std::shared_ptr<ipc::PipeConnection> control,
        EventLoop &loop,
        TitleResolver titleResolver = {});
    ~UiConfirmationBridge();

    UiConfirmationBridge(const UiConfirmationBridge &) = delete;
    UiConfirmationBridge &operator=(const UiConfirmationBridge &) = delete;

    void stop();
    bool sendFirstFrame(
        std::uint64_t activationId, std::string &error);
    bool submitSettings(
        std::string_view json, std::uint64_t baseVersion,
        std::string &error);
    bool submitDecision(std::uint64_t requestId, bool accepted);
    void dropConnectionForTest();

    Signal<const ConfirmationEvent &> requestReceived;
    Signal<std::uint64_t> cancelReceived;
    Signal<std::uint64_t> playbackReconnectRequested;
    Signal<const SettingsSnapshotEvent &> settingsReceived;
    Signal<const std::string &> settingsSubmitFailed;
    Signal<const std::string &> settingsUpdateRejected;
    Signal<const std::string &> settingsRuntimeApplyFailed;
    Signal<std::uint64_t, const std::string &> settingsCommitted;
    Signal<const std::string &> connectionLost;
    Signal<const std::string &> logMessage;

private:
    std::uint64_t nextMessageId() noexcept;
    void readLoop(std::stop_token stop);
    bool post(std::function<void()> task);

    std::shared_ptr<ipc::PipeConnection> m_control;
    EventLoop &m_loop;
    TitleResolver m_titleResolver;
    std::jthread m_reader;
    std::mutex m_pendingMutex;
    std::unordered_map<
        std::uint64_t, ipc::CastConfirmationRequestInfo>
        m_pending;
    std::atomic_uint64_t m_nextRequestId{100};
    std::atomic_bool m_alive{true};
    std::atomic_bool m_stopped{false};
};

} // namespace mcast::avalonia::qtfree

