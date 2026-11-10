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

#include <chrono>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
    std::string &error)
{
    Message request;
    request.header.type = MessageType::UiActivateRequest;
    request.header.source = ProcessRole::Protocol;
    request.header.destination = ProcessRole::Shell;
    request.header.messageId = 2;
    request.header.sessionId = 1;
    if (!control.writeMessage(request, 2000ms, error))
        return false;

    Message decision;
    if (!control.readMessage(decision, 3000ms, error)
        || decision.header.type != MessageType::UiActivateDecision
        || decision.header.correlationId != request.header.messageId
        || !mcast::ipc::validateMessageDirection(
            ConnectionChannel::Control, decision.header, error)) {
        if (error.empty())
            error = "invalid UI activation decision";
        return false;
    }
    Message grant;
    if (!control.readMessage(grant, 3000ms, error))
        return false;
    const auto decoded = mcast::ipc::decodePeerIdentityGrantMessage(
        grant, error);
    if (!decoded || decoded->role != ProcessRole::Ui
        || decoded->activationId != request.header.messageId) {
        error = "invalid UI identity grant";
        return false;
    }
    uiGrant = *decoded;
    return true;
}

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
        mcast::ipc::PeerIdentityGrant uiGrant;
        if (!options.requestUi || !requestUi(*control, uiGrant, error)) {
            std::cerr << "UI activation failed: " << error << '\n';
            return 3;
        }
        if (!protocolPlaybackServer(options, uiGrant, error)) {
            std::cerr << "playback server failed: " << error << '\n';
            return 4;
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
