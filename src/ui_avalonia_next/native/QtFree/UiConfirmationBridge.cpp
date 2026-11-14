// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "QtFree/UiConfirmationBridge.h"

#include "ipc/NamedPipe.h"

#include <chrono>
#include <utility>

namespace mcast::avalonia::qtfree {
namespace {

using namespace std::chrono_literals;

std::string identityTitle(const std::string &title)
{
    return title;
}

} // namespace

UiConfirmationBridge::UiConfirmationBridge(
    std::shared_ptr<ipc::PipeConnection> control,
    EventLoop &loop, TitleResolver titleResolver)
    : m_control(std::move(control))
    , m_loop(loop)
    , m_titleResolver(
          titleResolver ? std::move(titleResolver)
                        : identityTitle)
    , m_reader([this](std::stop_token stop) { readLoop(stop); })
{
}

UiConfirmationBridge::~UiConfirmationBridge()
{
    stop();
}

void UiConfirmationBridge::stop()
{
    if (m_stopped.exchange(true))
        return;
    m_alive.store(false);
    m_reader.request_stop();
    if (m_control)
        m_control->close();
    if (m_reader.joinable())
        m_reader.join();
    {
        const std::scoped_lock lock(m_pendingMutex);
        m_pending.clear();
    }
}

std::uint64_t UiConfirmationBridge::nextMessageId() noexcept
{
    std::uint64_t id = m_nextRequestId.fetch_add(1);
    if (id == 0)
        id = m_nextRequestId.fetch_add(1);
    return id;
}

bool UiConfirmationBridge::post(std::function<void()> task)
{
    if (m_stopped)
        return false;
    return m_loop.post(std::move(task));
}

bool UiConfirmationBridge::sendFirstFrame(
    std::uint64_t activationId, std::string &error)
{
    if (!m_control) {
        error = "ControlPipe is unavailable";
        return false;
    }
    ipc::Message firstFrame;
    firstFrame.header.type = ipc::MessageType::UiFirstFrame;
    firstFrame.header.source = ipc::ProcessRole::Ui;
    firstFrame.header.destination = ipc::ProcessRole::Shell;
    firstFrame.header.messageId = nextMessageId();
    firstFrame.header.correlationId = activationId;
    firstFrame.header.sessionId = 1;
    return m_control->writeMessage(firstFrame, 2s, error);
}

bool UiConfirmationBridge::submitSettings(
    std::string_view json, std::uint64_t baseVersion,
    std::string &error)
{
    if (!m_control || json.empty()) {
        error = "settings update is unavailable";
        return false;
    }
    auto message = ipc::makeUiUpdateSettingsMessage(
        nextMessageId(), json, error);
    if (!message) {
        const std::string reason = error;
        post([this, reason] {
            if (m_alive.load())
                settingsSubmitFailed.emit(reason);
        });
        return false;
    }
    message->header.stateVersion = baseVersion;
    if (!m_control->writeMessage(*message, 2s, error)) {
        const std::string reason = error;
        post([this, reason] {
            if (m_alive.load())
                settingsSubmitFailed.emit(reason);
        });
        return false;
    }
    return true;
}

bool UiConfirmationBridge::submitDecision(
    std::uint64_t requestId, bool accepted)
{
    ipc::CastConfirmationRequestInfo request;
    {
        const std::scoped_lock lock(m_pendingMutex);
        const auto found = m_pending.find(requestId);
        if (found == m_pending.end())
            return false;
        request = found->second;
        m_pending.erase(found);
    }

    std::string error;
    const ipc::CastConfirmationDecisionInfo decision{
        request.protocol,
        request.claimId,
        accepted,
    };
    const auto message = ipc::makeUiCastConfirmationDecisionMessage(
        nextMessageId(), requestId, decision, error);
    if (!message || !m_control
        || !m_control->writeMessage(*message, 2s, error)) {
        const std::string reason =
            "confirmation result send failed: " + error;
        post([this, reason] {
            if (m_alive.load())
                logMessage.emit(reason);
        });
        return false;
    }
    return true;
}

void UiConfirmationBridge::readLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        ipc::Message message;
        std::string error;
        if (!m_control
            || !m_control->readMessage(message, 24h, error)) {
            if (!stop.stop_requested()) {
                const std::string reason = error;
                post([this, reason] {
                    if (m_alive.load())
                        connectionLost.emit(reason);
                });
            }
            return;
        }
        if (!ipc::validateMessageDirection(
                ipc::ConnectionChannel::Control,
                message.header, error)) {
            const std::string reason =
                "invalid ControlPipe message direction: " + error;
            post([this, reason] {
                if (m_alive.load())
                    logMessage.emit(reason);
            });
            continue;
        }

        if (message.header.type
            == ipc::MessageType::UiCastConfirmationRequest) {
            const auto request =
                ipc::decodeUiCastConfirmationRequestMessage(
                    message, message.header.correlationId, error);
            if (!request) {
                const std::string reason =
                    "invalid confirmation request: " + error;
                post([this, reason] {
                    if (m_alive.load())
                        logMessage.emit(reason);
                });
                continue;
            }
            {
                const std::scoped_lock lock(m_pendingMutex);
                m_pending[message.header.messageId] = *request;
            }
            ConfirmationEvent event{
                message.header.messageId,
                *request,
                m_titleResolver(request->mediaTitle),
            };
            post([this, event = std::move(event)] {
                if (m_alive.load())
                    requestReceived.emit(event);
            });
        } else if (
            message.header.type
            == ipc::MessageType::UiCastConfirmationCancel) {
            const auto requestId =
                ipc::decodeUiCastConfirmationCancelMessage(
                    message, error);
            if (!requestId) {
                const std::string reason =
                    "invalid confirmation cancel: " + error;
                post([this, reason] {
                    if (m_alive.load())
                        logMessage.emit(reason);
                });
                continue;
            }
            {
                const std::scoped_lock lock(m_pendingMutex);
                m_pending.erase(*requestId);
            }
            post([this, id = *requestId] {
                if (m_alive.load())
                    cancelReceived.emit(id);
            });
        } else if (
            message.header.type
            == ipc::MessageType::UiPlaybackReconnect) {
            const auto requestId =
                ipc::decodeUiPlaybackReconnectMessage(message, error);
            if (!requestId) {
                const std::string reason =
                    "invalid playback reconnect message: " + error;
                post([this, reason] {
                    if (m_alive.load())
                        logMessage.emit(reason);
                });
                continue;
            }
            post([this, id = *requestId] {
                if (m_alive.load())
                    playbackReconnectRequested.emit(id);
            });
        } else if (
            message.header.type
            == ipc::MessageType::SettingsSnapshot) {
            const auto snapshot =
                ipc::decodeSettingsSnapshotMessage(message, error);
            if (!snapshot) {
                const std::string reason =
                    "invalid settings snapshot: " + error;
                post([this, reason] {
                    if (m_alive.load())
                        logMessage.emit(reason);
                });
                continue;
            }
            SettingsSnapshotEvent event{
                snapshot->json,
                message.header.stateVersion,
                snapshot->resolvedLanguage,
            };
            post([this, event = std::move(event)] {
                if (m_alive.load())
                    settingsReceived.emit(event);
            });
        } else if (
            message.header.type
            == ipc::MessageType::SettingsUpdateResult) {
            const auto result =
                ipc::decodeSettingsUpdateResultMessage(
                    message, message.header.correlationId, error);
            if (!result) {
                const std::string reason =
                    "invalid settings result: " + error;
                post([this, reason] {
                    if (m_alive.load())
                        logMessage.emit(reason);
                });
                continue;
            }
            post([this,
                  status = result->status,
                  reason = result->error,
                  resolvedLanguage = result->resolvedLanguage,
                  version = message.header.stateVersion] {
                if (!m_alive.load())
                    return;
                if (status == ipc::SettingsUpdateStatus::Rejected) {
                    settingsUpdateRejected.emit(reason);
                    return;
                }
                if (status
                    == ipc::SettingsUpdateStatus::
                        SavedRuntimeFailed) {
                    settingsRuntimeApplyFailed.emit(reason);
                }
                settingsCommitted.emit(version, resolvedLanguage);
            });
        }
    }
}

} // namespace mcast::avalonia::qtfree

