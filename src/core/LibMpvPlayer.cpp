#include "LibMpvPlayer.h"

#include <mpv/client.h>

LibMpvPlayer::LibMpvPlayer(QObject *parent)
    : MpvCore(parent)
{
    // 这个外壳走的是"画进原生窗口"那条路。写出来而不是靠默认值，是因为
    // 这个决定会影响 mpv 的启动选项，值得在代码里看得见。
    setOutputMode(WindowOutput);
}

LibMpvPlayer::~LibMpvPlayer() = default;

void LibMpvPlayer::applyStartupOptions(mpv_handle *mpv)
{
    // 先让基类把它那套（如果当前模式需要）设上，再加我们自己的。
    // 现在 WindowOutput 模式下基类什么都不做，但这条链路要显式，免得将来
    // 基类加了什么东西这边悄悄漏掉。
    MpvCore::applyStartupOptions(mpv);

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
