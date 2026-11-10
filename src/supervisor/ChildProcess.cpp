// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "supervisor/ChildProcess.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace mcast::supervisor {
namespace {

std::string windowsError(std::string_view operation, DWORD code)
{
    return std::string(operation) + " failed with Win32 error "
        + std::to_string(code);
}

} // namespace

std::wstring quoteWindowsArgument(const std::wstring &argument)
{
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;

    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(ch);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'"');
    return result;
}

ChildProcess::~ChildProcess()
{
    close();
}

ChildProcess::ChildProcess(ChildProcess &&other) noexcept
    : m_process(std::exchange(other.m_process, nullptr))
    , m_thread(std::exchange(other.m_thread, nullptr))
    , m_processId(std::exchange(other.m_processId, 0))
{
}

ChildProcess &ChildProcess::operator=(ChildProcess &&other) noexcept
{
    if (this != &other) {
        close();
        m_process = std::exchange(other.m_process, nullptr);
        m_thread = std::exchange(other.m_thread, nullptr);
        m_processId = std::exchange(other.m_processId, 0);
    }
    return *this;
}

bool ChildProcess::start(
    const std::filesystem::path &executable,
    const std::wstring &arguments,
    const std::filesystem::path &workingDirectory,
    std::string &error)
{
    close();
    if (executable.empty()) {
        error = "child executable path is empty";
        return false;
    }

    std::wstring commandLine = quoteWindowsArgument(executable.wstring());
    if (!arguments.empty()) {
        commandLine.push_back(L' ');
        commandLine.append(arguments);
    }
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const std::wstring directory = workingDirectory.wstring();
    if (!CreateProcessW(
            executable.c_str(), mutableCommand.data(), nullptr, nullptr,
            FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
            directory.empty() ? nullptr : directory.c_str(),
            &startup, &process)) {
        error = windowsError("CreateProcessW", GetLastError());
        return false;
    }

    m_process = process.hProcess;
    m_thread = process.hThread;
    m_processId = process.dwProcessId;
    return true;
}

bool ChildProcess::running() const
{
    if (!m_process)
        return false;
    return WaitForSingleObject(
               static_cast<HANDLE>(m_process), 0) == WAIT_TIMEOUT;
}

bool ChildProcess::assignToJob(void *job, std::string &error)
{
    if (!m_process || !job) {
        error = "cannot assign child to an invalid job";
        return false;
    }
    if (!AssignProcessToJobObject(
            static_cast<HANDLE>(job), static_cast<HANDLE>(m_process))) {
        error = windowsError("AssignProcessToJobObject", GetLastError());
        return false;
    }
    return true;
}

std::optional<std::uint32_t> ChildProcess::waitForExit(
    std::chrono::milliseconds timeout, std::string &error)
{
    if (!m_process)
        return std::nullopt;
    const DWORD wait = WaitForSingleObject(
        static_cast<HANDLE>(m_process),
        static_cast<DWORD>(std::clamp<std::int64_t>(
            timeout.count(), 0, 24 * 60 * 60 * 1000)));
    if (wait == WAIT_TIMEOUT)
        return std::nullopt;
    if (wait != WAIT_OBJECT_0) {
        error = windowsError("WaitForSingleObject(child)", GetLastError());
        return std::nullopt;
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(
            static_cast<HANDLE>(m_process), &exitCode)) {
        error = windowsError("GetExitCodeProcess", GetLastError());
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(exitCode);
}

bool ChildProcess::terminate(
    std::uint32_t exitCode, std::chrono::milliseconds timeout,
    std::string &error)
{
    if (!m_process || !running()) {
        close();
        return true;
    }
    if (!TerminateProcess(
            static_cast<HANDLE>(m_process), exitCode)) {
        error = windowsError("TerminateProcess", GetLastError());
        return false;
    }
    const DWORD wait = WaitForSingleObject(
        static_cast<HANDLE>(m_process),
        static_cast<DWORD>(std::clamp<std::int64_t>(
            timeout.count(), 0, 60 * 1000)));
    if (wait != WAIT_OBJECT_0) {
        error = "child did not terminate before timeout";
        return false;
    }
    close();
    return true;
}

void ChildProcess::close()
{
    if (m_thread) {
        CloseHandle(static_cast<HANDLE>(std::exchange(m_thread, nullptr)));
    }
    if (m_process) {
        CloseHandle(static_cast<HANDLE>(std::exchange(m_process, nullptr)));
    }
    m_processId = 0;
}

} // namespace mcast::supervisor
