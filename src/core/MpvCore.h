#pragma once

#include "MediaPlayer.h"

#include <atomic>
#include <thread>

struct mpv_handle;

/**
 * MpvCore —— libmpv 播放核心。
 *
 * 一个 mpv 实例，加上围着它转的一切：事件线程、状态缓存、命令、画面调节。
 * **它不认识"画面往哪出"这件事** —— 那是外壳的事，见下。
 *
 * 它有三种用法，对应三种外壳：
 *
 *   LibMpvPlayer    继承它，补上 setVideoWindow —— 走 mpv 的 wid 选项，
 *                   画面画进一个原生窗口。旧的 Widgets 界面用这条。
 *   QML 那套        不继承它，而是持有一个它，再走 mpv 的 render API
 *                   自己渲染。QML 那边要继承 QQuickFramebufferObject，
 *                   两个 QObject 没法多重继承，所以只能"持有"。
 *   MpvMediaPlayer  另外那个后端（起 mpv.exe + 命名管道），跟它没关系。
 *
 * ── 为什么要拆出这一层 ────────────────────────────────────────────────────
 *
 * 原来 LibMpvPlayer 是一个整体：既管 mpv 实例和事件线程，又管画面往哪个窗口出。
 * 加 QML 界面时发现，这两件事里**只有一处**（mpv 的 wid 选项）是跟"原生窗口"
 * 绑死的，其余四百多行（事件线程、状态机、按键换算、滤镜链）两个外壳完全一样。
 * 不拆的话就得把四百行抄一遍，抄出来的第二份迟早跟第一份不一样。
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

public:
    explicit MpvCore(QObject *parent = nullptr);
    ~MpvCore() override;

    /** 建好 mpv 实例（不加载任何东西）。 */
    bool start();

    /** 停掉后台线程并销毁 mpv 实例。 */
    void shutdown();

    bool isRunning() const { return m_mpv != nullptr; }

    // ── MediaPlayer ──────────────────────────────────────────────────────

    void load(const QString &uri) override;
    void play() override;
    void pause() override;
    void stop() override;
    void seekTo(double seconds) override;
    void setVolumePercent(int percent) override;
    void setMuted(bool muted) override;

    /**
     * 把画面画到某个窗口里。**核心这一层做不了这件事** —— 它手里只有 mpv
     * 实例，不知道画面该往哪去。所以这里是个空实现，等子类去覆盖：
     *
     *   LibMpvPlayer 覆盖它，设 mpv 的 wid 选项
     *   QML 那套      压根不调它，走 render API
     *
     * 基类留一个能用的默认实现（而不是做成纯虚的），是为了让它**可以被实例化** ——
     * QML 那边要直接拿一个 MpvCore 用，它不继承这个类，只是持有。
     */
    void setVideoWindow(quintptr windowId) override;

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
     * mpv 实例。给子类在运行中改选项用。start() 之前是 nullptr。
     *
     * 为什么放开这个：子类要设的那些选项（wid 就是）是"这个外壳特有的"，
     * 基类不该知道；但设置动作得落到同一个 mpv 实例上。
     */
    mpv_handle *handle() const { return m_mpv; }

    /**
     * 在 mpv_initialize() 之前调一次，让子类设"只能初始化前设"的选项。
     *
     * 为什么需要这个口子：mpv 的某些选项必须在初始化之前设好（wid 就是），
     * 而初始化在基类的 start() 里，子类插不进去。所以留这么一个钩子。
     *
     * 参数直接把 mpv 实例递出去，子类就不用去摸基类的成员。
     * 基类什么都不做。
     */
    virtual void applyStartupOptions(mpv_handle *mpv);

private:
    /** 后台线程：一直等 mpv 的事件，翻成 Qt 信号。 */
    void eventLoop();

    /**
     * 锐度走视频滤镜链，不走属性 —— 为什么，见 .cpp 里那段。
     * 滤镜参数 mpv 不当属性暴露，所以这个值自己记。
     */
    bool applySharpen(int value);

    mpv_handle *m_mpv = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_stopRequested{false};

    std::atomic<double> m_positionSec{0.0};
    std::atomic<double> m_durationSec{0.0};
    std::atomic<int>    m_volumePercent{100};
    std::atomic<bool>   m_muted{false};

    /** 当前锐度（我们这边的 -100~100）。只有界面线程读写。 */
    int m_sharpen = 0;
};
