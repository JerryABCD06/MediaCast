// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"
#include "supervisor/ChildProcess.h"
#include "supervisor/ProcessIdentity.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using mcast::ipc::ConnectionChannel;
using mcast::ipc::Message;
using mcast::ipc::MessageType;
using mcast::ipc::ProcessRole;

struct Options {
    std::string instance = "default";
    std::filesystem::path protocol;
    std::filesystem::path ui;
    bool testExitAfterFirstFrame = false;
    bool testMode = false;
};

std::string narrow(const std::wstring &value)
{
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
        return {};
    std::string result(size - 1, '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
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

bool safeInstanceId(const std::string &value)
{
    if (value.empty() || value.size() > 64)
        return false;
    for (unsigned char ch : value) {
        if (!std::isalnum(ch) && ch != '-' && ch != '_')
            return false;
    }
    return true;
}

bool parseOptions(int argc, wchar_t **argv, Options &options, std::string &error)
{
    const auto directory =
        mcast::supervisor::currentExecutablePath().parent_path();
    options.protocol = directory / L"MCast.Protocol.exe";
    options.ui = directory / L"MCast.Ui.exe";
    bool pathOverride = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring argument = argv[index];
        constexpr std::wstring_view instancePrefix = L"--instance=";
        constexpr std::wstring_view protocolPrefix = L"--protocol=";
        constexpr std::wstring_view uiPrefix = L"--ui=";
        if (argument.starts_with(instancePrefix)) {
            options.instance = narrow(
                argument.substr(instancePrefix.size()));
        } else if (argument.starts_with(protocolPrefix)) {
            options.protocol = argument.substr(protocolPrefix.size());
            pathOverride = true;
        } else if (argument.starts_with(uiPrefix)) {
            options.ui = argument.substr(uiPrefix.size());
            pathOverride = true;
        } else if (argument == L"--test-exit-after-first-frame") {
            options.testExitAfterFirstFrame = true;
        } else if (argument == L"--test") {
            options.testMode = true;
        } else {
            error = "unknown argument";
            return false;
        }
    }
    if (!safeInstanceId(options.instance)) {
        error = "invalid --instance";
        return false;
    }
    if (pathOverride && !options.testMode) {
        error = "executable path overrides require --test";
        return false;
    }
    return true;
}

struct ChildRecord {
    ProcessRole role = ProcessRole::Unknown;
    mcast::supervisor::ChildProcess process;
    mcast::ipc::HelloExpectation expectation;
};

class ShellRuntime
{
public:
    explicit ShellRuntime(Options options)
        : m_options(std::move(options))
    {
    }

    ~ShellRuntime()
    {
        cleanup();
    }

    bool initialize(std::string &error)
    {
        m_nonce = mcast::supervisor::randomHexToken(16);
        if (!m_nonce) {
            error = "cannot generate instance nonce";
            return false;
        }

        const std::wstring instance = widen(m_options.instance);
        std::wstring mutexName = L"Local\\MediaCast.Shell."
            + mcast::ipc::currentUserSid();
        if (m_options.testMode)
            mutexName += L"." + instance;
        m_mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
        if (!m_mutex) {
            error = "CreateMutexW failed: " + std::to_string(GetLastError());
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            error = "another Shell instance is already running";
            return false;
        }

        m_job = CreateJobObjectW(nullptr, nullptr);
        if (!m_job) {
            error = "CreateJobObjectW failed";
            return false;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                m_job, JobObjectExtendedLimitInformation,
                &limits, sizeof(limits))) {
            error = "SetInformationJobObject failed";
            return false;
        }

        m_firstFrame = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_firstFrame) {
            error = "CreateEventW failed";
            return false;
        }

        m_listener = std::make_unique<mcast::ipc::PipeListener>(
            mcast::ipc::makeControlPipeName(instance));
        if (!m_listener->valid()) {
            error = "control pipe listener failed";
            return false;
        }

        if (!launchProtocol(error))
            return false;
        return true;
    }

    int run()
    {
        std::cout << "SHELL_READY instance=" << m_options.instance
                  << " pid=" << GetCurrentProcessId()
                  << std::endl;

        while (true) {
            std::string acceptError;
            auto connection = m_listener->accept(200ms, acceptError);
            if (connection) {
                if (m_connections.size() >= 8) {
                    std::cerr << "connection limit reached\n";
                } else {
                    auto done = std::make_shared<std::atomic_bool>(false);
                    auto slot = std::make_unique<ConnectionSlot>();
                    slot->done = done;
                    slot->thread = std::jthread(
                        [this, connection = std::move(connection),
                         done]() mutable {
                        handleConnection(std::move(connection));
                        done->store(true);
                    });
                    m_connections.push_back(std::move(slot));
                }
            } else if (!acceptError.empty()
                       && acceptError != "pipe accept timed out") {
                std::cerr << "accept: " << acceptError << '\n';
            }
            reapConnections();

            if (m_options.testExitAfterFirstFrame
                && WaitForSingleObject(m_firstFrame, 0) == WAIT_OBJECT_0) {
                std::cout << "SHELL_FIRST_FRAME_OK\n";
                return 0;
            }

            {
                std::scoped_lock lock(m_uiMutex);
                if (m_uiState == UiState::Running) {
                    std::scoped_lock recordsLock(m_recordsMutex);
                    ChildRecord *ui = findRecord(ProcessRole::Ui);
                    if (ui && !ui->process.running()) {
                        std::cerr << "UI process exited\n";
                        m_uiState = UiState::None;
                    }
                }
                if (m_uiState == UiState::Running
                    && WaitForSingleObject(m_firstFrame, 0) != WAIT_OBJECT_0
                    && std::chrono::steady_clock::now() - m_uiStartedAt
                        > 5000ms) {
                    std::cerr << "UI FirstFrame timed out\n";
                    return 7;
                }
            }

        }
    }

private:
    struct ConnectionSlot {
        std::jthread thread;
        std::shared_ptr<std::atomic_bool> done;
    };

    void reapConnections()
    {
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            if ((*it)->done->load()) {
                if ((*it)->thread.joinable())
                    (*it)->thread.join();
                it = m_connections.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool launchProtocol(std::string &error)
    {
        std::wstring arguments =
            L"--instance=" + widen(m_options.instance)
            + L" --nonce=" + widen(*m_nonce)
            + L" --shell-pid=" + std::to_wstring(GetCurrentProcessId())
            + L" --request-ui";
        return launch(
            ProcessRole::Protocol, m_options.protocol, arguments, error);
    }

    bool launchUi(std::uint64_t activationId, std::string &error)
    {
        std::wstring arguments =
            L"--instance=" + widen(m_options.instance)
            + L" --nonce=" + widen(*m_nonce)
            + L" --shell-pid=" + std::to_wstring(GetCurrentProcessId())
            + L" --activation-id=" + std::to_wstring(activationId);
        return launch(ProcessRole::Ui, m_options.ui, arguments, error);
    }

    bool activateUi(
        std::uint64_t activationId, std::uint64_t &effectiveId,
        std::string &error)
    {
        std::scoped_lock lock(m_uiMutex);
        if (m_uiState == UiState::Running
            || m_uiState == UiState::Starting) {
            effectiveId = m_activationId;
            return true;
        }
        m_uiState = UiState::Starting;
        m_activationId = activationId;
        effectiveId = activationId;
        ResetEvent(m_firstFrame);
        if (!launchUi(activationId, error)) {
            m_uiState = UiState::Failed;
            return false;
        }
        m_uiState = UiState::Running;
        m_uiStartedAt = std::chrono::steady_clock::now();
        return true;
    }

    bool launch(
        ProcessRole role, const std::filesystem::path &path,
        const std::wstring &arguments, std::string &error)
    {
        auto record = std::make_unique<ChildRecord>();
        record->role = role;
        if (!record->process.start(
                path, arguments, path.parent_path(), error))
            return false;
        if (!record->process.assignToJob(m_job, error)) {
            record->process.terminate(1, 1000ms, error);
            return false;
        }

        const auto actualPath =
            mcast::supervisor::queryProcessPath(record->process.processId());
        const auto hash = actualPath
            ? mcast::supervisor::sha256File(*actualPath)
            : std::nullopt;
        if (!actualPath || !hash) {
            error = "cannot identify launched child";
            record->process.terminate(1, 1000ms, error);
            return false;
        }

        record->expectation = {
            record->process.processId(),
            mcast::supervisor::currentBuildId(),
            *m_nonce,
            *hash,
            0x7,
        };
        ChildRecord *published = nullptr;
        std::scoped_lock lock(m_recordsMutex);
        m_records.push_back(std::move(record));
        published = m_records.back().get();

        if (!published->process.resume(error)) {
            std::string terminateError;
            published->process.terminate(1, 1000ms, terminateError);
            m_records.pop_back();
            return false;
        }
        return true;
    }

    ChildRecord *findRecord(ProcessRole role)
    {
        for (const auto &record : m_records) {
            if (record->role == role)
                return record.get();
        }
        return nullptr;
    }

    std::optional<mcast::ipc::PeerIdentityGrant> makeGrant(
        ProcessRole role, std::uint64_t activationId, std::string &error)
    {
        std::scoped_lock lock(m_recordsMutex);
        ChildRecord *record = findRecord(role);
        if (!record) {
            error = "child record is missing";
            return std::nullopt;
        }
        return mcast::ipc::PeerIdentityGrant{
            role,
            record->expectation.peerProcessId,
            record->expectation.buildId,
            record->expectation.instanceNonce,
            record->expectation.executableHash,
            activationId,
        };
    }

    void handleConnection(std::unique_ptr<mcast::ipc::PipeConnection> connection)
    {
        std::string error;
        Message helloMessage;
        if (!connection->readMessage(helloMessage, 3000ms, error)) {
            std::cerr << "hello read: " << error << '\n';
            return;
        }
        const auto hello = mcast::ipc::decodeHelloMessage(
            helloMessage, ConnectionChannel::Control, error);
        if (!hello) {
            std::cerr << "hello invalid: " << error << '\n';
            return;
        }

        ChildRecord *record = nullptr;
        {
            std::scoped_lock lock(m_recordsMutex);
            record = findRecord(helloMessage.header.source);
        }
        if (!record) {
            std::cerr << "hello from unknown role\n";
            return;
        }
        if (connection->peerProcessId() != record->process.processId()) {
            std::cerr << "hello pipe peer is not the launched child\n";
            return;
        }
        auto expectation = record->expectation;
        if (hello->processId != record->process.processId()) {
            std::cerr << "hello PID is not the launched child\n";
            return;
        }
        if (!mcast::ipc::validateHello(*hello, expectation, error)) {
            std::cerr << "hello rejected: " << error << '\n';
            return;
        }

        const mcast::ipc::HelloAckInfo ack{
            true,
            mcast::ipc::kProtocolVersionMajor,
            mcast::ipc::kProtocolVersionMinor,
            *m_nonce,
            0x7,
            "accepted",
        };
        auto ackMessage = mcast::ipc::makeHelloAckMessage(
            ConnectionChannel::Control, ProcessRole::Shell,
            helloMessage.header.source, 2, helloMessage.header.messageId,
            ack, error);
        if (!ackMessage
            || !connection->writeMessage(*ackMessage, 2000ms, error)) {
            std::cerr << "hello ack: " << error << '\n';
            return;
        }

        if (helloMessage.header.source == ProcessRole::Ui) {
            auto protocolGrant = makeGrant(
                ProcessRole::Protocol, 1, error);
            if (!protocolGrant) {
                std::cerr << "protocol grant: " << error << '\n';
                return;
            }
            auto grantMessage = mcast::ipc::makePeerIdentityGrantMessage(
                ProcessRole::Ui, 4, 1, *protocolGrant, error);
            if (!grantMessage
                || !connection->writeMessage(*grantMessage, 2000ms, error)) {
                std::cerr << "protocol grant send: " << error << '\n';
                return;
            }
        }

        while (true) {
            Message message;
            if (!connection->readMessage(message, 3000ms, error))
                return;
            if (message.header.source != helloMessage.header.source
                || !mcast::ipc::validateMessageDirection(
                    ConnectionChannel::Control, message.header, error)) {
                std::cerr << "invalid message direction: " << error << '\n';
                return;
            }

            if (message.header.type == MessageType::UiActivateRequest
                && helloMessage.header.source == ProcessRole::Protocol) {
                std::string activationError;
                std::uint64_t effectiveActivationId = 0;
                if (!activateUi(
                        message.header.messageId, effectiveActivationId,
                        activationError)) {
                    std::cerr << "UI launch failed: " << activationError << '\n';
                    return;
                }
                Message decision;
                decision.header.type = MessageType::UiActivateDecision;
                decision.header.source = ProcessRole::Shell;
                decision.header.destination = ProcessRole::Protocol;
                decision.header.messageId = 3;
                decision.header.correlationId = message.header.messageId;
                decision.header.sessionId = message.header.sessionId;
                if (!connection->writeMessage(decision, 2000ms, error)) {
                    std::cerr << "activation decision: " << error << '\n';
                    return;
                }
                auto uiGrant = makeGrant(
                    ProcessRole::Ui, effectiveActivationId, error);
                if (!uiGrant) {
                    std::cerr << "UI grant: " << error << '\n';
                    return;
                }
                auto grantMessage = mcast::ipc::makePeerIdentityGrantMessage(
                    ProcessRole::Protocol, 4, effectiveActivationId,
                    *uiGrant, error);
                if (!grantMessage
                    || !connection->writeMessage(
                        *grantMessage, 2000ms, error)) {
                    std::cerr << "UI grant send: " << error << '\n';
                    return;
                }
            } else if (message.header.type == MessageType::UiFirstFrame
                       && helloMessage.header.source == ProcessRole::Ui) {
                bool accepted = false;
                {
                    std::scoped_lock lock(m_uiMutex);
                    if (m_uiState == UiState::Running
                        && message.header.correlationId == m_activationId) {
                        SetEvent(m_firstFrame);
                        accepted = true;
                    }
                }
                if (!accepted) {
                    std::cerr << "stale UiFirstFrame generation\n";
                    return;
                }
                return;
            }
        }
    }

    void cleanup()
    {
        if (m_listener)
            m_listener->stop();
        for (const auto &slot : m_connections) {
            if (slot->thread.joinable())
                slot->thread.join();
        }
        m_connections.clear();

        std::scoped_lock lock(m_recordsMutex);
        for (const auto &record : m_records) {
            std::string error;
            record->process.terminate(0, 1000ms, error);
        }
        m_records.clear();
        if (m_firstFrame)
            CloseHandle(m_firstFrame);
        if (m_mutex)
            CloseHandle(m_mutex);
        if (m_job)
            CloseHandle(m_job);
        m_firstFrame = nullptr;
        m_mutex = nullptr;
        m_job = nullptr;
    }

    Options m_options;
    std::optional<std::string> m_nonce;
    HANDLE m_mutex = nullptr;
    HANDLE m_job = nullptr;
    HANDLE m_firstFrame = nullptr;
    std::unique_ptr<mcast::ipc::PipeListener> m_listener;
    std::vector<std::unique_ptr<ChildRecord>> m_records;
    std::mutex m_recordsMutex;
    std::vector<std::unique_ptr<ConnectionSlot>> m_connections;
    enum class UiState {
        None,
        Starting,
        Running,
        Failed,
    };
    UiState m_uiState = UiState::None;
    std::uint64_t m_activationId = 0;
    std::chrono::steady_clock::time_point m_uiStartedAt{};
    std::mutex m_uiMutex;
};

} // namespace

int wmain(int argc, wchar_t **argv)
{
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    ShellRuntime shell(std::move(options));
    if (!shell.initialize(error)) {
        std::cerr << "Shell init failed: " << error << '\n';
        return 2;
    }
    return shell.run();
}
