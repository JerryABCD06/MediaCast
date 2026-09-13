// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>
#include <QtGlobal>

/**
 * 一个"画面调节项"的描述。
 *
 * 界面靠它自己长出下拉列表来 —— 名字、量程、中性值都在这里，界面不需要知道
 * 底下是 mpv 还是别的什么，也不需要为每一项单独写一段代码。
 */
struct PictureControlInfo
{
    QString name;      // 短名（"brightness"）。命令和查询都用它，别改。
    QString label;     // 给人看的名字（"亮度"）。
    int     min = 0;
    int     max = 0;
    int     neutral = 0;   // 复位到这儿，也是"没调过"的意思。
};

/**
 * MediaPlayer —— 播放器的抽象接口。
 *
 * 这一层存在的理由只有一个：让 DLNA 那一层**不知道**底下是什么。今天是
 * "mpv.exe 独立进程 + JSON IPC"；将来换成 libmpv 嵌入时，只需要再写一个实现类，
 * DLNA 那边一行都不用动。
 *
 * 两条约定，两条都是踩过坑才定下来的：
 *
 * 1. **加载是异步的。** load() 返回时片子还没准备好，所以用 ready() 信号告诉上层。
 *    DLNA 的 TRANSITIONING 状态就是在等这一声 —— 少了它，控制点会永远卡在
 *    "正在准备"。
 *
 * 2. **状态读取必须是取缓存，不能现问底下。** 位置、时长、音量这些值由实现自己
 *    缓存好（mpv 会把变化主动推过来）。上层随时读都是立刻返回 —— 绝不能在读一个
 *    状态值时去等一次跨进程往返，那会把 SOAP 请求卡住。
 */
class MediaPlayer : public QObject
{
    Q_OBJECT

public:
    explicit MediaPlayer(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

    ~MediaPlayer() override = default;

    // ── 命令 ──────────────────────────────────────────────────────────────

    /** 加载并开始播放一个地址。可以是本地路径，也可以是 http 地址。 */
    virtual void load(const QString &uri) = 0;

    virtual void play() = 0;
    virtual void pause() = 0;

    /**
     * 停止播放，但**内容仍然装着** —— 之后再调 play() 应该能接着放。
     *
     * 这条语义是 UPnP 定的（停止不等于卸载）。实现方别把它做成"卸载" ——
     * mpv 的 `stop` 命令就是卸载，拿它实现这个方法的话，停止之后按播放会
     * 一点反应都没有：状态机以为在放，播放器手上却已经没有文件了。
     */
    virtual void stop() = 0;

    /**
     * 把内容卸掉，播放器回到"手上什么都没有"。
     *
     * 和 stop() 的区别就是上面那一条。会话结束（挂断投送）时该用这个 ——
     * 那时候确实不该再留着上一条片子。默认实现退化成 stop()，不认识这两种
     * 区别的后端不用管。
     */
    virtual void unload() { stop(); }

    virtual void seekTo(double seconds) = 0;

    /** 0..100，和 DLNA 的 RenderingControl 同一把尺子。 */
    virtual void setVolumePercent(int percent) = 0;
    virtual void setMuted(bool muted) = 0;

    // ── 状态读取（取缓存，立刻返回）──────────────────────────────────────

    virtual double positionSeconds() const = 0;
    virtual double durationSeconds() const = 0;

    /**
     * 当前媒体的总字节数。拿不到就返回 0 —— 所以给个默认实现，不认识它的后端不用管。
     *
     * 这是给"按字节跳转"用的（DLNA 的 X_DLNA_REL_BYTE）：控制点给的是**字节位置**，
     * 而我们手上只有时间轴，得靠"总字节数 + 总时长"按比例换算过去。
     */
    virtual qint64 mediaSizeBytes() const { return 0; }

    virtual int    volumePercent() const = 0;
    virtual bool   isMuted() const = 0;

    /**
     * 让播放器把画面画到指定的窗口里。
     *
     * 现在底下是独立进程的 mpv，它有自己的窗口，所以实现是空的。
     * 将来换成 libmpv 嵌入时，UI 会把自己某个控件的窗口句柄传进来。
     *
     * 现在就把这个入口留好，是为了避免将来为了加它而回头改接口 —— 改接口会波及
     * 每一个实现，那正是拆解耦最痛的时刻。
     */
    virtual void setVideoWindow(quintptr windowId) = 0;

    // ── 画面调节 ──────────────────────────────────────────────────────────
    //
    // 做成"一张表 + 两个通用方法"，而不是十几对 get/set —— 以后加一项只要往表里
    // 加一行，接口、界面、DLNA 那边都不用动。

    /** 有哪些可调的项。不做画面调节的后端就返回空表，界面会自己把那一行收起来。 */
    virtual QVector<PictureControlInfo> pictureControls() const = 0;

    /** 设某一项的值。认不出这个名字（或后端不做这项）返回 false。 */
    virtual bool setPictureControl(const QString &name, int value) = 0;

    /** 读某一项当前的值。认不出就返回 0。 */
    virtual int pictureControlValue(const QString &name) const = 0;

    /** 全部复位到中性值。拿上面那张表挨个设回去，不各个后端各写一遍。 */
    void resetPictureControls()
    {
        for (const PictureControlInfo &control : pictureControls())
            setPictureControl(control.name, control.neutral);
    }

signals:
    /** 播放引擎自身的状态文字（"正在启动"、"已就绪"之类），给界面显示用。 */
    void statusChanged(const QString &text);

    /** 诊断日志行。界面和日志文件都看着它。 */
    void logMessage(const QString &text);

    /** 文件加载完成、可以播了。 */
    void ready();

    /**
     * **真正**开始加载了。
     *
     * 大多数后端里它和 load() 是同一时刻。但 render API 模式下，加载可能要等
     * 渲染面就绪才能发出去（没有渲染上下文，mpv 开不了视频输出）—— 那时候
     * 这个信号会明显晚于 load()。
     *
     * 上层用它给"加载超时"计时：从调用 load() 开始算的话，等界面的那段时间
     * 会被算进去，白白超时。
     */
    void loadStarted();

    /** 播放结束：播完了、被停掉了、或者出错。 */
    void ended();

    /**
     * 播放器没了 —— 进程退出、崩溃，总之播放能力消失了。
     * 这不是"播放结束"，收到之后上层应当把投送会话也结束掉。
     */
    void lost();

    void positionChanged(double seconds);
    void durationChanged(double seconds);
    void volumeChanged(int percent);
    void muteChanged(bool muted);
    void pausedChanged(bool paused);

    /**
     * 某个画面调节项变了。**不管是谁改的**都从这里报 —— 界面拖的、控制点设的、
     * 还是复位一口气全改的，都要经过 setPictureControl，所以这里是唯一的出口。
     *
     * DLNA 那边靠它给订阅过的控制点推事件；没有这个信号的话，电脑上拖一下亮度，
     * 手机那边的界面不会知道。
     */
    void pictureControlChanged(const QString &name, int value);

    /**
     * **文件自带的**标签变了（标题 / 艺术家 / 专辑 / 歌词……）。
     *
     * 这和协议层给的那份元数据**不是一回事**，两边各有各的用处：
     *
     *   协议给的（DLNA 是 DIDL）—— 控制点对"这是什么"的描述，投送一开始就有，
     *                              但可能是文件名、可能是占位符。
     *   文件自带的（这个信号）  —— 文件自己里面写的，得等文件打开了才知道。
     *                              图片和视频基本是空的（实测：手机照片、录屏、
     *                              网上下载的视频里一个有用的字段都没有），
     *                              音频则普遍带，**歌词也只有这儿才有**。
     *
     * 所以谁都不覆盖谁：协议给的就用协议的，没给的拿这里的补。
     * 键名随容器变（同一个字段 FLAC 里叫 title、MKV 里叫 TITLE），取值要不分大小写。
     *
     * 不做这一步的后端不实现也不会怎样 —— 什么都不发就是了。
     */
    void metadataChanged(const QVariantMap &tags);
};
