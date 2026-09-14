// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QIcon>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTime>
#include <QWidget>
#include <QtQml/qqml.h>

#include <memory>

#include "core/AppSettings.h"
#include "core/LegalDocs.h"
#include "core/MpvCore.h"
#include "core/MpvQmlItem.h"
#include "core/PlaybackController.h"
#include "core/Tr.h"
#include "platform/windows/WindowsMediaControls.h"
#include "protocols/dlna/DlnaRenderer.h"
#include "ui/NewUiWindow.h"
#include "ui/TrayIcon.h"
#include "ui/UiState.h"

/**
 * 组装整个程序。这里只做五件事：造零件、接日志、接 Windows 媒体面板、开窗口、起服务。
 *
 * （"开窗口"现在只剩托盘；主界面是**有东西投过来才开**的 —— 见下面 mediaChanged
 *   那一段。启动时不再弹任何窗口，这是旧界面退场之后的样子。）
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

    // ── 日志落盘 ─────────────────────────────────────────────────────────
    //
    // 这一段落放在这里（而不是像别处一样按"谁用到谁"往后放），是因为下面
    // **语言那一段自己就要写日志** —— 扫到几个语言文件、用的哪个，正是排查
    // "界面怎么是英文的"时第一眼要看的东西，丢了就没法查。
    //
    // 界面上一行标签只显示最后一条，排查问题时根本不够用；有文件才能看到完整
    // 的先后顺序。（这是临时脚手架 —— 做正式界面时，日志位置会挪到用户目录下。）
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

    // ── 语言 ─────────────────────────────────────────────────────────────
    //
    // 有哪些语言、哪个键对应哪句话、系统语言该对到哪个 —— 全在 Tr 里，它扫的是
    // exe 旁边的 lang/ 目录（明文 JSON，不编译）。加一种语言就是往那个目录里丢
    // 一个文件，见 lang/README.md。
    //
    // **必须先 scan 再建 UiState**：UiState 一造好就会去问 Tr "现在该用哪个语言"，
    // 那时候清单还没扫出来，跟随系统就会一路落到基准语言去。
    Tr tr;
    QObject::connect(&tr, &Tr::logMessage, writeLog);
    tr.scan();

    // 给 QML 一份 —— 设置页的语言列表就是它扫出来的那些。
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Tr", &tr);

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
    // 设置文件：主程序旁边那份 JSON。没有就按默认值生成一份出来 ——
    // 用户想改直接拿记事本改，不用翻界面。
    AppSettings settings;
    UiState uiState(&settings, &tr);
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "UiState", &uiState);
    // 设置页直接读这一份 —— 它是"唯一的那一份"，界面不该再存副本。
    // 名字叫 Settings 而不是 AppSettings：QML 那边写 `Settings.castNewCast`
    // 比 `AppSettings.uiDarkMode` 顺眼，而类名的事是 C++ 的事。
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Settings", &settings);

    // ── 法律文本 ─────────────────────────────────────────────────────────
    //
    // 隐私 / 法律 / 商标 / 编解码器那几份声明，以及第三方组件的许可全文。
    // 它只做两件事：告诉界面"有哪些、叫什么"，以及按名字把文件读出来 ——
    // 文本来自 exe 旁边的 legal/ 和 licenses/，不编进 exe（理由见 LegalDocs.h）。
    //
    // 构造时会自己查一遍文件在不在，缺了就在日志里点名 —— 发布包缺许可文本是
    // 合规问题，不该等用户点开才发现。
    LegalDocs legal;
    // 和 Tr 一个套路：先构造、接日志、再 scan() —— 自检那几行才不会白喊。
    QObject::connect(&legal, &LegalDocs::logMessage, writeLog);
    legal.scan();
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Legal", &legal);

    // 名字分两个，别混：
    //   applicationName        —— 给机器看的。决定配置目录（%LOCALAPPDATA%\MCast）之类。
    //   applicationDisplayName —— 给人看的。窗口标题、对话框标题用它。
    QApplication::setApplicationName(QStringLiteral("MCast"));
    QApplication::setApplicationDisplayName(QStringLiteral("Media Cast"));
    // 版本号：和设备描述里的 modelNumber 是同一个数，改的时候两处一起改。
    QApplication::setApplicationVersion(QStringLiteral("0.1"));

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

    // 设置文件：主程序旁边那份 JSON。没有就按默认值生成一份出来 ——
    // 用户想改直接拿记事本改，不用翻界面。
    writeLog(QStringLiteral("设置文件：%1（%2）")
                 .arg(settings.filePath(),
                      settings.isPersistent() ? QStringLiteral("可读写")
                                              : QStringLiteral("写不进去，改动不会保留")));
    QObject::connect(&settings, &AppSettings::logMessage, writeLog);

    // ── 播放后端 ─────────────────────────────────────────────────────────
    //
    // 走 **render API**：mpv 的画面由 QML 场景图合成，所以它能被裁剪、被别的控件
    // 压住（旧界面那条 wid 路做不到 —— 视频是独立的原生子窗口，永远盖在所有 QML
    // 之上）。**一个 mpv 实例只能有一条画面输出路径**（vo 只能设一次，wid 和
    // render API 互斥），旧界面退场之后只剩这一条，那个过渡开关也就删掉了。
    //
    // `LibMpvPlayer`（wid 外壳）和 `MpvMediaPlayer`（外部 mpv.exe + 命名管道）
    // 都还在源码里、没人构造了 —— 留着是"排查问题时能换回来"的那条退路。
    std::unique_ptr<MpvCore> ownedPlayer;
    {
        auto core = std::make_unique<MpvCore>();
        core->setOutputMode(MpvCore::RenderApiOutput);
        ownedPlayer = std::move(core);
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

    // 控制器也给 QML —— 新界面靠它判断"现在屏幕上是不是空的"，
    // 好决定显示画面还是显示投屏指引。
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Playback", &playback);

    // "现在放的是哪一条"（标题/歌手/专辑）也得让 QML 读得到 —— 新界面底下那条
    // 控制栏的标题和副标题就是读它。**匿名注册**：QML 只需要能读它的字段，
    // 不需要拿这个名字去 new 一个出来（它是纯数据）。
    qmlRegisterAnonymousType<NowPlaying>("MediaCast", 1);

    DlnaRenderer renderer(&playback);

    // 这台设备在网络里的身份（名字、地址、唯一标识、版本）给 QML 一份 ——
    // 设置页的「关于」读它。**只读**，界面改不了这些东西。
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Device", &renderer);

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
    // 常驻的意义全在这儿：窗口关了它还在，菜单里能叫回界面、能挂"勿扰"、能退出。
    // 它手上只有门面，而且**不持有任何窗口** —— 「打开界面」是发个信号过来，
    // 开哪个、怎么开，由这儿决定。
    TrayIcon tray(&renderer);
    QObject::connect(&tray, &TrayIcon::logMessage,
                     &renderer, &DlnaRenderer::logMessage);

    // ── 新界面（QML）─────────────────────────────────────────────────────
    //
    // 正式的界面。旧的 Widgets 那一套还留着，但降级成"测试界面" —— 只在托盘
    // 菜单里手工打开，DLNA 回归测试要有个看得见的观测窗口。
    //
    // 它懒加载 —— 没人点它就不建 QML 引擎，程序启动该多快还多快。
    //
    // 它的日志和大门面走同一条路。
    NewUiWindow newUi;
    QObject::connect(&newUi, &NewUiWindow::logMessage,
                     &renderer, &DlnaRenderer::logMessage);

    // 新界面注册给 QML：界面上"关窗并断开投送"要调它。它自己不认识 DLNA，
    // 只把意思发出来，由这儿接到门面上 —— 和 UiState / Playback 一个路子。
    //
    // **必须赶在第一个 QML 引擎建起来之前注册**，所以放在这儿而不是文件开头。
    qmlRegisterSingletonInstance("MediaCast", 1, 0, "Shell", &newUi);
    QObject::connect(&newUi, &NewUiWindow::castEndRequested,
                     &renderer, &DlnaRenderer::endSession);

    // 托盘那个「打开主界面」。
    QObject::connect(&tray, &TrayIcon::openMainUiRequested,
                     &newUi, &NewUiWindow::show);
    // 有东西要投过来（或者界面上按了播放），就把新界面拉起来。
    //
    // 这**不只是一句方便**：render API 模式下 mpv 的视频输出要等画面 item
    // 真的开始渲染才开得起来，所以新界面必须先在这儿起来，片子才放得出来。
    // 控制器那边会把加载攒住，等渲染面就绪再真的发出去 —— 时序不用我们操心。
    QObject::connect(&playback, &PlaybackController::mediaChanged,
                     &newUi, &NewUiWindow::show);
    QObject::connect(&uiState, &UiState::languageChanged,
                     &newUi, &NewUiWindow::retranslate);
    // 托盘菜单那几句是死的 —— QMenu 不会自己重画，得有人把文字重新设一遍。
    // （新界面那边靠 QML 的绑定重算，这里靠这一句。）
    QObject::connect(&uiState, &UiState::languageChanged,
                     &tray, &TrayIcon::retranslate);

    // 标题里可能装着**兜底出来的字**（协议层没给标题时拿类型名顶上：中文写
    // 「视频」，英文得是 "Video"）。那是开着片子的时候算好的，语言再一变它不会
    // 自己重跑 —— 叫控制器重算一遍，界面和 Windows 媒体面板才跟着更新。
    QObject::connect(&uiState, &UiState::languageChanged,
                     &playback, &PlaybackController::retranslate);

    // ── 设置文件接到网络上那几项 ─────────────────────────────────────────
    //
    // 两个方向都要：
    //   文件变了  -> 服务跟着变（用户拿记事本改了，或者以后加设置界面）
    //   服务变了  -> 写回文件（托盘的「暂停接收投送」也要记住）
    //
    // 不会来回弹：两边 setter 都是"值一样就不动"。
    QObject::connect(&settings, &AppSettings::broadcastChanged,
                     &renderer, &DlnaRenderer::setBroadcasting);
    QObject::connect(&settings, &AppSettings::broadcastIntervalChanged,
                     &renderer, &DlnaRenderer::setAliveIntervalMs);
    QObject::connect(&settings, &AppSettings::acceptNewCastChanged, &renderer,
                     [&renderer](bool accept) {
        if (accept)
            renderer.resumeAccepting();
        else
            renderer.pauseAccepting();
    });
    QObject::connect(&renderer, &DlnaRenderer::acceptingChanged,
                     &settings, &AppSettings::setAcceptNewCast);

    // ── 画面调节（「显示效果」那十来项）：改了记下来，开机再放回去 ────────
    //
    // 方向只有一条：**播放器变了 → 写文件**。反方向不接 —— 设置文件里那份只在
    // 开机那一次读（见下面 player->start() 之后那几行），运行中它只是"备忘"，
    // 权威始终在播放器手里。这样就不存在"两份状态谁说了算"。
    //
    // 放在这一层而不是塞进播放器：**存哪儿、什么时候存**是应用的事，不是
    // "怎么调 mpv"的事（见 MediaPlayer 那条抽象边界）。
    //
    // 顺带一提，控制点（手机）调的画面走的是同一条路 —— 它也是"播放器的值变了"，
    // 所以手机上把亮度拉过之后，那一条也会记住。
    QObject::connect(&playback, &PlaybackController::pictureControlChanged,
                     &settings, [&settings](const QString &name, int value) {
        settings.setPictureValue(name, value);
    });

    // ── 开跑 ─────────────────────────────────────────────────────────────
    //
    // **启动时只有托盘，不弹窗口。** 这台设备平时的样子就是"在托盘里待着、等手机
    // 投过来"；有东西投过来时 mediaChanged 会把主界面拉起来（见上面那段）。
    // 用户想主动看看，托盘菜单里有「打开主界面」。
    tray.show();

    // 媒体面板要挂在**一个窗口**上（WinRT 那边只能拿窗口号换 SMTC 对象）。
    //
    // **不能挂到主界面那扇窗上**：它是"关了真销毁、下次再建一扇新的"，窗口号会变，
    // 而面板挂上去之后就跟死在这一个窗口号上（重挂也来不及）。所以另起一个
    // **常驻的、不显示的宿主窗口**，它活多久面板就活多久。
    //
    // 面板要的只是"一个属于本进程的顶层窗口"：它显示什么、按了什么，全走信号，
    // 跟这个窗口长什么样、在不在屏幕上都无关。
    QWidget mediaHostWindow;
    mediaHostWindow.setWindowTitle(QStringLiteral("Media Cast"));
    // `Qt::Tool`：不进任务栏、不进 Alt+Tab。这个窗口只是给面板"递一个窗口号"用的，
    // 用户不该看见它、更不该在任务栏里发现多了一个窗口。
    mediaHostWindow.setWindowFlags(Qt::Tool);
    mediaHostWindow.resize(1, 1);
    // 先 show 一次再藏起来：那个 interop 拿的是"系统里真的存在的一个窗口"，
    // 让平台那边先把窗口建出来更稳（不 show 直接问窗口号，Qt 也会建，
    // 但这里不想赌）。1×1、又是工具窗，露一下不会被人看见。
    mediaHostWindow.show();
    mediaHostWindow.hide();
    mediaControls.attachToWindow(static_cast<quintptr>(mediaHostWindow.winId()));

    player->start();
    renderer.start();

    // 服务起来之后再把设置里那几项应用上去 —— 广播间隔和"要不要广播"直接
    // 设就行；「是否接收投送」要走暂停/恢复那条路，那要求服务已经起来了。
    renderer.setAliveIntervalMs(settings.broadcastIntervalMs());
    renderer.setBroadcasting(settings.broadcast());
    if (!settings.acceptNewCast())
        renderer.pauseAccepting();

    // 把上次记住的画面调节放回去。
    //
    // **必须等 player->start()** —— 在那之前 mpv 实例还没建起来，设属性是空操作
    // （MpvCore::setPictureControl 一开头就 `if (!m_mpv) return false;`）。
    //
    // 文件里没有的项**不碰** —— 让播放器留着自己的默认值，比我们猜一个"0"
    // 塞进去靠谱（后端以后加一项、或者某一项的中性值不是 0 都不会出错）。
    const QVariantMap savedPicture = settings.pictureValues();
    for (auto it = savedPicture.constBegin(); it != savedPicture.constEnd(); ++it)
        playback.setPictureControl(it.key(), it.value().toInt());

    return app.exec();
}
