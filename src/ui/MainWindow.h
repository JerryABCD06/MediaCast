// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QWidget>

#include "core/NowPlaying.h"

class QCheckBox;
class QCloseEvent;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QShowEvent;
class QSlider;

class DlnaRenderer;

/**
 * MainWindow —— 界面。
 *
 * 它**只认识 DlnaRenderer 一个对象**：命令从这里发出去，状态从这里读回来。
 * 界面拿不到播放器，也拿不到 SSDP / HTTP / SOAP / GENA 里的任何一个 ——
 * 于是"顺手直接操作播放器、结果状态机跟不上"那类错误，在结构上不可能发生。
 *
 * 现在里面装的还是调试面板：地址框、几个播放控制、进度条、音量，外加几行状态和日志。
 * 做正式界面时替换的只是这个类，别的模块一行都不用动。
 */
class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(DlnaRenderer *renderer, QWidget *parent = nullptr);

protected:
    /**
     * 窗口第一次显示时，把画面区的窗口号交给播放器。
     *
     * 必须等显示之后才拿得到 —— 控件真正变成系统里的一个窗口，是在它被显示的那一刻。
     * 这也是内嵌方案和外部进程方案最不一样的一步：进程版的 mpv 自己有窗口，不需要这个。
     */
    void showEvent(QShowEvent *event) override;

    /**
     * 关掉窗口**不是**退出程序。
     *
     * 这个程序的形态是常驻后台的媒体接收器：窗口只是它的一个界面，关掉之后它还要
     * 继续待在网络上、继续接受投送。真退出走托盘菜单里的「退出」。
     */
    void closeEvent(QCloseEvent *event) override;

private:
    /** 把控件都造出来并摆好。 */
    void buildUi();

    /** 把控件接到门面上 —— 命令往哪发，状态从哪来。 */
    void connectUi();

    /** 按"进度条位置 + 总时长"刷新那行时间文字。 */
    void refreshTimeText();

    /** 按当前选中的那一项，把滑块的范围和值对上。 */
    void syncPictureSlider();

    /** 把"正在播放什么"更新到界面上。 */
    void updateNowPlaying(const NowPlaying &info);

    DlnaRenderer *m_renderer = nullptr;

    QWidget     *m_videoArea = nullptr;
    bool         m_videoWindowGiven = false;

    QLineEdit   *m_urlEdit = nullptr;
    QCheckBox   *m_highRateCheck = nullptr;
    QPushButton *m_playButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_stopButton = nullptr;

    // 队列三件套里的「上一首/下一首」。没有可去的地方时会自己灰掉 ——
    // 亮着却按不动比灰着更让人恼火。
    QPushButton *m_prevButton = nullptr;
    QPushButton *m_nextButton = nullptr;

    QPushButton *m_disconnectButton = nullptr;

    QSlider     *m_posSlider = nullptr;
    QLabel      *m_timeLabel = nullptr;
    QSlider     *m_volSlider = nullptr;
    QLabel      *m_volLabel = nullptr;
    QPushButton *m_muteButton = nullptr;

    // 画面调节那一行：一个下拉 + 一根滑块。整个一行放在一个容器里，播放后端
    // 要是没有可调的项（比如那个没实现的进程版后端），整行直接藏起来。
    QWidget     *m_pictureRow = nullptr;
    QComboBox   *m_pictureCombo = nullptr;
    QSlider     *m_pictureSlider = nullptr;
    QLabel      *m_pictureValueLabel = nullptr;
    QPushButton *m_pictureResetButton = nullptr;

    QLabel      *m_playerStatusLabel = nullptr;
    QLabel      *m_nowPlayingLabel = nullptr;
    QLabel      *m_rendererStatusLabel = nullptr;
    QLabel      *m_logLabel = nullptr;

    /** 当前片子的总时长（秒）。进度条换算要用它。 */
    double m_durationSec = 0.0;
};
