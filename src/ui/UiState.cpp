// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "UiState.h"

#include "core/AppSettings.h"
#include "core/Tr.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QStyleHints>
#include <QTranslator>

namespace {

// 设置文件里存的是"System"/"Light"/"Dark"这三个词 —— 那是给人看的，
// 也是用户拿记事本打开会看到的东西。内部用的是枚举，两边在这儿翻。

int themeModeFromName(const QString &name)
{
    if (name == QLatin1String("Light"))
        return UiState::Light;
    if (name == QLatin1String("Dark"))
        return UiState::Dark;
    return UiState::System;
}

QString themeModeName(int mode)
{
    switch (mode) {
    case UiState::Light: return QStringLiteral("Light");
    case UiState::Dark:  return QStringLiteral("Dark");
    default:             return QStringLiteral("System");
    }
}

} // namespace

UiState::UiState(AppSettings *settings, Tr *tr, QObject *parent)
    : QObject(parent)
    , m_settings(settings)
    , m_tr(tr)
{
    // 先把设置文件里的值读进来 —— 它们是"上次关掉时的样子"。
    if (m_settings) {
        m_themeMode = themeModeFromName(m_settings->darkMode());
        m_language = m_settings->language();
        if (m_language == QLatin1String("System"))
            m_language.clear();   // 空串 = 跟随系统，内部就是这么表示的
    }

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

    // 用户在外面（或者别的模块）改了设置文件里的值 —— 也跟着变。
    // 两个方向都走同一条路：setThemeMode/setLanguage 会写回设置，写入又是
    // 幂等的（值一样就不动），所以不会来回弹。
    if (m_settings) {
        connect(m_settings, &AppSettings::darkModeChanged, this,
                [this](const QString &name) { setThemeMode(themeModeFromName(name)); });
        connect(m_settings, &AppSettings::languageChanged, this,
                [this](const QString &code) {
            setLanguage(code == QLatin1String("System") ? QString() : code);
        });
    }
}

UiState::~UiState()
{
    // 析构前把翻译器摘下来。installTranslator 存的是裸指针，不摘的话
    // 对象没了它还留着 —— 退出那一刻正好用到翻译就是野指针。
    //
    // 注意 Tr **只摘不删** —— 它是 main() 那边造的，比这里活得久。
    if (m_tr)
        QCoreApplication::removeTranslator(m_tr);

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

void UiState::setThemeMode(int mode)
{
    if (mode < System || mode > Dark || m_themeMode == mode)
        return;

    m_themeMode = mode;
    applyTheme();
    emit themeModeChanged();

    // 写回设置文件 —— 下次启动还是这个样子。
    if (m_settings)
        m_settings->setDarkMode(themeModeName(mode));
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

    // 同上。空串在内部表示"跟随系统"，写进文件时换成给人看的 System。
    if (m_settings)
        m_settings->setLanguage(code.isEmpty() ? QStringLiteral("System") : code);
}

void UiState::applyLanguage()
{
    // 换语言 = 先把旧的摘干净，再装新的。不先摘就装，两个翻译器同时挂着，
    // 查表按安装顺序来，结果会变成"一半新语言一半旧语言"。
    if (m_tr)
        QCoreApplication::removeTranslator(m_tr);

    if (m_qtTranslator) {
        QCoreApplication::removeTranslator(m_qtTranslator);
        delete m_qtTranslator;
        m_qtTranslator = nullptr;
    }

    // 一、我们自己的字符串，也就是界面和托盘上那些。
    //
    //     Tr 本身就是个 QTranslator，查的是 lang/*.json。先让它把表换成
    //     当前语言，再装上 —— 顺序反了的话，装上去的会是上一张表。
    //
    //     装成"空表"也要装：查不到的键它返回空串，Qt 会继续往下走到源码原文。
    //     旧界面那些还没搬过来的中文就是靠这条路保持原样的。
    if (m_tr) {
        m_tr->setLanguage(m_language);
        QCoreApplication::installTranslator(m_tr);
    }

    // 该用哪个 locale 由 Tr 说了算：语言文件在不在、系统语言跟哪个对得上，
    // 只有它知道。它给的代码一定是有效的（最坏是基准语言）。
    const QLocale locale(m_tr ? m_tr->effectiveLanguage() : QStringLiteral("en_US"));

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
