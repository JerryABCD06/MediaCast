// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "UiSession.h"

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
#include <charconv>
#include <limits>
#include <optional>
#include <string_view>

namespace {

using namespace std::chrono_literals;
using namespace mcast::ipc;

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

std::optional<HelloInfo> selfHello(const std::string &nonce)
{
    const auto hash = mcast::supervisor::sha256File(
        mcast::supervisor::currentExecutablePath());
    if (!hash)
        return std::nullopt;
    return HelloInfo{
        GetCurrentProcessId(),
        mcast::supervisor::currentBuildId(),
        nonce,
        *hash,
        0x7,
    };
}

bool hello(
    PipeConnection &connection, ConnectionChannel channel,
    ProcessRole destination, std::uint64_t messageId,
    const std::string &nonce, std::string &error)
{
    const auto info = selfHello(nonce);
    if (!info) {
        error = "cannot hash Ui executable";
        return false;
    }
    auto message = makeHelloMessage(
        channel, ProcessRole::Ui, destination, messageId, *info, error);
    return message && connection.writeMessage(*message, 2000ms, error);
}

bool helloAck(
    PipeConnection &connection, ConnectionChannel channel,
    std::uint64_t correlationId, const std::string &nonce,
    std::string &error)
{
    Message message;
    if (!connection.readMessage(message, 3000ms, error))
        return false;
    const auto ack = decodeHelloAckMessage(
        message, correlationId, channel, error);
    if (!ack) {
        return false;
    }
    if (!ack->accepted) {
        error = ack->reason;
        return false;
    }
    if (ack->instanceNonce != nonce) {
        error = "HelloAck nonce mismatch";
        return false;
    }
    return true;
}

std::optional<std::uint64_t> parseUnsignedDecimal(const std::string &value)
{
    if (value.empty())
        return std::nullopt;
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        return std::nullopt;
    return result;
}

bool connectPlayback(
    const UiSession::Options &options, const PeerIdentityGrant &protocol,
    std::unique_ptr<PipeConnection> &playback,
    std::string &error, bool reconnect = false)
{
    const auto connectTimeout = reconnect ? 1500ms : 5000ms;
    const auto ackTimeout = reconnect ? 1000ms : 3000ms;
    const auto writeTimeout = reconnect ? 1000ms : 2000ms;
    auto connection = PipeClient::connect(
        makePlaybackPipeName(widen(options.instance)),
        connectTimeout, error);
    if (!connection)
        return false;
    if (connection->peerProcessId() != protocol.processId) {
        error = "Protocol PID does not match Shell grant";
        return false;
    }
    const auto path = mcast::supervisor::queryProcessPath(protocol.processId);
    const auto hash = path ? mcast::supervisor::sha256File(*path) : std::nullopt;
    if (!hash || *hash != protocol.executableHash) {
        error = "Protocol executable hash does not match Shell grant";
        return false;
    }
    if (!hello(
            *connection, ConnectionChannel::Playback,
            ProcessRole::Protocol, 3, options.nonce, error)
        || !helloAck(
            *connection, ConnectionChannel::Playback, 3, options.nonce,
            error))
        return false;

    Message request;
    request.header.type = MessageType::SessionSnapshotRequest;
    request.header.source = ProcessRole::Ui;
    request.header.destination = ProcessRole::Protocol;
    request.header.messageId = 4;
    request.header.sessionId = 1;
    if (!connection->writeMessage(request, writeTimeout, error))
        return false;

    Message snapshot;
    if (!connection->readMessage(snapshot, ackTimeout, error)
        || snapshot.header.type != MessageType::SessionSnapshot
        || snapshot.header.correlationId != request.header.messageId
        || snapshot.header.sessionId != request.header.sessionId
        || snapshot.header.stateVersion == 0
        || (static_cast<std::uint16_t>(snapshot.header.flags)
            & static_cast<std::uint16_t>(MessageFlags::Response))
            == 0
        || !validateMessageDirection(
            ConnectionChannel::Playback, snapshot.header, error)) {
        if (error.empty())
            error = "invalid SessionSnapshot";
        return false;
    }
    playback = std::move(connection);
    return true;
}

} // namespace

UiSession::Options
UiSession::parseArguments(int argc, char **argv, std::string &error)
{
    Options options;
    bool instanceArgumentSeen = false;
    bool ipcArgumentSeen = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view instancePrefix = "--instance=";
        constexpr std::string_view noncePrefix = "--nonce=";
        constexpr std::string_view shellPidPrefix = "--shell-pid=";
        constexpr std::string_view activationPrefix = "--activation-id=";
        if (argument == "--standalone") {
            options.standalone = true;
        } else if (argument == "--confirmation-only") {
            options.confirmationOnly = true;
        } else if (argument == "--test-takeover-window") {
            options.testTakeoverWindow = true;
        } else if (argument.starts_with(instancePrefix)) {
            instanceArgumentSeen = true;
            ipcArgumentSeen = true;
            options.instance = argument.substr(instancePrefix.size());
        } else if (argument.starts_with(noncePrefix)) {
            ipcArgumentSeen = true;
            options.nonce = argument.substr(noncePrefix.size());
        } else if (argument.starts_with(shellPidPrefix)) {
            ipcArgumentSeen = true;
            const auto value = parseUnsignedDecimal(
                argument.substr(shellPidPrefix.size()));
            if (!value || *value == 0
                || *value > std::numeric_limits<std::uint32_t>::max()) {
                error = "invalid --shell-pid";
                return {};
            }
            options.shellPid = static_cast<std::uint32_t>(*value);
        } else if (argument.starts_with(activationPrefix)) {
            ipcArgumentSeen = true;
            const auto value = parseUnsignedDecimal(
                argument.substr(activationPrefix.size()));
            if (!value || *value == 0) {
                error = "invalid --activation-id";
                return {};
            }
            options.activationId = *value;
        }
    }

    if (instanceArgumentSeen && options.instance.empty()) {
        error = "empty --instance";
        return {};
    }
    if (ipcArgumentSeen && options.instance.empty()) {
        error = "Ui IPC arguments require --instance";
        return {};
    }
    options.enabled = !options.instance.empty();
    if (!options.enabled && !options.standalone) {
        error = "MCast.Ui.exe requires a Shell launch context";
        return {};
    }
    if (options.enabled
        && (options.nonce.empty() || options.shellPid == 0
            || options.activationId == 0)) {
        error = "incomplete Ui IPC arguments";
        options.enabled = false;
    }
    return options;
}

UiSession::UiSession() = default;
UiSession::~UiSession() = default;

bool UiSession::connectToPeers(const Options &options, std::string &error)
{
    m_options = options;
    if (!options.enabled)
        return true;

    m_control = PipeClient::connect(
        makeControlPipeName(widen(options.instance)), 3000ms, error);
    if (!m_control)
        return false;
    if (m_control->peerProcessId() != options.shellPid) {
        error = "ControlPipe server is not the expected Shell";
        return false;
    }
    if (!hello(
            *m_control, ConnectionChannel::Control,
            ProcessRole::Shell, 1, options.nonce, error)
        || !helloAck(
            *m_control, ConnectionChannel::Control, 1, options.nonce, error))
        return false;

    Message settingsMessage;
    if (!m_control->readMessage(settingsMessage, 3000ms, error))
        return false;
    const auto settingsJson =
        decodeSettingsSnapshotMessage(settingsMessage, error);
    if (!settingsJson)
        return false;
    m_settingsJson = *settingsJson;
    m_settingsVersion = settingsMessage.header.stateVersion;

    Message grantMessage;
    if (!m_control->readMessage(grantMessage, 3000ms, error))
        return false;
    const auto protocolGrant =
        decodePeerIdentityGrantMessage(grantMessage, error);
    if (!protocolGrant || protocolGrant->role != ProcessRole::Protocol
        || protocolGrant->activationId != options.activationId
        || protocolGrant->instanceNonce != options.nonce
        || protocolGrant->buildId
            != mcast::supervisor::currentBuildId()) {
        error = "invalid Protocol identity grant";
        return false;
    }
    if (options.confirmationOnly)
        return true;
    m_protocolGrant = *protocolGrant;
    m_hasProtocolGrant = true;
    return connectPlayback(
        options, *protocolGrant, m_playback, error, false);
}

std::unique_ptr<mcast::ipc::PipeConnection>
UiSession::reconnectPlayback(std::string &error)
{
    error.clear();
    if (!m_options.enabled || m_options.confirmationOnly
        || !m_hasProtocolGrant) {
        error = "Ui playback reconnect has no protocol grant";
        return nullptr;
    }
    std::unique_ptr<mcast::ipc::PipeConnection> playback;
    if (!connectPlayback(
            m_options, m_protocolGrant, playback, error, true)) {
        return nullptr;
    }
    return playback;
}

std::unique_ptr<mcast::ipc::PipeConnection>
UiSession::takeControlConnection()
{
    return std::move(m_control);
}

std::unique_ptr<mcast::ipc::PipeConnection>
UiSession::takePlaybackConnection()
{
    return std::move(m_playback);
}
