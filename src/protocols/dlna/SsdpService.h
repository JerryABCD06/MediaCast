#pragma once

#include <QHostAddress>
#include <QNetworkInterface>
#include <QObject>
#include <QString>

class QUdpSocket;
class QTimer;

/**
 * SsdpService —— DLNA 的"发现"层，也就是别人怎么知道我们存在。
 *
 * 局域网里的控制点（手机、电脑上的 DLNA 客户端）不知道网络上有哪些设备，它会往组播
 * 地址 239.255.255.250:1900 广播一句 M-SEARCH 问"谁是 MediaRenderer？"。同时，设备
 * 自己也应该定期往同一个地址广播 NOTIFY ssdp:alive，主动说"我还在"。控制点收到之后，
 * 会去抓设备描述（description.xml）来看这台设备的详细信息。
 *
 * 这个类做两件事：定期发 ssdp:alive / 退出时发 ssdp:byebye；以及收到 M-SEARCH 之后
 * 单播回一句"我在这里"。
 *
 * 关于网卡：这台机器上有三块"看起来能用"的接口 —— 真实的 Wi-Fi、代理软件建的隧道、
 * 以及若干虚拟网卡。组播包如果从隧道出去就永远到不了局域网，而且不会报任何错。
 * 所以必须显式挑一块网卡，并用 setMulticastInterface 指定出口。这一点是实测过的，
 * 不是理论上的讲究。
 */
class SsdpService : public QObject
{
    Q_OBJECT

public:
    /**
     * HTTP 服务端口。SSDP 广播出去的 LOCATION 指向的就是这个端口，
     * 所以两处必须用同一个数字，放在这里当唯一的出处。
     */
    static constexpr quint16 DefaultHttpPort = 8200;

    explicit SsdpService(QObject *parent = nullptr);
    ~SsdpService() override;

    /** 挑网卡、建套接字、开始定期广播。返回 false 表示没能启动。 */
    bool start();

    /** 广播 ssdp:byebye 并释放套接字。 */
    void stop();

    /**
     * 改广播间隔，单位毫秒。
     *
     * 默认 10 秒（高频）。之所以默认就这么勤，是因为实测发现控制点很可能收不到我们的
     * 搜索请求 —— 手机当热点时它自己发不出组播 —— 于是它只能靠听我们的广播来发现我们，
     * 广播间隔直接决定了"要扫几次才能找到"。接上正经路由器之后可以放大到 60 秒。
     */
    void setAliveIntervalMs(int ms);

    /**
     * 要不要定期对外广播（alive）。
     *
     * 关掉 **不等于** 停服务：socket 还开着、M-SEARCH 照常响应 —— 只是不主动
     * 隔一会儿喊一嗓子。有些网络嫌广播吵，或者用户就是不想被满网找。
     */
    void setBroadcasting(bool on);

    /**
     * 让设备在网络里"消失一下再回来"—— 先发 ssdp:byebye，1.5 秒后再广播 alive。
     *
     * 这是给"电脑端强制挂断"用的。有些控制点收到 TransportState 变化之后，仍然把
     * 这台设备记成"当前投屏目标"，界面上那条"正在投屏到 XXX"的横幅不肯消 —— 因为
     * 从它的角度看，设备还在网上，随时还能继续投。让它暂时消失，控制点才会真正
     * 放下这个会话。
     *
     * byebye 本来就是设备关机时该发的正式通知，用在这儿不算歪门邪道；但代价是
     * **同网段的所有控制点都会短暂地看到这台设备消失又出现**，所以只在真的要
     * "赶走"控制点时才调用。
     */
    void announceGoingAwayBriefly();

    /** 界面上显示用的一句话摘要。 */
    QString description() const;

    /** 设备标识，形如 uuid:xxxx-...。设备描述里的 UDN 字段要用它。 */
    QString udn() const { return m_udn; }

    /** 对用户可见的设备名（取计算机名）。 */
    QString friendlyName() const { return m_friendlyName; }

    /** 本机在选定网卡上的地址。 */
    QString localAddress() const { return m_localAddress; }

    /** 广播出去的设备描述地址，形如 http://10.0.0.5:8200/description.xml。 */
    QString locationUrl() const { return m_locationUrl; }

signals:
    /** 状态文字变化。 */
    void statusChanged(const QString &text);

    /** 过程日志：网卡挑选、收到的包等。 */
    void logMessage(const QString &text);

private:
    void chooseInterface();
    void loadOrCreateUdn();
    QString readOrCreateUuid();
    void sendAlive();
    void sendNotify(const QString &notificationType, bool alive);
    void sendSearchResponse(const QHostAddress &to, quint16 port, const QString &searchTarget);
    void handleSearch(const QHostAddress &sender, quint16 senderPort, const QString &request);
    void readPendingDatagrams();

    QUdpSocket       *m_socket = nullptr;
    QTimer           *m_timer  = nullptr;
    QNetworkInterface m_interface;

    QString m_interfaceName;
    QString m_localAddress;
    QString m_locationUrl;
    QString m_udn;              // 形如 uuid:xxxxxxxx-xxxx-...
    QString m_friendlyName;     // 控制点上显示的名字
    QString m_configPath;       // 设备标识实际存到了哪个文件
    bool    m_udnPersisted = false;
    quint32 m_bootId = 0;       // BOOTID.UPNP.ORG：每次启动递增，控制点用它识别"设备重启了"
    int     m_aliveIntervalMs = 10000;
    /** 要不要定期对外广播。见 setBroadcasting() 的说明。 */
    bool    m_broadcasting = true;
    bool    m_running = false;
};
