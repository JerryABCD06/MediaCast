// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ipc/NamedPipe.h"

#include <windows.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace mcast::ipc {
namespace {

constexpr std::uint32_t kMaxWaitMilliseconds = 24 * 60 * 60 * 1000;

std::string windowsError(std::string_view operation, DWORD code)
{
    return std::string(operation) + " failed with Win32 error "
        + std::to_string(code);
}

DWORD boundedTimeout(std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0)
        return 0;
    return static_cast<DWORD>(std::min<std::int64_t>(
        timeout.count(), kMaxWaitMilliseconds));
}

using Deadline = std::chrono::steady_clock::time_point;

Deadline makeDeadline(std::chrono::milliseconds timeout)
{
    return std::chrono::steady_clock::now()
        + std::chrono::milliseconds(std::max<std::int64_t>(
            0, timeout.count()));
}

DWORD remainingTimeout(Deadline deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
        return 0;
    return boundedTimeout(std::chrono::duration_cast<
        std::chrono::milliseconds>(deadline - now));
}

struct OverlappedEvent {
    OVERLAPPED overlapped{};
    HANDLE event = nullptr;

    OverlappedEvent()
    {
        event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        overlapped.hEvent = event;
    }

    ~OverlappedEvent()
    {
        if (event)
            CloseHandle(event);
    }

    bool valid() const noexcept { return event != nullptr; }
};

bool waitForIo(
    HANDLE handle, OverlappedEvent &operation, DWORD timeout,
    DWORD &transferred, std::string &error)
{
    const auto finishCancelled = [&] {
        CancelIoEx(handle, &operation.overlapped);
        DWORD ignored = 0;
        GetOverlappedResult(
            handle, &operation.overlapped, &ignored, TRUE);
    };

    const DWORD wait = WaitForSingleObject(operation.event, timeout);
    if (wait == WAIT_TIMEOUT) {
        finishCancelled();
        error = "pipe I/O timed out";
        return false;
    }
    if (wait != WAIT_OBJECT_0) {
        const DWORD waitError = GetLastError();
        finishCancelled();
        error = windowsError("WaitForSingleObject(pipe I/O)", waitError);
        return false;
    }
    if (!GetOverlappedResult(
            handle, &operation.overlapped, &transferred, FALSE)) {
        error = windowsError("GetOverlappedResult", GetLastError());
        return false;
    }
    return true;
}

bool readExact(
    HANDLE handle, std::uint8_t *data, std::size_t size,
    Deadline deadline, std::mutex &startMutex,
    std::atomic_bool &closing, std::string &error)
{
    std::size_t offset = 0;
    while (offset < size) {
        OverlappedEvent operation;
        if (!operation.valid()) {
            error = windowsError("CreateEventW(read)", GetLastError());
            return false;
        }

        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(size - offset,
                                  std::numeric_limits<DWORD>::max()));
        const DWORD timeout = remainingTimeout(deadline);
        if (timeout == 0) {
            error = "pipe I/O deadline expired";
            return false;
        }
        DWORD received = 0;
        bool pending = false;
        {
            std::scoped_lock startLock(startMutex);
            if (closing.load()) {
                error = "pipe connection is closing";
                return false;
            }
            if (!ReadFile(
                    handle, data + offset, request, &received,
                    &operation.overlapped)) {
                const DWORD code = GetLastError();
                if (code != ERROR_IO_PENDING) {
                    error = windowsError("ReadFile(pipe)", code);
                    return false;
                }
                pending = true;
            }
        }
        if (pending) {
            DWORD transferred = 0;
            if (!waitForIo(
                    handle, operation, timeout, transferred, error))
                return false;
            received = transferred;
        }
        if (received == 0) {
            error = "pipe closed while reading";
            return false;
        }
        offset += received;
    }
    return true;
}

bool writeAll(
    HANDLE handle, const std::uint8_t *data, std::size_t size,
    Deadline deadline, std::mutex &startMutex,
    std::atomic_bool &closing, std::string &error)
{
    std::size_t offset = 0;
    while (offset < size) {
        OverlappedEvent operation;
        if (!operation.valid()) {
            error = windowsError("CreateEventW(write)", GetLastError());
            return false;
        }

        const DWORD request = static_cast<DWORD>(
            std::min<std::size_t>(size - offset,
                                  std::numeric_limits<DWORD>::max()));
        const DWORD timeout = remainingTimeout(deadline);
        if (timeout == 0) {
            error = "pipe I/O deadline expired";
            return false;
        }
        DWORD written = 0;
        bool pending = false;
        {
            std::scoped_lock startLock(startMutex);
            if (closing.load()) {
                error = "pipe connection is closing";
                return false;
            }
            if (!WriteFile(
                    handle, data + offset, request, &written,
                    &operation.overlapped)) {
                const DWORD code = GetLastError();
                if (code != ERROR_IO_PENDING) {
                    error = windowsError("WriteFile(pipe)", code);
                    return false;
                }
                pending = true;
            }
        }
        if (pending) {
            DWORD transferred = 0;
            if (!waitForIo(
                    handle, operation, timeout, transferred, error))
                return false;
            written = transferred;
        }
        if (written == 0) {
            error = "pipe accepted zero bytes";
            return false;
        }
        offset += written;
    }
    return true;
}

struct SecurityAttributes {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;

    SecurityAttributes()
    {
        const std::wstring sid = currentUserSid();
        if (sid.empty())
            return;
        const std::wstring sddl =
            L"O:" + sid + L"G:" + sid
            + L"D:P(A;;GA;;;SY)(A;;GA;;;" + sid + L")";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return;
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;
    }

    ~SecurityAttributes()
    {
        if (descriptor)
            LocalFree(descriptor);
    }

    bool valid() const noexcept { return descriptor != nullptr; }
};

HANDLE createServerInstance(
    const std::wstring &name, bool first, std::uint32_t maxInstances,
    std::string &error)
{
    SecurityAttributes security;
    if (!security.valid()) {
        error = windowsError(
            "build pipe security descriptor", GetLastError());
        return nullptr;
    }

    DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
    if (first)
        openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
    const DWORD pipeMode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
        | PIPE_REJECT_REMOTE_CLIENTS;
    constexpr DWORD bufferSize = 64 * 1024;

    HANDLE handle = CreateNamedPipeW(
        name.c_str(), openMode, pipeMode, maxInstances,
        bufferSize, bufferSize, 0, &security.attributes);
    if (handle == INVALID_HANDLE_VALUE) {
        error = windowsError("CreateNamedPipeW", GetLastError());
        return nullptr;
    }
    return handle;
}

std::wstring pipeName(std::wstring_view prefix, std::wstring_view instance)
{
    const std::wstring sid = currentUserSid();
    if (sid.empty() || instance.empty())
        return {};
    std::wstring name = L"\\\\.\\pipe\\MediaCast.";
    name.append(prefix);
    name.push_back(L'.');
    name.append(sid);
    name.push_back(L'.');
    name.append(instance);
    return name;
}

} // namespace

std::wstring currentUserSid()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};

    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        CloseHandle(token);
        return {};
    }

    std::vector<std::uint8_t> buffer(size);
    if (!GetTokenInformation(
            token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return {};
    }
    CloseHandle(token);

    const auto *user =
        reinterpret_cast<const TOKEN_USER *>(buffer.data());
    wchar_t *sidText = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &sidText))
        return {};
    std::wstring result(sidText);
    LocalFree(sidText);
    return result;
}

std::wstring makeControlPipeName(std::wstring_view instance)
{
    return pipeName(L"Control", instance);
}

std::wstring makePlaybackPipeName(std::wstring_view instance)
{
    return pipeName(L"Playback", instance);
}

std::wstring makeShellCommandPipeName(std::wstring_view instance)
{
    return pipeName(L"ShellCommands", instance);
}

std::wstring makeProtocolCommandPipeName(std::wstring_view instance)
{
    return pipeName(L"ProtocolCommands", instance);
}

PipeConnection::PipeConnection(
    void *handle, Kind kind, std::uint32_t peerProcessId)
    : m_handle(handle)
    , m_kind(kind)
    , m_peerProcessId(peerProcessId)
{
}

PipeConnection::~PipeConnection()
{
    close();
}

bool PipeConnection::valid() const noexcept
{
    return m_handle.load() != nullptr && !m_closing.load();
}

bool PipeConnection::readMessage(
    Message &message, std::chrono::milliseconds timeout, std::string &error)
{
    std::scoped_lock lock(m_readMutex);
    std::shared_lock lifecycleLock(m_lifecycleMutex);
    if (m_closing.load()) {
        error = "pipe connection is closing";
        return false;
    }
    HANDLE handle = static_cast<HANDLE>(m_handle.load());
    if (!handle) {
        error = "pipe connection is closed";
        return false;
    }

    const Deadline deadline = makeDeadline(timeout);
    std::array<std::uint8_t, kHeaderSize> header{};
    if (!readExact(
            handle, header.data(), header.size(), deadline,
            m_ioStartMutex, m_closing, error))
        return false;

    HeaderDecodeResult decodedHeader = decodeHeader(header);
    if (decodedHeader.status != DecodeStatus::Ok) {
        std::ostringstream bytes;
        for (const std::uint8_t byte : header) {
            bytes << std::hex << std::uppercase
                  << static_cast<unsigned int>(byte) << ' ';
        }
        error = "invalid IPC header: " + bytes.str();
        return false;
    }

    const std::uint32_t payloadSize =
        static_cast<std::uint32_t>(header[54])
        | static_cast<std::uint32_t>(header[55]) << 8
        | static_cast<std::uint32_t>(header[56]) << 16
        | static_cast<std::uint32_t>(header[57]) << 24;
    if (payloadSize > kMaxPayloadSize) {
        error = "IPC payload exceeds limit";
        return false;
    }

    std::vector<std::uint8_t> frame(kHeaderSize + payloadSize);
    std::copy(header.begin(), header.end(), frame.begin());
    if (payloadSize != 0
        && !readExact(
            handle, frame.data() + kHeaderSize, payloadSize, deadline,
            m_ioStartMutex, m_closing, error))
        return false;

    FrameDecodeResult decoded = decodeFrame(frame);
    if (decoded.status != DecodeStatus::Ok
        || decoded.consumed != frame.size()) {
        error = "invalid IPC frame";
        return false;
    }
    message = std::move(decoded.message);
    return true;
}

bool PipeConnection::writeMessage(
    const Message &message, std::chrono::milliseconds timeout,
    std::string &error)
{
    std::scoped_lock lock(m_writeMutex);
    std::shared_lock lifecycleLock(m_lifecycleMutex);
    if (m_closing.load()) {
        error = "pipe connection is closing";
        return false;
    }
    HANDLE handle = static_cast<HANDLE>(m_handle.load());
    if (!handle) {
        error = "pipe connection is closed";
        return false;
    }
    const std::vector<std::uint8_t> frame = encodeFrame(message);
    if (frame.empty()) {
        error = "IPC message cannot be encoded";
        return false;
    }
    return writeAll(
        handle, frame.data(), frame.size(), makeDeadline(timeout),
        m_ioStartMutex, m_closing, error);
}

void PipeConnection::close()
{
    HANDLE handle = nullptr;
    {
        std::scoped_lock startLock(m_ioStartMutex);
        m_closing = true;
        handle = static_cast<HANDLE>(m_handle.load());
        if (handle)
            CancelIoEx(handle, nullptr);
    }
    if (!handle)
        return;
    std::unique_lock lifecycleLock(m_lifecycleMutex);
    handle = static_cast<HANDLE>(m_handle.exchange(nullptr));
    if (!handle)
        return;
    if (m_kind == Kind::Server)
        DisconnectNamedPipe(handle);
    CloseHandle(handle);
}

PipeListener::PipeListener(
    std::wstring name, std::uint32_t maxInstances)
    : m_name(std::move(name))
    , m_maxInstances(std::clamp<std::uint32_t>(maxInstances, 1, 16))
{
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) {
        m_valid = false;
        return;
    }
    std::string error;
    m_valid = ensurePendingInstance(error);
}

PipeListener::~PipeListener()
{
    stop();
    std::scoped_lock lock(m_acceptMutex);
    discardPendingInstance();
    if (m_stopEvent) {
        CloseHandle(static_cast<HANDLE>(m_stopEvent));
        m_stopEvent = nullptr;
    }
}

bool PipeListener::ensurePendingInstance(std::string &error)
{
    if (m_stopping.load()) {
        error = "pipe listener is stopping";
        return false;
    }
    if (m_pendingHandle.load())
        return true;
    if (m_name.empty()) {
        error = "pipe listener is not configured";
        return false;
    }
    void *created = createServerInstance(
        m_name, m_firstInstance, m_maxInstances, error);
    if (!created) {
        m_valid = false;
        return false;
    }
    m_pendingHandle.store(created);
    m_valid = true;
    return true;
}

void PipeListener::discardPendingInstance()
{
    void *value = m_pendingHandle.exchange(nullptr);
    if (!value)
        return;
    HANDLE handle = static_cast<HANDLE>(value);
    CancelIoEx(handle, nullptr);
    DisconnectNamedPipe(handle);
    CloseHandle(handle);
}

void PipeListener::stop()
{
    void *value = nullptr;
    {
        std::scoped_lock stateLock(m_stateMutex);
        m_stopping = true;
        m_valid = false;
        value = m_pendingHandle.load();
    }
    if (m_stopEvent)
        SetEvent(static_cast<HANDLE>(m_stopEvent));
    if (value) {
        HANDLE handle = static_cast<HANDLE>(value);
        CancelIoEx(handle, nullptr);
    }
}

std::unique_ptr<PipeConnection> PipeListener::accept(
    std::chrono::milliseconds timeout, std::string &error)
{
    std::scoped_lock lock(m_acceptMutex);
    if (!m_stopEvent || m_stopping.load()) {
        error = "pipe listener is stopping";
        return nullptr;
    }
    if (!ensurePendingInstance(error))
        return nullptr;
    if (m_stopping.load()) {
        error = "pipe listener is stopping";
        discardPendingInstance();
        return nullptr;
    }

    HANDLE handle = static_cast<HANDLE>(m_pendingHandle.load());
    OverlappedEvent operation;
    if (!operation.valid()) {
        error = windowsError("CreateEventW(connect)", GetLastError());
        return nullptr;
    }

    if (!ConnectNamedPipe(handle, &operation.overlapped)) {
        const DWORD code = GetLastError();
        if (code == ERROR_PIPE_CONNECTED) {
            // The client won the race between CreateNamedPipe and connect.
        } else if (code == ERROR_IO_PENDING) {
            DWORD transferred = 0;
            HANDLE events[2] = {
                operation.event,
                static_cast<HANDLE>(m_stopEvent),
            };
            const DWORD wait = WaitForMultipleObjects(
                2, events, FALSE, boundedTimeout(timeout));
            if (wait == WAIT_OBJECT_0 + 1) {
                CancelIoEx(handle, &operation.overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(
                    handle, &operation.overlapped, &ignored, TRUE);
                error = "pipe listener is stopping";
                discardPendingInstance();
                return nullptr;
            }
            if (wait == WAIT_TIMEOUT) {
                CancelIoEx(handle, &operation.overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(
                    handle, &operation.overlapped, &ignored, TRUE);
                error = "pipe accept timed out";
                discardPendingInstance();
                return nullptr;
            }
            if (wait != WAIT_OBJECT_0
                || !GetOverlappedResult(
                    handle, &operation.overlapped,
                    &transferred, FALSE)) {
                error = windowsError(
                    "ConnectNamedPipe completion", GetLastError());
                discardPendingInstance();
                return nullptr;
            }
        } else {
            error = windowsError("ConnectNamedPipe", code);
            discardPendingInstance();
            return nullptr;
        }
    }

    m_pendingHandle.store(nullptr);
    m_firstInstance = false;
    DWORD peerPid = 0;
    if (!GetNamedPipeClientProcessId(handle, &peerPid) || peerPid == 0) {
        error = windowsError(
            "GetNamedPipeClientProcessId", GetLastError());
        DisconnectNamedPipe(handle);
        CloseHandle(handle);
        return nullptr;
    }
    auto connection = std::unique_ptr<PipeConnection>(
        new PipeConnection(handle, PipeConnection::Kind::Server, peerPid));

    std::scoped_lock stateLock(m_stateMutex);
    if (m_stopping.load()) {
        connection->close();
        error = "pipe listener is stopping";
        return nullptr;
    }

    // Keep a fresh instance ready so a client may connect while the accepted
    // connection is being processed.
    std::string nextError;
    ensurePendingInstance(nextError);
    return connection;
}

std::unique_ptr<PipeConnection> PipeClient::connect(
    std::wstring_view name, std::chrono::milliseconds timeout,
    std::string &error)
{
    if (name.empty()) {
        error = "pipe name is empty";
        return nullptr;
    }
    if (timeout.count() < 0) {
        error = "pipe connect timeout is negative";
        return nullptr;
    }

    const std::wstring ownedName(name);
    const auto bounded = std::chrono::milliseconds(std::min<std::int64_t>(
        timeout.count(), kMaxWaitMilliseconds));
    const auto deadline = std::chrono::steady_clock::now() + bounded;
    while (true) {
        HANDLE handle = CreateFileW(
        ownedName.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT
            | SECURITY_IDENTIFICATION,
        nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            DWORD peerPid = 0;
            if (!GetNamedPipeServerProcessId(handle, &peerPid)
                || peerPid == 0) {
                error = windowsError(
                    "GetNamedPipeServerProcessId", GetLastError());
                CloseHandle(handle);
                return nullptr;
            }
            return std::unique_ptr<PipeConnection>(
                new PipeConnection(
                    handle, PipeConnection::Kind::Client, peerPid));
        }

        const DWORD code = GetLastError();
        if (code != ERROR_PIPE_BUSY && code != ERROR_FILE_NOT_FOUND) {
            error = windowsError("CreateFileW(pipe)", code);
            return nullptr;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error = windowsError("timed out waiting for pipe", code);
            return nullptr;
        }
        const auto remaining = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline - now);
        if (code == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(
                ownedName.c_str(),
                static_cast<DWORD>(std::max<std::int64_t>(
                    1, std::min<std::int64_t>(
                        remaining.count(), kMaxWaitMilliseconds))));
        } else {
            std::this_thread::sleep_for(std::min(
                remaining, std::chrono::milliseconds(10)));
        }
    }
}

} // namespace mcast::ipc
