// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ipc/Messages.h"

#include <QObject>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace mcast::ipc {
class PipeConnection;
}

namespace mcast::ui_process {

class UiConfirmationBridge final : public QObject
{
    Q_OBJECT

public:
    explicit UiConfirmationBridge(
        std::shared_ptr<ipc::PipeConnection> control,
        QObject *parent = nullptr);
    ~UiConfirmationBridge() override;

    bool sendFirstFrame(std::uint64_t activationId, std::string &error);

public slots:
    void submitDecision(qulonglong requestId, bool accepted);

signals:
    void requestReceived(qulonglong requestId, const QVariantMap &request);
    void cancelReceived(qulonglong requestId);
    void connectionLost(const QString &reason);
    void logMessage(const QString &message);

private:
    void readLoop(std::stop_token stop);
    std::uint64_t nextMessageId() noexcept;

    std::shared_ptr<ipc::PipeConnection> m_control;
    std::jthread m_reader;
    std::mutex m_pendingMutex;
    std::unordered_map<std::uint64_t, ipc::CastConfirmationRequestInfo>
        m_pending;
    std::atomic_uint64_t m_nextMessageId{100};
};

} // namespace mcast::ui_process
