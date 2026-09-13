// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>

class QTcpServer;
class QTcpSocket;
class GenaManager;
class SoapHandler;

/**
 * HttpServer —— 监听 8200 端口，把设备描述和三份 SCPD 发出去。
 *
 * 为什么必须有这么个东西：SSDP 只是"打个招呼"，控制点收到回应后会上门来抓设备描述
 * （description.xml）。抓不到它就把这台设备当死的丢掉，界面上什么都不显示。所以
 * "能被发现"和"能被看见"是两回事，中间差的就是这个 HTTP 服务。
 *
 * 这里用的路径和 SsdpService 广播出去的 LOCATION 必须一致：
 *   GET /description.xml            设备描述
 *   GET /AVTransport/scpd           AVTransport 的能力清单
 *   GET /RenderingControl/scpd      RenderingControl 的能力清单
 *   GET /ConnectionManager/scpd     ConnectionManager 的能力清单
 *
 * 四个 SOAP 控制端点和三个事件订阅端点现在还没实现 —— 控制点会看到设备、能看到
 * 有哪些服务，但点播放会报错。那是下一步的事。
 */
class HttpServer : public QObject
{
    Q_OBJECT

public:
    explicit HttpServer(QObject *parent = nullptr);
    ~HttpServer() override;

    bool start(quint16 port, const QString &friendlyName, const QString &udn);
    void stop();

    /**
     * 接上 SOAP 处理器。不接的话，设备描述和三份能力清单照常能取，
     * 但控制端点会一律回"操作失败" —— 也就是设备看得见、使不上。
     */
    void setSoapHandler(SoapHandler *handler) { m_soap = handler; }

    /** 接上事件订阅处理器（GENA）。不接的话，订阅请求会被回绝。 */
    void setGenaHandler(GenaManager *handler) { m_gena = handler; }

signals:
    void statusChanged(const QString &text);
    void logMessage(const QString &text);

private:
    void onNewConnection();
    void onReadyRead();
    void onDisconnected();
    void respond(QTcpSocket *socket, const QByteArray &request);

    QTcpServer *m_server = nullptr;
    SoapHandler *m_soap = nullptr;
    GenaManager *m_gena = nullptr;
    QString     m_friendlyName;
    QString     m_udn;
    QHash<QTcpSocket *, QByteArray> m_buffers;   // 每个连接攒到一半的请求
    bool        m_running = false;
};
