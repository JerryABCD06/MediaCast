// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "skeleton/SkeletonClient.h"

#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"
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
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace mcast::skeleton {
namespace {

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
    const auto path = mcast::supervisor::currentExecutablePath()
        .parent_path() / L"MCast.Protocol.log";
    std::ofstream log(path, std::ios::app);
    if (log)
        log << text << '\n';
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

bool requestUi(
    PipeConnection &control, mcast::ipc::PeerIdentityGrant &uiGrant,
    std::uint64_t requestId, bool &started, std::string &error)
{
    Message request;
    request.header.type = MessageType::UiActivateRequest;
    request.header.source = ProcessRole::Protocol;
    request.header.destination = ProcessRole::Shell;
    request.header.messageId = requestId;
    request.header.sessionId = 1;
    if (!control.writeMessage(request, 2000ms, error))
        return false;

    Message decision;
    if (!control.readMessage(decision, 3000ms, error))
        return false;
    if (decision.header.sessionId != request.header.sessionId) {
        error = "UI activation decision session mismatch";
        return false;
    }
    const auto decodedDecision =
        mcast::ipc::decodeUiActivateDecisionMessage(
            decision, request.header.messageId, error);
    if (!decodedDecision)
        return false;
    started = decodedDecision->started;
    if (!started)
        return true;

    Message grant;
    if (!control.readMessage(grant, 3000ms, error))
        return false;
    const auto decoded = mcast::ipc::decodePeerIdentityGrantMessage(
        grant, error);
    if (!decoded || decoded->role != ProcessRole::Ui
        || decoded->activationId != decodedDecision->activationId) {
        error = "invalid UI identity grant";
        return false;
    }
    uiGrant = *decoded;
    return true;
}

bool sendUiOpenReady(
    PipeConnection &control, std::uint64_t messageId, std::string &error)
{
    Message ready;
    ready.header.type = MessageType::UiOpenReady;
    ready.header.source = ProcessRole::Protocol;
    ready.header.destination = ProcessRole::Shell;
    ready.header.messageId = messageId;
    ready.header.sessionId = 1;
    return control.writeMessage(ready, 2000ms, error);
}

bool waitForUiOpenCommand(PipeConnection &control, std::string &error)
{
    Message command;
    if (!control.readMessage(command, 24h, error))
        return false;
    if (command.header.type != MessageType::UiOpenRequest
        || !mcast::ipc::validateMessageDirection(
            ConnectionChannel::Control, command.header, error)) {
        if (error.empty())
            error = "invalid UiOpenRequest";
        return false;
    }
    return true;
}

void protocolCommandServer(
    const ClientOptions &options, std::atomic_bool &receivingEnabled,
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
            || request.header.type != MessageType::SetReceivingEnabled
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Command, request.header, error)) {
            protocolLog("Protocol command invalid: " + error);
            std::cerr << "Protocol command rejected: " << error << '\n';
            continue;
        }
        const mcast::ipc::PayloadReader reader(request.payload);
        const auto enabled = reader.getBool(
            static_cast<std::uint16_t>(mcast::ipc::FieldId::ReceivingEnabled));
        if (!reader.valid() || !enabled) {
            protocolLog("Protocol command payload is invalid");
            std::cerr << "Protocol command payload is invalid\n";
            continue;
        }
        receivingEnabled = *enabled;
        protocolLog(
            std::string("Protocol receiving enabled=")
            + (*enabled ? "true" : "false"));

        mcast::ipc::PayloadBuilder builder;
        builder.addBool(
            static_cast<std::uint16_t>(mcast::ipc::FieldId::ReceivingEnabled),
            *enabled);
        Message state;
        state.header.type = MessageType::ReceivingState;
        state.header.source = ProcessRole::Protocol;
        state.header.destination = ProcessRole::Shell;
        state.header.flags = mcast::ipc::MessageFlags::Response;
        state.header.messageId = 1;
        state.header.correlationId = request.header.messageId;
        state.header.sessionId = request.header.sessionId;
        state.payload = std::move(builder).finish().value_or(
            std::vector<std::uint8_t>{});
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
        std::atomic_bool &receivingEnabled)
        : m_listener(
            mcast::ipc::makeProtocolCommandPipeName(
                widen(options.instance)), 2)
        , m_options(options)
        , m_receivingEnabled(receivingEnabled)
    {
        if (!m_listener.valid()) {
            std::cerr << "Protocol command listener failed\n";
            return;
        }
        m_thread = std::jthread([this](std::stop_token stop) {
            protocolCommandServer(
                m_options, m_receivingEnabled, m_listener, stop);
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
    std::jthread m_thread;
};

bool protocolPlaybackServer(
    const ClientOptions &options,
    const mcast::ipc::PeerIdentityGrant &expectedUi,
    std::string &error)
{
    mcast::ipc::PipeListener listener(
        mcast::ipc::makePlaybackPipeName(widen(options.instance)), 2);
    if (!listener.valid()) {
        error = "playback pipe listener failed";
        return false;
    }

    auto connection = listener.accept(8000ms, error);
    if (!connection)
        return false;

    Message helloMessage;
    if (!connection->readMessage(helloMessage, 3000ms, error))
        return false;
    const auto hello = mcast::ipc::decodeHelloMessage(
        helloMessage, ConnectionChannel::Playback, error);
    if (!hello)
        return false;

    if (connection->peerProcessId() != expectedUi.processId
        || hello->processId != expectedUi.processId) {
        error = "playback peer PID does not match Shell grant";
        return false;
    }
    const HelloExpectation expectation{
        expectedUi.processId,
        expectedUi.buildId,
        expectedUi.instanceNonce,
        expectedUi.executableHash,
        0x7,
    };
    if (!mcast::ipc::validateHello(*hello, expectation, error))
        return false;

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
        return false;

    Message snapshotRequest;
    if (!connection->readMessage(snapshotRequest, 3000ms, error)
        || snapshotRequest.header.type != MessageType::SessionSnapshotRequest
        || !mcast::ipc::validateMessageDirection(
            ConnectionChannel::Playback, snapshotRequest.header, error))
        return false;

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
        return false;

    std::cout << "PLAYBACK_SNAPSHOT_SERVED\n";
    // Keep the PlaybackPipe alive for the Ui lifetime. The current skeleton
    // does not consume the new playback command/state stream yet, but must not
    // tear the connection down immediately after the legacy snapshot.
    while (true) {
        Message trailing;
        if (!connection->readMessage(trailing, 24h, error))
            break;
    }
    return true;
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
        constexpr std::wstring_view shellPidPrefix = L"--shell-pid=";
        constexpr std::wstring_view activationPrefix = L"--activation-id=";
        constexpr std::wstring_view requestRoundsPrefix =
            L"--request-ui-rounds=";
        if (argument.starts_with(instancePrefix)) {
            options.instance = narrow(
                argument.substr(instancePrefix.size()).c_str());
        } else if (argument.starts_with(noncePrefix)) {
            options.nonce = narrow(
                argument.substr(noncePrefix.size()).c_str());
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
        } else if (argument == L"--request-ui") {
            options.requestUi = true;
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
    std::string error;
    auto control = connectControl(options, error);
    if (!control) {
        std::cerr << "control connection failed: " << error << '\n';
        return 2;
    }

    if (options.role == ProcessRole::Protocol) {
        std::atomic_bool receivingEnabled = true;
        ProtocolCommandServer commandServer(options, receivingEnabled);
        std::atomic_bool heartbeatFailed = false;
        std::string heartbeatError;
        std::jthread heartbeat([&](std::stop_token stop) {
            std::uint64_t messageId = 1;
            while (!stop.stop_requested()) {
                Message heartbeatMessage;
                heartbeatMessage.header.type = MessageType::Heartbeat;
                heartbeatMessage.header.source = ProcessRole::Protocol;
                heartbeatMessage.header.destination = ProcessRole::Shell;
                heartbeatMessage.header.messageId = messageId++;
                heartbeatMessage.header.sessionId = 1;
                if (!control->writeMessage(
                        heartbeatMessage, 2000ms, heartbeatError)) {
                    heartbeatFailed = true;
                    return;
                }
                for (int slice = 0; slice < 10 && !stop.stop_requested();
                     ++slice)
                    std::this_thread::sleep_for(100ms);
            }
        });

        const bool persistent = options.persistent;
        bool waitForUiOpenBeforeRound = !options.requestUi;
        std::uint32_t round = 0;
        while (true) {
            if (waitForUiOpenBeforeRound
                && (!sendUiOpenReady(
                        *control, 1000 + round, error)
                    || !waitForUiOpenCommand(*control, error))) {
                std::cerr << "UiOpenRequest wait failed: "
                          << error << '\n';
                return 3;
            }
            if (heartbeatFailed) {
                error = heartbeatError.empty()
                    ? "Protocol heartbeat failed" : heartbeatError;
                std::cerr << error << '\n';
                return 3;
            }

            mcast::ipc::PeerIdentityGrant uiGrant;
            bool started = false;
            if (!requestUi(
                    *control, uiGrant, 2 + round, started, error)) {
                std::cerr << "UI activation failed: " << error << '\n';
                return 3;
            }
            if (started
                && !protocolPlaybackServer(options, uiGrant, error)) {
                std::cerr << "playback server failed: " << error << '\n';
                return 4;
            }
            if (persistent) {
                if (started
                    && !waitForUiExit(
                        uiGrant.processId, 24h, error)) {
                    std::cerr << "Ui lifetime wait failed: "
                              << error << '\n';
                    return 5;
                }
                waitForUiOpenBeforeRound = true;
                ++round;
                continue;
            }
            if (round + 1 < options.uiRequestRounds) {
                if (!started
                    || !waitForUiExit(
                        uiGrant.processId, 5000ms, error)) {
                    std::cerr << "Ui round barrier failed: "
                              << error << '\n';
                    return 5;
                }
            }
            if (++round >= options.uiRequestRounds)
                break;
        }
    } else if (options.role == ProcessRole::Ui) {
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
