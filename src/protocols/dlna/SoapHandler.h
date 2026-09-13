// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "core/NowPlaying.h"
#include "core/PlaybackController.h"

#include <QObject>
#include <QString>

/**
 * SoapHandler —— 把控制点发来的 SOAP 请求翻译成对 PlaybackController 的调用。
 *
 * 这一层只干三件事：解析 XML 取参数、翻成中性调用、拼一段 XML 回过去。
 *
 * "当前在播什么、处于什么状态、队列里有什么"那三本账**不在这儿** ——
 * 在 PlaybackController 里。以前它们混在这个类里，结果是每加一个协议就得再记
 * 一遍同样三本账。
 *
 * ── 这一层还负责一件事：把中性的话翻成 DLNA 的话 ──────────────────────────
 *
 * 控制器说的是中性的枚举（`State::Playing` / `PlayMode::RepeatAll`）；DLNA 说的是
 * 规范定义好的字符串（`PLAYING` / `REPEAT_ALL`）。两边词汇表不一样，翻译是这一层
 * 的事 —— 将来加别的协议时，那个协议写它自己的翻译，两个协议互不干扰。
 *
 * 同理，**DIDL-Lite 元数据的解析也在这层**：那是 UPnP 的东西，不是通用概念。
 */
class SoapHandler : public QObject
{
    Q_OBJECT

public:
    explicit SoapHandler(PlaybackController *controller, QObject *parent = nullptr);

    /**
     * 处理一次 SOAP 调用，返回完整的响应 XML（出错时是 SOAP Fault）。
     *
     * service 是 "AVTransport" / "RenderingControl" / "ConnectionManager"；
     * action 是动作名，来自 SOAPACTION 头；body 是请求体原文。
     */
    QString handle(const QString &service, const QString &action, const QString &body);

    /** 当前传输状态，**已经是 DLNA 的词**。GENA 推事件时要用。 */
    QString transportState() const;

    /** 当前正在播放的内容。界面和 Windows 媒体面板都读它。 */
    NowPlaying nowPlaying() const;

signals:
    void logMessage(const QString &text);

    /** 传输状态变化。**参数是 DLNA 的词**，不是控制器的枚举。 */
    void transportStateChanged(const QString &state);

    /** "正在播放什么"变了（换片子，或者投送结束）。 */
    void nowPlayingChanged(const NowPlaying &info);

    /**
     * 队列或播放模式变了。
     *
     * 四个值一起报，是因为两个收件人各要一半：GENA 要把 NextAVTransportURI 和
     * CurrentPlayMode 写进状态推给控制点，Windows 媒体面板只关心前两个（那对
     * 「上一首/下一首」按钮亮不亮）。后两个是 DLNA 的词 —— 别的协议不需要跟着它走。
     */
    void queueChanged(bool hasNext, bool hasPrevious,
                      const QString &nextUri, const QString &playMode);

    /** 当前装的是哪个地址变了。这个信号只给 GENA 用。 */
    void mediaChanged(const QString &uri, const QString &metadata);

    /** 画面调节的三个标准值变了（**DLNA 单位**，0~100，50 = 原样）。 */
    void pictureControlsChanged(int brightness, int contrast, int sharpness);

private:
    // ── 词汇表翻译（中性 ↔ DLNA）─────────────────────────────────────────
    //
    // 只在这几处翻，别处一律用中性的话。将来接第二种协议时，那边写自己的一套。

    static QString dlnaStateName(PlaybackController::State state);
    static QString dlnaPlayModeName(PlaybackController::PlayMode mode);
    static QString dlnaLoadStatusName(PlaybackController::LoadStatus status);

    /** DLNA 的播放模式字符串 → 中性枚举。认不出来返回 false（调用方回 SOAP 错误）。 */
    static bool parsePlayMode(const QString &text, PlaybackController::PlayMode &out);

    /**
     * 后端那个 -100~100 的值，换成 DLNA 的 0~100（**50 才是"原样"**）。
     *
     * 这道换算只许有一处，所以 GetXxx 和事件推送都走它。
     */
    int dlnaPictureValue(const char *control) const;

    /**
     * 把一条 DIDL-Lite 元数据翻成 MediaRequest。
     *
     * 标题三层取值里的第一层在这里：控制点给的 dc:title 要先过"像不像标题"的筛子
     * —— vivo 相册投图片时，dc:title 里塞的就是文件名本身，那不是标题。
     * 后两层（从地址的文件名推、用类型名顶上）跟协议无关，在控制器里。
     */
    PlaybackController::MediaRequest mediaRequestFromSoap(const QString &uri,
                                                          const QString &metadata);

    QString handleAvTransport(const QString &action, const QString &body);
    QString handleRenderingControl(const QString &action, const QString &body);
    QString handleConnectionManager(const QString &action, const QString &body);

    PlaybackController *m_ctl = nullptr;
};
