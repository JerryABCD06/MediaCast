// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "supervisor/ProcessIdentity.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <vector>

namespace mcast::supervisor {

std::string currentBuildId()
{
#ifdef MCAST_BUILD_ID
    return MCAST_BUILD_ID;
#else
    return "unknown";
#endif
}

std::filesystem::path currentExecutablePath()
{
    std::wstring buffer(32768, L'\0');
    const DWORD size = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size >= buffer.size())
        return {};
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

std::optional<std::filesystem::path>
queryProcessPath(std::uint32_t processId)
{
    if (processId == 0)
        return std::nullopt;
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
        return std::nullopt;

    std::wstring buffer(32768, L'\0');
    DWORD size = static_cast<DWORD>(buffer.size());
    const BOOL ok = QueryFullProcessImageNameW(
        process, 0, buffer.data(), &size);
    CloseHandle(process);
    if (!ok || size == 0)
        return std::nullopt;
    buffer.resize(size);
    return std::filesystem::path(buffer);
}

std::optional<std::string>
sha256File(const std::filesystem::path &path)
{
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return std::nullopt;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<std::uint8_t> hashObject;
    std::vector<std::uint8_t> digest;
    auto cleanup = [&] {
        if (hash)
            BCryptDestroyHash(hash);
        if (algorithm)
            BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
    };

    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        cleanup();
        return std::nullopt;
    }
    DWORD objectSize = 0;
    DWORD resultSize = 0;
    DWORD ignored = 0;
    if (BCryptGetProperty(
            algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize),
            &ignored, 0) < 0
        || BCryptGetProperty(
            algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&resultSize), sizeof(resultSize),
            &ignored, 0) < 0) {
        cleanup();
        return std::nullopt;
    }
    hashObject.resize(objectSize);
    digest.resize(resultSize);
    if (BCryptCreateHash(
            algorithm, &hash, hashObject.data(),
            static_cast<ULONG>(hashObject.size()), nullptr, 0, 0) < 0) {
        cleanup();
        return std::nullopt;
    }

    std::array<std::uint8_t, 64 * 1024> buffer{};
    while (true) {
        DWORD read = 0;
        if (!ReadFile(
                file, buffer.data(), static_cast<DWORD>(buffer.size()),
                &read, nullptr)) {
            cleanup();
            return std::nullopt;
        }
        if (read == 0)
            break;
        if (BCryptHashData(
                hash, buffer.data(), read, 0) < 0) {
            cleanup();
            return std::nullopt;
        }
    }
    if (BCryptFinishHash(
            hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        cleanup();
        return std::nullopt;
    }
    cleanup();

    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(digest.size() * 2);
    for (std::uint8_t byte : digest) {
        text.push_back(hex[byte >> 4]);
        text.push_back(hex[byte & 0x0F]);
    }
    return text;
}

std::optional<std::string> randomHexToken(std::uint32_t bytes)
{
    if (bytes == 0 || bytes > 128)
        return std::nullopt;
    std::vector<std::uint8_t> data(bytes);
    if (BCryptGenRandom(
            nullptr, data.data(), static_cast<ULONG>(data.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return std::nullopt;
    constexpr char hex[] = "0123456789abcdef";
    std::string text;
    text.reserve(data.size() * 2);
    for (std::uint8_t byte : data) {
        text.push_back(hex[byte >> 4]);
        text.push_back(hex[byte & 0x0F]);
    }
    return text;
}

} // namespace mcast::supervisor
