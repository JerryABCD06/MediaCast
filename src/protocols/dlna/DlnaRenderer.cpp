#include "DlnaRenderer.h"

#include "GenaManager.h"
#include "HttpServer.h"
#include "core/PlaybackController.h"
#include "SoapHandler.h"
#include "SsdpService.h"

DlnaRenderer::DlnaRenderer(PlaybackController *controller, QObject *parent)
    : QObject(parent)
    , m_ctl(controller)
{
    m_ssdp = new SsdpService(this);
    m_soap = new SoapHandler(controller, this);
    m_gena = new GenaManager(this);
    m_http = new HttpServer(this);

    m_http->setSoapHandler(m_soap);
    m_http->setGenaHandler(m_gena);

    // ── 内部连线：这几根线以前散在 main() 里，现在归模块自己管 ──────────────

    // 传输状态一变就推给订阅过的控制点，同时转发给界面。
    connect(m_soap, &SoapHandler::transportStateChanged,
            m_gena, &GenaManager::pushTransportState);
    connect(m_soap, &SoapHandler::transportStateChanged,
            this, &DlnaRenderer::transportStateChanged);
    connect(m_soap, &SoapHandler::nowPlayingChanged,
            this, &DlnaRenderer::nowPlayingChanged);

    // 队列变了分两路走，两路要的东西不一样：
    //   一路原样推给订阅过的控制点 —— 它要的是"你排的那条我收到了"，
    //        所以 NextAVTransportURI 和 CurrentPlayMode 都得带上；
    //   一路只报"前后有没有地方可去"给界面和 Windows 媒体面板 —— 它们只需要这两个
    //        布尔值来决定那对按钮亮不亮。
    connect(m_soap, &SoapHandler::queueChanged, m_gena, &GenaManager::pushQueueState);
    connect(m_soap, &SoapHandler::queueChanged, this,
            [this](bool hasNext, bool hasPrevious, const QString &, const QString &) {
        emit queueChanged(hasNext, hasPrevious);
    });

    // "现在装的是哪一条"也要推给控制点 —— 不然电脑上按「上一首」时手机毫不知情。
    connect(m_soap, &SoapHandler::mediaChanged, m_gena, &GenaManager::pushMediaState);

    // 画面调节也一样：界面拖一下亮度，订阅过的控制点要收到通知。
    connect(m_soap, &SoapHandler::pictureControlsChanged,
            m_gena, &GenaManager::pushPictureControls);

    if (m_ctl) {
        // 音量和静音是两个独立的信号，但事件里要一起报，所以每次都取当前的两个值。
        connect(m_ctl, &PlaybackController::volumeChanged, this, [this](int volume) {
            m_gena->pushRendering(volume, m_ctl->isMuted());
            emit volumeChanged(volume);
        });
        connect(m_ctl, &PlaybackController::muteChanged, this, [this](bool muted) {
            m_gena->pushRendering(m_ctl->volumePercent(), muted);
            emit muteChanged(muted);
        });

        // 界面要显示的状态也一并转发 —— 这样界面就不必认识播放器。
        connect(m_ctl, &PlaybackController::playerStatusChanged, this, &DlnaRenderer::playerStatusChanged);
        // 播放器的状态变化同时也写进日志。不然"到底启没启动"在事后无从查起。
        connect(m_ctl, &PlaybackController::playerStatusChanged, this, &DlnaRenderer::logMessage);
        connect(m_ctl, &PlaybackController::logMessage, this, &DlnaRenderer::logMessage);
        connect(m_ctl, &PlaybackController::positionChanged, this, &DlnaRenderer::positionChanged);
        connect(m_ctl, &PlaybackController::durationChanged, this, &DlnaRenderer::durationChanged);
        connect(m_ctl, &PlaybackController::pausedChanged, this, &DlnaRenderer::pausedChanged);

        // "播放器没了就结束会话"这条规矩现在在控制器里 —— 那是状态机的事，
        // 不该让协议层去记。这里只需要知道一声，日志里留个痕。
        connect(m_ctl, &PlaybackController::playerLost, this, [this] {
            emit logMessage(QStringLiteral("播放器没了，投送会话已结束"));
        });
    }

    // ── 日志汇集 ────────────────────────────────────────────────────────
    connect(m_ssdp, &SsdpService::logMessage, this, &DlnaRenderer::logMessage);
    connect(m_http, &HttpServer::logMessage, this, &DlnaRenderer::logMessage);
    connect(m_soap, &SoapHandler::logMessage, this, &DlnaRenderer::logMessage);
    connect(m_gena, &GenaManager::logMessage, this, &DlnaRenderer::logMessage);

    // ── 状态摘要 ────────────────────────────────────────────────────────
    connect(m_ssdp, &SsdpService::statusChanged, this, [this](const QString &text) {
        m_ssdpStatus = text;
        updateStatus();
    });
    connect(m_http, &HttpServer::statusChanged, this, [this](const QString &text) {
        m_httpStatus = text;
        updateStatus();
    });
}

DlnaRenderer::~DlnaRenderer() = default;

void DlnaRenderer::updateStatus()
{
    emit statusChanged(QStringLiteral("%1    %2").arg(m_ssdpStatus, m_httpStatus));
}

bool DlnaRenderer::start()
{
    if (m_running)
        return true;

    // 顺序要紧：SSDP 先起，因为它负责挑网卡、拿设备标识和友好名 ——
    // HTTP 服务要把这些写进设备描述里。
    const bool ssdpOk = m_ssdp->start();
    const bool httpOk = m_http->start(SsdpService::DefaultHttpPort,
                                      m_ssdp->friendlyName(),
                                      m_ssdp->udn());

    m_running = ssdpOk && httpOk;
    emit runningChanged(m_running);
    return m_running;
}

void DlnaRenderer::stop()
{
    if (!m_running)
        return;

    // 先停 HTTP 再停 SSDP：反过来的话，控制点会先看不到设备、再去抓一个已经关掉的
    // 描述地址，日志里会多出一串没意义的失败。
    m_http->stop();
    m_ssdp->stop();

    m_running = false;
    emit runningChanged(false);
}

void DlnaRenderer::pauseAccepting()
{
    if (!m_accepting)
        return;

    // 先把当前投送结束掉：状态归零、给订阅者推事件，手机那边的投屏界面会收起来。
    //
    // 这一步不能省。光停服务的话，手机手上已经有我们的控制地址，照样能继续指挥播放
    // —— 那种"搜不到但还能控制"的半隐状态，比干脆说再见更让人糊涂。
    if (transportState() != QLatin1String("NO_MEDIA_PRESENT"))
        endSession();

    // 再从网络上消失。stop() 内部会先发一条 byebye，控制点不用干等 max-age 过期。
    stop();

    m_accepting = false;
    emit acceptingChanged(false);
    emit logMessage(QStringLiteral("已暂停接收投送：设备已从网络上消失"));
}

void DlnaRenderer::resumeAccepting()
{
    if (m_accepting)
        return;

    m_accepting = true;

    // start() 里 SSDP 一起来就会立刻广播一次 alive，HTTP 也跟着回来。
    start();

    emit acceptingChanged(true);
    emit logMessage(QStringLiteral("已恢复接收投送"));
}

void DlnaRenderer::openUri(const QString &uri)
{
    // 走"本地播放"那个入口，不是控制点那个。两条路的状态机完全一样，
    // 区别只在来源 —— 副标题要显示「本地播放」而不是「DLNA 投送」。
    //
    // 本地播放没有 DIDL 元数据，所以请求里只有地址 —— 标题交给控制器从文件名推。
    PlaybackController::MediaRequest request;
    request.uri = uri;
    m_ctl->openUri(request, QStringLiteral("本地播放"));
}

void DlnaRenderer::play()
{
    m_ctl->play();
}

void DlnaRenderer::pause()
{
    m_ctl->pause();
}

void DlnaRenderer::stopTransport()
{
    m_ctl->stop();
}

void DlnaRenderer::endSession()
{
    m_ctl->endSession();

    // 光把状态归零还不够：有些控制点会一直把设备记成"当前投屏目标"，界面上那条
    // 横幅不肯消。让它暂时从网络上消失一下，控制点才会真的放下这个会话。
    m_ssdp->announceGoingAwayBriefly();
}

void DlnaRenderer::next()
{
    m_ctl->next();
}

void DlnaRenderer::previous()
{
    m_ctl->previous();
}

bool DlnaRenderer::hasNext() const { return m_ctl->hasNext(); }
bool DlnaRenderer::hasPrevious() const { return m_ctl->hasPrevious(); }

void DlnaRenderer::seekTo(double seconds)
{
    if (m_ctl)
        m_ctl->seekTo(seconds);
}

void DlnaRenderer::setVolumePercent(int percent)
{
    if (m_ctl)
        m_ctl->setVolumePercent(percent);
}

void DlnaRenderer::setMuted(bool muted)
{
    if (m_ctl)
        m_ctl->setMuted(muted);
}

void DlnaRenderer::setVideoWindow(quintptr windowId)
{
    // 记一笔：内嵌方案能不能画上画面，第一步就是窗口号有没有正确传到。
    // 事后排查"有声音没画面"时，这一行是第一个要看的地方。
    emit logMessage(QStringLiteral("画面区窗口号 = 0x%1").arg(windowId, 0, 16));

    if (m_ctl)
        m_ctl->setVideoWindow(windowId);
}

QVector<PictureControlInfo> DlnaRenderer::pictureControls() const
{
    return m_ctl ? m_ctl->pictureControls() : QVector<PictureControlInfo>();
}

void DlnaRenderer::setPictureControl(const QString &name, int value)
{
    if (m_ctl)
        m_ctl->setPictureControl(name, value);
}

int DlnaRenderer::pictureControlValue(const QString &name) const
{
    return m_ctl ? m_ctl->pictureControlValue(name) : 0;
}

void DlnaRenderer::resetPictureControls()
{
    if (m_ctl)
        m_ctl->resetPictureControls();
}

void DlnaRenderer::setAliveIntervalMs(int ms)
{
    m_ssdp->setAliveIntervalMs(ms);
}

QString DlnaRenderer::deviceName() const { return m_ssdp->friendlyName(); }
QString DlnaRenderer::address()    const { return m_ssdp->localAddress(); }
QString DlnaRenderer::locationUrl() const { return m_ssdp->locationUrl(); }
QString DlnaRenderer::transportState() const { return m_soap->transportState(); }

double DlnaRenderer::positionSeconds() const { return m_ctl ? m_ctl->positionSeconds() : 0.0; }
double DlnaRenderer::durationSeconds() const { return m_ctl ? m_ctl->durationSeconds() : 0.0; }
int    DlnaRenderer::volumePercent()   const { return m_ctl ? m_ctl->volumePercent() : 0; }
bool   DlnaRenderer::isMuted()         const { return m_ctl && m_ctl->isMuted(); }

int DlnaRenderer::subscriptionCount() const { return m_gena->subscriptionCount(); }
