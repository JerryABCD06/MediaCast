#include "NewUiWindow.h"

#include "platform/windows/WindowFrame.h"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickWindow>
#include <QUrl>

NewUiWindow::NewUiWindow(QObject *parent)
    : QObject(parent)
{
}

NewUiWindow::~NewUiWindow() = default;

bool NewUiWindow::isLoaded() const
{
    return m_window != nullptr;
}

bool NewUiWindow::load()
{
    // 窗口还在就什么都不用做。
    if (m_window)
        return true;

    // 引擎建过、窗口却没了 —— 说明那个 QML Window 被销毁了。**这时候不能
    // 重建**：实测在同一个进程里加载第二个引擎会崩在 Qt6Qml 里（NewUiWindow
    // 的 m_engine->load() 那一行上，出错模块 Qt6Qmld）。
    //
    // 正常情况下根本走不到这儿：NewUiWindow.qml 里设了 autoDestroy: false，
    // 关窗口只是藏起来。真走到了就认了 —— 界面这次运行期间不再打开，但投送
    // 和播放都不受影响。总比崩掉强。
    if (m_engine)
        return false;

    m_engine = new QQmlApplicationEngine(this);

    // FluentUI 那个 QML 模块被 CMake 拷到了 exe 旁边的 qml/ 下。
    // 引擎默认不会往那儿找，得说一声。发布版也是同样的目录结构，
    // 所以这里不用按 Debug/Release 分支。
    m_engine->addImportPath(QCoreApplication::applicationDirPath() + QStringLiteral("/qml"));

    // QML 的报错默认只往 stderr 吐，界面和日志里都看不见 —— 出了事只能猜。
    // 接到日志上，"哪一行、什么东西找不到"一眼就能看见。
    connect(m_engine, &QQmlApplicationEngine::warnings, this,
            [this](const QList<QQmlError> &errors) {
        for (const QQmlError &error : errors)
            emit logMessage(QStringLiteral("QML 报错：%1").arg(error.toString()));
    });

    m_engine->load(QUrl(QStringLiteral("qrc:/ui/NewUiWindow.qml")));

    if (m_engine->rootObjects().isEmpty()) {
        emit logMessage(QStringLiteral("新界面加载失败 —— 看上面的 QML 报错"));
        return false;
    }

    // 根对象得是个窗口，否则后面 show()/raise() 都是空谈。
    m_window = qobject_cast<QQuickWindow *>(m_engine->rootObjects().constFirst());
    if (!m_window) {
        emit logMessage(QStringLiteral("新界面的根对象不是窗口，加载失败"));
        return false;
    }

    // ── 兜底：窗口真被销毁的话，m_window 就成了野指针 ─────────────────────
    //
    // 正常情况下走不到（见 NewUiWindow.qml 里的 autoDestroy: false），但
    // 这个指针是裸的，不会自己变空 —— 万一哪天窗口真被销毁了，下一次
    // show() 就会去碰已经释放的内存，崩在 Qt6Gui 里什么都看不出来。
    // （踩过一次：Qt6Guid+0x34BA22，读 0xFFFFFFFFFFFFFFFF。）
    connect(m_window, &QObject::destroyed, this, [this] {
        m_window = nullptr;
        emit logMessage(QStringLiteral("新界面窗口被销毁了 —— 这次运行期间不再打开它，投送照常"));
    });

    // FluentUI 建这个窗口的时候少设了两个样式位，补上。
    // 为什么在这儿补、补的是哪两个，见 WindowFrame.h 的注释。
    WindowFrame::ensureSnapFlags(m_window);

    return true;
}

void NewUiWindow::show()
{
    if (!load())
        return;

    // load() 里窗口没建出来的话已经 return 了，这里只是把话说死：
    // 宁愿什么都不做，也不要再去碰一个可能已经没了的窗口。
    if (!m_window)
        return;

    m_window->show();
    m_window->raise();
    m_window->requestActivate();

    emit logMessage(QStringLiteral("新界面已打开（实验）"));
}

void NewUiWindow::retranslate()
{
    // 界面还没建起来的话，等它建起来时本来就用的新语言，不用管。
    if (m_engine)
        m_engine->retranslate();
}
