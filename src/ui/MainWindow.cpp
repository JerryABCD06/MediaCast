#include "MainWindow.h"

#include "protocols/dlna/DlnaRenderer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include <cmath>

namespace {

/** 把秒数变成 0:00 这种好读的时间。 */
QString formatTime(double seconds)
{
    if (!std::isfinite(seconds) || seconds < 0.0)
        seconds = 0.0;

    const int total = static_cast<int>(seconds);
    return QStringLiteral("%1:%2")
        .arg(total / 60, 2, 10, QLatin1Char('0'))
        .arg(total % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

MainWindow::MainWindow(DlnaRenderer *renderer, QWidget *parent)
    : QWidget(parent)
    , m_renderer(renderer)
{
    buildUi();
    connectUi();
}

void MainWindow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);

    if (m_videoWindowGiven || !m_videoArea)
        return;

    m_videoWindowGiven = true;
    m_renderer->setVideoWindow(static_cast<quintptr>(m_videoArea->winId()));
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // 不接受关闭、只是藏起来 —— 关掉窗口不等于退出程序。
    //
    // 注意顺序：hide() 在前、ignore() 在后无所谓，但 ignore() 必须有 ——
    // 少了它，窗口会真的被销毁，第二次从托盘点「打开主界面」就调不出东西了。
    event->ignore();
    hide();
}

void MainWindow::buildUi()
{
    // 界面上自称的名字。项目全名是 "Media Cast Receiver"，标题栏用短一点的。
    setWindowTitle(QStringLiteral("Media Cast"));
    resize(900, 640);

    // 给播放器画画面用的地方。内嵌方案里，mpv 就是把画面直接画进这个控件。
    // 涂成黑的，没画面的时候看起来也像块屏幕，而不是一片空白。
    m_videoArea = new QWidget;
    m_videoArea->setAutoFillBackground(true);
    QPalette videoPalette = m_videoArea->palette();
    videoPalette.setColor(QPalette::Window, Qt::black);
    m_videoArea->setPalette(videoPalette);
    m_videoArea->setMinimumHeight(260);

    m_urlEdit = new QLineEdit;
    m_urlEdit->setPlaceholderText(QStringLiteral("粘贴视频文件路径，或 http 地址"));

    m_nowPlayingLabel = new QLabel(QStringLiteral("等待投送"));
    m_nowPlayingLabel->setAlignment(Qt::AlignCenter);
    m_nowPlayingLabel->setWordWrap(true);

    // 广播频率：控制点可能收不到我们的搜索请求（手机当热点时它自己发不出组播），
    // 于是它只能靠听我们的广播来发现我们。广播越勤越容易被搜到，默认就开着。
    m_highRateCheck = new QCheckBox(QStringLiteral("高频广播（10 秒一次，更容易被搜到）"));
    m_highRateCheck->setChecked(true);

    m_playButton       = new QPushButton(QStringLiteral("播放"));
    m_pauseButton      = new QPushButton(QStringLiteral("暂停"));
    m_stopButton       = new QPushButton(QStringLiteral("停止"));

    m_prevButton       = new QPushButton(QStringLiteral("上一首"));
    m_nextButton       = new QPushButton(QStringLiteral("下一首"));

    // 一开始没有前后可去，先灰着。队列一变，queueChanged 会把它们点亮。
    m_prevButton->setEnabled(false);
    m_nextButton->setEnabled(false);

    m_disconnectButton = new QPushButton(QStringLiteral("断开投屏"));

    // 可勾选按钮：按下去=暂停，弹起来=继续。状态由按钮自己记着，不用另开变量。
    m_pauseButton->setCheckable(true);

    // 地址框和「播放」放同一行。
    //
    // 这个按钮的活儿只有一件：把上面那个框里的地址读进来开始放。它跟"暂停/停止"
    // 那一排不是一类东西 —— 混在一起容易让人以为是"继续播放"。
    auto *urlRow = new QHBoxLayout;
    urlRow->addWidget(m_urlEdit, 1);
    urlRow->addWidget(m_playButton);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_prevButton);
    buttonRow->addWidget(m_pauseButton);
    buttonRow->addWidget(m_stopButton);
    buttonRow->addWidget(m_nextButton);
    buttonRow->addWidget(m_disconnectButton);

    // 进度条用 0~1000 的"千分比"，显示时再按总时长换算成秒。
    // 这样刻度跟片子长短无关，一分钟的短片和三小时的电影都合用。
    m_posSlider = new QSlider(Qt::Horizontal);
    m_posSlider->setRange(0, 1000);

    m_timeLabel = new QLabel(QStringLiteral("0:00 / 0:00"));

    auto *posRow = new QHBoxLayout;
    posRow->addWidget(m_posSlider, 1);
    posRow->addWidget(m_timeLabel);

    m_volSlider = new QSlider(Qt::Horizontal);
    m_volSlider->setRange(0, 100);
    m_volSlider->setValue(100);

    m_volLabel = new QLabel(QStringLiteral("100"));

    m_muteButton = new QPushButton(QStringLiteral("静音"));
    m_muteButton->setCheckable(true);

    auto *volRow = new QHBoxLayout;
    volRow->addWidget(new QLabel(QStringLiteral("音量")));
    volRow->addWidget(m_volSlider, 1);
    volRow->addWidget(m_volLabel);
    volRow->addWidget(m_muteButton);

    // ── 画面调节 ─────────────────────────────────────────────────────────
    //
    // 下拉里放的是**播放后端自己报上来的**项，界面不写死任何一项的名字和量程。
    // 换后端、加一项，这里一行都不用改。
    m_pictureCombo = new QComboBox;
    m_pictureSlider = new QSlider(Qt::Horizontal);
    m_pictureValueLabel = new QLabel(QStringLiteral("0"));
    m_pictureResetButton = new QPushButton(QStringLiteral("复位"));

    m_pictureRow = new QWidget;
    auto *pictureLayout = new QHBoxLayout(m_pictureRow);
    pictureLayout->setContentsMargins(0, 0, 0, 0);
    pictureLayout->addWidget(new QLabel(QStringLiteral("画面")));
    pictureLayout->addWidget(m_pictureCombo);
    pictureLayout->addWidget(m_pictureSlider, 1);
    pictureLayout->addWidget(m_pictureValueLabel);
    pictureLayout->addWidget(m_pictureResetButton);

    m_playerStatusLabel = new QLabel(QStringLiteral("播放器: 准备中 ..."));
    m_playerStatusLabel->setAlignment(Qt::AlignCenter);
    m_playerStatusLabel->setWordWrap(true);

    m_rendererStatusLabel = new QLabel(QStringLiteral("DLNA: 准备中 ..."));
    m_rendererStatusLabel->setAlignment(Qt::AlignCenter);
    m_rendererStatusLabel->setWordWrap(true);

    m_logLabel = new QLabel;
    m_logLabel->setAlignment(Qt::AlignCenter);
    m_logLabel->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_videoArea, 1);
    layout->addWidget(m_nowPlayingLabel);
    layout->addLayout(urlRow);
    layout->addWidget(m_highRateCheck);
    layout->addLayout(buttonRow);
    layout->addLayout(posRow);
    layout->addLayout(volRow);
    layout->addWidget(m_pictureRow);
    layout->addWidget(m_playerStatusLabel, 1);
    layout->addWidget(m_rendererStatusLabel);
    layout->addWidget(m_logLabel);

    // 把播放后端报上来的画面调节项填进下拉。一个都没有就把整行藏起来 ——
    // 空着比摆一排按不动的滑块好。
    const QVector<PictureControlInfo> controls = m_renderer->pictureControls();
    for (const PictureControlInfo &control : controls)
        m_pictureCombo->addItem(control.label, control.name);

    if (controls.isEmpty())
        m_pictureRow->setVisible(false);
    else
        syncPictureSlider();
}

void MainWindow::syncPictureSlider()
{
    const QString name = m_pictureCombo->currentData().toString();
    if (name.isEmpty())
        return;

    // 量程是从播放器那儿现问的，界面不自己记一份 —— 记一份就得跟着后端一起改。
    for (const PictureControlInfo &control : m_renderer->pictureControls()) {
        if (control.name != name)
            continue;

        // 改范围和值都会触发 valueChanged；那会反过来又去设一遍，没必要。
        const QSignalBlocker blocker(m_pictureSlider);
        m_pictureSlider->setRange(control.min, control.max);
        const int value = m_renderer->pictureControlValue(name);
        m_pictureSlider->setValue(value);
        m_pictureValueLabel->setText(QString::number(value));
        return;
    }
}

void MainWindow::refreshTimeText()
{
    const double pos = m_durationSec > 0.0
        ? m_durationSec * m_posSlider->value() / m_posSlider->maximum()
        : 0.0;
    m_timeLabel->setText(formatTime(pos) + QStringLiteral(" / ") + formatTime(m_durationSec));
}

void MainWindow::updateNowPlaying(const NowPlaying &info)
{
    if (!info.hasTitle()) {
        m_nowPlayingLabel->setText(QStringLiteral("等待投送"));
        return;
    }

    QString text = QStringLiteral("正在播放：") + info.title;
    if (!info.artist.isEmpty())
        text += QStringLiteral("   ·   ") + info.artist;
    if (!info.album.isEmpty())
        text += QStringLiteral("   （") + info.album + QStringLiteral("）");
    m_nowPlayingLabel->setText(text);
}

void MainWindow::connectUi()
{
    // ── 门面 → 界面：状态显示 ────────────────────────────────────────────

    QObject::connect(m_renderer, &DlnaRenderer::playerStatusChanged,
                     m_playerStatusLabel, &QLabel::setText);
    QObject::connect(m_renderer, &DlnaRenderer::statusChanged,
                     m_rendererStatusLabel, &QLabel::setText);

    QObject::connect(m_renderer, &DlnaRenderer::nowPlayingChanged, this,
                     [this](const NowPlaying &info) { updateNowPlaying(info); });

    // 队列前后有没有地方可去 —— 那两个按钮照这个亮灭。
    QObject::connect(m_renderer, &DlnaRenderer::queueChanged, this,
                     [this](bool hasNext, bool hasPrevious) {
        m_nextButton->setEnabled(hasNext);
        m_prevButton->setEnabled(hasPrevious);
    });

    // 日志只看最后一条 —— 挑网卡的过程有好几行，全塞进标签会看不清。
    QObject::connect(m_renderer, &DlnaRenderer::logMessage,
                     m_logLabel, &QLabel::setText);

    QObject::connect(m_highRateCheck, &QCheckBox::toggled, m_renderer, [this](bool on) {
        m_renderer->setAliveIntervalMs(on ? 10000 : 60000);
    });

    // ── 进度 ────────────────────────────────────────────────────────────

    QObject::connect(m_renderer, &DlnaRenderer::durationChanged, this, [this](double seconds) {
        m_durationSec = seconds;
        refreshTimeText();
    });

    QObject::connect(m_renderer, &DlnaRenderer::positionChanged, this, [this](double seconds) {
        // 用户正按着进度条拖的时候，别让播放器报的位置把滑块抢回去。
        if (m_posSlider->isSliderDown())
            return;
        if (m_durationSec > 0.0)
            m_posSlider->setValue(static_cast<int>(m_posSlider->maximum() * seconds / m_durationSec));
        refreshTimeText();
    });

    // 拖动过程中只更新时间文字，松手才真正跳过去。
    // 边拖边发 seek 会把播放器淹掉。
    QObject::connect(m_posSlider, &QSlider::sliderMoved, this,
                     [this](int) { refreshTimeText(); });

    QObject::connect(m_posSlider, &QSlider::sliderReleased, this, [this] {
        if (m_durationSec > 0.0)
            m_renderer->seekTo(m_durationSec * m_posSlider->value() / m_posSlider->maximum());
    });

    // ── 音量与静音 ──────────────────────────────────────────────────────

    QObject::connect(m_volSlider, &QSlider::valueChanged, m_renderer, [this](int value) {
        m_volLabel->setText(QString::number(value));
        m_renderer->setVolumePercent(value);
    });

    QObject::connect(m_renderer, &DlnaRenderer::volumeChanged, this, [this](int percent) {
        // 这里是"播放器告诉我们音量变了"，而滑块一动又会发命令回去，
        // 不加这层阻断两边就会来回打架。QSignalBlocker 让这次赋值不发出信号。
        const QSignalBlocker blocker(m_volSlider);
        m_volSlider->setValue(percent);
        m_volLabel->setText(QString::number(percent));
    });

    QObject::connect(m_muteButton, &QPushButton::toggled, m_renderer,
                     [this](bool muted) { m_renderer->setMuted(muted); });

    QObject::connect(m_renderer, &DlnaRenderer::muteChanged, this, [this](bool muted) {
        const QSignalBlocker blocker(m_muteButton);
        m_muteButton->setChecked(muted);
    });

    // ── 画面调节 ────────────────────────────────────────────────────────
    //
    // 换一项就把滑块接到那一项的量程和当前值上；拖滑块就直接设过去。
    // 底下是什么属性、有没有这一项，界面一概不知道，问门面要。

    QObject::connect(m_pictureCombo, &QComboBox::currentIndexChanged, this,
                     [this](int) { syncPictureSlider(); });

    QObject::connect(m_pictureSlider, &QSlider::valueChanged, this, [this](int value) {
        const QString name = m_pictureCombo->currentData().toString();
        if (name.isEmpty())
            return;
        m_pictureValueLabel->setText(QString::number(value));
        m_renderer->setPictureControl(name, value);
    });

    QObject::connect(m_pictureResetButton, &QPushButton::clicked, this, [this] {
        m_renderer->resetPictureControls();
        syncPictureSlider();
    });

    // 暂停状态也同步回按钮：片子播完时按钮会自动弹起来。
    QObject::connect(m_renderer, &DlnaRenderer::pausedChanged, this, [this](bool paused) {
        const QSignalBlocker blocker(m_pauseButton);
        m_pauseButton->setChecked(paused);
    });

    // ── 界面 → 门面：所有命令都从这里走 ─────────────────────────────────
    //
    // 这一整块必须走门面。以前这些按钮直接命令播放器，结果 DLNA 那层不知道状态变了，
    // 手机上就会显示成另一个样子 —— 那三处 bug 的根因都在这里。

    QObject::connect(m_playButton, &QPushButton::clicked, m_renderer, [this] {
        const QString url = m_urlEdit->text().trimmed();
        if (url.isEmpty()) {
            m_logLabel->setText(QStringLiteral("请先填入要播放的地址"));
            return;
        }
        // 顺序要紧：先把暂停按钮弹起来（那会触发一次 play），再设新地址，
        // 这样状态最终停在 TRANSITIONING 而不是被打回 PLAYING。
        m_pauseButton->setChecked(false);
        m_renderer->openUri(url);
    });

    QObject::connect(m_pauseButton, &QPushButton::toggled, m_renderer, [this](bool paused) {
        if (paused) m_renderer->pause();
        else        m_renderer->play();
    });

    // 停止：停下播放，但会话还在 —— 手机上还能接着按播放继续。
    QObject::connect(m_stopButton, &QPushButton::clicked, m_renderer, [this] {
        m_pauseButton->setChecked(false);
        m_renderer->stopTransport();
    });

    // 上一首 / 下一首。走门面 —— 和手机按的是同一条路，状态机才不会两边不一致。
    QObject::connect(m_prevButton, &QPushButton::clicked, m_renderer,
                     [this] { m_renderer->previous(); });
    QObject::connect(m_nextButton, &QPushButton::clicked, m_renderer,
                     [this] { m_renderer->next(); });

    // 断开投屏：门面内部会把状态归零、推事件、再让设备在网络里消失一下 ——
    // 有些控制点只有看到设备真的不在了，才会把那条"正在投屏"的横幅收掉。
    QObject::connect(m_disconnectButton, &QPushButton::clicked, m_renderer, [this] {
        m_pauseButton->setChecked(false);
        m_renderer->endSession();
    });
}
