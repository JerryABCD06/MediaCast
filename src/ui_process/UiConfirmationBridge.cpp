// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ui_process/UiConfirmationBridge.h"

#include "ipc/NamedPipe.h"

#include <QString>

#include <chrono>
#include <string_view>
#include <utility>

namespace mcast::ui_process {
namespace {

using namespace std::chrono_literals;

} // namespace

UiConfirmationBridge::UiConfirmationBridge(
    std::shared_ptr<ipc::PipeConnection> control,
    QObject *parent)
    : QObject(parent)
    , m_control(std::move(control))
{
    QString pipelineError;
    m_metadataPipeline.loadNearExecutable(&pipelineError);
    m_reader = std::jthread([this](std::stop_token stop) {
        readLoop(stop);
    });
}

UiConfirmationBridge::~UiConfirmationBridge()
{
    m_reader.request_stop();
    if (m_control)
        m_control->close();
    if (m_reader.joinable())
        m_reader.join();
}

std::uint64_t UiConfirmationBridge::nextMessageId() noexcept
{
    std::uint64_t id = m_nextMessageId.fetch_add(1);
    if (id == 0)
        id = m_nextMessageId.fetch_add(1);
    return id;
}

bool UiConfirmationBridge::sendFirstFrame(
    std::uint64_t activationId, std::string &error)
{
    if (!m_control)
        return false;
    ipc::Message firstFrame;
    firstFrame.header.type = ipc::MessageType::UiFirstFrame;
    firstFrame.header.source = ipc::ProcessRole::Ui;
    firstFrame.header.destination = ipc::ProcessRole::Shell;
    firstFrame.header.messageId = nextMessageId();
    firstFrame.header.correlationId = activationId;
    firstFrame.header.sessionId = 1;
    return m_control->writeMessage(firstFrame, 2000ms, error);
}

bool UiConfirmationBridge::submitSettings(
    const QByteArray &json, std::uint64_t baseVersion,
    std::string &error)
{
    if (!m_control || json.isEmpty()) {
        error = "settings update is unavailable";
        return false;
    }
    auto message = ipc::makeUiUpdateSettingsMessage(
        nextMessageId(),
        std::string_view(json.constData(), json.size()),
        error);
    if (!message) {
        emit settingsSubmitFailed(
            QString::fromStdString(error));
        return false;
    }
    message->header.stateVersion = baseVersion;
    if (!m_control->writeMessage(*message, 2000ms, error)) {
        emit settingsSubmitFailed(
            QString::fromStdString(error));
        return false;
    }
    return true;
}

void UiConfirmationBridge::submitDecision(
    qulonglong requestId, bool accepted)
{
    ipc::CastConfirmationRequestInfo request;
    {
        std::scoped_lock lock(m_pendingMutex);
        const auto it = m_pending.find(requestId);
        if (it == m_pending.end())
            return;
        request = it->second;
        m_pending.erase(it);
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
        || !m_control->writeMessage(*message, 2000ms, error)) {
        emit logMessage(
            QStringLiteral("确认结果回传失败：%1")
                .arg(QString::fromStdString(error)));
    }
}

void UiConfirmationBridge::readLoop(std::stop_token stop)
{
    while (!stop.stop_requested()) {
        ipc::Message message;
        std::string error;
        if (!m_control
            || !m_control->readMessage(message, 24h, error)) {
            if (!stop.stop_requested()) {
                emit connectionLost(QString::fromStdString(error));
            }
            return;
        }
        if (!ipc::validateMessageDirection(
                ipc::ConnectionChannel::Control,
                message.header, error)) {
            emit logMessage(
                QStringLiteral("ControlPipe 消息方向非法：%1")
                    .arg(QString::fromStdString(error)));
            continue;
        }

        if (message.header.type
            == ipc::MessageType::UiCastConfirmationRequest) {
            const auto request =
                ipc::decodeUiCastConfirmationRequestMessage(
                    message, message.header.correlationId, error);
            if (!request) {
                emit logMessage(
                    QStringLiteral("确认请求无效：%1")
                        .arg(QString::fromStdString(error)));
                continue;
            }
            {
                std::scoped_lock lock(m_pendingMutex);
                m_pending[message.header.messageId] = *request;
            }
            MetadataPipelineInput metadata;
            metadata.protocolTitle =
                QString::fromStdString(request->mediaTitle);
            const MetadataPipelineResult resolved =
                m_metadataPipeline.resolve(metadata);
            QVariantMap fields;
            fields.insert(
                QStringLiteral("protocol"),
                static_cast<int>(request->protocol));
            fields.insert(
                QStringLiteral("claimId"),
                QVariant::fromValue<qulonglong>(request->claimId));
            fields.insert(
                QStringLiteral("peerId"),
                QString::fromStdString(request->peerId));
            fields.insert(
                QStringLiteral("peerName"),
                QString::fromStdString(request->peerDisplayName));
            fields.insert(
                QStringLiteral("peerAddress"),
                QString::fromStdString(request->peerAddress));
            fields.insert(
                QStringLiteral("mediaTitle"),
                resolved.title);
            fields.insert(
                QStringLiteral("timeoutMs"),
                QVariant::fromValue<qulonglong>(
                    request->remainingTimeoutMs));
            emit requestReceived(
                message.header.messageId, fields);
        } else if (message.header.type
                   == ipc::MessageType::UiCastConfirmationCancel) {
            if (!ipc::decodeUiCastConfirmationCancelMessage(
                    message, error)) {
                emit logMessage(
                    QStringLiteral("确认取消消息无效：%1")
                        .arg(QString::fromStdString(error)));
                continue;
            }
            {
                std::scoped_lock lock(m_pendingMutex);
                m_pending.erase(message.header.correlationId);
            }
            emit cancelReceived(message.header.correlationId);
        } else if (
            message.header.type
            == ipc::MessageType::UiPlaybackReconnect) {
            const auto requestId =
                ipc::decodeUiPlaybackReconnectMessage(
                    message, error);
            if (!requestId) {
                emit logMessage(
                    QStringLiteral("播放管道重连消息无效：%1")
                        .arg(QString::fromStdString(error)));
                continue;
            }
            emit playbackReconnectRequested(*requestId);
        } else if (
            message.header.type
            == ipc::MessageType::SettingsSnapshot) {
            const auto json =
                ipc::decodeSettingsSnapshotMessage(message, error);
            if (!json) {
                emit logMessage(
                    QStringLiteral("设置快照无效：%1")
                        .arg(QString::fromStdString(error)));
                continue;
            }
            emit settingsReceived(
                QByteArray::fromStdString(json->json),
                message.header.stateVersion,
                QString::fromStdString(json->resolvedLanguage));
        } else if (
            message.header.type
            == ipc::MessageType::SettingsUpdateResult) {
            const auto result =
                ipc::decodeSettingsUpdateResultMessage(
                    message, message.header.correlationId, error);
            if (!result) {
                emit logMessage(
                    QStringLiteral("设置更新结果无效：%1")
                        .arg(QString::fromStdString(error)));
                continue;
            }
            if (result->status
                == ipc::SettingsUpdateStatus::Rejected) {
                emit settingsUpdateRejected(
                    QString::fromStdString(result->error));
            } else {
                if (result->status
                    == ipc::SettingsUpdateStatus::
                        SavedRuntimeFailed) {
                    emit settingsRuntimeApplyFailed(
                        QString::fromStdString(result->error));
                }
                emit settingsCommitted(
                    message.header.stateVersion,
                    QString::fromStdString(
                        result->resolvedLanguage));
            }
        }
    }
}

} // namespace mcast::ui_process
