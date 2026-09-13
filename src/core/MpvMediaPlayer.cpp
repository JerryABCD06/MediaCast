// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "MpvMediaPlayer.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QCoreApplication>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

MpvMediaPlayer::MpvMediaPlayer(QObject *parent)
    : MediaPlayer(parent)
{
}

MpvMediaPlayer::~MpvMediaPlayer()
{
    shutdown();
}

void MpvMediaPlayer::start(const QString &mpvExePath)
{
    if (m_process)
        return;                             // 已经在跑了，不重复启动

    const QString pipePath = QStringLiteral("\\\\.\\pipe\\") + m_pipeName;

    m_process = new QProcess(this);
    m_process->setProgram(mpvExePath);
    m_process->setArguments({
        QStringLiteral("--idle=yes"),        // 没文件也保持运行，等着我们发 loadfile
        QStringLiteral("--force-window=yes"),// 即使空闲也把播放窗口开出来
        QStringLiteral("--no-terminal"),     // 不用它自带的终端交互
        QStringLiteral("--input-ipc-server=") + pipePath,

        // ── 关掉 mpv 自带的一切控制 ────────────────────────────────────────
        //
        // mpv 默认带一套交互：鼠标移上去弹出控制条、空格暂停、方向键快进快退、
        // q 直接退出。这些是**唯一还能绕过我们状态机的入口** —— 用户用它们改了什么，
        // DLNA 那一层完全不知道，于是手机上显示的状态就会和电脑上实际的不一致
        // （典型症状：横幅说暂停、进度条却在走）。
        //
        // 所以这里把控制权收回：mpv 只负责把画面显示出来，所有操作都从我们这边走。
        QStringLiteral("--osc=no"),                     // 不要屏幕控制条
        QStringLiteral("--no-input-default-bindings"),  // 不要默认快捷键
        QStringLiteral("--cursor-autohide=always"),     // 鼠标盖在画面上也藏起来
    });

    // mpv.exe 是控制台程序。这里告诉 Windows 别给它分配黑色控制台窗口，
    // 否则我们会看到多出来一个没用的 cmd 窗口。
    m_process->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });

    connect(m_process, &QProcess::started, this, [this] {
        emit statusChanged(QStringLiteral("mpv 已启动，正在连接 IPC 通道 ..."));
        connectToPipe();
    });

    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit statusChanged(QStringLiteral("mpv 启动失败：") + m_process->errorString());
    });

    connect(m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (m_shuttingDown) {
            // 是我们自己关的，属于正常收尾，不用惊动上层。
            emit statusChanged(QStringLiteral("mpv 已关闭"));
            return;
        }
        emit statusChanged(QStringLiteral("mpv 进程意外退出"));
        emit lost();
    });

    emit statusChanged(QStringLiteral("正在启动 mpv ..."));
    m_process->start();
}

void MpvMediaPlayer::shutdown()
{
    m_shuttingDown = true;

    if (m_socket) {
        m_socket->abort();
    }

    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(3000))
            m_process->kill();
    }
}

void MpvMediaPlayer::load(const QString &uri)
{
    // "replace" 表示替换掉当前正在播的东西（而不是排队追加）。
    sendCommand(QJsonArray{
        QStringLiteral("loadfile"),
        uri,
        QStringLiteral("replace"),
    });
}

void MpvMediaPlayer::setPaused(bool paused)
{
    // mpv 的 pause 属性是个开关：true 暂停，false 继续。
    sendCommand(QJsonArray{
        QStringLiteral("set_property"),
        QStringLiteral("pause"),
        paused,
    });
}

void MpvMediaPlayer::play()
{
    setPaused(false);
}

void MpvMediaPlayer::pause()
{
    setPaused(true);
}

void MpvMediaPlayer::setVideoWindow(quintptr windowId)
{
    // 独立进程的 mpv 画在自己的窗口里，不需要我们给它一个窗口。
    // 这个函数是给将来的 libmpv 实现留的位置，到时候它会真的用上 windowId。
    Q_UNUSED(windowId);
}

void MpvMediaPlayer::stop()
{
    sendCommand(QJsonArray{ QStringLiteral("stop") });
}

void MpvMediaPlayer::seekTo(double seconds)
{
    // "absolute" 是"跳到第 N 秒"，而不是"往前/往后 N 秒"。
    sendCommand(QJsonArray{
        QStringLiteral("seek"),
        seconds,
        QStringLiteral("absolute"),
    });
}

void MpvMediaPlayer::setVolumePercent(int percent)
{
    // mpv 的音量刻度本来就是 0..100，和 UPnP 的 RenderingControl 完全一致，
    // 不需要像 ExoPlayer 那样换算成 0.0~1.0 的增益。
    sendCommand(QJsonArray{
        QStringLiteral("set_property"),
        QStringLiteral("volume"),
        qBound(0, percent, 100),
    });
}

void MpvMediaPlayer::setMuted(bool muted)
{
    sendCommand(QJsonArray{
        QStringLiteral("set_property"),
        QStringLiteral("mute"),
        muted,
    });
}

void MpvMediaPlayer::observeProperties()
{
    // 订阅之后，这些属性一旦变化，mpv 会主动推 property-change 事件过来。
    // 不订阅就只能我们不停去问——既费事，又不及时。
    const struct { int id; const char *name; } properties[] = {
        { IdTimePos,  "time-pos" },
        { IdDuration, "duration" },
        { IdPause,    "pause"    },
        { IdVolume,   "volume"   },
        { IdMute,     "mute"     },
    };

    for (const auto &property : properties) {
        sendCommand(QJsonArray{
            QStringLiteral("observe_property"),
            property.id,
            QString::fromLatin1(property.name),
        });
    }
}

void MpvMediaPlayer::dispatchPropertyChange(const QJsonObject &event)
{
    const int id = event.value(QStringLiteral("id")).toInt(-1);
    const QJsonValue data = event.value(QStringLiteral("data"));

    // 属性暂时没有值时 mpv 会推 null（例如停止播放后 time-pos 就没了），
    // 这时候当成 0，界面才会正确地归零。
    switch (id) {
    case IdTimePos:
        m_positionSeconds = data.isDouble() ? data.toDouble() : 0.0;
        emit positionChanged(m_positionSeconds);
        break;
    case IdDuration:
        m_durationSeconds = data.isDouble() ? data.toDouble() : 0.0;
        emit durationChanged(m_durationSeconds);
        break;
    case IdPause:
        emit pausedChanged(data.toBool(false));
        break;
    case IdVolume:
        m_volumePercent = data.isDouble() ? qRound(data.toDouble()) : 0;
        emit volumeChanged(m_volumePercent);
        break;
    case IdMute:
        m_muted = data.toBool(false);
        emit muteChanged(m_muted);
        break;
    default:
        break;
    }
}

bool MpvMediaPlayer::isConnected() const
{
    return m_socket && m_socket->state() == QLocalSocket::ConnectedState;
}

void MpvMediaPlayer::sendCommand(const QJsonArray &command)
{
    if (!isConnected()) {
        emit statusChanged(QStringLiteral("mpv IPC 还没连上，命令没有发出去"));
        return;
    }

    QJsonObject object;
    object[QStringLiteral("command")] = command;

    // mpv 的 IPC 协议是「一行一条 JSON」，所以末尾必须补一个换行符。
    QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    payload.append('\n');

    m_socket->write(payload);
    m_socket->flush();
}

void MpvMediaPlayer::createSocket()
{
    m_socket = new QLocalSocket(this);

    connect(m_socket, &QLocalSocket::connected, this, [this] {
        m_retriesLeft = 50;                 // 连上了就把重试次数恢复，方便以后重连
        emit statusChanged(QStringLiteral("已连接 mpv IPC 管道：\\\\.\\pipe\\") + m_pipeName);
        observeProperties();                // 一连上就开始订阅，之后 mpv 会主动报状态

        // 顺手把 mpv 窗口的标题改成我们的名字 —— 用户看到的是产品名而不是 "mpv"。
        // 这件事以前写在 main() 里，属于界面越界去碰 mpv 的细节。
        sendCommand(QJsonArray{
            QStringLiteral("set_property"),
            QStringLiteral("title"),
            QCoreApplication::applicationName(),
        });

        emit connected();
    });

    connect(m_socket, &QLocalSocket::disconnected, this, [this] {
        emit statusChanged(QStringLiteral("与 mpv 的 IPC 连接已断开"));
    });

    connect(m_socket, &QLocalSocket::readyRead, this, &MpvMediaPlayer::readAvailable);

    connect(m_socket, &QLocalSocket::errorOccurred, this,
            [this](QLocalSocket::LocalSocketError) { handleSocketError(); });
}

void MpvMediaPlayer::connectToPipe()
{
    if (!m_socket)
        createSocket();

    // 正在连接或者已经连上了，就不用再发起一次。
    if (m_socket->state() != QLocalSocket::UnconnectedState)
        return;

    m_socket->connectToServer(m_pipeName);
}

void MpvMediaPlayer::handleSocketError()
{
    // 连上之后才发生的错误不属于"还没找到管道"，交给 disconnected 处理。
    if (isConnected())
        return;

    // mpv 刚启动时管道还没建好，第一次连接失败是正常的，过 100ms 再试。
    if (m_retriesLeft > 0) {
        --m_retriesLeft;
        QTimer::singleShot(100, this, &MpvMediaPlayer::connectToPipe);
        return;
    }

    emit statusChanged(QStringLiteral("连接 mpv IPC 失败：") + m_socket->errorString());
}

void MpvMediaPlayer::readAvailable()
{
    m_buffer.append(m_socket->readAll());

    // mpv 每一条 JSON 后面都是一个换行符。可能一次读到半条，也可能一次读到好几条，
    // 所以按换行切开，只处理完整的行，剩下的留在缓冲区里等下一批数据。
    int newlineIndex;
    while ((newlineIndex = m_buffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_buffer.left(newlineIndex);
        m_buffer.remove(0, newlineIndex + 1);

        if (line.trimmed().isEmpty())
            continue;

        const QJsonObject event = QJsonDocument::fromJson(line).object();
        const QString eventName = event.value(QStringLiteral("event")).toString();

        if (eventName == QLatin1String("property-change")) {
            // 属性变化属于"背景噪音"：播放时每秒都会来好几条。交给专门的信号处理，
            // 不往界面上那行"mpv 回复"里送，否则它会一直在跳。
            dispatchPropertyChange(event);
        } else if (eventName == QLatin1String("file-loaded")) {
            // mpv 说"文件已经准备好了"。DLNA 就是在等这一声。
            emit ready();
        } else if (eventName == QLatin1String("start-file")) {
            emit logMessage(QStringLiteral("mpv 开始加载媒体 ..."));
        } else if (eventName == QLatin1String("end-file")) {
            // 文件结束：可能是播完了，也可能是被停掉或加载失败。
            //
            // 播放失败时 mpv 会把原因写在 file_error 里。以前这里只把事件名打出来，
            // 结果"投上去没反应"就只能靠猜 —— 是地址过期、被 CDN 拒绝、还是网络不通，
            // 全看不见。现在把原因显示出来。
            const QString reason = event.value(QStringLiteral("reason")).toString();
            const QString fileError = event.value(QStringLiteral("file_error")).toString();

            if (!fileError.isEmpty())
                emit logMessage(QStringLiteral("mpv 播放失败：%1").arg(fileError));
            else
                emit logMessage(QStringLiteral("mpv 结束播放（%1）").arg(reason));

            emit ended();
        } else {
            emit logMessage(QString::fromUtf8(line));
        }
    }
}
