#include "MpvCore.h"

#include <mpv/client.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QtGlobal>

#include <cmath>

namespace {

/**
 * 画面调节项 —— 这张表是照 mpv 自己的选项表抄的，不是凭印象写的。
 *
 * 怎么来的：`mpv --list-options`，把每一项的取值范围和默认值都打出来对着填。
 * 表里没有"色温""颜色增益""梯形校正"这几项，因为 mpv 根本没有对应属性 ——
 * 它们是 DLNA RenderingControl 里的标准名字，但要在这套后端上做出来得自己写
 * shader，那是另一个量级的事。宁可少几项，也不要摆一排按了没反应的滑块。
 *
 * scale 是"我们的整数 × scale = 塞给 mpv 的浮点数"。mpv 那几项 -100~100 的
 * 正好乘以 1；像 sharpen、video-align 这类 0~1 / -1~1 的，就用 0.01 把它换算成
 * 整数来给界面用 —— 界面不需要知道底下是浮点。
 */
struct PictureControlSpec
{
    const char *name;         // 短名，界面和 DLNA 那边都用它
    const char *label;        // 给人看的名字
    const char *mpvProperty;
    int         min;
    int         max;
    int         neutral;      // 复位到这儿
    double      scale;
};

const PictureControlSpec kPictureControls[] = {
    { "brightness", "亮度",     "brightness",     -100, 100, 0, 1.0  },
    { "contrast",   "对比度",   "contrast",       -100, 100, 0, 1.0  },
    { "saturation", "饱和度",   "saturation",     -100, 100, 0, 1.0  },
    { "gamma",      "伽马",     "gamma",          -100, 100, 0, 1.0  },
    { "hue",        "色相",     "hue",            -100, 100, 0, 1.0  },
    // 锐度这里是空的：它不走属性，走视频滤镜链。原因见 applySharpen 上面那段。
    { "sharpen",    "锐度",     nullptr,          -100, 100, 0, 1.0  },
    { "zoom",       "画面缩放", "video-zoom",      -20,  20, 0, 1.0  },
    // 位置用 pan 而不是 align。
    //
    // align 是"在**留白**里挪画面"：没有留白就没得挪。我们这窗口是宽屏，视频被加的
    // 是左右留白，于是"垂直位置"拉了半天纹丝不动 —— 实测就是这样。
    // pan 是直接平移画面，什么时候都有反应（代价是会裁掉移出画面的部分，但"把画面
    // 往上挪一点"本来就是干这个的）。
    { "pan-x",      "水平位置", "video-pan-x",   -100, 100, 0, 0.01 },
    { "pan-y",      "垂直位置", "video-pan-y",   -100, 100, 0, 0.01 },
    { "rotate",     "旋转",     "video-rotate",      0, 359, 0, 1.0  },
};

const PictureControlSpec *findPictureSpec(const QString &name)
{
    for (const PictureControlSpec &spec : kPictureControls) {
        if (name == QLatin1String(spec.name))
            return &spec;
    }
    return nullptr;
}

/** 每个被订阅的属性配一个编号，事件回来时用它区分是哪一条变了。 */
enum PropertyId {
    IdTimePos  = 1,
    IdDuration = 2,
    IdPause    = 3,
    IdVolume   = 4,
    IdMute     = 5,
};

/** 把 mpv 的返回码翻成人能看的话。 */
QString mpvError(int code)
{
    return QString::fromUtf8(mpv_error_string(code));
}

} // namespace

MpvCore::MpvCore(QObject *parent)
    : MediaPlayer(parent)
{
}

MpvCore::~MpvCore()
{
    shutdown();
}

bool MpvCore::start()
{
    if (m_mpv)
        return true;

    m_stopRequested = false;

    m_mpv = mpv_create();
    if (!m_mpv) {
        emit statusChanged(QStringLiteral("libmpv 初始化失败：mpv_create 返回空"));
        return false;
    }

    // 没文件也保持运行，等着我们发 loadfile。
    mpv_set_option_string(m_mpv, "idle", "yes");

    // 和进程版一样的理由：关掉 mpv 自带的一切控制。用户能直接操作的地方，就是能绕过
    // 状态机的地方 —— 那正是前面三处 bug 的根因。
    mpv_set_option_string(m_mpv, "osc", "no");
    mpv_set_option_string(m_mpv, "input-default-bindings", "no");
    mpv_set_option_string(m_mpv, "cursor-autohide", "always");
    mpv_set_option_string(m_mpv, "terminal", "no");

    // 把"只能初始化前设"的那几个选项交给子类 —— 基类不知道画面要往哪出。
    applyStartupOptions(m_mpv);

    const int rc = mpv_initialize(m_mpv);
    if (rc < 0) {
        emit statusChanged(QStringLiteral("libmpv 初始化失败：") + mpvError(rc));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    // 只要警告以上的日志，全开的话噪音太大。
    mpv_request_log_messages(m_mpv, "warn");

    // 订阅这几条：变了 mpv 会主动推事件过来，不用我们一直去问。
    mpv_observe_property(m_mpv, IdTimePos,  "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdPause,    "pause",    MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, IdVolume,   "volume",   MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, IdMute,     "mute",     MPV_FORMAT_FLAG);

    // 窗口标题。以前这行写在 main() 里，那属于界面越界去碰 mpv 的细节。
    mpv_set_property_string(m_mpv, "title",
                            QCoreApplication::applicationName().toUtf8().constData());

    m_thread = std::thread(&MpvCore::eventLoop, this);

    emit statusChanged(QStringLiteral("libmpv 已就绪"));
    return true;
}

void MpvCore::shutdown()
{
    m_stopRequested = true;

    // mpv_wait_event 正阻塞着，叫醒它，让后台线程有机会看到停止标志。
    if (m_mpv)
        mpv_wakeup(m_mpv);

    if (m_thread.joinable())
        m_thread.join();

    if (m_mpv) {
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
}

void MpvCore::eventLoop()
{
    while (true) {
        // -1 表示"一直等到有事发生"。不轮询，所以事件是零延迟的；
        // 代价是退出时要靠 mpv_wakeup 把它叫醒。
        mpv_event *event = mpv_wait_event(m_mpv, -1.0);

        if (!event || event->event_id == MPV_EVENT_NONE) {
            if (m_stopRequested.load())
                break;
            continue;
        }

        switch (event->event_id) {
        case MPV_EVENT_PROPERTY_CHANGE: {
            auto *prop = static_cast<mpv_event_property *>(event->data);
            if (!prop)
                break;

            // data 为空表示这个属性暂时没有值（比如停止播放后 time-pos 就没了），
            // 当成 0 处理，界面才会正确地归零。
            switch (event->reply_userdata) {
            case IdTimePos: {
                const double v = prop->data ? *static_cast<double *>(prop->data) : 0.0;
                m_positionSec.store(v);
                emit positionChanged(v);
                break;
            }
            case IdDuration: {
                const double v = prop->data ? *static_cast<double *>(prop->data) : 0.0;
                m_durationSec.store(v);
                emit durationChanged(v);
                break;
            }
            case IdPause: {
                const bool paused = prop->data && (*static_cast<int *>(prop->data) != 0);
                emit pausedChanged(paused);
                break;
            }
            case IdVolume: {
                const int v = prop->data ? qRound(*static_cast<double *>(prop->data)) : 0;
                m_volumePercent.store(v);
                emit volumeChanged(v);
                break;
            }
            case IdMute: {
                const bool muted = prop->data && (*static_cast<int *>(prop->data) != 0);
                m_muted.store(muted);
                emit muteChanged(muted);
                break;
            }
            default:
                break;
            }
            break;
        }

        case MPV_EVENT_FILE_LOADED:
            // "文件加载好了" —— DLNA 的 TRANSITIONING 就是在等这一声。
            emit ready();
            break;

        case MPV_EVENT_START_FILE:
            emit logMessage(QStringLiteral("开始加载媒体 ..."));
            break;

        case MPV_EVENT_END_FILE: {
            auto *endFile = static_cast<mpv_event_end_file *>(event->data);
            if (endFile && endFile->error < 0)
                emit logMessage(QStringLiteral("播放失败：") + mpvError(endFile->error));
            emit ended();
            break;
        }

        case MPV_EVENT_LOG_MESSAGE: {
            auto *msg = static_cast<mpv_event_log_message *>(event->data);
            if (msg)
                emit logMessage(QStringLiteral("mpv: ")
                                + QString::fromUtf8(msg->text).trimmed());
            break;
        }

        case MPV_EVENT_SHUTDOWN:
            emit lost();
            break;

        default:
            break;
        }

        if (m_stopRequested.load())
            break;
    }
}

void MpvCore::load(const QString &uri)
{
    if (!m_mpv || uri.isEmpty())
        return;

    // 换片子的时候先把缓存的进度清掉。
    //
    // 不清的话，从"发出 loadfile"到"mpv 真的把新文件打开"之间有个空档，那会儿
    // time-pos 还是**上一条**的位置。控制点正好在这时候来问 GetPositionInfo，
    // 就会拿到上一条的进度 —— 它那边的进度条会跳一下。设备上每秒都被问一次，
    // 这个空档撞上的概率不小。
    m_positionSec = 0.0;
    m_durationSec = 0.0;

    // 用数组形式传参，不拼字符串 —— URL 里带引号或反斜杠时，拼字符串迟早出错。
    const QByteArray path = uri.toUtf8();
    const char *command[] = {"loadfile", path.constData(), "replace", nullptr};
    mpv_command(m_mpv, command);
}

void MpvCore::play()
{
    if (!m_mpv)
        return;
    int flag = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvCore::pause()
{
    if (!m_mpv)
        return;
    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void MpvCore::stop()
{
    if (!m_mpv)
        return;
    const char *command[] = {"stop", nullptr};
    mpv_command(m_mpv, command);
}

void MpvCore::seekTo(double seconds)
{
    if (!m_mpv)
        return;
    const QByteArray target = QByteArray::number(seconds, 'f', 3);
    const char *command[] = {"seek", target.constData(), "absolute", nullptr};
    mpv_command(m_mpv, command);
}

void MpvCore::setVolumePercent(int percent)
{
    if (!m_mpv)
        return;
    double volume = qBound(0, percent, 100);
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &volume);
}

void MpvCore::setMuted(bool muted)
{
    if (!m_mpv)
        return;
    int flag = muted ? 1 : 0;
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &flag);
}

qint64 MpvCore::mediaSizeBytes() const
{
    if (!m_mpv)
        return 0;

    // 控制点的流要是没报 Content-Length（vivo 那种临时媒体服务器就常常不报），
    // mpv 这边就是 0 —— 那就如实说"不知道"，让按字节跳转回一个明确的错误，
    // 而不是拿一个瞎猜的比例去跳。
    int64_t size = 0;
    if (mpv_get_property(m_mpv, "file-size", MPV_FORMAT_INT64, &size) < 0)
        return 0;
    return size > 0 ? size : 0;
}

void MpvCore::setVideoWindow(quintptr windowId)
{
    // 核心这一层不知道怎么把画面送进一个窗口 —— 那是外壳的事（LibMpvPlayer
    // 覆盖了它，去设 mpv 的 wid 选项）。
    //
    // 留一个空实现而不是做成纯虚函数，是为了让这个类**能被直接实例化**：
    // QML 那边的视频区不继承它（它要继承 QQuickFramebufferObject，两个 QObject
    // 没法多重继承），只是持有一个，走 render API 自己渲染。
    //
    // 走到这儿说明有人给了一个窗口号，但当前这个后端不接管它 —— 这多半是接错
    // 了线，说一声，别让它悄悄失效。
    if (windowId != 0)
        emit logMessage(QStringLiteral("画面输出：当前后端不通过窗口输出，已忽略窗口号"));
}

void MpvCore::applyStartupOptions(mpv_handle *mpv)
{
    // 基类没有"画面往哪出"这回事 —— 子类才做得了这个决定。
    Q_UNUSED(mpv);
}

QVector<PictureControlInfo> MpvCore::pictureControls() const
{
    QVector<PictureControlInfo> out;
    out.reserve(static_cast<int>(std::size(kPictureControls)));

    for (const PictureControlSpec &spec : kPictureControls) {
        PictureControlInfo info;
        info.name = QString::fromLatin1(spec.name);
        info.label = QString::fromUtf8(spec.label);
        info.min = spec.min;
        info.max = spec.max;
        info.neutral = spec.neutral;
        out.append(info);
    }
    return out;
}

bool MpvCore::setPictureControl(const QString &name, int value)
{
    if (!m_mpv)
        return false;

    const PictureControlSpec *spec = findPictureSpec(name);
    if (!spec)
        return false;

    // 锐度是唯一一个走滤镜的，单独办。
    if (!spec->mpvProperty)
        return applySharpen(qBound(spec->min, value, spec->max));

    // 不加 const：mpv_set_property 收的是 void*，const 指针塞不进去。
    double raw = qBound(spec->min, value, spec->max) * spec->scale;

    // 先按浮点塞。mpv 里有些属性是整数型（video-rotate 就是），浮点塞不进去，
    // 那就换成整数再试一次 —— 表里不用为这种小事多加一个字段。
    int rc = mpv_set_property(m_mpv, spec->mpvProperty, MPV_FORMAT_DOUBLE, &raw);
    if (rc < 0) {
        int64_t asInteger = static_cast<int64_t>(std::llround(raw));
        rc = mpv_set_property(m_mpv, spec->mpvProperty, MPV_FORMAT_INT64, &asInteger);
    }

    if (rc < 0)
        return false;

    // 成了才报。没成就别报 —— 报了控制点那边会显示一个其实没生效的值。
    emit pictureControlChanged(name, qBound(spec->min, value, spec->max));
    return true;
}

int MpvCore::pictureControlValue(const QString &name) const
{
    if (!m_mpv)
        return 0;

    const PictureControlSpec *spec = findPictureSpec(name);
    if (!spec)
        return 0;

    // 锐度走的是滤镜链，它的参数 mpv 不当属性暴露 —— 自己记着（只有界面线程动它）。
    if (!spec->mpvProperty)
        return m_sharpen;

    // 这里是直接问 mpv，没有缓存 —— 按接口的约定本该避免"读个值还要往返一趟"，
    // 但这是**同进程内的**属性读取，没有 I/O，问一次比维护一份缓存更省事也更准。
    double raw = 0.0;
    if (mpv_get_property(m_mpv, spec->mpvProperty, MPV_FORMAT_DOUBLE, &raw) < 0) {
        int64_t asInteger = 0;
        if (mpv_get_property(m_mpv, spec->mpvProperty, MPV_FORMAT_INT64, &asInteger) < 0)
            return spec->neutral;
        raw = static_cast<double>(asInteger);
    }

    return static_cast<int>(std::lround(raw / spec->scale));
}

bool MpvCore::applySharpen(int value)
{
    // 为什么不设 mpv 那个 --sharpen 属性：
    //
    // **实测它是空的**。两张截图逐像素比，--sharpen 从 0 拉到满，画面差 0 个像素。
    // 它是给老 vo=gpu 写的一个着色器钩子，我们用的是 mpv 0.41 的默认 vo（gpu-next），
    // 那条路根本没接。日志里连个警告都没有，就是这么不声不响地不干活。
    //
    // 走视频滤镜链里的 unsharp 就不一样了：它是 ffmpeg 的滤镜，跟在解码后面，
    // 跟用哪个 vo 没关系。同样两张截图比，这一项差 9.6% 的像素。
    //
    // 注意：设 vf 是**整条滤镜链一起换**，不是往里加一道。现在别处没人用 vf，
    // 所以没有冲突；将来要是加了别的滤镜，得改成"拼一串"而不是直接赋值。
    if (value == 0) {
        // 不锐化就把滤镜摘掉，别让它白白占一道工序。
        if (mpv_set_property_string(m_mpv, "vf", "") < 0)
            return false;
    } else {
        // unsharp 的 luma_amount 可用范围是 -2~5：正数锐化、负数模糊。
        // 我们这边是 -100~100，正半边映射到 0.5~3.0，负半边映射到 -2.0~0。
        const double amount = value > 0 ? 0.5 + value * 0.025 : value * 0.02;
        const QString vf = QStringLiteral("unsharp=luma_msize_x=7:luma_msize_y=7:luma_amount=%1")
                               .arg(amount, 0, 'f', 2);
        if (mpv_set_property_string(m_mpv, "vf", vf.toUtf8().constData()) < 0)
            return false;
    }

    m_sharpen = value;
    emit pictureControlChanged(QStringLiteral("sharpen"), value);
    return true;
}
