// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "ImageSequencePlaylist.h"
#include "MediaPlayer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

struct mpv_handle;
class WrappedTsProxy;
namespace mcast::core {
class ImageSequenceStrategy;
}

/**
 * MpvCore —— libmpv 播放核心。
 *
 * 一个 mpv 实例，加上围着它转的一切：事件线程、状态缓存、命令、画面调节。
 * 画面统一走 mpv 的 render API，由 QML 场景图合成。MpvCore 管播放状态和
 * 事件线程，MpvQmlItem 只负责把画面画出来。
 *
 * ── 线程 ─────────────────────────────────────────────────────────────────
 *
 * 这里是本项目唯一开线程的地方，原因是 libmpv 的工作方式：它的 API 是事件驱动的，
 * 取事件的那个函数（mpv_wait_event）会一直阻塞到有事发生。要是放在界面线程里等，
 * 界面就彻底冻住了 —— 鼠标点不动、窗口不刷新。
 *
 * 所以有一个后台线程专门等事件，等到了就翻成 Qt 信号发出去（Qt 会自动把它转到界面
 * 线程）。命令方向相反：从界面线程直接调 libmpv 就行，它的接口是线程安全的。
 *
 * 这一层把线程完全包在里面 —— DLNA、界面、外壳都感觉不到它存在。
 */
class MpvCore : public MediaPlayer
{
    Q_OBJECT

    /// 引擎起来了没有。QML 那边靠它决定要不要显示"未就绪"的占位文字 ——
    /// 写成属性而不是只留个 isRunning() 方法，是因为 QML 读不了 C++ 的方法。
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)
    Q_PROPERTY(
        QVariantMap mediaInfo READ mediaInfo NOTIFY mediaInfoChanged)

public:
    explicit MpvCore(QObject *parent = nullptr);
    ~MpvCore() override;

    /**
     * 渲染面挂上来了没有。
     *
     * **为什么需要它**：render API 模式下，mpv 的视频输出要等到有人给它一个
     * 渲染上下文才能开，而那个上下文要等画面 item 真的开始渲染才建得起来。
     * 在这之前发 loadfile 的话，mpv 会报 "No render context set" 然后放弃 ——
     * 而且**不会再重试**：视频永远出不来，播放也起不来。
     *
     * 所以 load() 在这之前只把地址**攒着**，等 MpvQmlItem 那边把上下文建好、
     * 调 setRendererAttached(true) 的时候才真的放出去。
     */
    Q_INVOKABLE void setRendererAttached(bool attached);
    bool isRendererAttached() const { return m_rendererAttached; }

    /**
     * mpv 说"画面有新帧了"（从渲染线程转过来的）。
     *
     * **这个中转是必需的，不是多此一举。** mpv 的更新回调在渲染线程上跑，而真正
     * 要重画的那个 QML item 随时可能在界面线程上被销毁（切页面、关窗口都会）。
     * 回调里直接拿 item 的指针去 invokeMethod 就是野指针访问 —— 实测崩在 Qt6Core。
     *
     * 绕到这儿就安全了：MpvCore 活到程序结束，invoke 它永远有效；再由它发信号，
     * item 已经不在了的话 Qt 自己会把连接摘掉。
     */
    Q_INVOKABLE void notifyRenderUpdate();

    /**
     * mpv 实例本身。start() 之前是 nullptr。
     *
     * 为什么放开到 public：**用 render API 渲染就必须拿到它** —— 建渲染上下文、
     * 每一帧渲染，第一个参数都是这个句柄。走这条路的是 MpvQmlItem，它不继承
     * MpvCore（要继承 QQuickFramebufferObject），所以 protected 挡不住它、
     * 也不该挡。
     *
     * 用的人请只做"外壳该做的事"（设输出选项、建渲染上下文）；播放命令和状态
     * 一律走这个类自己的方法，别绕过去 —— 绕过去就等于跳过了状态机，项目早期
     * 那几个难查的 bug 全是这么来的。
     */
    mpv_handle *handle() const { return m_mpv; }

    /** 建好 mpv 实例（不加载任何东西）。 */
    bool start();

    /** 停掉后台线程并销毁 mpv 实例。 */
    void shutdown();

    /** 真正把 loadfile 发出去的地方。load() 和 setRendererAttached() 都走它。 */
    void loadNow(const QString &uri);

    bool isRunning() const { return m_mpv != nullptr; }

    /** MPV 当前媒体的轨道、容器和编码摘要，供信息面板只读显示。 */
    QVariantMap mediaInfo() const;

signals:
    /** isRunning() 的取值变了。 */
    void runningChanged();

    /** `mediaInfo` 更新。 */
    void mediaInfoChanged();

    /**
     * 画面有新帧，该重画了。
     *
     * 只有 MpvQmlItem 会连它（它自己也只做一件事：调 update()）。
     * 用信号而不是直接持有 item，是因为发出来的这一方在**渲染线程**上 ——
     * 见 notifyRenderUpdate() 的说明。
     */
    void renderUpdate();

    /** render API 已完成首笔实际渲染，播放命令可以安全下发。 */
    void rendererReady();

    // 这行 public: 不能省。moc 的规则是"从 signals: 开始、后面全当信号"，
    // 直到遇到下一个访问说明符才停 —— 少了它，下面那些普通成员函数会被当成
    // 信号，moc 直接报 "Not a signal declaration"。
    // （signals: 展开成 public 加一个给 moc 看的标记，所以它对 C++ 的访问控制
    //   是多余的、对 moc 不是 —— 别当成废话删掉。）
public:

    // ── MediaPlayer ──────────────────────────────────────────────────────

    void load(const MediaLoadRequest &request) override;
    void play() override;
    void pause() override;
    void stop() override;
    void unload() override;
    void seekTo(double seconds) override;
    void setVolumePercent(int percent) override;
    void setMuted(bool muted) override;

    // 画面调节。这张表是照 mpv 自己的选项表抄的（`mpv --list-options` 的实测结果），
    // 不是凭印象写的：色温、颜色增益、梯形校正这几项 mpv 根本没有对应属性，
    // 所以这里也没有 —— 硬凑出来只会让界面上多几个按了没反应的滑块。
    QVector<PictureControlInfo> pictureControls() const override;
    bool setPictureControl(const QString &name, int value) override;
    int  pictureControlValue(const QString &name) const override;

    // 这些值由后台线程更新、界面线程随时读 —— 所以用原子变量，不加锁。
    double positionSeconds() const override { return m_positionSec.load(); }
    double durationSeconds() const override { return m_durationSec.load(); }

    /** 当前文件有多大。按字节跳转要靠它换算成时间。 */
    qint64 mediaSizeBytes() const override;

    int    volumePercent()   const override { return m_volumePercent.load(); }
    bool   isMuted()         const override { return m_muted.load(); }

protected:
    /**
     * 在 mpv_initialize() 之前设置只能初始化前生效的输出选项。
     *
     * 生产 Ui 使用基类的 render API 设置；无头测试或未来的其他渲染壳可以覆盖。
     */
    virtual void applyStartupOptions(mpv_handle *mpv);

private:
    /** 后台线程：一直等 mpv 的事件，翻成 Qt 信号。 */
    void eventLoop();

    /** 普通媒体直接加载。协议层负责提前决定是否需要兼容路线。 */
    void beginLoad(const QString &uri);
    void activateLoad(const MediaLoadRequest &request);
    void prepareRouteLoad(const QString &logicalUri);
    bool loadImageSequencePlan(const MediaLoadRequest &request);
    bool loadMfImageSequence(
        const QVector<mcast::core::ImageSequenceSegment> &segments,
        const QString &logicalUri);
    void loadWrappedTransportStream(
        const QVector<mcast::core::ImageSequenceSegment> &segments,
        const QString &logicalUri);
    void loadMpvFile(
        const QString &mpvUri, const QString &logicalUri,
        const QString &temporaryImageListPath = {},
        bool emitStarted = true);
    void clearTemporaryImageSequence();
    QString logicalMediaUri(const QString &mpvUri) const;
    void resetImageTimeline();
    void publishMediaSummary();
    void refreshMediaInfoSnapshot();
    void finishPacketCorruptBurst();

    /**
     * 锐度走视频滤镜链，不走属性 —— 为什么，见 .cpp 里那段。
     * 滤镜参数 mpv 不当属性暴露，所以这个值自己记。
     */
    bool applySharpen(int value);

    mpv_handle *m_mpv = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};
    /**
     * 渲染面挂上来了没有。
     *
     * 界面线程和渲染线程都会碰它：渲染线程建好上下文之后回调过来置 true，
     * 界面线程在 load() 里读。用原子变量省得加锁。
     */
    std::atomic<bool> m_rendererAttached{false};

    /**
     * 片子播到头了没有（mpv 的 eof-reached）。
     *
     * 只有 play() 用它：播完之后再按播放，得先回到 0，否则一放开又立刻 EOF，
     * 用户看到的是"按了没反应"。开了 keep-open 之后文件还在手上，所以能重放。
     */
    std::atomic<bool> m_eofReached{false};

    /**
     * 文件加载好之后要不要自己开播。
     *
     * 平时是 true：投上来一条就该放。**但 stop() 和 pause() 会把它按下去** ——
     * 加载要花时间（网络地址尤其慢），用户完全可能在"还在加载"的时候就让停，
     * 那时候这一声"加载好了"要是照常开播，就成了"我明明停了，它自己又放起来"。
     *
     * 只在 eventLoop 和调用线程之间传，用原子量省得加锁。
     */
    std::atomic<bool> m_playOnLoad{true};
    std::atomic<std::uint64_t> m_pendingLoadGeneration{0};
    std::atomic<std::uint64_t> m_activeLoadGeneration{0};

    /**
     * 渲染面还没就绪时，先攒在这儿的那条地址。
     *
     * 只由界面线程读写（load() 和 setRendererAttached() 都在界面线程）。
     */
    std::optional<MediaLoadRequest> m_pendingLoad;
    std::optional<MediaLoadRequest> m_lastLoadRequest;
    mutable std::mutex m_uriMutex;
    mutable std::mutex m_mediaInfoMutex;
    QVariantMap m_mediaInfo;
    QString m_lastRequestedUri;
    QString m_activeMpvUri;
    QString m_activeImageListPath;
    QString m_eventMediaUri;
    QString m_pendingArtworkUri;
    bool m_artworkPending = false;
    int m_artworkRetryCount = 0;
    std::atomic<bool> m_loadFailed{false};

    std::unique_ptr<WrappedTsProxy> m_wrappedTsProxy;
    std::unique_ptr<mcast::core::ImageSequenceStrategy>
        m_imageSequence;
    std::atomic<bool> m_imageSequenceActive{false};
    std::atomic<std::uint64_t> m_imageSequenceLoadGeneration{0};

    std::atomic<double> m_positionSec{0.0};
    std::atomic<double> m_durationSec{0.0};
    bool m_packetCorruptBurstActive = false;
    int m_packetCorruptCount = 0;
    std::chrono::steady_clock::time_point m_packetCorruptLast;
    std::atomic<int>    m_volumePercent{100};
    std::atomic<bool>   m_muted{false};

    /** 当前锐度（我们这边的 -100~100）。只有界面线程读写。 */
    int m_sharpen = 0;
};
