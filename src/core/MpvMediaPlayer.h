// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "MediaPlayer.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QString>

/**
 * MpvMediaPlayer —— 管理外部 mpv.exe 进程，并通过 Windows 命名管道与它对话。
 *
 * 它做两件事：
 *   1. 启动 mpv.exe，让它在 \\.\pipe\mcast-mpv 上建立一个 JSON IPC 通道；
 *   2. 把播放指令翻译成 mpv 的 JSON 命令，通过那条管道发过去。
 *
 * 每个下面的小方法，本质上就是拼一条 mpv 命令。查 mpv 命令的权威依据是它自己的文档
 * （mpv.io/manual 里的 "List of Input Commands" / "Properties"）。
 */
class MpvMediaPlayer : public MediaPlayer
{
    Q_OBJECT

public:
    explicit MpvMediaPlayer(QObject *parent = nullptr);
    ~MpvMediaPlayer() override;

    /** 启动 mpv 并自动尝试连接 IPC 管道（mpv 起来需要时间，内部会自动重试）。 */
    void start(const QString &mpvExePath);

    /** 关闭 IPC 连接并结束 mpv 进程。程序退出时用。 */
    void shutdown();

    // ── MediaPlayer 接口的实现 ──────────────────────────────────────────
    // 语义写在 MediaPlayer.h 里，这里只写 mpv 这一侧特有的部分。

    /** loadfile：换掉当前正在播的内容（"replace"，不是排队追加）。 */
    void load(const QString &uri) override;

    void play() override;
    void pause() override;

    /** mpv 回到空闲状态，进程不退出，随时能再播下一个。 */
    void stop() override;

    void seekTo(double seconds) override;
    void setVolumePercent(int percent) override;
    void setMuted(bool muted) override;

    /**
     * 独立进程的 mpv 有自己的窗口，这里什么都不用做。
     * 留一个空实现，是为了将来换成 libmpv 时上层代码不用改。
     */
    void setVideoWindow(quintptr windowId) override;

    // 画面调节：这个后端没做。
    //
    // 它是留着排查问题时用的（起一个 mpv.exe、隔着命名管道喊话），要实现得往那套
    // IPC 里再加一批命令。这里如实返回"没有可调的项"，界面会把那一行自己收起来 ——
    // 摆一排按了没反应的滑块，比不摆还糟。
    QVector<PictureControlInfo> pictureControls() const override { return {}; }
    bool setPictureControl(const QString &, int) override { return false; }
    int  pictureControlValue(const QString &) const override { return 0; }

    /** 暂停或继续。对外的名字是 play() / pause()，这个更底层，内部用。 */
    void setPaused(bool paused);

    /** 当前是否已经和 mpv 建立好 IPC 连接。 */
    bool isConnected() const;

    // ── 缓存下来的播放状态 ────────────────────────────────────────────────
    // mpv 是主动把变化推过来的（observe_property），所以这里随时有一份新鲜的值。
    // 上层要读状态时读这几个函数就行，**绝不要**为了问一个值去等一次 IPC 往返 ——
    // 那是一来一回的网络式等待，会把 SOAP 请求卡住。
    double positionSeconds() const override { return m_positionSeconds; }
    double durationSeconds() const override { return m_durationSeconds; }
    int    volumePercent()   const override { return m_volumePercent; }
    bool   isMuted()         const override { return m_muted; }

    /** 发送一条 mpv 命令，例如 {"set_property", "title", "你好"}。 */
    void sendCommand(const QJsonArray &command);

signals:
    /** 状态文字变化，直接给界面显示用。 */
    void statusChanged(const QString &status);

    /** IPC 通道建立完成。 */
    void connected();

    // ready / ended / lost / positionChanged / durationChanged / volumeChanged /
    // muteChanged / pausedChanged 都声明在 MediaPlayer 里了。
    // 这里不重复声明 —— 同一件事有两个名字，上层就会不知道该认哪个。

private:
    // 每个被订阅的属性配一个编号，mpv 推事件时用它说明"是哪一条变了"。
    enum PropertyId {
        IdTimePos  = 1,
        IdDuration = 2,
        IdPause    = 3,
        IdVolume   = 4,
        IdMute     = 5,
    };

    void createSocket();
    void connectToPipe();
    void handleSocketError();
    void readAvailable();
    void observeProperties();
    void dispatchPropertyChange(const QJsonObject &event);

    QProcess     *m_process = nullptr;
    QLocalSocket *m_socket  = nullptr;

    QString    m_pipeName = QStringLiteral("mcast-mpv");
    QByteArray m_buffer;                    // 半行数据先攒着，凑齐一行再解析
    int        m_retriesLeft = 50;          // 每次重试间隔 100ms，最多等 5 秒
    bool       m_shuttingDown = false;      // 是我们自己关的，还是它自己没的

    double m_positionSeconds = 0.0;
    double m_durationSeconds = 0.0;
    int    m_volumePercent   = 100;
    bool   m_muted           = false;
};
