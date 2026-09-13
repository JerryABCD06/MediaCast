#include "WindowsMediaControls.h"

#include <QMetaObject>

#include <chrono>
#include <cstdint>

// 顺序要紧：Qt 的头文件已经由上面那行带进来了，Windows/WinRT 的一律排在后面。
// 反过来的话 windows.h 里那堆宏会跑进 Qt 的头里捣乱。
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <roapi.h>
#include <systemmediatransportcontrolsinterop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

// 别写 using namespace Windows::Media —— SDK 的 winrt/roapi.h 和 cppwinrt 投影里
// 各有一个叫 Windows 的东西，不加限定的写法会撞名。
namespace WMedia = winrt::Windows::Media;

namespace {

/** WinRT 的时间单位是 100 纳秒一格。 */
using WinrtTimeSpan = std::chrono::duration<std::int64_t, std::ratio<1, 10000000>>;

WinrtTimeSpan toTimeSpan(double seconds)
{
    if (!(seconds > 0.0)) // 顺带把 NaN 也拦下
        seconds = 0.0;
    return std::chrono::duration_cast<WinrtTimeSpan>(std::chrono::duration<double>(seconds));
}

/** QString 转 WinRT 的字符串。hstring 自己会拷一份，不用管 utf16 那块内存的寿命。 */
winrt::hstring toHString(const QString &text)
{
    return winrt::hstring(reinterpret_cast<wchar_t const *>(text.utf16()),
                          static_cast<std::uint32_t>(text.size()));
}

} // namespace

// ── 实现体 ────────────────────────────────────────────────────────────────
//
// 所有 WinRT 类型都关在这里，头文件那边只看得到一个指针。
struct WindowsMediaControls::Impl
{
    WMedia::SystemMediaTransportControls smtc{nullptr};

    winrt::event_token buttonToken{};
    winrt::event_token seekToken{};
    bool buttonSubscribed = false;
    bool seekSubscribed = false;

    /** 会话有没有对系统开着。开着的时候标题才有地方显示。 */
    bool enabled = false;

    /** 最近一次拿到的"正在放什么"。开会话的时候要用它把标题补上。 */
    NowPlaying info;
    bool hasInfo = false;

    double position = 0.0;
    double duration = 0.0;
};

namespace {

/**
 * 把系统报来的按钮翻译成我们自己的信号。
 *
 * 单独拎出来是因为调用点要跨线程 —— 那个 lambda 里不适合塞一大段 switch。
 * 信号在 Qt 里是 public 的成员函数，所以外面也能发。
 */
void emitActionForButton(WindowsMediaControls *self, int button)
{
    switch (static_cast<WMedia::SystemMediaTransportControlsButton>(button))
    {
    case WMedia::SystemMediaTransportControlsButton::Play:
        emit self->playRequested();
        break;
    case WMedia::SystemMediaTransportControlsButton::Pause:
        emit self->pauseRequested();
        break;
    case WMedia::SystemMediaTransportControlsButton::Stop:
        emit self->stopRequested();
        break;
    case WMedia::SystemMediaTransportControlsButton::Next:
        emit self->nextRequested();
        break;
    case WMedia::SystemMediaTransportControlsButton::Previous:
        emit self->previousRequested();
        break;
    default:
        // 录音、快进、频道加减这些我们不做，面板上也没点亮，不会走到这儿。
        break;
    }
}

} // namespace

WindowsMediaControls::WindowsMediaControls(QObject *parent)
    : QObject(parent)
    , m_impl(new Impl)
{
}

WindowsMediaControls::~WindowsMediaControls()
{
    detach();
    delete m_impl;
    m_impl = nullptr;
}

bool WindowsMediaControls::attachToWindow(quintptr windowId)
{
    if (m_attached)
        return true;
    if (windowId == 0)
        return false;

    // 界面线程在 Qt 里一般已经做过 OleInitialize（那就是单线程套间），这里再要一次
    // 是幂等的。真失败了也不当致命错误 —— 后面的调用自己会报出真正的原因。
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    }
    catch (winrt::hresult_error const &)
    {
        // 已经用别的模式初始化过了。继续往下走，让真正的调用去报错。
    }

    try
    {
        // 这个 interop 接口不在普通的激活路径上：得先拿到它的工厂，再用窗口号换
        // 真正的 SMTC 对象。这是桌面程序用 SMTC 的唯一入口。
        winrt::hstring className = L"Windows.Media.SystemMediaTransportControls";
        winrt::com_ptr<ISystemMediaTransportControlsInterop> interop;
        winrt::check_hresult(::RoGetActivationFactory(
            reinterpret_cast<HSTRING>(winrt::get_abi(className)),
            winrt::guid_of<ISystemMediaTransportControlsInterop>(),
            interop.put_void()));

        WMedia::SystemMediaTransportControls smtc{nullptr};
        winrt::check_hresult(interop->GetForWindow(
            reinterpret_cast<HWND>(windowId),
            winrt::guid_of<WMedia::SystemMediaTransportControls>(),
            winrt::put_abi(smtc)));

        m_impl->smtc = smtc;

        // 面板上给哪几个按钮点亮。上一首/下一首先不点 —— 队列那套还没做，
        // 点着却按不动比灰着更糟。
        m_impl->smtc.IsPlayEnabled(true);
        m_impl->smtc.IsPauseEnabled(true);
        m_impl->smtc.IsStopEnabled(true);
        m_impl->smtc.IsNextEnabled(false);
        m_impl->smtc.IsPreviousEnabled(false);

        // 按钮按下。回调可能在别的线程上，所以这里只做一件事：把动作甩回界面线程。
        m_impl->buttonToken = m_impl->smtc.ButtonPressed(
            [this](WMedia::SystemMediaTransportControls const &,
                   WMedia::SystemMediaTransportControlsButtonPressedEventArgs const &args) {
                const int button = static_cast<int>(args.Button());
                QMetaObject::invokeMethod(
                    this, [this, button] { emitActionForButton(this, button); },
                    Qt::QueuedConnection);
            });
        m_impl->buttonSubscribed = true;

        // 面板里拖进度条。同样要甩回界面线程。
        m_impl->seekToken = m_impl->smtc.PlaybackPositionChangeRequested(
            [this](WMedia::SystemMediaTransportControls const &,
                   WMedia::PlaybackPositionChangeRequestedEventArgs const &args) {
                const auto hundredNs = std::chrono::duration_cast<
                    std::chrono::duration<double>>(args.RequestedPlaybackPosition());
                const double seconds = hundredNs.count();
                QMetaObject::invokeMethod(
                    this, [this, seconds] { emit seekRequested(seconds); },
                    Qt::QueuedConnection);
            });
        m_impl->seekSubscribed = true;

        // ★ 这一步很关键：SMTC 的 IsEnabled **默认就是 true**。
        //
        // 不显式关掉的话，程序一挂上去，面板里就凭空多出一条会话 —— 那一刻什么都
        // 没在放，系统只好拿"未知应用 + 可执行文件的路径"来凑数。之前那个怪东西
        // 就是这么来的。会话只在这两件事之一成立时才打开：有东西在放，或者刚投送过
        // 还停着（暂停也算）。
        m_impl->smtc.PlaybackStatus(WMedia::MediaPlaybackStatus::Closed);
        m_impl->smtc.IsEnabled(false);
    }
    catch (winrt::hresult_error const &e)
    {
        emit logMessage(QStringLiteral("Windows 媒体面板没接上：0x%1")
                            .arg(static_cast<unsigned>(e.code().value), 8, 16, QLatin1Char('0')));
        return false;
    }

    m_attached = true;
    emit logMessage(QStringLiteral("Windows 媒体面板已接入（有东西在放的时候才会出现在音量面板里）"));
    return true;
}

void WindowsMediaControls::detach()
{
    if (!m_impl || !m_attached)
        return;

    try
    {
        if (m_impl->enabled)
        {
            m_impl->smtc.PlaybackStatus(WMedia::MediaPlaybackStatus::Closed);
            m_impl->smtc.IsEnabled(false);
            m_impl->enabled = false;
        }
        if (m_impl->buttonSubscribed)
        {
            m_impl->smtc.ButtonPressed(m_impl->buttonToken);
            m_impl->buttonSubscribed = false;
        }
        if (m_impl->seekSubscribed)
        {
            m_impl->smtc.PlaybackPositionChangeRequested(m_impl->seekToken);
            m_impl->seekSubscribed = false;
        }
    }
    catch (winrt::hresult_error const &)
    {
        // 收摊时的失败没什么可做的：进程本来就要走了。
    }

    m_impl->smtc = nullptr;
    m_attached = false;
}

void WindowsMediaControls::setNowPlaying(const NowPlaying &info)
{
    if (!m_impl)
        return;

    m_impl->info = info;
    m_impl->hasInfo = true;

    pushDisplay();
}

void WindowsMediaControls::pushDisplay()
{
    if (!m_attached || !m_impl->smtc || !m_impl->enabled)
    {
        return;
    }

    const NowPlaying &info = m_impl->info;

    // ── 标题和副标题 ─────────────────────────────────────────────────────
    //
    // 标题：控制器那边已经兜过底了（真标题 → 从文件名推 → 类型名），还会空着
    // 只可能是"连类型都认不出来"。那时候给个"未知"，别让面板上留一块空白。
    const QString title = info.title.isEmpty() ? tr("media_unknown") : info.title;

    // 副标题：有歌手显示歌手，没有显示专辑，都没有就是"未知"。
    //
    // **不带协议名。** 面板是给用户看的东西，没必要让他看见 "DLNA" 这种实现
    // 细节（以前这儿直接写 "DLNA 投送"）。来源只用说清"投送"还是"本地播放"，
    // 拼成「来源 - 副标题」。
    //
    // 视频和图片没有"歌手"这个概念，系统在那个位置统一叫"副标题"，所以三种
    // 类型写的是同一个值。
    QString sub = info.artist;
    if (sub.isEmpty())
        sub = info.album;
    if (sub.isEmpty())
        sub = tr("media_unknown");

    const QByteArray sourceKey = info.source == MediaSource::Local
                                     ? QByteArrayLiteral("media_source_local")
                                     : QByteArrayLiteral("media_source_cast");
    const QString subtitle = tr(sourceKey.constData()) + QStringLiteral(" - ") + sub;

    // 这一句是**日志**用的，不翻译（见 lang/README.md 那条规矩）。
    const QString kindName = mediaKindLabel(info.kind);

    try
    {
        auto updater = m_impl->smtc.DisplayUpdater();

        // 先清干净再写。类型是会变的（这次放音乐、下次投图片），不清的话上一段内容
        // 的属性会留在卡片上，出现"图片卡片里挂着上一次的专辑名"这种怪事。
        updater.ClearAll();

        switch (info.kind)
        {
        case MediaKind::Audio:
            // 音乐卡片：标题 + 歌手 + 专辑。
            updater.Type(WMedia::MediaPlaybackType::Music);
            {
                auto music = updater.MusicProperties();
                music.Title(toHString(title));
                music.Artist(toHString(subtitle));
                music.AlbumTitle(toHString(info.album));
            }
            break;

        case MediaKind::Image:
            // 图片卡片：标题 + 副标题。以前一律按音乐报，图片在面板里就显示得不对。
            updater.Type(WMedia::MediaPlaybackType::Image);
            {
                auto image = updater.ImageProperties();
                image.Title(toHString(title));
                image.Subtitle(toHString(subtitle));
            }
            break;

        default:
            // 视频，以及认不出类型的情况。视频卡片也是标题 + 副标题。
            updater.Type(WMedia::MediaPlaybackType::Video);
            {
                auto video = updater.VideoProperties();
                video.Title(toHString(title));
                video.Subtitle(toHString(subtitle));
            }
            break;
        }

        updater.Update();

        emit logMessage(QStringLiteral("Windows 媒体面板 -> %1 ｜ %2 ｜ %3")
                            .arg(title, subtitle,
                                 kindName.isEmpty() ? QStringLiteral("未知类型") : kindName));
    }
    catch (winrt::hresult_error const &e)
    {
        emit logMessage(QStringLiteral("Windows 媒体面板写标题失败：0x%1")
                            .arg(static_cast<unsigned>(e.code().value), 8, 16, QLatin1Char('0')));
    }
}

void WindowsMediaControls::setTransportState(const QString &state)
{
    if (!m_impl)
        return;

    if (!m_attached || !m_impl->smtc)
        return;

    // 会话开不开，要看两件事：传输状态 + 手上到底有没有内容。
    //
    // 只看状态不够。实测踩到过一条：在电脑端点「断开投屏」（状态归成
    // NO_MEDIA_PRESENT、会话收起）之后，手机会**补发**一条 Stop，状态就变成了
    // STOPPED —— 于是会话又被打开，可这时候手上什么都没有，面板里就留一条空的
    // "未知应用"挂在那儿，一直到关程序为止。日志里那条
    // 「Windows 媒体面板 -> （无标题）｜ ｜ 未知类型」就是它。
    //
    // 多加的这一条同时守住了最开始那个需求：没东西放的时候，面板里干干净净。
    const bool hasMedia = (state != QLatin1String("NO_MEDIA_PRESENT"))
                          && !m_impl->info.isEmpty();

    WMedia::MediaPlaybackStatus status = WMedia::MediaPlaybackStatus::Closed;
    if (state == QLatin1String("PLAYING"))
        status = WMedia::MediaPlaybackStatus::Playing;
    else if (state == QLatin1String("PAUSED_PLAYBACK"))
        status = WMedia::MediaPlaybackStatus::Paused;
    else if (state == QLatin1String("TRANSITIONING"))
        status = WMedia::MediaPlaybackStatus::Changing;
    else if (hasMedia)
        status = WMedia::MediaPlaybackStatus::Stopped;

    try
    {
        if (!hasMedia)
        {
            if (m_impl->enabled)
            {
                m_impl->smtc.PlaybackStatus(WMedia::MediaPlaybackStatus::Closed);
                m_impl->smtc.IsEnabled(false);
                m_impl->enabled = false;
                emit logMessage(QStringLiteral("Windows 媒体面板：投送结束，会话收起"));
            }
            return;
        }

        if (!m_impl->enabled)
        {
            m_impl->smtc.IsEnabled(true);
            m_impl->enabled = true;

            // 会话是刚开的，标题得补一次 —— 通知的先后顺序是"先报内容、后报状态"，
            // 报内容那会儿会话还没开，写不进去。
            if (m_impl->hasInfo)
                pushDisplay();
        }

        m_impl->smtc.PlaybackStatus(status);
    }
    catch (winrt::hresult_error const &e)
    {
        emit logMessage(QStringLiteral("Windows 媒体面板同步状态失败：0x%1")
                            .arg(static_cast<unsigned>(e.code().value), 8, 16, QLatin1Char('0')));
    }
}

void WindowsMediaControls::setPosition(double seconds)
{
    if (!m_impl)
        return;
    m_impl->position = seconds;
    pushTimeline();
}

void WindowsMediaControls::setQueueAvailability(bool hasNext, bool hasPrevious)
{
    if (!m_impl || !m_attached || !m_impl->smtc)
        return;

    try
    {
        m_impl->smtc.IsNextEnabled(hasNext);
        m_impl->smtc.IsPreviousEnabled(hasPrevious);

        emit logMessage(QStringLiteral("Windows 媒体面板：上一条/下一条按钮 -> %1 / %2")
                            .arg(hasPrevious ? QStringLiteral("亮") : QStringLiteral("灰"),
                                 hasNext ? QStringLiteral("亮") : QStringLiteral("灰")));
    }
    catch (winrt::hresult_error const &e)
    {
        emit logMessage(QStringLiteral("Windows 媒体面板改队列按钮失败：0x%1")
                            .arg(static_cast<unsigned>(e.code().value), 8, 16, QLatin1Char('0')));
    }
}

void WindowsMediaControls::setDuration(double seconds)
{
    if (!m_impl)
        return;
    m_impl->duration = seconds;
    pushTimeline();
}

void WindowsMediaControls::pushTimeline()
{
    if (!m_attached || !m_impl->smtc || !m_impl->enabled)
        return;

    // 时长还不知道的时候（片子刚开个头）就别报了：报一个 0 到 0 的时间轴，
    // 面板上的进度条会一直卡在最左边，看着像坏了。
    if (!(m_impl->duration > 0.0))
        return;

    try
    {
        WMedia::SystemMediaTransportControlsTimelineProperties timeline;
        timeline.StartTime(toTimeSpan(0.0));
        timeline.EndTime(toTimeSpan(m_impl->duration));
        timeline.MinSeekTime(toTimeSpan(0.0));
        timeline.MaxSeekTime(toTimeSpan(m_impl->duration));
        timeline.Position(toTimeSpan(m_impl->position));
        m_impl->smtc.UpdateTimelineProperties(timeline);
    }
    catch (winrt::hresult_error const &)
    {
        // 进度报不上去不影响播放，不值得刷日志（这个函数每秒都会被叫一次）。
    }
}
