// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "skeleton/SkeletonClient.h"

#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"
#include "supervisor/ProcessIdentity.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

namespace mcast::skeleton {
namespace {

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
        if (argument.starts_with(instancePrefix)) {
            options.instance = narrow(
                argument.substr(instancePrefix.size()).c_str());
        } else if (argument.starts_with(noncePrefix)) {
            options.nonce = narrow(
                argument.substr(noncePrefix.size()).c_str());
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
    return true;
}

int runClient(const ClientOptions &options)
{
    using namespace std::chrono_literals;
    using namespace mcast::ipc;

    const std::wstring instance(
        options.instance.begin(), options.instance.end());
    std::string error;
    auto connection = PipeClient::connect(
        makeControlPipeName(instance), 3000ms, error);
    if (!connection) {
        std::cerr << "connect failed: " << error << '\n';
        return 2;
    }

    const auto hash = supervisor::sha256File(
        supervisor::currentExecutablePath());
    if (!hash) {
        std::cerr << "cannot hash executable\n";
        return 3;
    }
    const HelloInfo hello{
        GetCurrentProcessId(),
        supervisor::currentBuildId(),
        options.nonce,
        *hash,
        0x7,
    };
    auto helloMessage = makeHelloMessage(
        ConnectionChannel::Control, options.role, ProcessRole::Shell,
        1, hello, error);
    if (!helloMessage
        || !connection->writeMessage(*helloMessage, 2000ms, error)) {
        std::cerr << "hello failed: " << error << '\n';
        return 3;
    }

    Message reply;
    if (!connection->readMessage(reply, 3000ms, error)) {
        std::cerr << "HelloAck read failed: " << error << '\n';
        return 4;
    }
    const auto ack = decodeHelloAckMessage(
        reply, 1, ConnectionChannel::Control, error);
    if (!ack || !ack->accepted) {
        std::cerr << "HelloAck rejected: "
                  << (ack ? ack->reason : error) << '\n';
        return 4;
    }
    if (ack->instanceNonce != options.nonce) {
        std::cerr << "HelloAck nonce mismatch\n";
        return 4;
    }

    if (options.role == ProcessRole::Protocol && options.requestUi) {
        Message request;
        request.header.type = MessageType::UiActivateRequest;
        request.header.source = ProcessRole::Protocol;
        request.header.destination = ProcessRole::Shell;
        request.header.messageId = 2;
        request.header.sessionId = 1;
        if (!connection->writeMessage(request, 2000ms, error)) {
            std::cerr << "UiActivateRequest failed: " << error << '\n';
            return 5;
        }
        Message decision;
        if (!connection->readMessage(decision, 3000ms, error)
            || decision.header.type != MessageType::UiActivateDecision
            || decision.header.correlationId != request.header.messageId
            || !validateMessageDirection(
                ConnectionChannel::Control, decision.header, error)) {
            std::cerr << "UI activation failed: "
                      << (error.empty() ? "invalid decision" : error) << '\n';
            return 5;
        }
    } else if (options.role == ProcessRole::Ui) {
        Message firstFrame;
        firstFrame.header.type = MessageType::UiFirstFrame;
        firstFrame.header.source = ProcessRole::Ui;
        firstFrame.header.destination = ProcessRole::Shell;
        firstFrame.header.messageId = 2;
        firstFrame.header.sessionId = 1;
        if (!connection->writeMessage(firstFrame, 2000ms, error)) {
            std::cerr << "UiFirstFrame failed: " << error << '\n';
            return 5;
        }
    }

    std::cout << "role=" << static_cast<int>(options.role)
              << " pid=" << GetCurrentProcessId() << " ok\n";
    return 0;
}

} // namespace mcast::skeleton
