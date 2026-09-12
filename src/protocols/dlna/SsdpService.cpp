#include "SsdpService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QHostAddress>
#include <QHostInfo>
#include <QLocale>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

namespace {

const QHostAddress kSsdpGroup(QStringLiteral("239.255.255.250"));
const quint16      kSsdpPort = 1900;

const int kMaxAgeSeconds   = 1800;

/**
 * 一台 MediaRenderer 要声明的六种身份。
 *
 * 少一种，某些控制点就会认为这台设备不完整：只关心设备类型的会查
 * device:MediaRenderer，而只按服务搜索的会直接问 AVTransport。所以六条都要发。
 */
QStringList notifyTargets(const QString &udn)
{
    return QStringList{
        QStringLiteral("upnp:rootdevice"),
        udn,
        QStringLiteral("urn:schemas-upnp-org:device:MediaRenderer:1"),
        QStringLiteral("urn:schemas-upnp-org:service:AVTransport:1"),
        QStringLiteral("urn:schemas-upnp-org:service:RenderingControl:1"),
        QStringLiteral("urn:schemas-upnp-org:service:ConnectionManager:1"),
    };
}

/** 格式按规范是「操作系统/版本 UPnP/版本 产品/版本」。 */
QString serverHeader()
{
    // 产品标识里不放空格 —— 这行是给机器读的，空格在有些解析器里是分隔符。
    return QStringLiteral("Windows/10.0 UPnP/1.1 MCast/0.1");
}

/**
 * 从 SSDP/HTTP 报文里取一个头字段的值。
 *
 * 按 HTTP 的规矩：字段名不区分大小写，冒号后的空格可有可无。各家控制点写法不一，
 * 有写 `ST: ` 的，也有写 `st:` 的，还有多加空格的。所以比较时统一大写、两面都 trim。
 */
QString headerValue(const QString &message, const QString &name)
{
    const QStringList lines = message.split(QStringLiteral("\r\n"));
    for (const QString &line : lines) {
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        if (line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) == 0)
            return line.mid(colon + 1).trimmed();
    }
    return QString();
}

} // namespace

SsdpService::SsdpService(QObject *parent)
    : QObject(parent)
{
}

SsdpService::~SsdpService()
{
    stop();
}

QString SsdpService::description() const
{
    if (!m_running)
        return QStringLiteral("SSDP: 未启动");

    QString text = QStringLiteral("SSDP: 网卡 %1 (%2)  →  %3")
                       .arg(m_interfaceName, m_localAddress, m_locationUrl);

    // 设备标识存不下来的话必须明说：控制点会把本机当成一台新设备，
    // 而表面上一切正常。这种问题不写在脸上就没人查得到。
    if (!m_udnPersisted)
        text += QStringLiteral("    [设备标识未能保存]");

    return text;
}

void SsdpService::chooseInterface()
{
    // 把所有接口都过一遍，能用的列出来，选中的那个排最前。出问题时这一行日志
    // 能省掉大量猜测。
    int bestScore = -1;

    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &candidate : all) {
        const QNetworkInterface::InterfaceFlags flags = candidate.flags();

        QString reject;
        if (!flags.testFlag(QNetworkInterface::IsUp))              reject = QStringLiteral("未启用");
        else if (!flags.testFlag(QNetworkInterface::IsRunning))    reject = QStringLiteral("未运行");
        else if (!flags.testFlag(QNetworkInterface::CanMulticast)) reject = QStringLiteral("不支持组播");
        else if (candidate.type() == QNetworkInterface::Loopback)  reject = QStringLiteral("回环接口");

        // 找这个接口上的 IPv4 地址。169.254 开头是"没拿到地址"时的自动地址，不能用。
        QString address;
        if (reject.isEmpty()) {
            const QList<QNetworkAddressEntry> entries = candidate.addressEntries();
            for (const QNetworkAddressEntry &entry : entries) {
                const QHostAddress ip = entry.ip();
                if (ip.protocol() != QAbstractSocket::IPv4Protocol) continue;
                if (ip.isLoopback() || ip.isLinkLocal()) continue;
                address = ip.toString();
                break;
            }
            if (address.isEmpty())
                reject = QStringLiteral("没有可用的 IPv4 地址");
        }

        if (!reject.isEmpty()) {
            emit logMessage(QStringLiteral("网卡「%1」跳过：%2")
                                .arg(candidate.humanReadableName(), reject));
            continue;
        }

        // 打分：真实的 Wi-Fi / 有线优先，其余（虚拟网卡、隧道）排后面。
        int score = 10;
        QString kind = QStringLiteral("其他");
        if (candidate.type() == QNetworkInterface::Wifi) {
            score = 100; kind = QStringLiteral("Wi-Fi");
        } else if (candidate.type() == QNetworkInterface::Ethernet) {
            score = 80;  kind = QStringLiteral("有线");
        }

        emit logMessage(QStringLiteral("网卡「%1」可用：%2  %3")
                            .arg(candidate.humanReadableName(), kind, address));

        if (score > bestScore) {
            bestScore = score;
            m_interface = candidate;
            m_interfaceName = QStringLiteral("%1 %2").arg(kind, candidate.humanReadableName());
            m_localAddress = address;
        }
    }
}

void SsdpService::loadOrCreateUdn()
{
    m_udn = QStringLiteral("uuid:") + readOrCreateUuid();
}

QString SsdpService::readOrCreateUuid()
{
    // UDN 是这台设备在网络上的身份证。控制点靠它认出"还是上次那台"，
    // 所以必须持久化 —— 每次启动都换新的，控制点就会看到一台全新设备。
    //
    // 两个候选位置，依次尝试：标准位置放不下时（权限、沙箱、只读盘），
    // 退到程序自己所在的目录。完全没有可写位置时才用临时标识，并明确报警。
    QStringList candidates;
    const QString standard = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!standard.isEmpty())
        candidates << standard;
    candidates << QCoreApplication::applicationDirPath();

    for (const QString &dir : candidates) {
        // 注意：mkpath 会返回成败。不管它的返回值，就等于把失败悄悄吞掉。
        if (!QDir().mkpath(dir))
            continue;

        QSettings settings(dir + QStringLiteral("/renderer.ini"), QSettings::IniFormat);
        QString uuid = settings.value(QStringLiteral("device/uuid")).toString();
        const bool isNew = uuid.isEmpty();

        if (isNew) {
            uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
            settings.setValue(QStringLiteral("device/uuid"), uuid);
            settings.sync();
        }

        // QSettings 写失败不会抛异常，得自己看状态。
        if (settings.status() != QSettings::NoError) {
            emit logMessage(QStringLiteral("位置 %1 写不进去，换下一个试试").arg(dir));
            continue;
        }

        m_configPath = dir + QStringLiteral("/renderer.ini");
        m_udnPersisted = true;
        emit logMessage(isNew
            ? QStringLiteral("已生成设备标识，保存到 %1").arg(m_configPath)
            : QStringLiteral("已从 %1 读回设备标识").arg(m_configPath));
        return uuid;
    }

    m_udnPersisted = false;
    emit logMessage(QStringLiteral("警告：所有候选位置都写不进去，本次使用临时设备标识。"
                                   "控制点会把本机认成一台新设备。"));
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool SsdpService::start()
{
    if (m_running)
        return true;

    chooseInterface();

    if (!m_interface.isValid()) {
        emit statusChanged(QStringLiteral("SSDP 未启动：找不到可用的网络接口"));
        return false;
    }

    loadOrCreateUdn();

    // 控制点上显示的名字。优先用计算机名 —— 用户在手机里看到自己的电脑名，
    // 比看到产品名更容易认出来是哪一台。
    m_friendlyName = qEnvironmentVariable("COMPUTERNAME");
    if (m_friendlyName.isEmpty())
        m_friendlyName = QHostInfo::localHostName();
    if (m_friendlyName.isEmpty())
        m_friendlyName = QStringLiteral("Windows PC");

    m_locationUrl = QStringLiteral("http://%1:%2/description.xml")
                        .arg(m_localAddress)
                        .arg(DefaultHttpPort);

    m_socket = new QUdpSocket(this);

    // ShareAddress：1900 端口上已经有卡巴斯基和系统 SSDP 服务在监听，
    // 不允许共享地址就根本绑不上。
    if (!m_socket->bind(QHostAddress::AnyIPv4, kSsdpPort,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        emit statusChanged(QStringLiteral("SSDP 未启动：绑定 1900 端口失败 —— ") + m_socket->errorString());
        delete m_socket;
        m_socket = nullptr;
        return false;
    }

    // 这两句是整件事的关键。不指定网卡的话，组播会顺着默认路由跑进代理软件的隧道，
    // 局域网里谁也收不到，而且不会有任何报错 —— 表面上一切正常。
    m_socket->setMulticastInterface(m_interface);
    if (!m_socket->joinMulticastGroup(kSsdpGroup, m_interface)) {
        emit statusChanged(QStringLiteral("SSDP 未启动：加入组播组失败 —— ") + m_socket->errorString());
        delete m_socket;
        m_socket = nullptr;
        return false;
    }

    connect(m_socket, &QUdpSocket::readyRead, this, &SsdpService::readPendingDatagrams);

    m_running = true;
    emit statusChanged(description());

    // UPnP 1.1 要求 alive 里带 BOOTID.UPNP.ORG，控制点靠它区分"设备重启了"和
    // "这只是同一条广播的重复"。用一个只增不减的秒数就够了。
    m_bootId = static_cast<quint32>(QDateTime::currentSecsSinceEpoch());

    // 立刻广播一次，再补一次 —— UDP 不保证送达，规范也建议同一条消息多发几遍。
    sendAlive();
    QTimer::singleShot(300, this, &SsdpService::sendAlive);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &SsdpService::sendAlive);
    if (m_broadcasting)
        m_timer->start(m_aliveIntervalMs);

    return true;
}

void SsdpService::setAliveIntervalMs(int ms)
{
    if (ms <= 0 || ms == m_aliveIntervalMs)
        return;

    m_aliveIntervalMs = ms;

    if (m_timer && m_broadcasting)
        m_timer->start(m_aliveIntervalMs);

    emit logMessage(QStringLiteral("SSDP 广播间隔改为 %1 秒").arg(ms / 1000.0, 0, 'g', 3));

    // 改完立刻广播一次，不用干等下一个周期 —— 用户刚勾上复选框就想看到效果。
    sendAlive();
}

void SsdpService::setBroadcasting(bool on)
{
    if (m_broadcasting == on)
        return;

    m_broadcasting = on;

    if (m_timer) {
        if (on)
            m_timer->start(m_aliveIntervalMs);
        else
            m_timer->stop();
    }

    if (on) {
        emit logMessage(QStringLiteral("已恢复定期广播（设备会被主动发现）"));
        // 开回来立刻喊一嗓子，别让用户干等一个周期。
        sendAlive();
    } else {
        // 关广播**不等于**停服务：socket 还开着、M-SEARCH 照常响应 ——
        // 手机主动搜还是找得到我们，只是我们不再隔一会儿喊一次。
        emit logMessage(QStringLiteral("已停止定期广播（仍会响应搜索）"));
    }
}

void SsdpService::announceGoingAwayBriefly()
{
    if (!m_running || !m_socket)
        return;

    emit logMessage(QStringLiteral("向网络宣告设备暂时离开（byebye），1.5 秒后重新广播"));

    const QStringList targets = notifyTargets(m_udn);
    for (const QString &target : targets)
        sendNotify(target, false);

    // 一两秒后再回来。太快的话控制点还没处理完 byebye，太慢的话用户会觉得设备丢了。
    QTimer::singleShot(1500, this, &SsdpService::sendAlive);
}

void SsdpService::stop()
{
    if (!m_running)
        return;

    if (m_timer) {
        m_timer->stop();
        delete m_timer;
        m_timer = nullptr;
    }

    if (m_socket) {
        // 走之前打声招呼，否则控制点要等 max-age 过期才会把我们划掉。
        const QStringList targets = notifyTargets(m_udn);
        for (const QString &target : targets)
            sendNotify(target, false);

        // 报一声：这条日志是"手机为什么会立刻把设备划掉"的答案。
        // （不发的话，控制点要等 max-age 过期才反应过来 —— 那可能是好几分钟，
        //   用户会觉得"我都退出了它还在那儿挂着"。）
        emit logMessage(QStringLiteral("已向网络宣告设备离开（byebye）"));

        // UDP 虽然是一发就走，但先 flush 一下再关，免得刚写进去的包跟着 socket
        // 一起没了。
        m_socket->flush();

        m_socket->leaveMulticastGroup(kSsdpGroup, m_interface);
        m_socket->close();
        delete m_socket;
        m_socket = nullptr;
    }

    m_running = false;
    emit statusChanged(QStringLiteral("SSDP: 已停止"));
}

void SsdpService::sendAlive()
{
    if (!m_running)
        return;

    const QStringList targets = notifyTargets(m_udn);
    for (const QString &target : targets)
        sendNotify(target, true);
}

void SsdpService::sendNotify(const QString &notificationType, bool alive)
{
    if (!m_socket)
        return;

    // 按规范：如果 NT 本身就是设备标识，USN 与它相同；否则是「设备标识::类型」。
    const QString usn = notificationType.startsWith(QLatin1String("uuid:"))
        ? notificationType
        : m_udn + QStringLiteral("::") + notificationType;

    QString message;
    message += QStringLiteral("NOTIFY * HTTP/1.1\r\n");
    message += QStringLiteral("HOST: %1:%2\r\n").arg(kSsdpGroup.toString()).arg(kSsdpPort);
    message += QStringLiteral("NT: %1\r\n").arg(notificationType);
    message += QStringLiteral("NTS: %1\r\n")
                   .arg(alive ? QStringLiteral("ssdp:alive") : QStringLiteral("ssdp:byebye"));
    message += QStringLiteral("USN: %1\r\n").arg(usn);

    if (alive) {
        // byebye 不带这几个字段：规范说 NTS 是 ssdp:byebye 时只带 HOST/NT/NTS/USN。
        message += QStringLiteral("CACHE-CONTROL: max-age=%1\r\n").arg(kMaxAgeSeconds);
        message += QStringLiteral("LOCATION: %1\r\n").arg(m_locationUrl);
        message += QStringLiteral("SERVER: %1\r\n").arg(serverHeader());
        message += QStringLiteral("BOOTID.UPNP.ORG: %1\r\n").arg(m_bootId);
    }

    message += QStringLiteral("\r\n");

    m_socket->writeDatagram(message.toUtf8(), kSsdpGroup, kSsdpPort);
}

void SsdpService::readPendingDatagrams()
{
    while (m_socket && m_socket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_socket->pendingDatagramSize()));

        QHostAddress sender;
        quint16 senderPort = 0;
        m_socket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        const QString text = QString::fromUtf8(datagram);

        if (text.startsWith(QLatin1String("M-SEARCH"), Qt::CaseInsensitive)) {
            handleSearch(sender, senderPort, text);
            continue;
        }

        const QString firstLine = text.section(QStringLiteral("\r\n"), 0, 0);

        emit logMessage(QStringLiteral("收到 SSDP 包 来自 %1:%2  %3 字节  |  %4")
                            .arg(sender.toString())
                            .arg(senderPort)
                            .arg(datagram.size())
                            .arg(firstLine));
    }
}

void SsdpService::handleSearch(const QHostAddress &sender, quint16 senderPort, const QString &request)
{
    // 控制点问的是一个具体的类型。ssdp:all 表示"把所有你能提供的都报一遍"。
    const QString searchTarget = headerValue(request, QStringLiteral("ST"));
    if (searchTarget.isEmpty())
        return;

    const QStringList allTypes = notifyTargets(m_udn);

    QStringList replyTypes;
    if (searchTarget.compare(QLatin1String("ssdp:all"), Qt::CaseInsensitive) == 0) {
        replyTypes = allTypes;
    } else {
        // 只有点名的类型是我们确实提供的，才回话。回一个自己都不支持的类型，
        // 控制点接下来会来抓 SCPD，然后发现对不上，反而更糟。
        for (const QString &type : allTypes) {
            if (type.compare(searchTarget, Qt::CaseInsensitive) == 0) {
                replyTypes << type;
                break;
            }
        }
    }

    if (replyTypes.isEmpty()) {
        emit logMessage(QStringLiteral("收到 M-SEARCH 来自 %1:%2，问的是 %3 —— 不是我们提供的，忽略")
                            .arg(sender.toString()).arg(senderPort).arg(searchTarget));
        return;
    }

    // 规范要求不要立刻回：MX 是控制点给的最长等待秒数，设备应该在 0..MX 之间随机等一段
    // 再回，否则一个网段里几十台设备会在同一瞬间一起回话，把请求方的缓冲区冲爆。
    //
    // 但窗口开太大是有代价的：控制点往往只等很短一会儿，我们要是抽到接近 MX 的延迟，
    // 它已经放弃了，表现就是"这个设备时有时无"。所以压到 0~200ms —— 既保留了随机错开，
    // 又几乎不可能迟到。
    const int mxSeconds = qMax(1, headerValue(request, QStringLiteral("MX")).toInt());
    const int delayMs = QRandomGenerator::global()->bounded(0, 200 + 1);

    emit logMessage(QStringLiteral("收到 M-SEARCH 来自 %1:%2  ST=%3  MX=%4 —— 将在 %5 ms 后回应 %6 条")
                        .arg(sender.toString())
                        .arg(senderPort)
                        .arg(searchTarget)
                        .arg(mxSeconds)
                        .arg(delayMs)
                        .arg(replyTypes.size()));

    const QHostAddress replyAddress = sender;
    QTimer::singleShot(delayMs, this, [this, replyAddress, senderPort, replyTypes] {
        for (const QString &type : replyTypes)
            sendSearchResponse(replyAddress, senderPort, type);
    });
}

void SsdpService::sendSearchResponse(const QHostAddress &to, quint16 port, const QString &searchTarget)
{
    if (!m_socket)
        return;

    const QString usn = searchTarget.startsWith(QLatin1String("uuid:"))
        ? searchTarget
        : m_udn + QStringLiteral("::") + searchTarget;

    // 注意这里是 ST 不是 NT —— 搜索回应和主动广播用的是两套字段名，
    // 混用的话严格的控制点会认不出来。
    QString message;
    message += QStringLiteral("HTTP/1.1 200 OK\r\n");
    message += QStringLiteral("CACHE-CONTROL: max-age=%1\r\n").arg(kMaxAgeSeconds);
    message += QStringLiteral("DATE: %1\r\n")
                   .arg(QLocale::c().toString(QDateTime::currentDateTimeUtc(),
                                              QStringLiteral("ddd, dd MMM yyyy HH:mm:ss 'GMT'")));
    message += QStringLiteral("EXT:\r\n");
    message += QStringLiteral("LOCATION: %1\r\n").arg(m_locationUrl);
    message += QStringLiteral("SERVER: %1\r\n").arg(serverHeader());
    message += QStringLiteral("ST: %1\r\n").arg(searchTarget);
    message += QStringLiteral("USN: %1\r\n").arg(usn);
    message += QStringLiteral("BOOTID.UPNP.ORG: %1\r\n").arg(m_bootId);
    message += QStringLiteral("\r\n");

    // 这是单播回应，直接发给提问的那台控制点。单播不看组播出口设置，
    // 走的是系统路由 —— 对方在同一个网段上，所以会从 Wi-Fi 出去。
    m_socket->writeDatagram(message.toUtf8(), to, port);
}
