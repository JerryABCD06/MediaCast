// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "skeleton/SkeletonClient.h"

#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"
#include "ipc/PlaybackPipeGateway.h"
#include "ipc/ProtocolControlDispatcher.h"
#include "playback/LateBindingPlaybackGateway.h"
#include "playback/PlanningPlaybackGateway.h"
#include "protocols/dlna/playback/DlnaPlaybackPlanner.h"
#include "protocols/dlna/runtime/DlnaProtocolRuntime.h"
#include "skeleton/ProtocolDlnaConfirmationPort.h"
#include "supervisor/ProcessIdentity.h"

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
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace mcast::skeleton {
namespace {

std::string g_protocolLogPath;
std::mutex g_protocolLogMutex;

using namespace std::chrono_literals;
using mcast::ipc::ConnectionChannel;
using mcast::ipc::HelloExpectation;
using mcast::ipc::HelloInfo;
using mcast::ipc::Message;
using mcast::ipc::MessageType;
using mcast::ipc::PipeConnection;
using mcast::ipc::ProcessRole;

std::string narrow(const wchar_t *value)
{
    if (!value)
        return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(size - 1, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring widen(const std::string &value)
{
    if (value.empty())
        return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1)
        return {};
    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    return result;
}

void protocolLog(const std::string &text)
{
    std::lock_guard lock(g_protocolLogMutex);
    const std::filesystem::path path =
        g_protocolLogPath.empty()
        ? mcast::supervisor::currentExecutablePath()
              .parent_path() / L"MCast.Protocol.log"
        : std::filesystem::path(
              std::u8string(
                  reinterpret_cast<const char8_t *>(
                      g_protocolLogPath.data()),
                  g_protocolLogPath.size()));
    std::ofstream log(path, std::ios::app);
    if (!log)
        return;
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char timestamp[32]{};
    std::snprintf(
        timestamp, sizeof(timestamp),
        "%04u-%02u-%02u %02u:%02u:%02u.%03u ",
        now.wYear, now.wMonth, now.wDay,
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    log << timestamp << text << '\n';
}

std::optional<HelloInfo> makeSelfHello(const ClientOptions &options)
{
    const auto hash = mcast::supervisor::sha256File(
        mcast::supervisor::currentExecutablePath());
    if (!hash)
        return std::nullopt;
    return HelloInfo{
        GetCurrentProcessId(),
        mcast::supervisor::currentBuildId(),
        options.nonce,
        *hash,
        0x7,
    };
}

bool sendHello(
    PipeConnection &connection, ConnectionChannel channel,
    ProcessRole source, ProcessRole destination, std::uint64_t messageId,
    const ClientOptions &options, std::string &error)
{
    const auto hello = makeSelfHello(options);
    if (!hello) {
        error = "cannot hash current executable";
        return false;
    }
    auto message = mcast::ipc::makeHelloMessage(
        channel, source, destination, messageId, *hello, error);
    return message && connection.writeMessage(*message, 2000ms, error);
}

bool receiveHelloAck(
    PipeConnection &connection, ConnectionChannel channel,
    std::uint64_t expectedCorrelation, const ClientOptions &options,
    std::string &error)
{
    Message reply;
    if (!connection.readMessage(reply, 3000ms, error))
        return false;
    const auto ack = mcast::ipc::decodeHelloAckMessage(
        reply, expectedCorrelation, channel, error);
    if (!ack || !ack->accepted) {
        error = ack ? ack->reason : error;
        return false;
    }
    if (ack->instanceNonce != options.nonce) {
        error = "HelloAck nonce mismatch";
        return false;
    }
    return true;
}

std::unique_ptr<PipeConnection> connectControl(
    const ClientOptions &options, std::string &error)
{
    auto connection = mcast::ipc::PipeClient::connect(
        mcast::ipc::makeControlPipeName(widen(options.instance)),
        3000ms, error);
    if (!connection)
        return nullptr;
    if (connection->peerProcessId() != options.shellPid) {
        error = "control pipe server is not the expected Shell";
        return nullptr;
    }
    if (!sendHello(
            *connection, ConnectionChannel::Control, options.role,
            ProcessRole::Shell, 1, options, error)
        || !receiveHelloAck(
            *connection, ConnectionChannel::Control, 1, options, error))
        return nullptr;
    return connection;
}

void protocolCommandServer(
    const ClientOptions &options, std::atomic_bool &receivingEnabled,
    std::atomic_uint64_t &receivingStateVersion,
    std::atomic_bool &broadcasting,
    std::atomic_uint32_t &broadcastIntervalMs,
    const std::function<bool(
        bool, bool &, std::uint64_t &, std::string &)> &applyReceiving,
    const std::function<bool(
        bool, std::uint32_t, bool &, std::uint32_t &, std::string &)>
        &applyBroadcast,
    mcast::ipc::PipeListener &listener, std::stop_token stop)
{
    while (!stop.stop_requested()) {
        std::string error;
        auto connection = listener.accept(24h, error);
        if (!connection) {
            if (!error.empty() && error != "pipe accept timed out"
                && !stop.stop_requested())
                std::cerr << "Protocol command accept: " << error << '\n';
            continue;
        }
        if (connection->peerProcessId() != options.shellPid) {
            const std::string reason = "peer PID "
                + std::to_string(connection->peerProcessId())
                + " is not Shell PID " + std::to_string(options.shellPid);
            protocolLog("Protocol command rejected: " + reason);
            std::cerr << "Protocol command rejected: " << reason << '\n';
            continue;
        }

        Message request;
        if (!connection->readMessage(request, 2000ms, error)
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Command, request.header, error)) {
            protocolLog("Protocol command invalid: " + error);
            std::cerr << "Protocol command rejected: " << error << '\n';
            continue;
        }
        Message state;
        state.header.source = ProcessRole::Protocol;
        state.header.destination = ProcessRole::Shell;
        state.header.flags = mcast::ipc::MessageFlags::Response;
        state.header.messageId = 1;
        state.header.correlationId = request.header.messageId;
        state.header.sessionId = request.header.sessionId;
        std::string applyError;
        bool applied = true;

        if (request.header.type == MessageType::SetReceivingEnabled) {
            const mcast::ipc::PayloadReader reader(request.payload);
            const auto enabled = reader.getBool(
                static_cast<std::uint16_t>(
                    mcast::ipc::FieldId::ReceivingEnabled));
            if (!reader.valid() || !enabled) {
                protocolLog("Protocol receiving payload is invalid");
                continue;
            }
            bool effective = receivingEnabled.load();
            std::uint64_t stateVersion =
                receivingStateVersion.load();
            if (applyReceiving)
                applied = applyReceiving(
                    *enabled, effective, stateVersion, applyError);
            receivingEnabled = effective;
            receivingStateVersion = stateVersion;
            mcast::ipc::PayloadBuilder builder;
            builder.addBool(
                static_cast<std::uint16_t>(
                    mcast::ipc::FieldId::ReceivingEnabled),
                effective);
            state.header.type = MessageType::ReceivingState;
            state.header.stateVersion = stateVersion;
            state.payload = std::move(builder).finish().value_or(
                std::vector<std::uint8_t>{});
            protocolLog(
                applied
                    ? std::string("Protocol receiving enabled=")
                        + (effective ? "true" : "false")
                    : "Protocol receiving command failed: "
                        + applyError);
        } else if (
            request.header.type == MessageType::SetDlnaBroadcast) {
            const mcast::ipc::PayloadReader reader(request.payload);
            const auto enabled = reader.getBool(
                static_cast<std::uint16_t>(
                    mcast::ipc::FieldId::BroadcastEnabled));
            const auto interval = reader.getUInt64(
                static_cast<std::uint16_t>(
                    mcast::ipc::FieldId::BroadcastIntervalMs));
            if (!reader.valid() || !enabled || !interval
                || *interval < 1000 || *interval > 3600000) {
                protocolLog("Protocol broadcast payload is invalid");
                continue;
            }
            bool effective = broadcasting.load();
            std::uint32_t effectiveInterval =
                broadcastIntervalMs.load();
            if (applyBroadcast)
                applied = applyBroadcast(
                    *enabled,
                    static_cast<std::uint32_t>(*interval),
                    effective, effectiveInterval, applyError);
            broadcasting = effective;
            broadcastIntervalMs = effectiveInterval;
            mcast::ipc::PayloadBuilder builder;
            builder.addBool(
                static_cast<std::uint16_t>(
                    mcast::ipc::FieldId::BroadcastEnabled),
                effective);
            builder.addUInt64(
                static_cast<std::uint16_t>(
                    mcast::ipc::FieldId::BroadcastIntervalMs),
                effectiveInterval);
            state.header.type = MessageType::DlnaBroadcastState;
            state.header.stateVersion = 1;
            state.payload = std::move(builder).finish().value_or(
                std::vector<std::uint8_t>{});
            protocolLog(
                applied
                    ? std::string("Protocol broadcast enabled=")
                        + (effective ? "true" : "false")
                        + " interval_ms="
                        + std::to_string(effectiveInterval)
                    : "Protocol broadcast command failed: "
                        + applyError);
        } else {
            protocolLog("Protocol command type is unsupported");
            continue;
        }

        if (!connection->writeMessage(state, 2000ms, error)) {
            protocolLog("Protocol command acknowledgement failed: " + error);
            std::cerr << "Protocol command acknowledgement failed: "
                      << error << '\n';
        } else {
            protocolLog("Protocol command acknowledgement queued");
            // DisconnectNamedPipe discards data that the client has not read.
            // Keep this server handle alive until the client reads the
            // acknowledgement and closes its side.
            Message closeNotice;
            std::string closeError;
            const bool peerClosed = connection->readMessage(
                closeNotice, 1000ms, closeError);
            protocolLog(
                peerClosed
                    ? "Protocol command peer sent trailing data"
                    : "Protocol command close wait: " + closeError);
        }
    }
}

class ProtocolCommandServer
{
public:
    ProtocolCommandServer(
        const ClientOptions &options,
        std::atomic_bool &receivingEnabled,
        std::atomic_uint64_t &receivingStateVersion,
        std::atomic_bool &broadcasting,
        std::atomic_uint32_t &broadcastIntervalMs,
        std::function<bool(
            bool, bool &, std::uint64_t &, std::string &)>
            applyReceiving,
        std::function<bool(
            bool, std::uint32_t, bool &, std::uint32_t &,
            std::string &)> applyBroadcast)
        : m_listener(
            mcast::ipc::makeProtocolCommandPipeName(
                widen(options.instance)), 2)
        , m_options(options)
        , m_receivingEnabled(receivingEnabled)
        , m_receivingStateVersion(receivingStateVersion)
        , m_broadcasting(broadcasting)
        , m_broadcastIntervalMs(broadcastIntervalMs)
        , m_applyReceiving(std::move(applyReceiving))
        , m_applyBroadcast(std::move(applyBroadcast))
    {
        if (!m_listener.valid()) {
            std::cerr << "Protocol command listener failed\n";
            return;
        }
        m_thread = std::jthread([this](std::stop_token stop) {
            protocolCommandServer(
                m_options, m_receivingEnabled,
                m_receivingStateVersion,
                m_broadcasting, m_broadcastIntervalMs,
                m_applyReceiving, m_applyBroadcast,
                m_listener, stop);
        });
    }

    ~ProtocolCommandServer()
    {
        m_thread.request_stop();
        m_listener.stop();
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    mcast::ipc::PipeListener m_listener;
    const ClientOptions &m_options;
    std::atomic_bool &m_receivingEnabled;
    std::atomic_uint64_t &m_receivingStateVersion;
    std::atomic_bool &m_broadcasting;
    std::atomic_uint32_t &m_broadcastIntervalMs;
    std::function<bool(
        bool, bool &, std::uint64_t &, std::string &)>
        m_applyReceiving;
    std::function<bool(
        bool, std::uint32_t, bool &, std::uint32_t &,
        std::string &)> m_applyBroadcast;
    std::jthread m_thread;
};

std::unique_ptr<PipeConnection> protocolPlaybackServer(
    const ClientOptions &options,
    const mcast::ipc::PeerIdentityGrant &expectedUi,
    std::string &error,
    std::function<void(std::string &)> onListening = {},
    std::stop_token stop = {})
{
    mcast::ipc::PipeListener listener(
        mcast::ipc::makePlaybackPipeName(widen(options.instance)), 2);
    if (!listener.valid()) {
        error = "playback pipe listener failed";
        return nullptr;
    }
    std::mutex activeConnectionMutex;
    PipeConnection *activeConnection = nullptr;
    std::jthread cancelAccept;
    if (stop.stop_possible()) {
        cancelAccept = std::jthread(
            [&](std::stop_token internalStop) {
                while (!internalStop.stop_requested()
                       && !stop.stop_requested()) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds{10});
                }
                if (stop.stop_requested()) {
                    listener.stop();
                    std::lock_guard lock(activeConnectionMutex);
                    if (auto *connection = activeConnection)
                        connection->close();
                }
            });
    }
    if (onListening) {
        onListening(error);
        if (!error.empty())
            return nullptr;
    }

    auto connection = listener.accept(8000ms, error);
    {
        std::lock_guard lock(activeConnectionMutex);
        activeConnection = connection.get();
    }
    struct CancelAcceptGuard {
        std::mutex &mutex;
        PipeConnection *&active;
        std::jthread &thread;
        ~CancelAcceptGuard()
        {
            {
                std::lock_guard lock(mutex);
                active = nullptr;
            }
            thread.request_stop();
            if (thread.joinable())
                thread.join();
        }
    } cancelAcceptGuard{
        activeConnectionMutex, activeConnection, cancelAccept};
    if (stop.stop_requested()) {
        error = "playback listener cancelled";
        return nullptr;
    }
    if (!connection)
        return nullptr;

    Message helloMessage;
    if (!connection->readMessage(helloMessage, 3000ms, error))
        return nullptr;
    const auto hello = mcast::ipc::decodeHelloMessage(
        helloMessage, ConnectionChannel::Playback, error);
    if (!hello)
        return nullptr;

    if (connection->peerProcessId() != expectedUi.processId
        || hello->processId != expectedUi.processId) {
        error = "playback peer PID does not match Shell grant";
        return nullptr;
    }
    const HelloExpectation expectation{
        expectedUi.processId,
        expectedUi.buildId,
        expectedUi.instanceNonce,
        expectedUi.executableHash,
        0x7,
    };
    if (!mcast::ipc::validateHello(*hello, expectation, error))
        return nullptr;

    const mcast::ipc::HelloAckInfo ack{
        true,
        mcast::ipc::kProtocolVersionMajor,
        mcast::ipc::kProtocolVersionMinor,
        options.nonce,
        0x7,
        "accepted",
    };
    auto ackMessage = mcast::ipc::makeHelloAckMessage(
        ConnectionChannel::Playback, ProcessRole::Protocol,
        ProcessRole::Ui, 2, helloMessage.header.messageId, ack, error);
    if (!ackMessage
        || !connection->writeMessage(*ackMessage, 2000ms, error))
        return nullptr;

    Message snapshotRequest;
    if (!connection->readMessage(snapshotRequest, 3000ms, error)
        || snapshotRequest.header.type != MessageType::SessionSnapshotRequest
        || !mcast::ipc::validateMessageDirection(
            ConnectionChannel::Playback, snapshotRequest.header, error))
        return nullptr;

    Message snapshot;
    snapshot.header.type = MessageType::SessionSnapshot;
    snapshot.header.source = ProcessRole::Protocol;
    snapshot.header.destination = ProcessRole::Ui;
    snapshot.header.flags = mcast::ipc::MessageFlags::Response;
    snapshot.header.messageId = 3;
    snapshot.header.correlationId = snapshotRequest.header.messageId;
    snapshot.header.sessionId = snapshotRequest.header.sessionId;
    snapshot.header.stateVersion = 1;
    if (!connection->writeMessage(snapshot, 2000ms, error))
        return nullptr;

    std::cout << "PLAYBACK_SNAPSHOT_SERVED\n";
    return connection;
}

bool waitForUiOrPlaybackEnd(
    std::uint32_t processId,
    const mcast::playback::PlaybackPipeGateway &playback,
    std::stop_token stop, std::string &reason)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) {
        reason = "Ui process exited";
        return true;
    }
    while (!stop.stop_requested()) {
        if (!playback.connected()) {
            reason = "PlaybackPipe disconnected";
            CloseHandle(process);
            return true;
        }
        const DWORD result = WaitForSingleObject(process, 100);
        if (result == WAIT_OBJECT_0) {
            reason = "Ui exited";
            CloseHandle(process);
            return true;
        }
        if (result != WAIT_TIMEOUT) {
            CloseHandle(process);
            reason = "Ui process wait failed";
            return false;
        }
    }
    CloseHandle(process);
    return false;
}

bool uiPlaybackClient(
    const ClientOptions &options,
    const mcast::ipc::PeerIdentityGrant &expectedProtocol,
    std::string &error)
{
    auto connection = mcast::ipc::PipeClient::connect(
        mcast::ipc::makePlaybackPipeName(widen(options.instance)),
        5000ms, error);
    if (!connection)
        return false;
    if (connection->peerProcessId() != expectedProtocol.processId) {
        error = "Protocol PID does not match Shell grant";
        return false;
    }
    const auto protocolPath = mcast::supervisor::queryProcessPath(
        expectedProtocol.processId);
    const auto protocolHash = protocolPath
        ? mcast::supervisor::sha256File(*protocolPath)
        : std::nullopt;
    if (!protocolHash || *protocolHash != expectedProtocol.executableHash) {
        error = "Protocol executable hash does not match Shell grant";
        return false;
    }
    if (!sendHello(
            *connection, ConnectionChannel::Playback,
            ProcessRole::Ui, ProcessRole::Protocol, 3, options, error)
        || !receiveHelloAck(
            *connection, ConnectionChannel::Playback, 3, options, error))
        return false;

    Message request;
    request.header.type = MessageType::SessionSnapshotRequest;
    request.header.source = ProcessRole::Ui;
    request.header.destination = ProcessRole::Protocol;
    request.header.messageId = 4;
    request.header.sessionId = 1;
    if (!connection->writeMessage(request, 2000ms, error))
        return false;

    Message snapshot;
    if (!connection->readMessage(snapshot, 3000ms, error)
        || snapshot.header.type != MessageType::SessionSnapshot
        || snapshot.header.correlationId != request.header.messageId
        || snapshot.header.stateVersion == 0
        || !mcast::ipc::validateMessageDirection(
            ConnectionChannel::Playback, snapshot.header, error)) {
        if (error.empty())
            error = "invalid session snapshot";
        return false;
    }

    std::cout << "PLAYBACK_SNAPSHOT_OK\n";
    return true;
}

bool waitForUiExit(
    std::uint32_t processId, std::chrono::milliseconds timeout,
    std::string &error)
{
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) {
        error = "cannot open Ui process while waiting for reconnect";
        return false;
    }
    const DWORD result = WaitForSingleObject(
        process, static_cast<DWORD>(timeout.count()));
    CloseHandle(process);
    if (result != WAIT_OBJECT_0) {
        error = "Ui process did not exit before the next activation round";
        return false;
    }
    return true;
}

} // namespace

bool parseClientOptions(
    int argc, wchar_t **argv, ipc::ProcessRole role,
    ClientOptions &options, std::string &error)
{
    options = {};
    options.role = role;
    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        constexpr std::wstring_view instancePrefix = L"--instance=";
        constexpr std::wstring_view noncePrefix = L"--nonce=";
        constexpr std::wstring_view friendlyNamePrefix =
            L"--friendly-name=";
        constexpr std::wstring_view udnPrefix = L"--udn=";
        constexpr std::wstring_view shellPidPrefix = L"--shell-pid=";
        constexpr std::wstring_view activationPrefix = L"--activation-id=";
        constexpr std::wstring_view dlnaPortPrefix =
            L"--dlna-http-port=";
        constexpr std::wstring_view dlnaSsdpPortPrefix =
            L"--dlna-ssdp-port=";
        constexpr std::wstring_view protocolLogPrefix =
            L"--protocol-log=";
        constexpr std::wstring_view receivingPrefix =
            L"--receiving-enabled=";
        constexpr std::wstring_view broadcastPrefix =
            L"--broadcast-enabled=";
        constexpr std::wstring_view broadcastIntervalPrefix =
            L"--broadcast-interval-ms=";
        constexpr std::wstring_view requestRoundsPrefix =
            L"--request-ui-rounds=";
        if (argument.starts_with(instancePrefix)) {
            options.instance = narrow(
                argument.substr(instancePrefix.size()).c_str());
        } else if (argument.starts_with(noncePrefix)) {
            options.nonce = narrow(
                argument.substr(noncePrefix.size()).c_str());
        } else if (argument.starts_with(friendlyNamePrefix)) {
            options.friendlyName = narrow(
                argument.substr(friendlyNamePrefix.size()).c_str());
        } else if (argument.starts_with(udnPrefix)) {
            options.udn = narrow(
                argument.substr(udnPrefix.size()).c_str());
        } else if (argument.starts_with(shellPidPrefix)) {
            const std::wstring value =
                argument.substr(shellPidPrefix.size());
            try {
                const unsigned long parsed = std::stoul(value);
                if (parsed == 0
                    || parsed > std::numeric_limits<std::uint32_t>::max())
                    throw std::out_of_range("pid");
                options.shellPid = static_cast<std::uint32_t>(parsed);
            } catch (...) {
                error = "invalid --shell-pid";
                return false;
            }
        } else if (argument.starts_with(activationPrefix)) {
            const std::wstring value =
                argument.substr(activationPrefix.size());
            try {
                options.activationId = std::stoull(value);
            } catch (...) {
                error = "invalid --activation-id";
                return false;
            }
        } else if (argument.starts_with(dlnaPortPrefix)) {
            const std::wstring value =
                argument.substr(dlnaPortPrefix.size());
            try {
                std::size_t consumed = 0;
                const unsigned long parsed =
                    std::stoul(value, &consumed);
                if (consumed != value.size()
                    || parsed
                        > std::numeric_limits<std::uint16_t>::max())
                    throw std::out_of_range("port");
                options.dlnaHttpPort =
                    static_cast<std::uint16_t>(parsed);
            } catch (...) {
                error = "invalid --dlna-http-port";
                return false;
            }
        } else if (argument.starts_with(dlnaSsdpPortPrefix)) {
            const std::wstring value =
                argument.substr(dlnaSsdpPortPrefix.size());
            try {
                std::size_t consumed = 0;
                const unsigned long parsed =
                    std::stoul(value, &consumed);
                if (consumed != value.size()
                    || parsed
                        > std::numeric_limits<std::uint16_t>::max())
                    throw std::out_of_range("port");
                options.dlnaSsdpPort =
                    static_cast<std::uint16_t>(parsed);
            } catch (...) {
                error = "invalid --dlna-ssdp-port";
                return false;
            }
        } else if (argument.starts_with(protocolLogPrefix)) {
            options.protocolLogPath = narrow(
                argument.substr(protocolLogPrefix.size()).c_str());
        } else if (argument.starts_with(receivingPrefix)) {
            const std::wstring value =
                argument.substr(receivingPrefix.size());
            if (value == L"1" || value == L"true") {
                options.initialReceivingEnabled = true;
            } else if (value == L"0" || value == L"false") {
                options.initialReceivingEnabled = false;
            } else {
                error = "invalid --receiving-enabled";
                return false;
            }
        } else if (argument.starts_with(broadcastPrefix)) {
            const std::wstring value =
                argument.substr(broadcastPrefix.size());
            if (value == L"1" || value == L"true") {
                options.initialBroadcasting = true;
            } else if (value == L"0" || value == L"false") {
                options.initialBroadcasting = false;
            } else {
                error = "invalid --broadcast-enabled";
                return false;
            }
        } else if (argument.starts_with(broadcastIntervalPrefix)) {
            const std::wstring value =
                argument.substr(broadcastIntervalPrefix.size());
            try {
                std::size_t consumed = 0;
                const unsigned long parsed =
                    std::stoul(value, &consumed);
                if (consumed != value.size()
                    || parsed < 1000 || parsed > 3600000)
                    throw std::out_of_range("interval");
                options.broadcastIntervalMs =
                    static_cast<std::uint32_t>(parsed);
            } catch (...) {
                error = "invalid --broadcast-interval-ms";
                return false;
            }
        } else if (argument == L"--request-ui") {
            options.requestUi = true;
        } else if (argument == L"--disable-ssdp") {
            options.disableSsdp = true;
        } else if (argument == L"--persistent") {
            options.persistent = true;
        } else if (argument.starts_with(requestRoundsPrefix)) {
            const std::wstring value =
                argument.substr(requestRoundsPrefix.size());
            try {
                const unsigned long parsed = std::stoul(value);
                if (parsed == 0 || parsed > 8)
                    throw std::out_of_range("rounds");
                options.uiRequestRounds =
                    static_cast<std::uint32_t>(parsed);
            } catch (...) {
                error = "invalid --request-ui-rounds";
                return false;
            }
        } else {
            error = "unknown argument";
            return false;
        }
    }
    if (options.instance.empty()) {
        error = "--instance is required";
        return false;
    }
    if (options.nonce.empty()) {
        error = "--nonce is required";
        return false;
    }
    if (options.friendlyName.empty()
        || !options.udn.starts_with("uuid:")) {
        error = "invalid DLNA device identity";
        return false;
    }
    if (options.shellPid == 0) {
        error = "--shell-pid is required";
        return false;
    }
    if (options.role == ProcessRole::Ui && options.activationId == 0) {
        error = "--activation-id is required for Ui";
        return false;
    }
    if (options.uiRequestRounds != 1 && !options.requestUi) {
        error = "--request-ui-rounds requires --request-ui";
        return false;
    }
    return true;
}

int runClient(const ClientOptions &options)
{
    g_protocolLogPath = options.protocolLogPath;
    std::string error;
    auto control = connectControl(options, error);
    if (!control) {
        std::cerr << "control connection failed: " << error << '\n';
        return 2;
    }

    if (options.role == ProcessRole::Protocol) {
        auto dispatcher = std::make_unique<
            mcast::ipc::ProtocolControlDispatcher>(
            std::move(control), 2);
        if (!dispatcher->start(error)) {
            std::cerr << "control dispatcher failed: "
                      << error << '\n';
            return 3;
        }

        auto playbackGateway =
            std::make_shared<
                mcast::playback::LateBindingPlaybackGateway>();
        mcast::dlna::planning::DlnaPlaybackPlanner dlnaPlaybackPlanner(
            protocolLog);
        mcast::playback::PlanningPlaybackGateway planningPlayback(
            *playbackGateway,
            [&dlnaPlaybackPlanner](
                const mcast::playback::MediaRequest &media,
                std::string &) {
                return dlnaPlaybackPlanner.plan(media);
            });
        ProtocolDlnaConfirmationPort dlnaConfirmationPort(
            *dispatcher);
        const bool persistent = options.persistent;
        std::mutex activationMutex;
        std::mutex playbackMutex;
        std::mutex reconnectOwnerMutex;
        std::condition_variable reconnectOwnerChanged;
        std::shared_ptr<std::stop_source> reconnectOwner;
        std::shared_ptr<mcast::playback::PlaybackPipeGateway>
            activePlayback;
        std::jthread playbackWatcher;
        std::function<void()> requestEndCast;

        const auto stopReconnectOwner = [&] {
            std::shared_ptr<std::stop_source> owner;
            {
                std::lock_guard lock(reconnectOwnerMutex);
                owner = reconnectOwner;
            }
            if (owner)
                owner->request_stop();
            std::unique_lock lock(reconnectOwnerMutex);
            reconnectOwnerChanged.wait(
                lock, [&] { return !reconnectOwner; });
        };

        const auto activateUi =
            [&](mcast::ipc::UiActivationOutcome &activation,
                std::string &activationError) {
                std::lock_guard activationLock(activationMutex);
                mcast::ipc::UiActivationOutcome presence;
                if (!dispatcher->requestUiActivation(
                        presence, 3s, activationError, true)) {
                    return false;
                }
                if (!presence.uiPresent)
                    stopReconnectOwner();
                if (!dispatcher->requestUiActivation(
                        activation, 3s, activationError)) {
                    return false;
                }
                if (!activation.started)
                    return true;
                if (!activation.grant) {
                    activationError =
                        "UI activation has no identity grant";
                    return false;
                }
                if (playbackWatcher.joinable()) {
                    playbackWatcher.request_stop();
                    playbackWatcher.join();
                }
                auto connection = protocolPlaybackServer(
                    options, *activation.grant, activationError);
                if (!connection)
                    return false;
                auto playback =
                    std::make_shared<
                        mcast::playback::PlaybackPipeGateway>(
                        std::move(connection), protocolLog,
                        [&] {
                            if (requestEndCast)
                                requestEndCast();
                        });
                playbackGateway->detach(
                    "Ui playback connection replaced");
                if (!playbackGateway->attach(
                        playback, activationError))
                    return false;
                {
                    std::lock_guard playbackLock(playbackMutex);
                    activePlayback = playback;
                }
                const std::uint32_t processId =
                    activation.grant->processId;
                playbackWatcher = std::jthread(
                    [&, playback, processId,
                     grant = *activation.grant](
                        std::stop_token stop) mutable {
                        auto currentPlayback = playback;
                        while (!stop.stop_requested()) {
                            std::string waitReason;
                            const bool ended =
                                waitForUiOrPlaybackEnd(
                                    processId, *currentPlayback,
                                    stop, waitReason);
                            if (stop.stop_requested())
                                return;
                            if (!ended) {
                                protocolLog(
                                    "Ui playback wait failed: "
                                    + waitReason);
                                return;
                            }
                            bool current = false;
                            {
                                std::lock_guard playbackLock(
                                    playbackMutex);
                                current =
                                    activePlayback.get()
                                    == currentPlayback.get();
                                if (current)
                                    activePlayback.reset();
                            }
                            if (!current)
                                return;
                            playbackGateway->detach(waitReason);
                            if (waitReason != "PlaybackPipe disconnected")
                                break;

                            protocolLog(
                                "PlaybackPipe disconnected; "
                                "requesting Ui reconnect");
                            auto reconnectStop =
                                std::make_shared<std::stop_source>();
                            {
                                std::lock_guard lock(reconnectOwnerMutex);
                                reconnectOwner = reconnectStop;
                            }
                            struct ReconnectOwnerGuard {
                                std::mutex &mutex;
                                std::condition_variable &changed;
                                std::shared_ptr<std::stop_source> &owner;
                                const std::shared_ptr<std::stop_source>
                                    &expected;
                                ~ReconnectOwnerGuard()
                                {
                                    {
                                        std::lock_guard lock(mutex);
                                        if (owner == expected)
                                            owner.reset();
                                    }
                                    changed.notify_all();
                                }
                            } reconnectOwnerGuard{
                                reconnectOwnerMutex,
                                reconnectOwnerChanged,
                                reconnectOwner,
                                reconnectStop,
                            };
                            std::string reconnectError;
                            std::unique_ptr<PipeConnection>
                                replacementConnection;
                            for (int attempt = 0; attempt < 3; ++attempt) {
                                if (stop.stop_requested())
                                    return;
                                if (attempt != 0) {
                                    for (int slice = 0;
                                         slice < 50
                                         && !stop.stop_requested()
                                         && !reconnectStop
                                                 ->stop_requested();
                                         ++slice) {
                                        std::this_thread::sleep_for(
                                            std::chrono::milliseconds{10});
                                    }
                                }
                                reconnectError.clear();
                                replacementConnection =
                                    protocolPlaybackServer(
                                        options, grant, reconnectError,
                                        [&](std::string &listenError) {
                                            static_cast<void>(
                                                dispatcher
                                                    ->requestPlaybackReconnect(
                                                        listenError));
                                        },
                                        reconnectStop->get_token());
                                if (replacementConnection)
                                    break;
                                protocolLog(
                                    "Ui playback reconnect attempt "
                                    + std::to_string(attempt + 1)
                                    + " failed: " + reconnectError);
                            }
                            if (reconnectStop->stop_requested())
                                return;
                            if (!replacementConnection) {
                                protocolLog(
                                    "Ui playback reconnect failed: "
                                    + reconnectError);
                                break;
                            }
                            auto replacement =
                                std::make_shared<
                                    mcast::playback::
                                        PlaybackPipeGateway>(
                                    std::move(
                                        replacementConnection),
                                    protocolLog,
                                    [&] {
                                        if (requestEndCast)
                                            requestEndCast();
                                    });
                            if (!playbackGateway->attach(
                                    replacement, reconnectError)) {
                                protocolLog(
                                    "Ui playback reattach failed: "
                                    + reconnectError);
                                break;
                            }
                            {
                                std::lock_guard playbackLock(
                                    playbackMutex);
                                activePlayback = replacement;
                            }
                            currentPlayback = replacement;
                            protocolLog(
                                "PlaybackPipe reconnected");
                        }
                        if (persistent) {
                            std::string readyError;
                            if (!dispatcher->sendUiOpenReady(
                                    readyError)) {
                                protocolLog(
                                    "UiOpenReady after playback end "
                                    "failed: " + readyError);
                            }
                        }
                    });
                return true;
            };

        mcast::dlna::runtime::DlnaProtocolRuntime dlnaRuntime(
            planningPlayback, dlnaConfirmationPort,
            mcast::dlna::runtime::DlnaProtocolRuntimeOptions{
                options.friendlyName,
                options.udn,
                options.dlnaHttpPort,
                options.dlnaSsdpPort,
                false,
                !options.disableSsdp,
                options.initialReceivingEnabled,
                options.initialBroadcasting,
                std::chrono::milliseconds(
                    options.broadcastIntervalMs),
                [&] {
                    mcast::ipc::UiActivationOutcome activation;
                    std::string activationError;
                    const bool succeeded =
                        activateUi(activation, activationError);
                    if (!succeeded) {
                        protocolLog(
                            "Ui activation from DLNA failed: "
                            + activationError);
                    }
                    return succeeded;
                },
                protocolLog,
                [&](bool enabled, std::uint64_t stateVersion) {
                    std::string stateError;
                    if (!dispatcher->sendReceivingState(
                            enabled, stateVersion, stateError)) {
                        protocolLog(
                            "Receiving state update failed: "
                            + stateError);
                    }
                },
            });
        requestEndCast = [&] {
            protocolLog("Ui end-cast request received");
            std::string disconnectError;
            if (dlnaRuntime.requestDisconnect(disconnectError)) {
                protocolLog("Ui end-cast request accepted");
            } else {
                protocolLog(
                    "Ui end-cast request failed: "
                    + disconnectError);
            }
        };
        if (!dlnaRuntime.start(error)) {
            std::cerr << "DLNA runtime failed: " << error << '\n';
            return 4;
        }

        std::atomic_bool receivingEnabled =
            options.initialReceivingEnabled;
        std::atomic_uint64_t receivingStateVersion = 1;
        std::atomic_bool broadcasting =
            options.initialBroadcasting;
        std::atomic_uint32_t broadcastIntervalMs =
            options.broadcastIntervalMs;
        ProtocolCommandServer commandServer(
            options, receivingEnabled, receivingStateVersion,
            broadcasting, broadcastIntervalMs,
            [&](bool enabled, bool &effective,
                std::uint64_t &stateVersion,
                std::string &receivingError) {
                const bool applied =
                    dlnaRuntime.setReceivingEnabled(
                        enabled, receivingError);
                effective = dlnaRuntime.receivingEnabled();
                stateVersion = dlnaRuntime.receivingStateVersion();
                return applied;
            },
            [&](bool enabled, std::uint32_t interval,
                bool &effective, std::uint32_t &effectiveInterval,
                std::string &broadcastError) {
                try {
                    dlnaRuntime.setBroadcasting(enabled);
                    dlnaRuntime.setBroadcastInterval(
                        std::chrono::milliseconds(interval));
                    effective = enabled;
                    effectiveInterval = interval;
                    broadcastError.clear();
                    return true;
                } catch (const std::exception &exception) {
                    broadcastError = exception.what();
                    return false;
                } catch (...) {
                    broadcastError = "unknown broadcast error";
                    return false;
                }
            });
        std::atomic_bool heartbeatFailed = false;
        std::string heartbeatError;
        std::mutex heartbeatMutex;
        std::jthread heartbeat([&](std::stop_token stop) {
            while (!stop.stop_requested()) {
                std::string sendError;
                if (!dispatcher->sendHeartbeat(sendError)) {
                    {
                        std::lock_guard lock(heartbeatMutex);
                        heartbeatError = std::move(sendError);
                    }
                    heartbeatFailed = true;
                    return;
                }
                for (int slice = 0; slice < 10 && !stop.stop_requested();
                     ++slice)
                    std::this_thread::sleep_for(100ms);
            }
        });

        if (persistent) {
            mcast::ipc::UiActivationOutcome activation;
            if (options.requestUi) {
                std::string activationError;
                if (!activateUi(activation, activationError)) {
                    std::cerr << "UI activation failed: "
                              << activationError << '\n';
                    return 3;
                }
            }
            // Ready must be re-armed after every activation. Production always
            // starts Protocol persistently, and a closed Ui is reopened by a
            // later Shell/tray UiOpenRequest on this same control connection.
            if (!dispatcher->sendUiOpenReady(error)) {
                std::cerr << "UiOpenReady failed: "
                          << error << '\n';
                return 3;
            }

            while (dispatcher->connected()) {
                if (!dispatcher->waitForUiOpenRequest(24h, error)) {
                    if (heartbeatFailed) {
                        std::lock_guard lock(heartbeatMutex);
                        error = heartbeatError.empty()
                            ? "Protocol heartbeat failed"
                            : heartbeatError;
                    }
                    std::cerr << "UiOpenRequest wait failed: "
                              << error << '\n';
                    return 3;
                }
                std::string activationError;
                if (!activateUi(activation, activationError)) {
                    std::cerr << "UI activation failed: "
                              << activationError << '\n';
                    return 3;
                }
            }
        } else {
            bool waitForUiOpenBeforeRound = !options.requestUi;
            std::uint32_t round = 0;
            while (true) {
                if (waitForUiOpenBeforeRound
                    && (!dispatcher->sendUiOpenReady(error)
                        || !dispatcher->waitForUiOpenRequest(
                            24h, error))) {
                    std::cerr << "UiOpenRequest wait failed: "
                              << error << '\n';
                    return 3;
                }
                if (heartbeatFailed) {
                    std::lock_guard lock(heartbeatMutex);
                    error = heartbeatError.empty()
                        ? "Protocol heartbeat failed" : heartbeatError;
                    std::cerr << error << '\n';
                    return 3;
                }
                mcast::ipc::UiActivationOutcome activation;
                std::string activationError;
                if (!activateUi(activation, activationError)) {
                    std::cerr << "UI activation failed: "
                              << activationError << '\n';
                    return 3;
                }
                if (round + 1 < options.uiRequestRounds) {
                    if (!activation.started
                        || !waitForUiExit(
                            activation.grant->processId,
                            5000ms, error)) {
                        std::cerr << "Ui round barrier failed: "
                                  << error << '\n';
                        return 5;
                    }
                }
                if (++round >= options.uiRequestRounds)
                    break;
            }
        }
        dispatcher->stop();
    } else if (options.role == ProcessRole::Ui) {
        Message languageCatalog;
        if (!control->readMessage(languageCatalog, 3000ms, error)
            || !mcast::ipc::decodeLanguageCatalogMessage(
                languageCatalog, error)) {
            std::cerr << "language catalog read failed: "
                      << error << '\n';
            return 4;
        }
        Message settingsSnapshot;
        if (!control->readMessage(settingsSnapshot, 3000ms, error)
            || !mcast::ipc::decodeSettingsSnapshotMessage(
                settingsSnapshot, error)) {
            std::cerr << "settings snapshot read failed: "
                      << error << '\n';
            return 4;
        }
        Message grant;
        if (!control->readMessage(grant, 3000ms, error)) {
            std::cerr << "protocol grant read failed: " << error << '\n';
            return 4;
        }
        const auto protocolGrant =
            mcast::ipc::decodePeerIdentityGrantMessage(grant, error);
        if (!protocolGrant
            || protocolGrant->role != ProcessRole::Protocol) {
            std::cerr << "invalid protocol grant: " << error << '\n';
            return 4;
        }
        if (!uiPlaybackClient(options, *protocolGrant, error)) {
            std::cerr << "playback client failed: " << error << '\n';
            return 4;
        }

        Message firstFrame;
        firstFrame.header.type = MessageType::UiFirstFrame;
        firstFrame.header.source = ProcessRole::Ui;
        firstFrame.header.destination = ProcessRole::Shell;
        firstFrame.header.messageId = 5;
        firstFrame.header.correlationId = options.activationId;
        firstFrame.header.sessionId = 1;
        if (!control->writeMessage(firstFrame, 2000ms, error)) {
            std::cerr << "UiFirstFrame failed: " << error << '\n';
            return 5;
        }
    } else {
        error = "unsupported role";
        std::cerr << error << '\n';
        return 6;
    }

    std::cout << "role=" << static_cast<int>(options.role)
              << " pid=" << GetCurrentProcessId() << " ok\n";
    return 0;
}

} // namespace mcast::skeleton
