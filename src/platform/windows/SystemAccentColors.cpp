// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "SystemAccentColors.h"

#include <QTimer>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <winrt/base.h>
#include <winrt/Windows.UI.ViewManagement.h>

namespace WView = winrt::Windows::UI::ViewManagement;

struct SystemAccentColors::Impl
{
    WView::UISettings settings{nullptr};
    bool firstRefresh = true;
};

namespace {

QColor fromWindowsColor(const winrt::Windows::UI::Color &color)
{
    return QColor(color.R, color.G, color.B, color.A);
}

} // namespace

SystemAccentColors::SystemAccentColors(QObject *parent)
    : QObject(parent)
    , m_impl(new Impl)
{
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
    } catch (winrt::hresult_error const &) {
        // Qt normally initialized STA already; continue so a real UISettings
        // failure is the one reported below.
    }

    try {
        m_impl->settings = WView::UISettings();
    } catch (winrt::hresult_error const &e) {
        emit logMessage(QStringLiteral("无法读取 Windows 系统主题色：%1")
                            .arg(QString::fromWCharArray(e.message().c_str())));
    }

    // ColorValuesChanged's projection is not linkable with the current SDK /
    // C++/WinRT combination. Seven GetColorValue calls every two seconds are
    // cheap, and refresh() emits nothing when the palette is unchanged.
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(2000);
    connect(m_pollTimer, &QTimer::timeout, this, [this] { refresh(); });
    m_pollTimer->start();
}

SystemAccentColors::~SystemAccentColors()
{
    delete m_impl;
    m_impl = nullptr;
}

void SystemAccentColors::refresh()
{
    if (!m_impl->settings)
        return;

    try {
        const QColor accent = fromWindowsColor(
            m_impl->settings.GetColorValue(WView::UIColorType::Accent));
        const QColor light1 = fromWindowsColor(
            m_impl->settings.GetColorValue(WView::UIColorType::AccentLight1));
        const QColor light2 = fromWindowsColor(
            m_impl->settings.GetColorValue(WView::UIColorType::AccentLight2));
        const QColor light3 = fromWindowsColor(
            m_impl->settings.GetColorValue(WView::UIColorType::AccentLight3));
        const QColor dark1 = fromWindowsColor(
            m_impl->settings.GetColorValue(WView::UIColorType::AccentDark1));
        const QColor dark2 = fromWindowsColor(
            m_impl->settings.GetColorValue(WView::UIColorType::AccentDark2));
        const QColor dark3 = fromWindowsColor(
            m_impl->settings.GetColorValue(WView::UIColorType::AccentDark3));

        const bool changed =
            accent != m_accent || light1 != m_light1 || light2 != m_light2
            || light3 != m_light3 || dark1 != m_dark1 || dark2 != m_dark2
            || dark3 != m_dark3;
        const bool first = m_impl->firstRefresh;
        m_impl->firstRefresh = false;

        m_accent = accent;
        m_light1 = light1;
        m_light2 = light2;
        m_light3 = light3;
        m_dark1 = dark1;
        m_dark2 = dark2;
        m_dark3 = dark3;
        if (first || changed)
            emit logMessage(QStringLiteral(
                "Windows 主题色：Accent %1，Light1 %2，Light2 %3，Light3 %4，"
                "Dark1 %5，Dark2 %6，Dark3 %7")
                .arg(accent.name(QColor::HexRgb),
                     light1.name(QColor::HexRgb),
                     light2.name(QColor::HexRgb),
                     light3.name(QColor::HexRgb),
                     dark1.name(QColor::HexRgb),
                     dark2.name(QColor::HexRgb),
                     dark3.name(QColor::HexRgb)));
        if (changed)
            emit colorsChanged();
    } catch (winrt::hresult_error const &e) {
        emit logMessage(QStringLiteral("读取 Windows 系统主题色失败：%1")
                            .arg(QString::fromWCharArray(e.message().c_str())));
    }
}
