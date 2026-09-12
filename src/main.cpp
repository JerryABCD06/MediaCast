#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTime>
#include <QtQml/qqml.h>

#include <memory>

#include "core/LibMpvPlayer.h"
#include "core/MpvCore.h"
#include "core/MpvQmlItem.h"
#include "core/PlaybackController.h"
#include "platform/windows/WindowsMediaControls.h"
#include "protocols/dlna/DlnaRenderer.h"
#include "ui/MainWindow.h"
#include "ui/NewUiWindow.h"
#include "ui/TrayIcon.h"
#include "ui/UiState.h"

/**
 * 组装整个程序。这里只做五件事：造零件、接日志、接 Windows 媒体面板、开窗口、起服务。
 *
 * 依赖是**单向**的，方向不能反：
 *
 *     界面 ──▶ DLNA 模块 ──▶ 播放器接口 ◀──实现── LibMpvPlayer
 *
 * 界面不认识播放器，DLNA 不认识 mpv。换播放后端只要改下面 player 那一行 ——
 * 别的文件一行都不用动。这件事刚刚被验证过：从"外部 mpv.exe"换成"内嵌 libmpv"，
 * DLNA 那三个文件一个字都没改。
 */
int main(int argc, char *argv[])
{
    // ── 图形后端 ─────────────────────────────────────────────────────────
    //
    // 新界面要用 QML 渲染，而里面的视频区将来是 libmpv 的 render API —— 那套只有
    // OpenGL 一种 GPU 后端，而 Qt 在 Windows 上默认走 D3D11。两者对不上。
    //
    // 所以整个进程的图形后端钉死成 OpenGL。这句话必须在**建任何窗口之前**执行，
    // 窗口建好了再设是不生效的。它只影响 Qt Quick，不影响旧的 Widgets 界面。
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);

    QApplication app(argc, argv);

    // ── 界面状态：语言 + 深浅 ────────────────────────────────────────────
    //
    // 这两样**只在这里存一份**。QML 那套界面和原生 Qt 控件（文件对话框、
    // 消息框、右键菜单）都向它看齐 —— 两个来源各说各话的后果是"QML 切成
    // 英文了，弹出的文件对话框还是中文"。
    //
    // 为什么需要这个类、它的三个设计约束是什么，写在 UiState.h 的头部。
    //
    // 注册成 QML 单例，QML 里 import MediaCast 就能拿到。它必须是 main() 的
    // 局部变量：QML 引擎只借不拥有这个对象，它先没了引擎那边就是野指针。
    // 声明在 newUi 之前，析构顺序自然就对（后声明的先析构）。
    UiState uiState;
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "UiState", &uiState);

    // 名字分两个，别混：
    //   applicationName        —— 给机器看的。决定配置目录（%LOCALAPPDATA%\MCast）之类。
    //   applicationDisplayName —— 给人看的。窗口标题、对话框标题用它。
    QApplication::setApplicationName(QStringLiteral("MCast"));
    QApplication::setApplicationDisplayName(QStringLiteral("Media Cast"));

    // 这是个常驻后台的媒体接收器：关掉窗口不等于退出程序。
    // 少了这一句，关窗口时 Qt 会直接退出，托盘图标跟着一起没。
    QApplication::setQuitOnLastWindowClosed(false);

    // ── 图标 ─────────────────────────────────────────────────────────────
    //
    // 这一步不能省，而且原因不直观：
    //
    // 任务栏、Alt+Tab、窗口左上角用的是**窗口图标**。Qt 在窗口图标为空的时候，
    // 会把它重置成系统那个通用的"白纸"图标 —— 它**不会**自动去拿 exe 资源里
    // 那个图标顶上。所以只给 exe 加图标资源的话，资源管理器里是对了，任务栏
    // 还是白纸。（这一条是实测出来的：把窗口图标抠出来看过。）
    //
    // 多个尺寸是有用的：Qt 会按场合自己挑，16/32 给任务栏，256 给高 DPI。
    // 只给一张大图让它缩放的话，小尺寸会糊。
    QIcon appIcon;
    appIcon.addFile(QStringLiteral(":/icons/mcast-16.png"),  QSize(16, 16));
    appIcon.addFile(QStringLiteral(":/icons/mcast-32.png"),  QSize(32, 32));
    appIcon.addFile(QStringLiteral(":/icons/mcast-48.png"),  QSize(48, 48));
    appIcon.addFile(QStringLiteral(":/icons/mcast-256.png"), QSize(256, 256));
    QApplication::setWindowIcon(appIcon);

    // 日志同时写一份到文件。界面上一行标签只显示最后一条，排查问题时根本不够用；
    // 有文件才能看到完整的先后顺序。
    // （这是临时脚手架 —— 做正式界面时，日志位置会挪到用户目录下。）
    QFile logFile(QCoreApplication::applicationDirPath() + QStringLiteral("/MCast.log"));

    // 追加而不是覆盖：上一次运行的日志往往正是要查的那一份。大到一定体积才清一次。
    if (logFile.exists() && logFile.size() > 2 * 1024 * 1024)
        logFile.remove();

    const bool logOk = logFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    if (!logOk)
        qWarning("日志文件打不开，日志只显示在界面上");
    else
        logFile.write(QStringLiteral("\n===== 程序启动 %1 =====\n")
                          .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                          .toUtf8());

    // 写日志的格式只留这一处，别处都调它 —— 两处各写各的，迟早长短不一。
    auto writeLog = [&logFile](const QString &text) {
        logFile.write(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")).toUtf8());
        logFile.write("  ");
        logFile.write(text.toUtf8());
        logFile.write("\n");
        logFile.flush();
    };

    // ── 零件 ─────────────────────────────────────────────────────────────
    // 播放后端二选一，改下面这一行就能换：
    //   LibMpvPlayer   —— 内嵌 libmpv，画面画进我们自己的窗口（当前用这个）
    //   MpvMediaPlayer —— 外部 mpv.exe + 命名管道（保留着，排查问题时能换回来，
    //                     但它需要把 mpv.exe 的路径传给 start()）
    // ── 播放后端 ─────────────────────────────────────────────────────────
    //
    // 【过渡开关】两套界面现在还并存，而**一个 mpv 实例只能有一条画面输出
    // 路径**（mpv 的 vo 只能设一次，wid 和 render API 互斥）。所以画面只可能
    // 出现在其中一边：
    //
    //   false → LibMpvPlayer 走 wid，画面画进旧界面的画面区。
    //           手机投屏时电脑上能看到画面 —— 现在就是这条。
    //   true  → MpvCore 走 render API，画面由新的 QML 界面渲染。
    //           代价是旧界面的画面区会变成空的。
    //
    // DLNA 那一层**两条路都不受影响** —— 它只认 MediaPlayer 接口，不关心
    // 画面往哪出。等旧界面退场之后，这个开关就能删掉，只剩上面那条。
    constexpr bool kRenderVideoInNewUi = false;

    std::unique_ptr<MpvCore> ownedPlayer;
    if (kRenderVideoInNewUi) {
        auto core = std::make_unique<MpvCore>();
        core->setOutputMode(MpvCore::RenderApiOutput);
        ownedPlayer = std::move(core);
    } else {
        // 构造里已经把自己设成 WindowOutput。
        ownedPlayer = std::make_unique<LibMpvPlayer>();
    }
    MpvCore *player = ownedPlayer.get();

    // 播放器注册给 QML，和 UiState 同样的道理 —— QML 引擎只借不拥有这个对象，
    // 所以 ownedPlayer 必须声明在 newUi 之前（后声明的先析构）。必须注册在
    // **创建之前**，所以放在这儿而不是上面 UiState 那一段。
    //
    // 从 QML 那边看到的是 MpvCore 这一层，看不到底下是哪个外壳 —— 界面不该
    // 关心画面往哪出。MpvCore 注册成"不可创建"：它只能由 C++ 造。
    qmlRegisterUncreatableType<MpvCore>("MediaCast", 1, 0, "MpvCore",
                                        QStringLiteral("MpvCore 只能由 C++ 创建"));
    qmlRegisterType<MpvQmlItem>("MediaCast", 1, 0, "MpvQmlItem");
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Player", player);

    // ── 播放控制 ─────────────────────────────────────────────────────────
    //
    // 协议无关的那一层：传输状态机、队列三格、当前媒体。DLNA 只跟它说话，
    // 不直接碰播放器 —— 将来加别的协议时，那个协议也接在这儿。
    PlaybackController playback(player);

    DlnaRenderer renderer(&playback);
    MainWindow   window(&renderer);

    // Windows 自己的媒体面板（按音量键弹出来的那个）。
    // 它在结构上很特别：**既是显示端，又是控制端**。所以显示的部分照界面那样接，
    // 控制的部分照"手机发来一条命令"那样走 —— 两条都不绕过门面。
    WindowsMediaControls mediaControls;

    // ── 日志落盘 ─────────────────────────────────────────────────────────
    // 门面已经把四个子模块和播放器的日志汇总成一路，这里接一次就够了。
    if (logOk)
        QObject::connect(&renderer, &DlnaRenderer::logMessage, writeLog);

    // ── Windows 媒体面板：显示往哪去，命令从哪回 ─────────────────────────
    //
    // 面板上按的播放/暂停，和手机上按的、界面上按的，走的是同一个入口。这是老规矩：
    // 绕开状态机的那几次 bug（停止、播放暂停、载入地址）就是那么来的。
    QObject::connect(&mediaControls, &WindowsMediaControls::logMessage,
                     &renderer, &DlnaRenderer::logMessage);

    QObject::connect(&renderer, &DlnaRenderer::nowPlayingChanged,
                     &mediaControls, &WindowsMediaControls::setNowPlaying);
    QObject::connect(&renderer, &DlnaRenderer::transportStateChanged,
                     &mediaControls, &WindowsMediaControls::setTransportState);
    QObject::connect(&renderer, &DlnaRenderer::positionChanged,
                     &mediaControls, &WindowsMediaControls::setPosition);
    QObject::connect(&renderer, &DlnaRenderer::durationChanged,
                     &mediaControls, &WindowsMediaControls::setDuration);

    QObject::connect(&mediaControls, &WindowsMediaControls::playRequested,
                     &renderer, &DlnaRenderer::play);
    QObject::connect(&mediaControls, &WindowsMediaControls::pauseRequested,
                     &renderer, &DlnaRenderer::pause);
    QObject::connect(&mediaControls, &WindowsMediaControls::stopRequested,
                     &renderer, &DlnaRenderer::stopTransport);
    QObject::connect(&mediaControls, &WindowsMediaControls::nextRequested,
                     &renderer, &DlnaRenderer::next);
    QObject::connect(&mediaControls, &WindowsMediaControls::previousRequested,
                     &renderer, &DlnaRenderer::previous);
    QObject::connect(&mediaControls, &WindowsMediaControls::seekRequested,
                     &renderer, &DlnaRenderer::seekTo);

    // 队列前后有没有地方可去 —— 面板上那对「上一首/下一首」照这个亮灭。
    QObject::connect(&renderer, &DlnaRenderer::queueChanged,
                     &mediaControls, &WindowsMediaControls::setQueueAvailability);

    // ── 托盘 ─────────────────────────────────────────────────────────────
    //
    // 常驻的意义全在这儿：窗口关了它还在，菜单里能叫回窗口、能挂"勿扰"、能退出。
    // 它和窗口一样，手上只有门面 —— 「暂停接收投送」该怎么做是 DLNA 那一层的事。
    TrayIcon tray(&window, &renderer);
    QObject::connect(&tray, &TrayIcon::logMessage,
                     &renderer, &DlnaRenderer::logMessage);

    // ── 新界面（QML，实验）────────────────────────────────────────────────
    //
    // 现在是"并存"状态：旧的 Widgets 界面照旧干活，新界面在托盘里手工打开。
    // 它懒加载 —— 没人点它就不建 QML 引擎，程序启动该多快还多快。
    //
    // 它的日志和大门面走同一条路。新界面自己还不会说话，等接了播放控制再说。
    NewUiWindow newUi;
    QObject::connect(&newUi, &NewUiWindow::logMessage,
                     &renderer, &DlnaRenderer::logMessage);
    QObject::connect(&tray, &TrayIcon::openNewUiRequested,
                     &newUi, &NewUiWindow::show);
    QObject::connect(&uiState, &UiState::languageChanged,
                     &newUi, &NewUiWindow::retranslate);

    // ── 开跑 ─────────────────────────────────────────────────────────────
    // 界面暂时照旧弹出来。以后换正式界面时，这里大概会变成"只留托盘"。
    window.show();
    tray.show();

    // 媒体面板要等窗口真的存在之后才能挂 —— 它认的是窗口号。
    mediaControls.attachToWindow(static_cast<quintptr>(window.winId()));

    player->start();
    renderer.start();

    return app.exec();
}
