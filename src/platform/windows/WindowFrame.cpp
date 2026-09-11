#include "WindowFrame.h"

#include <QWindow>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace WindowFrame
{

void ensureSnapFlags(QWindow *window)
{
#ifdef Q_OS_WIN
    if (!window)
        return;

    // winId() 会顺手把原生窗口建出来（如果还没建），所以这个调用不挑时机。
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (!hwnd)
        return;

    const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
    const LONG_PTR wanted = style | WS_SYSMENU | WS_MINIMIZEBOX;
    if (wanted == style)
        return;

    ::SetWindowLongPtrW(hwnd, GWL_STYLE, wanted);

    // 光改样式位不够。系统是在处理 WM_NCCALCSIZE 的时候重算非客户区的，
    // 不主动喊它一声，新样式要等到用户下次拖窗口大小才生效 —— 中间那段时间
    // 窗口处于一种"样式位说一套、实际样子是另一套"的错位状态。
    // SWP_FRAMECHANGED 就是"边框变了，重算一次"。
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE
                       | SWP_NOZORDER | SWP_NOACTIVATE);
#else
    Q_UNUSED(window);
#endif
}

}
