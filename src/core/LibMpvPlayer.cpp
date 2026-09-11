#include "LibMpvPlayer.h"

#include <mpv/client.h>

LibMpvPlayer::LibMpvPlayer(QObject *parent)
    : MpvCore(parent)
{
}

LibMpvPlayer::~LibMpvPlayer() = default;

void LibMpvPlayer::applyStartupOptions(mpv_handle *mpv)
{
    if (m_windowId == 0 || !mpv)
        return;

    // wid 是"只能初始化前设"的选项之一，所以走这个钩子而不是 setVideoWindow。
    int64_t wid = static_cast<int64_t>(m_windowId);
    mpv_set_option(mpv, "wid", MPV_FORMAT_INT64, &wid);
}

void LibMpvPlayer::setVideoWindow(quintptr windowId)
{
    m_windowId = windowId;

    if (windowId == 0)
        return;

    // 实例还没起来（start() 之前），上面那行已经记下了，等 applyStartupOptions()
    // 去设。这儿只管"实例已经在了"的情况 —— 和原来的代码走的是同一件事。
    mpv_handle *mpv = handle();
    if (!mpv)
        return;

    // wid 是可以随时改的属性，实例建好之后也能改。
    int64_t wid = static_cast<int64_t>(windowId);
    mpv_set_property(mpv, "wid", MPV_FORMAT_INT64, &wid);
}
