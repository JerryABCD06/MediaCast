#include "UiState.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QStyleHints>
#include <QTranslator>

UiState::UiState(QObject *parent)
    : QObject(parent)
{
    // 开机就把两样都落实一遍。先语言后外观，顺序无所谓，互不干涉。
    applyLanguage();
    applyTheme();

    // System 模式下，用户在 Windows 设置里改深色时我们自己得跟上，
    // 否则界面会一直停在开机那一刻的样子。
    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this] {
        if (m_themeMode == System)
            emit darkChanged();
    });
}

UiState::~UiState()
{
    // 析构前把翻译器摘下来。installTranslator 存的是裸指针，不摘的话
    // 对象没了它还留着 —— 退出那一刻正好用到翻译就是野指针。
    if (m_appTranslator) {
        QCoreApplication::removeTranslator(m_appTranslator);
        delete m_appTranslator;
        m_appTranslator = nullptr;
    }
    if (m_qtTranslator) {
        QCoreApplication::removeTranslator(m_qtTranslator);
        delete m_qtTranslator;
        m_qtTranslator = nullptr;
    }
}

bool UiState::dark() const
{
    // 不去自己算"系统是不是深色"，那个判断 Windows 各版本口径不一。
    // 交给 Qt：colorScheme 设成 Unknown 时它跟随系统，设成 Light/Dark
    // 时它就返回我们设的那个。三种模式都问到同一个答案。
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

QString UiState::systemLanguageCode()
{
    // 只取系统语言清单里的**第一个**，不能把 QLocale::system() 整个拿去用。
    //
    // Qt 在 Windows 上会把用户在系统设置里排过的所有语言都列出来当候选。
    // 一台装了「中文(简体) + 英文(美国)」的机器上实测拿到的是：
    //
    //   zh-Hans-CN | zh-CN | zh-Hans | zh | en-Latn-US | en-US | en-Latn | en
    //
    // QTranslator::load 会顺着这一串往下找，中文那几项没有对应的 .qm 时
    // 就一路落到英文去，结果中文系统上弹出英文界面。只认第一个就没这问题。
    const QLocale systemLocale = QLocale::system();
    const QStringList uiLanguages = systemLocale.uiLanguages();
    if (uiLanguages.isEmpty())
        return systemLocale.name();
    return QLocale(uiLanguages.constFirst()).name();
}

void UiState::setThemeMode(int mode)
{
    if (mode < System || mode > Dark || m_themeMode == mode)
        return;

    m_themeMode = mode;
    applyTheme();
    emit themeModeChanged();
}

void UiState::applyTheme()
{
    QStyleHints *hints = QGuiApplication::styleHints();
    switch (m_themeMode) {
    case Light:
        hints->setColorScheme(Qt::ColorScheme::Light);
        break;
    case Dark:
        hints->setColorScheme(Qt::ColorScheme::Dark);
        break;
    case System:
    default:
        // Unknown 的意思是"交给平台决定"，也就是跟随系统。
        hints->unsetColorScheme();
        break;
    }

    // 原生控件已经跟着变了，QML 那边在等这个信号去改 FluTheme。
    emit darkChanged();
}

void UiState::setLanguage(const QString &code)
{
    if (m_language == code)
        return;

    m_language = code;
    applyLanguage();
    emit languageChanged();
}

void UiState::applyLanguage()
{
    // 换语言 = 先把旧的摘干净，再装新的。不先摘就装，两个翻译器同时挂着，
    // 查表按安装顺序来，结果会变成"一半新语言一半旧语言"。
    if (m_appTranslator) {
        QCoreApplication::removeTranslator(m_appTranslator);
        delete m_appTranslator;
        m_appTranslator = nullptr;
    }
    if (m_qtTranslator) {
        QCoreApplication::removeTranslator(m_qtTranslator);
        delete m_qtTranslator;
        m_qtTranslator = nullptr;
    }

    const QLocale locale(m_language.isEmpty() ? systemLanguageCode() : m_language);

    // 一、我们自己的字符串。找不到就什么都不装，那时显示的正是源码原文，
    //     也就是中文。所以中文环境下即使一个 .qm 都没有也不影响。
    auto *appTranslator = new QTranslator(this);
    if (appTranslator->load(locale, QStringLiteral("MCast"),
                            QStringLiteral("_"), QStringLiteral(":/i18n"))) {
        m_appTranslator = appTranslator;
        QCoreApplication::installTranslator(m_appTranslator);
    } else {
        delete appTranslator;
    }

    // 二、Qt 自己的字符串，也就是"原生控件"那一半。
    //
    //     文件对话框上的"打开/取消"、消息框上的"确定/是/否"、右键菜单里的
    //     "复制/粘贴"，这些字不是我们写的，是 Qt 内置的。只装第一份的话，
    //     QML 界面变英文了，弹出来的文件对话框还是中文。
    //
    //     Qt 6 把翻译按模块拆开了，qtbase 管 QtCore/QtGui/QtWidgets 这一层，
    //     够覆盖所有原生控件。目录从 QLibraryInfo 问，不写死路径。
    auto *qtTranslator = new QTranslator(this);
    const QString qtTranslations =
        QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (qtTranslator->load(locale, QStringLiteral("qtbase"),
                           QStringLiteral("_"), qtTranslations)) {
        m_qtTranslator = qtTranslator;
        QCoreApplication::installTranslator(m_qtTranslator);
    } else {
        // 英文是 Qt 的源语言，qtbase_en.qm 是个空文件甚至不存在，载不到属于
        // 正常，不装它时显示的本来就是英文。
        delete qtTranslator;
    }
}
