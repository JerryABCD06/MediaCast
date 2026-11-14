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
#include "platform/windows/AccentPalette.h"
#include "platform/windows/SplashWindow.h"
#include "platform/windows/TrayIcon.h"
#include "shell/ShellLanguage.h"
#include "shell/ShellSettings.h"
#include "supervisor/ChildProcess.h"
#include "supervisor/ProcessIdentity.h"

#include <windows.h>

#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using mcast::ipc::ConnectionChannel;
using mcast::ipc::Message;
using mcast::ipc::MessageType;
using mcast::ipc::ProcessRole;

constexpr auto kUiOpenReadyTimeout = 7000ms;
constexpr auto kHandoffAckTimeout = kUiOpenReadyTimeout + 5000ms;

namespace WView = winrt::Windows::UI::ViewManagement;

struct Options {
    std::string instance = "default";
    std::filesystem::path protocol;
    std::filesystem::path ui;
    bool testExitAfterFirstFrame = false;
    bool testMode = false;
    bool testHold = false;
    bool testToggleReceiving = false;
    bool testToggleReceivingAfterFirstFrame = false;
    bool testToggleBroadcast = false;
    bool testPersistent = false;
    bool testEnableSsdp = false;
    bool testForceReceivingEnabled = false;
    bool background = false;
    std::uint32_t testUiRounds = 1;
};

class MainThreadApartment
{
public:
    MainThreadApartment()
    {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            m_initialized = true;
        } catch (const winrt::hresult_error &) {
        }
    }

    ~MainThreadApartment()
    {
        if (m_initialized)
            winrt::uninit_apartment();
    }

private:
    bool m_initialized = false;
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

std::wstring quoteWindowsArgument(std::wstring_view value)
{
    if (value.empty())
        return L"\"\"";
    if (value.find_first_of(L" \t\"") == std::wstring_view::npos)
        return std::wstring(value);

    std::wstring result;
    result.push_back(L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(ch);
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

bool systemUsesDarkTheme()
{
    DWORD light = 1;
    DWORD size = sizeof(light);
    if (RegGetValueW(
            HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
            &light, &size)
        != ERROR_SUCCESS) {
        return false;
    }
    return light == 0;
}

bool effectiveDarkMode(const std::string &configured)
{
    if (configured == "Dark")
        return true;
    if (configured == "Light")
        return false;
    return systemUsesDarkTheme();
}

mcast::platform::AccentPair splashAccentColors(
    const std::string &configured)
{
    mcast::platform::AccentPair result =
        mcast::platform::WindowsBlueAccent;
    if (configured != "System")
        return result;
    try {
        WView::UISettings settings;
        const auto light = settings.GetColorValue(
            WView::UIColorType::AccentDark1);
        const auto dark = settings.GetColorValue(
            WView::UIColorType::AccentLight2);
        result.light = (static_cast<unsigned int>(light.R) << 16)
            | (static_cast<unsigned int>(light.G) << 8)
            | static_cast<unsigned int>(light.B);
        result.dark = (static_cast<unsigned int>(dark.R) << 16)
            | (static_cast<unsigned int>(dark.G) << 8)
            | static_cast<unsigned int>(dark.B);
    } catch (const winrt::hresult_error &exception) {
        std::cerr << "System accent unavailable; using Windows Blue: "
                  << winrt::to_string(exception.message()) << '\n';
    }
    return result;
}

mcast::platform::TrayTexts toWideTrayTexts(
    const mcast::shell::TrayTexts &texts)
{
    return {
        widen(texts.openMain),
        widen(texts.pauseReceiving),
        widen(texts.resumeReceiving),
        widen(texts.quit),
        widen(texts.tooltipReceiving),
        widen(texts.tooltipPaused),
    };
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

bool sameExecutableAsCurrent(
    std::uint32_t processId, std::string &error)
{
    const auto path = mcast::supervisor::queryProcessPath(processId);
    const auto peerHash = path
        ? mcast::supervisor::sha256File(*path) : std::nullopt;
    const auto selfHash = mcast::supervisor::sha256File(
        mcast::supervisor::currentExecutablePath());
    if (!path || !peerHash || !selfHash) {
        error = "cannot identify Shell peer executable";
        return false;
    }
    if (*peerHash != *selfHash) {
        error = "Shell peer executable hash mismatch";
        return false;
    }
    return true;
}

struct FindMainWindowState {
    DWORD processId = 0;
    HWND window = nullptr;
};

BOOL CALLBACK findProcessMainWindow(HWND window, LPARAM parameter)
{
    auto *state = reinterpret_cast<FindMainWindowState *>(parameter);
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    if (processId != state->processId || !IsWindowVisible(window)
        || GetWindow(window, GW_OWNER) != nullptr
        || GetWindowTextLengthW(window) == 0)
        return TRUE;
    state->window = window;
    return FALSE;
}

void bringProcessWindowToFront(std::uint32_t processId)
{
    FindMainWindowState state{processId, nullptr};
    EnumWindows(findProcessMainWindow, reinterpret_cast<LPARAM>(&state));
    if (!state.window)
        return;
    ShowWindow(state.window, SW_RESTORE);
    SetWindowPos(
        state.window, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (!SetForegroundWindow(state.window)) {
        FLASHWINFO flash{};
        flash.cbSize = sizeof(flash);
        flash.hwnd = state.window;
        flash.dwFlags = FLASHW_TRAY;
        flash.uCount = 1;
        FlashWindowEx(&flash);
    }
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
        constexpr std::wstring_view testUiRoundsPrefix =
            L"--test-ui-rounds=";
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
        } else if (argument == L"--test-hold") {
            options.testHold = true;
        } else if (argument == L"--test-toggle-receiving") {
            options.testToggleReceiving = true;
        } else if (argument ==
                   L"--test-toggle-receiving-after-first-frame") {
            options.testToggleReceiving = true;
            options.testToggleReceivingAfterFirstFrame = true;
        } else if (argument ==
                   L"--test-toggle-broadcast-after-first-frame") {
            options.testToggleBroadcast = true;
        } else if (argument == L"--test-persistent") {
            options.testPersistent = true;
        } else if (argument == L"--test-enable-ssdp") {
            options.testEnableSsdp = true;
        } else if (argument == L"--test-force-receiving-enabled") {
            options.testForceReceivingEnabled = true;
        } else if (argument == L"--background") {
            options.background = true;
        } else if (argument.starts_with(testUiRoundsPrefix)) {
            try {
                const unsigned long parsed = std::stoul(
                    argument.substr(testUiRoundsPrefix.size()));
                if (parsed == 0 || parsed > 8)
                    throw std::out_of_range("rounds");
                options.testUiRounds =
                    static_cast<std::uint32_t>(parsed);
            } catch (...) {
                error = "invalid --test-ui-rounds";
                return false;
            }
        } else {
            error = "unknown argument";
            return false;
        }
    }
    if (!safeInstanceId(options.instance)) {
        error = "invalid --instance";
        return false;
    }
    if (options.instance != "default" && !options.testMode) {
        error = "custom --instance requires --test";
        return false;
    }
    if (pathOverride && !options.testMode) {
        error = "executable path overrides require --test";
        return false;
    }
    if (options.testUiRounds != 1 && !options.testMode) {
        error = "--test-ui-rounds requires --test";
        return false;
    }
    if (options.testHold && !options.testMode) {
        error = "--test-hold requires --test";
        return false;
    }
    if (options.testToggleReceiving && !options.testMode) {
        error = "--test-toggle-receiving requires --test";
        return false;
    }
    if (options.testToggleBroadcast && !options.testMode) {
        error = "--test-toggle-broadcast-after-first-frame requires --test";
        return false;
    }
    if (options.testPersistent && !options.testMode) {
        error = "--test-persistent requires --test";
        return false;
    }
    if (options.testEnableSsdp && !options.testMode) {
        error = "--test-enable-ssdp requires --test";
        return false;
    }
    if (options.testForceReceivingEnabled && !options.testMode) {
        error = "--test-force-receiving-enabled requires --test";
        return false;
    }
    return true;
}

std::wstring shellMutexName(const Options &options)
{
    std::wstring name = L"Local\\MediaCast.Shell."
        + mcast::ipc::currentUserSid();
    name += L"." + widen(options.instance);
    return name;
}

void appendUInt32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
}

std::optional<std::uint32_t> readUInt32(
    std::span<const std::uint8_t> bytes, std::size_t &offset)
{
    if (offset + 4 > bytes.size())
        return std::nullopt;
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8)
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    return value;
}

std::vector<std::uint8_t>
encodeHandoffArguments(int argc, wchar_t **argv)
{
    std::vector<std::uint8_t> bytes;
    const std::uint32_t count =
        argc > 1 ? static_cast<std::uint32_t>(argc - 1) : 0;
    appendUInt32(bytes, count);
    for (int index = 1; index < argc; ++index) {
        const std::string argument = narrow(argv[index]);
        appendUInt32(bytes, static_cast<std::uint32_t>(argument.size()));
        bytes.insert(bytes.end(), argument.begin(), argument.end());
    }
    return bytes;
}

bool decodeHandoffArguments(
    std::span<const std::uint8_t> bytes,
    std::vector<std::string> &arguments, std::string &error)
{
    std::size_t offset = 0;
    const auto count = readUInt32(bytes, offset);
    if (!count || *count > 64) {
        error = "invalid Shell handoff argument count";
        return false;
    }
    arguments.clear();
    arguments.reserve(*count);
    for (std::uint32_t index = 0; index < *count; ++index) {
        const auto size = readUInt32(bytes, offset);
        if (!size || *size > 32768 || offset + *size > bytes.size()) {
            error = "invalid Shell handoff argument";
            return false;
        }
        arguments.emplace_back(
            reinterpret_cast<const char *>(bytes.data() + offset), *size);
        offset += *size;
    }
    if (offset != bytes.size()) {
        error = "Shell handoff argument payload has trailing bytes";
        return false;
    }
    return true;
}

std::optional<Message> makeHandoffRequest(
    int argc, wchar_t **argv, std::string &error)
{
    const DWORD cwdLength = GetCurrentDirectoryW(0, nullptr);
    std::vector<wchar_t> cwdBuffer(cwdLength + 1);
    if (cwdLength == 0
        || GetCurrentDirectoryW(
               static_cast<DWORD>(cwdBuffer.size()), cwdBuffer.data())
            == 0) {
        error = "cannot read current directory";
        return std::nullopt;
    }
    cwdBuffer.resize(cwdLength);

    const auto arguments = encodeHandoffArguments(argc, argv);
    mcast::ipc::PayloadBuilder builder;
    if (!builder.addBytes(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ShellHandoffArguments),
            arguments)
        || !builder.addString(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ShellHandoffWorkingDirectory),
            narrow(std::wstring(cwdBuffer.data(), cwdLength)))) {
        error = "cannot encode Shell handoff request";
        return std::nullopt;
    }

    Message request;
    request.header.type = MessageType::ShellHandoffRequest;
    request.header.source = ProcessRole::Shell;
    request.header.destination = ProcessRole::Shell;
    request.header.messageId = 1;
    request.header.sessionId = 1;
    request.payload = std::move(builder).finish().value_or(
        std::vector<std::uint8_t>{});
    if (!mcast::ipc::validateMessageDirection(
            ConnectionChannel::Control, request.header, error))
        return std::nullopt;
    return request;
}

bool sendHandoff(
    const Options &options, int argc, wchar_t **argv,
    std::string &error)
{
    const std::uint64_t handoffId =
        (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32)
        ^ GetTickCount64();
    std::string lastError;
    for (std::uint64_t attempt = 0; attempt < 10; ++attempt) {
        auto connection = mcast::ipc::PipeClient::connect(
            mcast::ipc::makeShellCommandPipeName(widen(options.instance)),
            500ms, lastError);
        if (!connection) {
            std::this_thread::sleep_for(100ms);
            continue;
        }
        if (!sameExecutableAsCurrent(
                connection->peerProcessId(), lastError)) {
            std::this_thread::sleep_for(100ms);
            continue;
        }
        AllowSetForegroundWindow(connection->peerProcessId());

        auto request = makeHandoffRequest(argc, argv, lastError);
        if (!request) {
            error = lastError;
            return false;
        }
        request->header.messageId = handoffId;
        if (!connection->writeMessage(*request, 1500ms, lastError)) {
            std::this_thread::sleep_for(100ms);
            continue;
        }

        Message ack;
        if (!connection->readMessage(ack, kHandoffAckTimeout, lastError)
            || ack.header.type != MessageType::ShellHandoffAck
            || ack.header.correlationId != request->header.messageId
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Control, ack.header, lastError)) {
            if (lastError.empty())
                lastError = "invalid Shell handoff acknowledgement";
            std::this_thread::sleep_for(100ms);
            continue;
        }

        const mcast::ipc::PayloadReader reader(ack.payload);
        const auto accepted = reader.getBool(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ShellHandoffAccepted));
        if (!reader.valid() || !accepted || !*accepted) {
            const auto reason = reader.getString(
                static_cast<std::uint16_t>(mcast::ipc::FieldId::ErrorText));
            const std::string rejection =
                reason.value_or("Shell handoff was rejected");
            Message complete;
            complete.header.type = MessageType::ShellHandoffComplete;
            complete.header.source = ProcessRole::Shell;
            complete.header.destination = ProcessRole::Shell;
            complete.header.messageId = handoffId;
            complete.header.correlationId = request->header.messageId;
            complete.header.sessionId = request->header.sessionId;
            connection->writeMessage(complete, 1000ms, lastError);
            Message finalAck;
            connection->readMessage(finalAck, 1500ms, lastError);
            error = rejection;
            return false;
        }
        Message complete;
        complete.header.type = MessageType::ShellHandoffComplete;
        complete.header.source = ProcessRole::Shell;
        complete.header.destination = ProcessRole::Shell;
        complete.header.messageId = handoffId;
        complete.header.correlationId = request->header.messageId;
        complete.header.sessionId = request->header.sessionId;
        if (!connection->writeMessage(complete, 1000ms, lastError)) {
            std::this_thread::sleep_for(100ms);
            continue;
        }

        Message finalAck;
        if (!connection->readMessage(finalAck, 1500ms, lastError)
            || finalAck.header.type != MessageType::ShellHandoffFinalAck
            || finalAck.header.correlationId != handoffId
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Control, finalAck.header, lastError)) {
            if (lastError.empty())
                lastError = "invalid final Shell handoff acknowledgement";
            std::this_thread::sleep_for(100ms);
            continue;
        }
        return true;
    }

    error = lastError.empty()
        ? "Shell command pipe is unavailable" : lastError;
    return false;
}

struct ChildRecord {
    ProcessRole role = ProcessRole::Unknown;
    mcast::supervisor::ChildProcess process;
    mcast::ipc::HelloExpectation expectation;
};

class ShellRuntime
{
public:
    ShellRuntime(Options options, HANDLE shellMutex)
        : m_options(std::move(options))
        , m_mutex(shellMutex)
        , m_splash([this](
              std::uint64_t token, SplashWindow::State state) {
            onSplashState(token, state);
        })
    {
    }

    ~ShellRuntime()
    {
        cleanup();
    }

    bool initialize(std::string &error)
    {
        const auto settingsDirectory =
            mcast::supervisor::currentExecutablePath().parent_path();
        const std::wstring settingsName =
            m_options.testMode
            ? L"MediaCast." + widen(m_options.instance) + L".json"
            : L"MediaCast.json";
        m_settings = std::make_unique<mcast::shell::ShellSettings>(
            settingsDirectory / settingsName);
        std::string settingsError;
        if (!m_settings->load(settingsError)) {
            std::cerr << "Settings load failed: " << settingsError << '\n';
        }
        std::string initialSettingsSaveError;
        if (!m_settings->save(initialSettingsSaveError)) {
            std::cerr << "Settings initial save failed: "
                      << initialSettingsSaveError << '\n';
        }
        m_language = std::make_unique<mcast::shell::ShellLanguage>(
            settingsDirectory / L"lang",
            [](const std::string &message) {
                std::cerr << "Language: " << message << '\n';
            });
        std::string languageError;
        if (!m_language->scan(languageError))
            std::cerr << "Language scan failed: "
                      << languageError << '\n';
        m_resolvedLanguage = m_language->resolve(
            m_settings->language(), languageError);
        if (!languageError.empty()) {
            std::cerr << "Language resolution: "
                      << languageError << '\n';
        }
        mcast::shell::TrayTexts shellTexts;
        if (!m_language->loadTrayTexts(
                m_resolvedLanguage, shellTexts, languageError)) {
            std::cerr << "Tray language load failed: "
                      << languageError << '\n';
            m_resolvedLanguage = "en_US";
            shellTexts = m_language->builtInEnglish();
        }
        m_trayTexts = toWideTrayTexts(shellTexts);
        if (!m_language->loadTerms(
                m_resolvedLanguage, m_terms, languageError)) {
            std::cerr << "Terminology load failed: "
                      << languageError << '\n';
            m_resolvedLanguage = "en_US";
            m_terms = m_language->builtInTerms();
        }
        m_splash.setTitle(widen(m_terms.appShort));
        m_receivingEnabled = m_settings->acceptNewCast();
        m_broadcasting = m_settings->broadcast();
        m_broadcastIntervalMs =
            static_cast<std::uint32_t>(
                m_settings->broadcastIntervalMs());
        if (m_options.testForceReceivingEnabled)
            m_receivingEnabled = true;
        const std::string darkMode = m_settings->darkMode();
        const mcast::platform::AccentPair accent =
            splashAccentColors(m_settings->accentMode());
        m_splash.setTheme(
            darkMode == "System", darkMode == "Dark");
        m_splash.setAccent(accent.light, accent.dark);

        m_nonce = mcast::supervisor::randomHexToken(16);
        if (!m_nonce) {
            error = "cannot generate instance nonce";
            return false;
        }

        const std::wstring instance = widen(m_options.instance);

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

        if (!launchProtocol(error)) {
            std::cerr << "Protocol launch failed: " << error << '\n';
            return false;
        }

        m_commandListener = std::make_unique<mcast::ipc::PipeListener>(
            mcast::ipc::makeShellCommandPipeName(instance), 1);
        if (!m_commandListener->valid()) {
            error = "Shell command pipe listener failed";
            std::cerr << error << '\n';
            return false;
        }
        m_commandThread = std::jthread([this](std::stop_token stop) {
            commandLoop(stop);
        });
        if (!m_options.testMode) {
            m_tray = std::make_unique<mcast::platform::NativeTrayIcon>(
                [this] {
                    std::string error;
                    if (!requestOpenUiFromHost(error))
                        std::cerr << "Tray open UI failed: "
                                  << error << '\n';
                },
                [this] { toggleReceiving(); },
                [this] { requestQuit(); },
                [this] {
                    std::lock_guard lock(m_settingsMutex);
                    const bool dark = effectiveDarkMode(
                        m_settings->darkMode());
                    const mcast::platform::AccentPair accent =
                        splashAccentColors(m_settings->accentMode());
                    m_tray->setDark(dark);
                    m_splash.setTheme(
                        m_settings->darkMode() == "System",
                        m_settings->darkMode() == "Dark");
                    m_splash.setAccent(accent.light, accent.dark);
                });
            m_tray->setTexts(m_trayTexts);
            m_tray->setDark(effectiveDarkMode(m_settings->darkMode()));
            m_tray->setReceivingEnabled(m_receivingEnabled);
            if (!m_tray->initialize(error))
                std::cerr << "Tray initialization failed: "
                          << error << '\n';
        }
        return true;
    }

    int run()
    {
        std::cout << "SHELL_READY instance=" << m_options.instance
                  << " pid=" << GetCurrentProcessId()
                  << std::endl;

        const auto startedAt = std::chrono::steady_clock::now();
        auto lastReceivingAttempt = startedAt - 1s;
        auto lastInitialReceivingAttempt = startedAt - 1s;
        auto lastReceivingReconcileAttempt = startedAt - 1s;
        bool receivingStateSynchronized = m_options.testMode;
        bool broadcastToggleDone = false;
        while (true) {
            if (m_tray)
                m_tray->pumpMessages();
            if (m_quitRequested.load())
                return 0;
            const auto now = std::chrono::steady_clock::now();
            if (!receivingStateSynchronized
                && now - startedAt > 250ms
                && now - lastInitialReceivingAttempt > 500ms) {
                lastInitialReceivingAttempt = now;
                bool effective = m_receivingEnabled;
                std::uint64_t stateVersion = 0;
                std::string syncError;
                if (sendReceivingEnabled(
                        m_receivingEnabled, effective,
                        stateVersion, syncError)) {
                    applyReceivingState(effective, stateVersion);
                    receivingStateSynchronized = true;
                } else if (now - startedAt > 5s) {
                    std::cerr << "Initial receiving state retry: "
                              << syncError << '\n';
                }
            }
            if (m_receivingReconcilePending.load()
                && now - lastReceivingReconcileAttempt > 500ms) {
                lastReceivingReconcileAttempt = now;
                bool effective = m_receivingEnabled;
                std::uint64_t stateVersion = 0;
                std::string reconcileError;
                if (sendReceivingEnabled(
                        m_receivingEnabled, effective,
                        stateVersion, reconcileError)) {
                    applyReceivingState(effective, stateVersion);
                    m_receivingReconcilePending = false;
                } else {
                    std::cerr << "Receiving reconciliation retry: "
                              << reconcileError << '\n';
                }
            }
            if (m_options.testToggleReceiving
                && now - startedAt > 10s) {
                std::cerr << "Receiving command test timed out\n";
                return 9;
            }
            const bool toggleReady =
                !m_options.testToggleReceivingAfterFirstFrame
                || m_firstFrameRounds.load() > 0;
            if (m_options.testToggleBroadcast && toggleReady
                && !broadcastToggleDone) {
                bool effective = m_broadcasting.load();
                std::uint32_t effectiveInterval =
                    m_broadcastIntervalMs.load();
                std::string broadcastError;
                if (sendDlnaBroadcast(
                        false, 30000, effective,
                        effectiveInterval, broadcastError)) {
                    m_broadcasting = effective;
                    m_broadcastIntervalMs = effectiveInterval;
                    broadcastToggleDone = true;
                    std::cout
                        << "SHELL_BROADCAST_TOGGLE_OK enabled="
                        << (effective ? "true" : "false")
                        << " interval_ms=" << effectiveInterval
                        << '\n';
                } else {
                    std::cerr << "Broadcast command retry: "
                              << broadcastError << '\n';
                }
            }
            if (m_options.testToggleReceiving && toggleReady
                && now - startedAt > 250ms
                && now - lastReceivingAttempt > 500ms) {
                lastReceivingAttempt = now;
                bool effective = true;
                std::uint64_t stateVersion = 0;
                std::string toggleError;
                if (sendReceivingEnabled(
                        false, effective, stateVersion, toggleError)) {
                    std::cout << "SHELL_RECEIVING_TOGGLE_OK enabled="
                              << (effective ? "true" : "false") << '\n';
                    return 0;
                }
                std::cerr << "Receiving command retry: "
                          << toggleError << '\n';
            }
            std::string acceptError;
            auto accepted = m_listener->accept(200ms, acceptError);
            if (accepted) {
                if (m_connections.size() >= 8) {
                    std::cerr << "connection limit reached\n";
                } else {
                    auto done = std::make_shared<std::atomic_bool>(false);
                    auto slot = std::make_unique<ConnectionSlot>();
                    slot->done = done;
                    auto connection = std::shared_ptr<mcast::ipc::PipeConnection>(
                        std::move(accepted));
                    slot->thread = std::jthread(
                        [this, connection = std::move(connection),
                         done]() mutable {
                        handleConnection(connection);
                        done->store(true);
                    });
                    m_connections.push_back(std::move(slot));
                }
            } else if (!acceptError.empty()
                       && acceptError != "pipe accept timed out") {
                std::cerr << "accept: " << acceptError << '\n';
            }
            reapConnections();

            if (m_splashFailed.load()) {
                std::cerr << "Shell cannot continue without splash\n";
                return 8;
            }

            if (m_options.testExitAfterFirstFrame
                && !m_options.testHold
                && m_firstFrameRounds.load() >= m_options.testUiRounds
                && m_splashShownRounds.load() >= m_options.testUiRounds
                && m_splashHiddenRounds.load() > 0
                && !m_splashVisible.load()
                && WaitForSingleObject(m_firstFrame, 0) == WAIT_OBJECT_0) {
                std::cout << "SHELL_FIRST_FRAME_OK\n";
                std::cout << "SHELL_UI_ROUNDS_OK rounds="
                          << m_firstFrameRounds.load() << '\n';
                return 0;
            }

            {
                std::scoped_lock lock(m_uiMutex);
                if (m_uiState == UiState::Running) {
                    std::scoped_lock recordsLock(m_recordsMutex);
                    ChildRecord *ui = findRecord(ProcessRole::Ui);
                    if (ui && !ui->process.running()) {
                        std::cerr << "UI process exited\n";
                        std::string terminateError;
                        ui->process.terminate(0, 1000ms, terminateError);
                        m_uiState = UiState::None;
                        m_splash.hideImmediately(m_activationId);
                        m_records.erase(std::remove_if(
                            m_records.begin(), m_records.end(),
                            [](const std::unique_ptr<ChildRecord> &record) {
                                return record->role == ProcessRole::Ui;
                            }),
                            m_records.end());
                    }
                }
                if (m_uiState == UiState::Running
                    && WaitForSingleObject(m_firstFrame, 0) != WAIT_OBJECT_0
                    && std::chrono::steady_clock::now() - m_uiStartedAt
                        > 15000ms) {
                    std::cerr
                        << "UI FirstFrame timed out; retiring Ui "
                           "without stopping Shell\n";
                    m_splash.hideImmediately(m_activationId);
                    ChildRecord *ui = findRecord(ProcessRole::Ui);
                    if (ui) {
                        std::string terminateError;
                        ui->process.terminate(0, 1000ms, terminateError);
                    }
                    m_records.erase(std::remove_if(
                        m_records.begin(), m_records.end(),
                        [](const std::unique_ptr<ChildRecord> &record) {
                            return record->role == ProcessRole::Ui;
                        }),
                        m_records.end());
                    m_uiState = UiState::None;
                }
            }

        }
    }

private:
    struct ConnectionSlot {
        std::jthread thread;
        std::shared_ptr<std::atomic_bool> done;
    };

    struct ConfirmationWaiter {
        std::mutex mutex;
        std::condition_variable readyCondition;
        std::chrono::steady_clock::time_point deadline{};
        bool ready = false;
        bool accepted = false;
        bool abandoned = false;
        bool cancelled = false;
    };

    struct ConfirmationThreadSlot {
        std::jthread thread;
        std::shared_ptr<std::atomic_bool> done;
    };

    struct SettingsPatchEffect {
        bool stale = false;
        bool previousReceiving = false;
        bool previousBroadcast = false;
        std::uint32_t previousBroadcastInterval = 10000;
        bool currentReceiving = false;
        bool currentBroadcast = false;
        std::uint32_t currentBroadcastInterval = 10000;
    };

    struct PendingUiConfirmation {
        std::shared_ptr<ConfirmationWaiter> waiter;
        std::shared_ptr<mcast::ipc::PipeConnection> uiConnection;
        std::uint64_t protocolGeneration = 0;
        std::uint64_t uiGeneration = 0;
        mcast::cast::ProtocolKind protocol =
            mcast::cast::ProtocolKind::None;
        std::uint64_t claimId = 0;
        std::uint64_t protocolRequestMessageId = 0;
        std::uint64_t uiRequestMessageId = 0;
        std::chrono::steady_clock::time_point deadline{};
    };

    struct ConfirmationCacheEntry {
        std::uint64_t requestMessageId = 0;
        std::uint64_t protocolGeneration = 0;
        mcast::cast::ProtocolKind protocol = mcast::cast::ProtocolKind::None;
        std::uint64_t claimId = 0;
        bool accepted = false;
    };

    struct ConfirmationCacheLookup {
        bool found = false;
        bool conflict = false;
        bool accepted = false;
    };

    struct ProtocolConnectionGuard {
        ProtocolConnectionGuard(
            ShellRuntime *runtime,
            std::shared_ptr<mcast::ipc::PipeConnection> peer)
            : owner(runtime)
            , connection(std::move(peer))
        {
        }

        ProtocolConnectionGuard(const ProtocolConnectionGuard &) = delete;
        ProtocolConnectionGuard &operator=(
            const ProtocolConnectionGuard &) = delete;

        ~ProtocolConnectionGuard()
        {
            if (owner)
                owner->clearProtocolConnection(connection);
        }

        ShellRuntime *owner = nullptr;
        std::shared_ptr<mcast::ipc::PipeConnection> connection;
    };

    struct UiConnectionGuard {
        UiConnectionGuard(
            ShellRuntime *runtime,
            std::shared_ptr<mcast::ipc::PipeConnection> peer)
            : owner(runtime)
            , connection(std::move(peer))
        {
        }

        UiConnectionGuard(const UiConnectionGuard &) = delete;
        UiConnectionGuard &operator=(const UiConnectionGuard &) = delete;

        ~UiConnectionGuard()
        {
            if (owner)
                owner->clearUiControlConnection(connection);
        }

        ShellRuntime *owner = nullptr;
        std::shared_ptr<mcast::ipc::PipeConnection> connection;
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

    void commandLoop(std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            std::string error;
            // A short polling accept closes and recreates the pending pipe
            // instance between waits; a second Shell can hit that tiny window.
            // The listener's stop event still interrupts this wait at shutdown.
            auto connection = m_commandListener->accept(24h, error);
            if (connection) {
                handleHandoffConnection(std::move(connection));
            } else if (!error.empty()
                       && error != "pipe accept timed out"
                       && !stop.stop_requested()) {
                std::cerr << "Shell command accept: " << error << '\n';
            }
        }
    }

    std::uint64_t registerProtocolConnection(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection)
    {
        {
            std::scoped_lock lock(m_protocolMutex);
            m_protocolConnection = connection;
            m_protocolAcceptsUiOpen = false;
        }
        const std::uint64_t generation =
            invalidateProtocolConfirmations();
        m_protocolReady.notify_all();
        return generation;
    }

    std::uint64_t registerUiControlConnection(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection)
    {
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(m_uiControlMutex);
            m_uiControlConnection = connection;
            generation = ++m_uiGeneration;
        }
        invalidateUiConfirmations(generation);
        m_uiControlChanged.notify_all();
        return generation;
    }

    void clearUiControlConnection(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection)
    {
        bool wasCurrent = false;
        {
            std::scoped_lock lock(m_uiControlMutex);
            if (m_uiControlConnection == connection) {
                m_uiControlConnection.reset();
                ++m_uiGeneration;
                wasCurrent = true;
            }
        }
        if (wasCurrent) {
            std::uint64_t generation = 0;
            {
                std::scoped_lock lock(m_uiControlMutex);
                generation = m_uiGeneration;
            }
            invalidateUiConfirmations(generation);
            m_uiControlChanged.notify_all();
        }
    }

    bool currentUiConnection(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection,
        std::uint64_t generation)
    {
        std::scoped_lock lock(m_uiControlMutex);
        return m_uiControlConnection == connection
            && m_uiGeneration == generation;
    }

    void clearProtocolConnection(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection)
    {
        bool wasCurrent = false;
        {
            std::scoped_lock lock(m_protocolMutex);
            if (m_protocolConnection == connection) {
                m_protocolConnection.reset();
                m_protocolAcceptsUiOpen = false;
                wasCurrent = true;
            }
        }
        if (wasCurrent)
            (void)invalidateProtocolConfirmations();
        m_protocolReady.notify_all();
    }

    void sendConfirmationCancels(
        const std::vector<std::pair<
            std::shared_ptr<mcast::ipc::PipeConnection>,
            std::uint64_t>> &targets)
    {
        for (const auto &[uiConnection, uiRequestId] : targets) {
            if (!uiConnection)
                continue;
            std::string error;
            const auto cancel =
                mcast::ipc::makeUiCastConfirmationCancelMessage(
                    nextMessageId(), uiRequestId, error);
            if (!cancel
                || !uiConnection->writeMessage(
                    *cancel, 2000ms, error)) {
                std::cerr << "UI confirmation cancel failed: "
                          << error << '\n';
            }
        }
    }

    std::uint64_t invalidateProtocolConfirmations()
    {
        std::uint64_t generation = 0;
        std::vector<std::pair<
            std::shared_ptr<mcast::ipc::PipeConnection>,
            std::uint64_t>> targets;
        {
            std::scoped_lock lock(m_confirmationMutex);
            generation = ++m_protocolGeneration;
            for (const auto &[id, pending] : m_pendingUiConfirmations) {
                targets.emplace_back(pending.uiConnection, id);
                {
                    std::scoped_lock waiterLock(pending.waiter->mutex);
                    pending.waiter->accepted = false;
                    pending.waiter->ready = true;
                }
                pending.waiter->readyCondition.notify_all();
            }
            m_pendingUiConfirmations.clear();
            m_cancelledProtocolConfirmations.clear();
            m_recentConfirmationDecisions.clear();
        }
        sendConfirmationCancels(targets);
        return generation;
    }

    void invalidateUiConfirmations(std::uint64_t currentGeneration)
    {
        std::vector<std::pair<
            std::shared_ptr<mcast::ipc::PipeConnection>,
            std::uint64_t>> targets;
        {
            std::scoped_lock lock(m_confirmationMutex);
            for (auto it = m_pendingUiConfirmations.begin();
                 it != m_pendingUiConfirmations.end();) {
                const auto &[id, pending] = *it;
                if (pending.uiGeneration >= currentGeneration) {
                    ++it;
                    continue;
                }
                targets.emplace_back(pending.uiConnection, id);
                {
                    std::scoped_lock waiterLock(pending.waiter->mutex);
                    pending.waiter->accepted = false;
                    pending.waiter->ready = true;
                }
                pending.waiter->readyCondition.notify_all();
                it = m_pendingUiConfirmations.erase(it);
            }
            m_recentConfirmationDecisions.clear();
        }
        sendConfirmationCancels(targets);
    }

    void markProtocolReadyForUiOpen()
    {
        {
            std::scoped_lock lock(m_protocolMutex);
            m_protocolAcceptsUiOpen = true;
        }
        m_protocolReady.notify_all();
    }

    bool requestProtocolOpenUi(std::string &error)
    {
        std::shared_ptr<mcast::ipc::PipeConnection> connection;
        {
            std::unique_lock lock(m_protocolMutex);
            if (!m_protocolReady.wait_for(
                    lock, kUiOpenReadyTimeout, [this] {
                        return m_protocolAcceptsUiOpen
                            && static_cast<bool>(m_protocolConnection);
                    })) {
                error = "Protocol is not ready for a Ui request";
                return false;
            }
            connection = m_protocolConnection;
            m_protocolAcceptsUiOpen = false;
        }

        Message request;
        request.header.type = MessageType::UiOpenRequest;
        request.header.source = ProcessRole::Shell;
        request.header.destination = ProcessRole::Protocol;
        request.header.messageId = GetTickCount64();
        if (request.header.messageId == 0)
            request.header.messageId = 1;
        request.header.sessionId = 1;
        if (!connection->writeMessage(request, 2000ms, error)) {
            std::scoped_lock lock(m_protocolMutex);
            if (m_protocolConnection == connection)
                m_protocolAcceptsUiOpen = true;
            return false;
        }
        return true;
    }

    std::optional<bool> requestCastConfirmation(
        mcast::ipc::CastConfirmationRequestInfo request,
        std::uint64_t requestMessageId, std::uint64_t generation)
    {
        if (m_shuttingDown.load())
            return std::nullopt;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(request.remainingTimeoutMs);
        if (std::chrono::steady_clock::now() >= deadline)
            return false;

        std::shared_ptr<mcast::ipc::PipeConnection> uiConnection;
        std::uint64_t uiGeneration = 0;

        auto waiter = std::make_shared<ConfirmationWaiter>();
        waiter->deadline = deadline;
        const std::uint64_t uiRequestMessageId = nextMessageId();
        request.remainingTimeoutMs =
            static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count());
        if (request.remainingTimeoutMs == 0)
            return false;

        std::string error;
        const auto uiRequest = mcast::ipc::makeUiCastConfirmationRequestMessage(
            uiRequestMessageId, requestMessageId, request, error);
        if (!uiRequest)
            return false;

        {
            std::scoped_lock uiLock(m_uiControlMutex);
            uiConnection = m_uiControlConnection;
            uiGeneration = m_uiGeneration;
            if (!uiConnection)
                return false;
            std::scoped_lock lock(m_confirmationMutex);
            if (generation != m_protocolGeneration)
                return std::nullopt;
            if (m_cancelledProtocolConfirmations.erase(
                    requestMessageId)
                != 0) {
                return std::nullopt;
            }
            m_pendingUiConfirmations.emplace(
                uiRequestMessageId,
                PendingUiConfirmation{
                    waiter,
                    uiConnection,
                    generation,
                    uiGeneration,
                    request.protocol,
                    request.claimId,
                    requestMessageId,
                    uiRequestMessageId,
                    deadline,
                });
        }
        if (!uiConnection->writeMessage(*uiRequest, 2000ms, error)) {
            std::scoped_lock lock(m_confirmationMutex);
            m_pendingUiConfirmations.erase(uiRequestMessageId);
            return false;
        }

        std::unique_lock lock(waiter->mutex);
        if (!waiter->readyCondition.wait_until(
                lock, deadline, [&] {
                    return waiter->ready || m_shuttingDown.load();
                })) {
            waiter->abandoned = true;
            lock.unlock();
            {
                std::scoped_lock confirmationLock(m_confirmationMutex);
                m_pendingUiConfirmations.erase(uiRequestMessageId);
            }
            std::string cancelError;
            const auto cancel =
                mcast::ipc::makeUiCastConfirmationCancelMessage(
                    nextMessageId(), uiRequestMessageId, cancelError);
            if (cancel
                && !uiConnection->writeMessage(
                    *cancel, 2000ms, cancelError)) {
                std::cerr << "UI confirmation cancel failed: "
                          << cancelError << '\n';
            }
            return false;
        }
        if (!waiter->ready)
            return std::nullopt;
        if (waiter->cancelled)
            return std::nullopt;
        const bool accepted = waiter->accepted;
        lock.unlock();
        {
            std::scoped_lock confirmationLock(m_confirmationMutex);
            if (generation != m_protocolGeneration)
                return std::nullopt;
            m_pendingUiConfirmations.erase(uiRequestMessageId);
        }
        if (!currentUiConnection(uiConnection, uiGeneration))
            return std::nullopt;
        return accepted;
    }

    void startProtocolConfirmation(
        mcast::ipc::CastConfirmationRequestInfo request,
        std::uint64_t requestMessageId, std::uint64_t generation,
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection)
    {
        reapConfirmationThreads();
        auto done = std::make_shared<std::atomic_bool>(false);
        std::lock_guard lock(m_confirmationThreadsMutex);
        if (!m_acceptConfirmationThreads || m_shuttingDown.load())
            return;
        ConfirmationThreadSlot slot;
        slot.done = done;
        slot.thread = std::jthread(
            [this, request = std::move(request), requestMessageId,
             generation, connection,
             done](std::stop_token stop) mutable {
                struct DoneGuard {
                    std::shared_ptr<std::atomic_bool> done;
                    ~DoneGuard() { done->store(true); }
                } doneGuard{done};
                if (stop.stop_requested())
                    return;
                const auto result = requestCastConfirmation(
                    request, requestMessageId, generation);
                if (!result || stop.stop_requested())
                    return;
                const bool accepted = *result;
                rememberConfirmationDecision(
                    requestMessageId, request.protocol,
                    request.claimId, accepted, generation);
                const mcast::ipc::CastConfirmationDecisionInfo decision{
                    request.protocol,
                    request.claimId,
                    accepted,
                };
                std::string error;
                const auto decisionMessage =
                    mcast::ipc::makeCastConfirmationDecisionMessage(
                        nextMessageId(), requestMessageId,
                        decision, error);
                if (!decisionMessage
                    || !connection->writeMessage(
                        *decisionMessage, 2000ms, error)) {
                    std::cerr << "cast confirmation decision: "
                              << error << '\n';
                    return;
                }
                std::cout << "SHELL_CAST_CONFIRMATION claim="
                          << request.claimId
                          << " protocol="
                          << static_cast<int>(request.protocol)
                          << " accepted=" << accepted
                          << '\n';
            });
        m_confirmationThreads.push_back(std::move(slot));
    }

    void reapConfirmationThreads()
    {
        std::vector<std::jthread> completed;
        {
            std::scoped_lock lock(m_confirmationThreadsMutex);
            for (auto it = m_confirmationThreads.begin();
                 it != m_confirmationThreads.end();) {
                if (!it->done || !it->done->load()) {
                    ++it;
                    continue;
                }
                completed.push_back(std::move(it->thread));
                it = m_confirmationThreads.erase(it);
            }
        }
        for (auto &thread : completed) {
            if (thread.joinable())
                thread.join();
        }
    }

    void cancelProtocolConfirmation(
        std::uint64_t requestMessageId,
        std::uint64_t generation)
    {
        std::shared_ptr<mcast::ipc::PipeConnection> uiConnection;
        std::uint64_t uiRequestMessageId = 0;
        std::shared_ptr<ConfirmationWaiter> waiter;
        {
            std::scoped_lock lock(m_confirmationMutex);
            const auto found = std::find_if(
                m_pendingUiConfirmations.begin(),
                m_pendingUiConfirmations.end(),
                [&](const auto &entry) {
                    return entry.second.protocolGeneration
                            == generation
                        && entry.second.protocolRequestMessageId
                            == requestMessageId;
                });
            if (found == m_pendingUiConfirmations.end()) {
                m_cancelledProtocolConfirmations.insert(
                    requestMessageId);
            } else {
                uiConnection = found->second.uiConnection;
                uiRequestMessageId =
                    found->second.uiRequestMessageId;
                waiter = found->second.waiter;
                {
                    std::scoped_lock waiterLock(waiter->mutex);
                    waiter->cancelled = true;
                    waiter->accepted = false;
                    waiter->ready = true;
                }
                m_pendingUiConfirmations.erase(found);
            }
        }
        if (!waiter)
            return;
        waiter->readyCondition.notify_all();
        sendConfirmationCancels({{uiConnection, uiRequestMessageId}});
    }

    void completeUiConfirmation(
        const Message &message,
        const std::shared_ptr<mcast::ipc::PipeConnection> &source,
        std::uint64_t uiGeneration)
    {
        if (!currentUiConnection(source, uiGeneration))
            return;
        std::string error;
        const auto decision =
            mcast::ipc::decodeUiCastConfirmationDecisionMessage(
                message, message.header.correlationId, error);
        if (!decision) {
            std::cerr << "UI cast confirmation decision: "
                      << error << '\n';
            return;
        }

        std::shared_ptr<ConfirmationWaiter> waiter;
        {
            std::scoped_lock lock(m_confirmationMutex);
            const auto it = m_pendingUiConfirmations.find(
                message.header.correlationId);
            if (it == m_pendingUiConfirmations.end()
                || it->second.protocolGeneration
                    != m_protocolGeneration
                || it->second.uiConnection != source
                || it->second.uiGeneration != uiGeneration
                || it->second.protocol != decision->protocol
                || it->second.claimId != decision->claimId) {
                return;
            }
            waiter = it->second.waiter;
            m_pendingUiConfirmations.erase(it);
        }
        {
            std::scoped_lock lock(waiter->mutex);
            waiter->accepted = decision->accepted;
            waiter->ready = true;
        }
        waiter->readyCondition.notify_all();
    }

    void cancelCastConfirmations()
    {
        m_shuttingDown.store(true);
        (void)invalidateProtocolConfirmations();
    }

    ConfirmationCacheLookup lookupConfirmationDecision(
        std::uint64_t requestMessageId,
        mcast::cast::ProtocolKind protocol, std::uint64_t claimId,
        std::uint64_t generation)
    {
        std::scoped_lock lock(m_confirmationMutex);
        const auto it = std::find_if(
            m_recentConfirmationDecisions.begin(),
            m_recentConfirmationDecisions.end(),
            [&](const auto &entry) {
                return entry.requestMessageId == requestMessageId
                    && entry.protocolGeneration == generation;
            });
        if (it == m_recentConfirmationDecisions.end())
            return {};
        if (it->protocol != protocol || it->claimId != claimId)
            return {false, true, false};
        return {true, false, it->accepted};
    }

    bool rememberConfirmationDecision(
        std::uint64_t requestMessageId, mcast::cast::ProtocolKind protocol,
        std::uint64_t claimId, bool accepted, std::uint64_t generation)
    {
        std::scoped_lock lock(m_confirmationMutex);
        if (generation != m_protocolGeneration)
            return false;
        constexpr std::size_t maxRecent = 32;
        if (m_recentConfirmationDecisions.size() >= maxRecent)
            m_recentConfirmationDecisions.pop_front();
        m_recentConfirmationDecisions.push_back(
            {requestMessageId, generation, protocol, claimId, accepted});
        return true;
    }

    std::uint64_t nextMessageId() noexcept
    {
        std::uint64_t id = m_nextMessageId.fetch_add(1);
        if (id == 0)
            id = m_nextMessageId.fetch_add(1);
        return id;
    }

    std::optional<std::uint32_t> liveUiProcessId()
    {
        std::scoped_lock lock(m_uiMutex);
        if (m_uiState != UiState::Running
            && m_uiState != UiState::Starting)
            return std::nullopt;
        std::scoped_lock recordsLock(m_recordsMutex);
        ChildRecord *ui = findRecord(ProcessRole::Ui);
        if (!ui || !ui->process.running())
            return std::nullopt;
        return ui->process.processId();
    }

    void uiPresenceSnapshot(
        bool &present, std::uint64_t &activationId,
        std::uint64_t &nextActivationId)
    {
        std::scoped_lock lock(m_uiMutex);
        activationId = m_activationId;
        nextActivationId = m_nextActivationId;
        present = m_uiState == UiState::Running
            || m_uiState == UiState::Starting;
        if (!present)
            return;
        std::scoped_lock recordsLock(m_recordsMutex);
        ChildRecord *ui = findRecord(ProcessRole::Ui);
        present = ui && ui->process.running();
    }

    bool requestOpenUiFromHost(std::string &error)
    {
        const auto uiProcessId = liveUiProcessId();
        if (uiProcessId) {
            bringProcessWindowToFront(*uiProcessId);
            return true;
        }
        if (requestProtocolOpenUi(error))
            return true;
        // A foreground startup may already have an UiActivateRequest in
        // flight through Protocol. Wait for that child to appear instead of
        // starting a second Ui without a Protocol-ready gate.
        const auto deadline =
            std::chrono::steady_clock::now() + 4000ms;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(50ms);
            const auto startedUi = liveUiProcessId();
            if (startedUi) {
                bringProcessWindowToFront(*startedUi);
                error.clear();
                return true;
            }
        }
        return false;
    }

    void requestQuit()
    {
        m_quitRequested.store(true);
    }

    void toggleReceiving()
    {
        const bool desired = !m_receivingEnabled;
        bool effective = m_receivingEnabled;
        std::uint64_t stateVersion = 0;
        std::string error;
        if (sendReceivingEnabled(
                desired, effective, stateVersion, error)) {
            applyReceivingState(effective, stateVersion);
        } else {
            std::cerr << "Receiving state command failed: "
                      << error << '\n';
        }
    }

    void applyReceivingState(
        bool enabled, std::uint64_t stateVersion)
    {
        bool previous = m_receivingEnabled.load();
        bool settingsChanged = false;
        bool persistenceFailed = false;
        std::string saveError;
        {
            std::lock_guard lock(m_receivingStateMutex);
            if (stateVersion < m_receivingStateVersion)
                return;
            previous = m_receivingEnabled.load();
            if (previous != enabled && m_settings) {
                std::lock_guard settingsLock(m_settingsMutex);
                if (m_settings->acceptNewCast() == enabled) {
                    settingsChanged = false;
                } else if (m_settings->setAcceptNewCastAndSave(
                               enabled, saveError)) {
                    ++m_settingsStateVersion;
                    settingsChanged = true;
                } else {
                    persistenceFailed = true;
                }
            }
            if (!persistenceFailed) {
                m_receivingStateVersion = stateVersion;
                m_receivingEnabled = enabled;
                if (m_tray)
                    m_tray->requestReceivingEnabled(enabled);
            }
        }
        if (persistenceFailed) {
            std::cerr << "Receiving setting save failed: "
                      << saveError << '\n';
            bool rollbackEffective = previous;
            std::uint64_t rollbackVersion = stateVersion;
            std::string rollbackError;
            if (sendReceivingEnabled(
                    previous, rollbackEffective,
                    rollbackVersion, rollbackError)) {
                m_receivingReconcilePending = false;
                std::lock_guard lock(m_receivingStateMutex);
                if (rollbackVersion >= m_receivingStateVersion) {
                    m_receivingStateVersion = rollbackVersion;
                    m_receivingEnabled = rollbackEffective;
                    if (m_tray)
                        m_tray->requestReceivingEnabled(
                            rollbackEffective);
                }
            } else {
                m_receivingReconcilePending = true;
                std::cerr << "Receiving state rollback failed: "
                          << rollbackError << '\n';
            }
            return;
        }
        if (settingsChanged)
            broadcastSettingsSnapshot();
    }

    bool sendReceivingEnabled(
        bool enabled, bool &effective,
        std::uint64_t &stateVersion, std::string &error)
    {
        std::uint32_t protocolProcessId = 0;
        std::string protocolHash;
        {
            std::scoped_lock lock(m_recordsMutex);
            ChildRecord *protocol = findRecord(ProcessRole::Protocol);
            if (!protocol || !protocol->process.running()) {
                error = "Protocol is not running";
                return false;
            }
            protocolProcessId = protocol->expectation.peerProcessId;
            protocolHash = protocol->expectation.executableHash;
        }

        auto connection = mcast::ipc::PipeClient::connect(
            mcast::ipc::makeProtocolCommandPipeName(
                widen(m_options.instance)),
            1000ms, error);
        if (!connection)
            return false;
        if (connection->peerProcessId() != protocolProcessId) {
            error = "Protocol command server PID mismatch";
            return false;
        }
        const auto path =
            mcast::supervisor::queryProcessPath(protocolProcessId);
        const auto hash = path
            ? mcast::supervisor::sha256File(*path) : std::nullopt;
        if (!hash || *hash != protocolHash) {
            error = "Protocol command server executable mismatch";
            return false;
        }

        mcast::ipc::PayloadBuilder builder;
        builder.addBool(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ReceivingEnabled),
            enabled);
        Message request;
        request.header.type = MessageType::SetReceivingEnabled;
        request.header.source = ProcessRole::Shell;
        request.header.destination = ProcessRole::Protocol;
        request.header.messageId = GetTickCount64();
        if (request.header.messageId == 0)
            request.header.messageId = 1;
        request.header.sessionId = 1;
        request.payload = std::move(builder).finish().value_or(
            std::vector<std::uint8_t>{});
        if (!connection->writeMessage(request, 1500ms, error))
            return false;

        Message state;
        if (!connection->readMessage(state, 4000ms, error)
            || state.header.type != MessageType::ReceivingState
            || state.header.correlationId != request.header.messageId
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Command, state.header, error)) {
            if (error.empty())
                error = "invalid receiving state response";
            return false;
        }
        const mcast::ipc::PayloadReader reader(state.payload);
        const auto value = reader.getBool(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ReceivingEnabled));
        if (!reader.valid() || !value) {
            error = "receiving state response has no value";
            return false;
        }
        effective = *value;
        stateVersion = state.header.stateVersion;
        return true;
    }

    bool sendSettingsSnapshot(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection,
        std::string &error)
    {
        if (!connection || !m_settings)
            return false;
        std::uint64_t version = 0;
        std::string snapshot;
        std::string resolvedLanguage;
        {
            std::lock_guard lock(m_settingsMutex);
            version = m_settingsStateVersion.load();
            snapshot = m_settings->snapshotJson();
            resolvedLanguage = m_resolvedLanguage;
        }
        auto message = mcast::ipc::makeSettingsSnapshotMessage(
            nextMessageId(), version,
            snapshot, resolvedLanguage, error);
        return message
            && connection->writeMessage(*message, 2000ms, error);
    }

    bool sendLanguageCatalog(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection,
        std::string &error)
    {
        if (!connection || !m_language)
            return false;
        std::string catalog;
        {
            std::lock_guard lock(m_settingsMutex);
            catalog = m_language->catalogJson(
                m_resolvedLanguage, error);
        }
        if (catalog.empty())
            return false;
        auto message = mcast::ipc::makeLanguageCatalogMessage(
            nextMessageId(), catalog, error);
        return message
            && connection->writeMessage(*message, 2000ms, error);
    }

    void broadcastSettingsSnapshot()
    {
        std::shared_ptr<mcast::ipc::PipeConnection> connection;
        {
            std::scoped_lock lock(m_uiControlMutex);
            connection = m_uiControlConnection;
        }
        if (!connection)
            return;
        std::string error;
        if (!sendSettingsSnapshot(connection, error))
            std::cerr << "Settings snapshot broadcast failed: "
                      << error << '\n';
    }

    bool sendSettingsUpdateResult(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection,
        std::uint64_t correlationId,
        std::uint64_t stateVersion,
        const std::string &resolvedLanguage,
        mcast::ipc::SettingsUpdateStatus status,
        const std::string &errorText, std::string &error)
    {
        const auto result = mcast::ipc::makeSettingsUpdateResultMessage(
            nextMessageId(), correlationId, stateVersion,
            mcast::ipc::SettingsUpdateResultInfo{
                status, resolvedLanguage, errorText},
            error);
        return result
            && connection
            && connection->writeMessage(*result, 2000ms, error);
    }

    bool sendDlnaBroadcast(
        bool enabled, std::uint32_t intervalMs,
        bool &effective, std::uint32_t &effectiveInterval,
        std::string &error)
    {
        std::uint32_t protocolProcessId = 0;
        std::string protocolHash;
        {
            std::scoped_lock lock(m_recordsMutex);
            ChildRecord *protocol = findRecord(ProcessRole::Protocol);
            if (!protocol || !protocol->process.running()) {
                error = "Protocol is not running";
                return false;
            }
            protocolProcessId = protocol->expectation.peerProcessId;
            protocolHash = protocol->expectation.executableHash;
        }

        auto connection = mcast::ipc::PipeClient::connect(
            mcast::ipc::makeProtocolCommandPipeName(
                widen(m_options.instance)),
            1000ms, error);
        if (!connection)
            return false;
        if (connection->peerProcessId() != protocolProcessId) {
            error = "Protocol command server PID mismatch";
            return false;
        }
        const auto path =
            mcast::supervisor::queryProcessPath(protocolProcessId);
        const auto hash = path
            ? mcast::supervisor::sha256File(*path) : std::nullopt;
        if (!hash || *hash != protocolHash) {
            error = "Protocol command server executable mismatch";
            return false;
        }

        mcast::ipc::PayloadBuilder builder;
        builder.addBool(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::BroadcastEnabled),
            enabled);
        builder.addUInt64(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::BroadcastIntervalMs),
            intervalMs);
        Message request;
        request.header.type = MessageType::SetDlnaBroadcast;
        request.header.source = ProcessRole::Shell;
        request.header.destination = ProcessRole::Protocol;
        request.header.messageId = GetTickCount64();
        if (request.header.messageId == 0)
            request.header.messageId = 1;
        request.header.sessionId = 1;
        request.payload = std::move(builder).finish().value_or(
            std::vector<std::uint8_t>{});
        if (!connection->writeMessage(request, 1500ms, error))
            return false;

        Message state;
        if (!connection->readMessage(state, 4000ms, error)
            || state.header.type != MessageType::DlnaBroadcastState
            || state.header.correlationId != request.header.messageId
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Command, state.header, error)) {
            if (error.empty())
                error = "invalid broadcast state response";
            return false;
        }
        const mcast::ipc::PayloadReader reader(state.payload);
        const auto value = reader.getBool(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::BroadcastEnabled));
        const auto interval = reader.getUInt64(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::BroadcastIntervalMs));
        if (!reader.valid() || !value || !interval
            || *interval > std::numeric_limits<std::uint32_t>::max()) {
            error = "broadcast state response is invalid";
            return false;
        }
        effective = *value;
        effectiveInterval =
            static_cast<std::uint32_t>(*interval);
        return true;
    }

    bool applySettingsPatch(
        std::string_view json, std::uint64_t baseVersion,
        SettingsPatchEffect &effect, std::string &error)
    {
        if (!m_settings)
            return false;
        std::lock_guard lock(m_settingsMutex);
        if (baseVersion != m_settingsStateVersion.load()) {
            effect.stale = true;
            error = "stale settings version";
            return false;
        }
        effect.previousReceiving = m_settings->acceptNewCast();
        effect.previousBroadcast = m_settings->broadcast();
        effect.previousBroadcastInterval =
            static_cast<std::uint32_t>(
                m_settings->broadcastIntervalMs());
        if (!m_settings->applyJsonPatchAndSave(json, error))
            return false;
        ++m_settingsStateVersion;
        effect.currentReceiving = m_settings->acceptNewCast();
        effect.currentBroadcast = m_settings->broadcast();
        effect.currentBroadcastInterval =
            static_cast<std::uint32_t>(
                m_settings->broadcastIntervalMs());
        const std::string previousLanguage = m_resolvedLanguage;
        std::string languageError;
        m_resolvedLanguage = m_language->resolve(
            m_settings->language(), languageError);
        if (!languageError.empty()) {
            std::cerr << "Language resolution: "
                      << languageError << '\n';
        }
        mcast::shell::TrayTexts shellTexts;
        if (!m_language->loadTrayTexts(
                m_resolvedLanguage, shellTexts, languageError)) {
            std::cerr << "Tray language load failed: "
                      << languageError << '\n';
            m_resolvedLanguage = "en_US";
            shellTexts = m_language->builtInEnglish();
        }
        m_trayTexts = toWideTrayTexts(shellTexts);
        if (!m_language->loadTerms(
                m_resolvedLanguage, m_terms, languageError)) {
            std::cerr << "Terminology load failed: "
                      << languageError << '\n';
            m_resolvedLanguage = "en_US";
            m_terms = m_language->builtInTerms();
        }
        m_splash.setTitle(widen(m_terms.appShort));
        if (m_tray && previousLanguage != m_resolvedLanguage) {
            m_tray->requestTexts(m_trayTexts);
        }
        const std::string darkMode = m_settings->darkMode();
        const mcast::platform::AccentPair accent =
            splashAccentColors(m_settings->accentMode());
        m_splash.setTheme(
            darkMode == "System", darkMode == "Dark");
        m_splash.setAccent(accent.light, accent.dark);
        if (m_tray)
            m_tray->requestDark(effectiveDarkMode(darkMode));
        return true;
    }

    void handleHandoffConnection(
        std::unique_ptr<mcast::ipc::PipeConnection> connection)
    {
        std::string identityError;
        if (!sameExecutableAsCurrent(
                connection->peerProcessId(), identityError)) {
            std::cerr << "Shell handoff rejected: "
                      << identityError << '\n';
            return;
        }

        std::string error;
        Message request;
        if (!connection->readMessage(request, 2500ms, error)
            || request.header.type != MessageType::ShellHandoffRequest
            || request.header.messageId == 0
            || request.header.sessionId == 0
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Control, request.header, error)) {
            std::cerr << "Shell handoff request invalid: " << error << '\n';
            return;
        }

        mcast::ipc::PayloadReader reader(request.payload);
        const auto packedArguments = reader.find(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ShellHandoffArguments),
            mcast::ipc::FieldType::Bytes);
        const auto workingDirectory = reader.getString(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ShellHandoffWorkingDirectory));
        std::vector<std::string> arguments;
        if (!reader.valid() || !packedArguments || !workingDirectory
            || workingDirectory->empty()
            || !decodeHandoffArguments(
                *packedArguments, arguments, error)) {
            std::cerr << "Shell handoff payload invalid: " << error << '\n';
            return;
        }

        const bool background = std::find(
            arguments.begin(), arguments.end(), "--background")
            != arguments.end();
        bool duplicate = false;
        {
            std::scoped_lock lock(m_handoffMutex);
            duplicate = m_completedHandoffs.contains(
                request.header.messageId);
        }
        bool accepted = true;
        std::string rejection;
        if (!duplicate && !background) {
            if (!requestOpenUiFromHost(rejection))
                accepted = false;
        }
        if (!duplicate && accepted) {
            std::scoped_lock lock(m_handoffMutex);
            if (m_completedHandoffs.size() > 256)
                m_completedHandoffs.clear();
            m_completedHandoffs.insert(request.header.messageId);
        }

        std::cout << "SHELL_HANDOFF args=" << arguments.size()
                  << " cwd=" << *workingDirectory
                  << " action=" << (background ? "background" : "open-ui")
                  << " accepted=" << (accepted ? "true" : "false")
                  << " duplicate=" << (duplicate ? "true" : "false")
                  << std::endl;

        mcast::ipc::PayloadBuilder builder;
        builder.addBool(
            static_cast<std::uint16_t>(
                mcast::ipc::FieldId::ShellHandoffAccepted),
            accepted);
        if (!accepted) {
            builder.addString(
                static_cast<std::uint16_t>(
                    mcast::ipc::FieldId::ErrorText),
                rejection.empty() ? "Shell handoff failed" : rejection);
        }
        Message ack;
        ack.header.type = MessageType::ShellHandoffAck;
        ack.header.source = ProcessRole::Shell;
        ack.header.destination = ProcessRole::Shell;
        ack.header.flags = mcast::ipc::MessageFlags::Response;
        ack.header.messageId = 2;
        ack.header.correlationId = request.header.messageId;
        ack.header.sessionId = request.header.sessionId;
        ack.payload = std::move(builder).finish().value_or(
            std::vector<std::uint8_t>{});
        std::string writeError;
        if (!connection->writeMessage(ack, 2000ms, writeError)) {
            std::cerr << "Shell handoff acknowledgement failed: "
                      << writeError << '\n';
            return;
        }

        Message complete;
        if (!connection->readMessage(complete, 1500ms, error)
            || complete.header.type != MessageType::ShellHandoffComplete
            || complete.header.correlationId != request.header.messageId
            || !mcast::ipc::validateMessageDirection(
                ConnectionChannel::Control, complete.header, error)) {
            std::cerr << "Shell handoff completion missing: " << error << '\n';
            return;
        }

        Message finalAck;
        finalAck.header.type = MessageType::ShellHandoffFinalAck;
        finalAck.header.source = ProcessRole::Shell;
        finalAck.header.destination = ProcessRole::Shell;
        finalAck.header.flags = mcast::ipc::MessageFlags::Response;
        finalAck.header.messageId = 3;
        finalAck.header.correlationId = request.header.messageId;
        finalAck.header.sessionId = request.header.sessionId;
        if (!connection->writeMessage(finalAck, 2000ms, error)) {
            std::cerr << "Shell handoff final acknowledgement failed: "
                      << error << '\n';
        }
    }

    bool launchProtocol(std::string &error)
    {
        std::wstring arguments =
            L"--instance=" + widen(m_options.instance)
            + L" --nonce=" + widen(*m_nonce)
            + L" --friendly-name="
            + [&] {
                  std::lock_guard lock(m_settingsMutex);
                  return quoteWindowsArgument(
                      widen(m_settings->deviceName()));
              }()
            + L" --udn="
            + [&] {
                  std::lock_guard lock(m_settingsMutex);
                  return quoteWindowsArgument(
                      widen(m_settings->deviceUdn()));
              }()
            + L" --shell-pid=" + std::to_wstring(GetCurrentProcessId())
            + L" --dlna-http-port="
            + (m_options.testMode ? L"0" : L"8200")
            + L" --receiving-enabled="
            + (m_receivingEnabled ? L"1" : L"0")
            + L" --broadcast-enabled="
            + (m_broadcasting.load() ? L"1" : L"0")
            + L" --broadcast-interval-ms="
            + std::to_wstring(m_broadcastIntervalMs.load());
        if (m_options.testMode && !m_options.testEnableSsdp)
            arguments += L" --disable-ssdp";
        if (m_options.testMode && m_options.testEnableSsdp)
            arguments += L" --dlna-ssdp-port=0";
        if (m_options.testMode && m_options.testEnableSsdp) {
            arguments += L" --protocol-log="
                + quoteWindowsArgument(
                    (mcast::supervisor::currentExecutablePath()
                           .parent_path()
                       / (L"MCast.Protocol."
                          + widen(m_options.instance) + L".log"))
                          .wstring());
        }
        if (!m_options.background)
            arguments += L" --request-ui";
        if (!m_options.testMode || m_options.testPersistent)
            arguments += L" --persistent";
        if (m_options.testMode && !m_options.background) {
            arguments += L" --request-ui-rounds="
                + std::to_wstring(m_options.testUiRounds);
        }
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
        bool &started, std::string &error)
    {
        std::scoped_lock lock(m_uiMutex);
        if (m_uiState == UiState::Running
            || m_uiState == UiState::Starting) {
            bool alive = false;
            std::uint32_t aliveProcessId = 0;
            {
                std::scoped_lock recordsLock(m_recordsMutex);
                ChildRecord *ui = findRecord(ProcessRole::Ui);
                alive = ui && ui->process.running();
                if (alive)
                    aliveProcessId = ui->process.processId();
                if (!alive && ui) {
                    std::string terminateError;
                    ui->process.terminate(0, 1000ms, terminateError);
                    m_records.erase(std::remove_if(
                        m_records.begin(), m_records.end(),
                        [](const std::unique_ptr<ChildRecord> &record) {
                            return record->role == ProcessRole::Ui;
                        }),
                        m_records.end());
                }
            }
            if (alive) {
                m_splash.bringToFront(m_activationId);
                bringProcessWindowToFront(aliveProcessId);
                effectiveId = m_activationId;
                started = false;
                return true;
            }
            m_uiState = UiState::None;
        }
        effectiveId = ++m_nextActivationId;
        started = true;
        m_uiState = UiState::Starting;
        m_activationId = effectiveId;
        ResetEvent(m_firstFrame);
        if (!m_splash.show(effectiveId)) {
            error = "cannot show splash window";
            m_splash.hideImmediately(effectiveId);
            m_uiState = UiState::Failed;
            return false;
        }
        if (!launchUi(effectiveId, error)) {
            m_splash.hideImmediately(effectiveId);
            m_uiState = UiState::Failed;
            return false;
        }
        m_uiState = UiState::Running;
        std::uint32_t processId = 0;
        {
            std::scoped_lock recordsLock(m_recordsMutex);
            ChildRecord *ui = findRecord(ProcessRole::Ui);
            if (ui)
                processId = ui->expectation.peerProcessId;
        }
        std::cout << "SHELL_UI_STARTED activation=" << effectiveId
                  << " pid=" << processId << '\n';
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

    void handleConnection(
        const std::shared_ptr<mcast::ipc::PipeConnection> &connection)
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

        std::optional<mcast::ipc::HelloExpectation> expectation;
        {
            std::scoped_lock lock(m_recordsMutex);
            ChildRecord *record = findRecord(helloMessage.header.source);
            if (!record) {
                std::cerr << "hello from unknown role\n";
                return;
            }
            if (connection->peerProcessId()
                != record->expectation.peerProcessId) {
                std::cerr << "hello pipe peer is not the launched child\n";
                return;
            }
            expectation = record->expectation;
        }
        if (hello->processId != expectation->peerProcessId) {
            std::cerr << "hello PID is not the launched child\n";
            return;
        }
        if (!mcast::ipc::validateHello(*hello, *expectation, error)) {
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
            if (!sendLanguageCatalog(connection, error)) {
                std::cerr << "language catalog send: "
                          << error << '\n';
                return;
            }
            if (!sendSettingsSnapshot(connection, error)) {
                std::cerr << "settings snapshot send: "
                          << error << '\n';
                return;
            }
        }

        std::unique_ptr<ProtocolConnectionGuard> protocolGuard;
        std::unique_ptr<UiConnectionGuard> uiGuard;
        std::uint64_t uiControlGeneration = 0;
        std::uint64_t confirmationGeneration = 0;
        if (helloMessage.header.source == ProcessRole::Protocol) {
            {
                std::lock_guard lock(m_receivingStateMutex);
                m_receivingStateVersion = 0;
            }
            confirmationGeneration =
                registerProtocolConnection(connection);
            protocolGuard = std::make_unique<ProtocolConnectionGuard>(
                this, connection);
        }

        if (helloMessage.header.source == ProcessRole::Ui) {
            uiGuard = std::make_unique<UiConnectionGuard>(
                this, connection);
            std::uint64_t currentActivation = 0;
            {
                std::scoped_lock lock(m_uiMutex);
                currentActivation = m_activationId;
            }
            auto protocolGrant = makeGrant(
                ProcessRole::Protocol, currentActivation, error);
            if (!protocolGrant) {
                std::cerr << "protocol grant: " << error << '\n';
                return;
            }
            auto grantMessage = mcast::ipc::makePeerIdentityGrantMessage(
                ProcessRole::Ui, 4, currentActivation, *protocolGrant, error);
            if (!grantMessage
                || !connection->writeMessage(*grantMessage, 2000ms, error)) {
                std::cerr << "protocol grant send: " << error << '\n';
                return;
            }
            uiControlGeneration =
                registerUiControlConnection(connection);
        }

        while (true) {
            Message message;
            const auto idleTimeout =
                helloMessage.header.source == ProcessRole::Ui
                ? 24h : 3000ms;
            if (!connection->readMessage(message, idleTimeout, error))
                return;
            if (message.header.source != helloMessage.header.source
                || !mcast::ipc::validateMessageDirection(
                    ConnectionChannel::Control, message.header, error)) {
                std::cerr << "invalid message direction: " << error << '\n';
                return;
            }

            if (message.header.type == MessageType::UiOpenReady
                && helloMessage.header.source == ProcessRole::Protocol) {
                markProtocolReadyForUiOpen();
            } else if (message.header.type == MessageType::UiActivateRequest
                && helloMessage.header.source == ProcessRole::Protocol) {
                const mcast::ipc::PayloadReader probeReader(
                    message.payload);
                const bool probeOnly = probeReader.getBool(
                    static_cast<std::uint16_t>(
                        mcast::ipc::FieldId::
                            UiActivationProbeOnly))
                    .value_or(false);
                if (!probeReader.valid()) {
                    std::cerr << "UI activation request payload invalid\n";
                    return;
                }
                if (probeOnly) {
                    bool present = false;
                    std::uint64_t currentActivationId = 0;
                    std::uint64_t nextActivationId = 0;
                    uiPresenceSnapshot(
                        present, currentActivationId,
                        nextActivationId);
                    std::uint64_t decisionActivationId =
                        currentActivationId;
                    if (decisionActivationId == 0)
                        decisionActivationId = nextActivationId + 1;
                    if (decisionActivationId == 0)
                        decisionActivationId = 1;
                    const mcast::ipc::UiActivateDecisionInfo decision{
                        false, decisionActivationId, present};
                    const auto reply =
                        mcast::ipc::makeUiActivateDecisionMessage(
                            3, message.header.messageId,
                            message.header.sessionId, decision, error);
                    if (!reply
                        || !connection->writeMessage(
                            *reply, 2000ms, error)) {
                        std::cerr << "UI presence decision: "
                                  << error << '\n';
                        return;
                    }
                    continue;
                }
                std::string activationError;
                std::uint64_t effectiveActivationId = 0;
                bool started = false;
                if (!activateUi(
                        message.header.messageId, effectiveActivationId,
                        started, activationError)) {
                    std::cerr << "UI launch failed: " << activationError << '\n';
                    return;
                }
                const mcast::ipc::UiActivateDecisionInfo activationDecision{
                    started, effectiveActivationId,
                    started || liveUiProcessId().has_value()};
                const auto decision =
                    mcast::ipc::makeUiActivateDecisionMessage(
                        3, message.header.messageId,
                        message.header.sessionId, activationDecision, error);
                if (!decision
                    || !connection->writeMessage(
                        *decision, 2000ms, error)) {
                    std::cerr << "activation decision: " << error << '\n';
                    return;
                }
                if (!started)
                    continue;
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
            } else if (message.header.type
                           == MessageType::ReceivingState
                       && helloMessage.header.source
                           == ProcessRole::Protocol) {
                const mcast::ipc::PayloadReader reader(
                    message.payload);
                const auto enabled = reader.getBool(
                    static_cast<std::uint16_t>(
                        mcast::ipc::FieldId::
                            ReceivingEnabled));
                if (!reader.valid() || !enabled) {
                    std::cerr << "receiving state payload invalid\n";
                    return;
                }
                applyReceivingState(
                    *enabled, message.header.stateVersion);
            } else if (message.header.type
                           == MessageType::CastConfirmationRequest
                       && helloMessage.header.source
                           == ProcessRole::Protocol) {
                const auto request =
                    mcast::ipc::decodeCastConfirmationRequestMessage(
                        message, error);
                if (!request) {
                    std::cerr << "cast confirmation request: "
                              << error << '\n';
                    return;
                }

                const auto cached = lookupConfirmationDecision(
                    message.header.messageId, request->protocol,
                    request->claimId, confirmationGeneration);
                if (cached.conflict) {
                    std::cerr << "cast confirmation message ID reused "
                                 "with a different claim\n";
                    return;
                }
                bool accepted = false;
                if (cached.found) {
                    accepted = cached.accepted;
                    const mcast::ipc::CastConfirmationDecisionInfo decision{
                        request->protocol,
                        request->claimId,
                        accepted,
                    };
                    const auto decisionMessage =
                        mcast::ipc::makeCastConfirmationDecisionMessage(
                            nextMessageId(), message.header.messageId,
                            decision, error);
                    if (!decisionMessage
                        || !connection->writeMessage(
                            *decisionMessage, 2000ms, error)) {
                        std::cerr << "cast confirmation decision: "
                                  << error << '\n';
                        return;
                    }
                    std::cout << "SHELL_CAST_CONFIRMATION claim="
                              << request->claimId
                              << " protocol="
                              << static_cast<int>(request->protocol)
                              << " accepted=" << accepted
                              << '\n';
                } else {
                    startProtocolConfirmation(
                        *request, message.header.messageId,
                        confirmationGeneration, connection);
                }
            } else if (message.header.type
                           == MessageType::CastConfirmationCancel
                       && helloMessage.header.source
                           == ProcessRole::Protocol) {
                const auto requestMessageId =
                    mcast::ipc::decodeCastConfirmationCancelMessage(
                        message, error);
                if (!requestMessageId) {
                    std::cerr << "cast confirmation cancel: "
                              << error << '\n';
                    return;
                }
                cancelProtocolConfirmation(
                    *requestMessageId, confirmationGeneration);
            } else if (message.header.type
                           == MessageType::PlaybackReconnectRequest
                       && helloMessage.header.source
                           == ProcessRole::Protocol) {
                const auto requestMessageId =
                    mcast::ipc::decodePlaybackReconnectRequestMessage(
                        message, error);
                if (!requestMessageId) {
                    std::cerr << "playback reconnect request: "
                              << error << '\n';
                    return;
                }
                std::shared_ptr<mcast::ipc::PipeConnection>
                    uiConnection;
                {
                    std::scoped_lock lock(m_uiControlMutex);
                    uiConnection = m_uiControlConnection;
                }
                if (!uiConnection) {
                    std::cerr
                        << "Playback reconnect has no UI ControlPipe\n";
                    continue;
                }
                const auto notify =
                    mcast::ipc::makeUiPlaybackReconnectMessage(
                        nextMessageId(), *requestMessageId, error);
                if (!notify
                    || !uiConnection->writeMessage(
                        *notify, 2000ms, error)) {
                    std::cerr << "UI playback reconnect notify: "
                              << error << '\n';
                    continue;
                }
            } else if (message.header.type
                           == MessageType::UiCastConfirmationDecision
                       && helloMessage.header.source == ProcessRole::Ui) {
                completeUiConfirmation(
                    message, connection, uiControlGeneration);
            } else if (message.header.type
                           == MessageType::UiUpdateSettings
                       && helloMessage.header.source == ProcessRole::Ui) {
                const auto json =
                    mcast::ipc::decodeUiUpdateSettingsMessage(
                        message, error);
                if (!json) {
                    std::cerr << "UI settings update: "
                              << error << '\n';
                    return;
                }
                const std::uint64_t baseVersion =
                    message.header.stateVersion;
                SettingsPatchEffect effect;
                if (!applySettingsPatch(
                        *json, baseVersion, effect, error)) {
                    std::cerr << "UI settings update failed: "
                              << error << '\n';
                    std::string resultError;
                    std::string resolvedLanguage;
                    {
                        std::lock_guard settingsLock(m_settingsMutex);
                        resolvedLanguage = m_resolvedLanguage;
                    }
                    if (!sendSettingsUpdateResult(
                            connection, message.header.messageId,
                            m_settingsStateVersion.load(),
                            resolvedLanguage,
                            mcast::ipc::SettingsUpdateStatus::Rejected,
                            error, resultError)) {
                        std::cerr
                            << "Settings rejection result failed: "
                            << resultError << '\n';
                    }
                    if (!sendSettingsSnapshot(connection, error)) {
                        std::cerr << "settings snapshot send: "
                                  << error << '\n';
                        return;
                    }
                    continue;
                }
                std::string runtimeError;
                if (effect.previousReceiving != effect.currentReceiving) {
                    bool effective = m_receivingEnabled;
                    std::uint64_t stateVersion = 0;
                    std::string receiveError;
                    if (sendReceivingEnabled(
                            effect.currentReceiving,
                            effective, stateVersion,
                            receiveError)) {
                        applyReceivingState(
                            effective, stateVersion);
                    } else {
                        std::cerr
                            << "UI receiving setting sync failed: "
                            << receiveError << '\n';
                        runtimeError =
                            "receiving runtime apply failed: "
                            + receiveError;
                    }
                }
                if (effect.previousBroadcast != effect.currentBroadcast
                    || effect.previousBroadcastInterval
                        != effect.currentBroadcastInterval) {
                    bool effective = m_broadcasting.load();
                    std::uint32_t effectiveInterval =
                        m_broadcastIntervalMs.load();
                    std::string broadcastError;
                    if (sendDlnaBroadcast(
                            effect.currentBroadcast,
                            effect.currentBroadcastInterval,
                            effective, effectiveInterval,
                            broadcastError)) {
                        m_broadcasting = effective;
                        m_broadcastIntervalMs =
                            effectiveInterval;
                    } else {
                        std::cerr
                            << "UI broadcast setting sync failed: "
                            << broadcastError << '\n';
                        if (runtimeError.empty())
                            runtimeError =
                                "broadcast runtime apply failed: "
                                + broadcastError;
                    }
                }
                std::string resultError;
                const bool runtimeApplied = runtimeError.empty();
                std::string resolvedLanguage;
                {
                    std::lock_guard settingsLock(m_settingsMutex);
                    resolvedLanguage = m_resolvedLanguage;
                }
                if (!sendSettingsUpdateResult(
                        connection, message.header.messageId,
                        m_settingsStateVersion.load(),
                        resolvedLanguage,
                        runtimeApplied
                            ? mcast::ipc::SettingsUpdateStatus::Applied
                            : mcast::ipc::SettingsUpdateStatus::
                                  SavedRuntimeFailed,
                        runtimeApplied
                            ? std::string{}
                            : "settings saved, but " + runtimeError,
                        resultError)) {
                    std::cerr << "Settings success result failed: "
                              << resultError << '\n';
                }
            } else if (message.header.type == MessageType::UiFirstFrame
                       && helloMessage.header.source == ProcessRole::Ui) {
                bool accepted = false;
                {
                    std::scoped_lock lock(m_uiMutex);
                    if (m_uiState == UiState::Running
                        && message.header.correlationId == m_activationId) {
                        SetEvent(m_firstFrame);
                        m_firstFrameRounds.fetch_add(1);
                        accepted = true;
                    }
                }
                if (!accepted) {
                    std::cerr << "stale UiFirstFrame generation\n";
                    return;
                }
                std::cout << "SHELL_UI_FIRST_FRAME activation="
                          << message.header.correlationId << '\n';
                m_splash.dismiss(message.header.correlationId);
                continue;
            }
        }
    }

    void cleanup()
    {
        m_splash.hideImmediately();
        if (m_listener)
            m_listener->stop();
        cancelCastConfirmations();
        if (m_commandThread.joinable()) {
            m_commandThread.request_stop();
            if (m_commandListener)
                m_commandListener->stop();
            m_commandThread.join();
        }
        m_commandListener.reset();
        {
            std::scoped_lock lock(m_recordsMutex);
            for (const auto &record : m_records) {
                std::string error;
                record->process.terminate(0, 1000ms, error);
            }
            m_records.clear();
        }
        {
            std::scoped_lock lock(m_confirmationThreadsMutex);
            m_acceptConfirmationThreads = false;
            for (auto &slot : m_confirmationThreads)
                slot.thread.request_stop();
        }
        for (auto &slot : m_confirmationThreads) {
            if (slot.thread.joinable())
                slot.thread.join();
        }
        m_confirmationThreads.clear();
        for (const auto &slot : m_connections) {
            if (slot->thread.joinable())
                slot->thread.join();
        }
        m_connections.clear();
        m_listener.reset();
        m_tray.reset();
        const std::filesystem::path testSettingsPath =
            m_settings ? m_settings->path() : std::filesystem::path{};
        m_settings.reset();
        if (m_options.testMode && !testSettingsPath.empty()) {
            std::error_code ignored;
            std::filesystem::remove(testSettingsPath, ignored);
            std::filesystem::path statePath = testSettingsPath;
            statePath.replace_filename(
                testSettingsPath.stem().wstring() + L".Shell.json");
            std::filesystem::remove(statePath, ignored);
        }
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

    void onSplashState(
        std::uint64_t token, SplashWindow::State state)
    {
        switch (state) {
        case SplashWindow::State::Shown:
            m_splashVisible.store(true);
            m_splashShownRounds.fetch_add(1);
            std::cout << "SHELL_SPLASH_SHOWN activation=" << token
                      << '\n';
            break;
        case SplashWindow::State::Hidden:
            m_splashVisible.store(false);
            m_splashHiddenRounds.fetch_add(1);
            std::cout << "SHELL_SPLASH_HIDDEN activation=" << token
                      << '\n';
            break;
        case SplashWindow::State::Failed:
            m_splashFailed.store(true);
            std::cerr << "Splash initialization failed\n";
            break;
        }
    }

    Options m_options;
    std::optional<std::string> m_nonce;
    HANDLE m_mutex = nullptr;
    HANDLE m_job = nullptr;
    HANDLE m_firstFrame = nullptr;
    std::unique_ptr<mcast::ipc::PipeListener> m_listener;
    std::unique_ptr<mcast::ipc::PipeListener> m_commandListener;
    std::jthread m_commandThread;
    std::vector<std::unique_ptr<ChildRecord>> m_records;
    std::mutex m_recordsMutex;
    std::vector<std::unique_ptr<ConnectionSlot>> m_connections;
    std::shared_ptr<mcast::ipc::PipeConnection> m_protocolConnection;
    std::shared_ptr<mcast::ipc::PipeConnection> m_uiControlConnection;
    std::mutex m_uiControlMutex;
    std::condition_variable m_uiControlChanged;
    bool m_protocolAcceptsUiOpen = false;
    std::mutex m_protocolMutex;
    std::condition_variable m_protocolReady;
    std::deque<ConfirmationCacheEntry>
        m_recentConfirmationDecisions;
    std::unordered_map<std::uint64_t, PendingUiConfirmation>
        m_pendingUiConfirmations;
    std::unordered_set<std::uint64_t>
        m_cancelledProtocolConfirmations;
    std::vector<ConfirmationThreadSlot> m_confirmationThreads;
    std::mutex m_confirmationThreadsMutex;
    bool m_acceptConfirmationThreads = true;
    std::uint64_t m_protocolGeneration = 0;
    std::uint64_t m_uiGeneration = 0;
    std::mutex m_confirmationMutex;
    enum class UiMode {
        None,
        Normal,
        ConfirmationOnly,
    };
    UiMode m_uiMode = UiMode::None;
    enum class UiState {
        None,
        Starting,
        Running,
        Failed,
    };
    UiState m_uiState = UiState::None;
    std::uint64_t m_activationId = 0;
    std::uint64_t m_nextActivationId = 0;
    std::atomic_uint32_t m_firstFrameRounds = 0;
    std::atomic_uint32_t m_splashShownRounds = 0;
    std::atomic_uint32_t m_splashHiddenRounds = 0;
    std::atomic_bool m_splashVisible = false;
    std::atomic_bool m_splashFailed = false;
    std::chrono::steady_clock::time_point m_uiStartedAt{};
    std::mutex m_uiMutex;
    std::mutex m_handoffMutex;
    std::unordered_set<std::uint64_t> m_completedHandoffs;
    std::unique_ptr<mcast::platform::NativeTrayIcon> m_tray;
    std::unique_ptr<mcast::shell::ShellSettings> m_settings;
    std::unique_ptr<mcast::shell::ShellLanguage> m_language;
    mcast::shell::Terminology m_terms;
    std::string m_resolvedLanguage = "en_US";
    mcast::platform::TrayTexts m_trayTexts;
    std::atomic_bool m_receivingEnabled{true};
    std::atomic_bool m_broadcasting{true};
    std::atomic_uint32_t m_broadcastIntervalMs{10000};
    std::atomic_bool m_receivingReconcilePending{false};
    std::uint64_t m_receivingStateVersion = 0;
    std::mutex m_receivingStateMutex;
    std::mutex m_settingsMutex;
    std::atomic_uint64_t m_nextMessageId{100};
    std::atomic_uint64_t m_settingsStateVersion{1};
    std::atomic_bool m_shuttingDown = false;
    std::atomic_bool m_quitRequested = false;

    // Declared last so its worker thread is stopped and joined before the
    // counters and mutex used by its state callback are destroyed.
    SplashWindow m_splash;
};

} // namespace

int runPrimaryShell(
    Options &options, HANDLE shellMutex,
    const std::wstring &mutexName, std::string &error)
{
    for (int attempt = 0; attempt < 20; ++attempt) {
        {
            ShellRuntime shell(options, shellMutex);
            if (shell.initialize(error))
                return shell.run();
        }

        if (attempt + 1 >= 20)
            break;
        std::this_thread::sleep_for(100ms);
        shellMutex = CreateMutexW(
            nullptr, FALSE, mutexName.c_str());
        if (!shellMutex) {
            error = "CreateMutexW retry failed: "
                + std::to_string(GetLastError());
            return 2;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(shellMutex);
            error = "another Shell became primary during initialization retry";
            return 3;
        }
    }
    std::cerr << "Shell init failed after retries: " << error << '\n';
    return 2;
}

int runShellMain(int argc, wchar_t **argv)
{
    MainThreadApartment apartment;
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        std::cerr << error << '\n';
        return 1;
    }

    const std::wstring mutexName = shellMutexName(options);
    HANDLE shellMutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (!shellMutex) {
        std::cerr << "CreateMutexW failed: " << GetLastError() << '\n';
        return 2;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        const bool sent = sendHandoff(options, argc, argv, error);
        CloseHandle(shellMutex);
        if (sent)
            return 0;

        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(100ms);
            HANDLE retryMutex = CreateMutexW(
                nullptr, FALSE, mutexName.c_str());
            if (!retryMutex) {
                std::cerr << "CreateMutexW retry failed: "
                          << GetLastError() << '\n';
                return 2;
            }
            if (GetLastError() != ERROR_ALREADY_EXISTS) {
                std::cerr << "Shell handoff failed; taking over as primary: "
                          << error << '\n';
                return runPrimaryShell(
                    options, retryMutex, mutexName, error);
            }
            CloseHandle(retryMutex);
        }
        std::cerr << "Shell handoff failed: " << error << '\n';
        return 3;
    }

    return runPrimaryShell(options, shellMutex, mutexName, error);
}

int wmain(int argc, wchar_t **argv)
{
    return runShellMain(argc, argv);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return runShellMain(__argc, __wargv);
}
