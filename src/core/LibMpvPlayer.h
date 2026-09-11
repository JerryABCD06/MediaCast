#pragma once

#include "MediaPlayer.h"

#include <atomic>
#include <thread>

struct mpv_handle;

/**
 * LibMpvPlayer —— 把 libmpv 内嵌进程序里当播放后端。
 *
 * 和 MpvMediaPlayer 的区别，一句话说清：
 *
 *   MpvMediaPlayer   起一个 mpv.exe，画面在它自己的窗口里，我们隔着命名管道喊话。
 *   LibMpvPlayer     把 mpv 当库用，画面直接画到我们指定的窗口里，没有多余的进程。
 *
 * 这里是本项目唯一开线程的地方，原因是 libmpv 的工作方式：它的 API 是事件驱动的，
 * 取事件的那个函数（mpv_wait_event）会一直阻塞到有事发生。要是放在界面线程里等，
 * 界面就彻底冻住了 —— 鼠标点不动、窗口不刷新。
 *
 * 所以有一个后台线程专门等事件，等到了就翻成 Qt 信号发出去（Qt 会自动把它转到界面
 * 线程）。命令方向相反：从界面线程直接调 libmpv 就行，它的接口是线程安全的。
 *
 * 这一层把线程完全包在里面 —— DLNA 和界面都感觉不到它存在。这正是前面三步解耦的用处。
 */
class LibMpvPlayer : public MediaPlayer
{
    Q_OBJECT

public:
    explicit LibMpvPlayer(QObject *parent = nullptr);
    ~LibMpvPlayer() override;

    /** 建好 mpv 实例（不加载任何东西）。窗口号可以提前给，也可以之后再给。 */
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

    /** 把画面画到这个窗口里。这是内嵌方案的关键一步，进程版做不到。 */
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
    quintptr m_windowId = 0;

    std::atomic<double> m_positionSec{0.0};
    std::atomic<double> m_durationSec{0.0};
    std::atomic<int>    m_volumePercent{100};
    std::atomic<bool>   m_muted{false};

    /** 当前锐度（我们这边的 -100~100）。只有界面线程读写。 */
    int m_sharpen = 0;
};
