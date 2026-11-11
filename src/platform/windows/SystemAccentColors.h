// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include "AccentPalette.h"

#include <QColor>
#include <QObject>

class QTimer;

/**
 * Windows 当前系统强调色的完整七级色阶。
 *
 * 数值直接来自 Windows.UI.ViewManagement.UISettings.GetColorValue()，也就是
 * Windows shell 自己生成的那一套；不是我们自己按亮度猜的。系统主题色变化时
 * 通过 ColorValuesChanged 事件刷新。
 */
class SystemAccentColors : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QColor accent READ accent NOTIFY colorsChanged)
    Q_PROPERTY(QColor accentLight1 READ accentLight1 NOTIFY colorsChanged)
    Q_PROPERTY(QColor accentLight2 READ accentLight2 NOTIFY colorsChanged)
    Q_PROPERTY(QColor accentLight3 READ accentLight3 NOTIFY colorsChanged)
    Q_PROPERTY(QColor accentDark1 READ accentDark1 NOTIFY colorsChanged)
    Q_PROPERTY(QColor accentDark2 READ accentDark2 NOTIFY colorsChanged)
    Q_PROPERTY(QColor accentDark3 READ accentDark3 NOTIFY colorsChanged)

public:
    explicit SystemAccentColors(QObject *parent = nullptr);
    ~SystemAccentColors() override;

    /** 立刻重新读取 Windows 当前色阶。 */
    void refresh();

    QColor accent() const { return m_accent; }
    QColor accentLight1() const { return m_light1; }
    QColor accentLight2() const { return m_light2; }
    QColor accentLight3() const { return m_light3; }
    QColor accentDark1() const { return m_dark1; }
    QColor accentDark2() const { return m_dark2; }
    QColor accentDark3() const { return m_dark3; }

signals:
    void colorsChanged();
    void logMessage(const QString &text);

private:
    struct Impl;
    Impl *m_impl = nullptr;
    QTimer *m_pollTimer = nullptr;

    QColor m_accent{0x00, 0x78, 0xd4};
    QColor m_light1{0x26, 0x8c, 0xdc};
    QColor m_light2{mcast::platform::WindowsBlueAccent.dark};
    QColor m_light3{0x60, 0xab, 0xe4};
    QColor m_dark1{mcast::platform::WindowsBlueAccent.light};
    QColor m_dark2{0x00, 0x54, 0x94};
    QColor m_dark3{0x00, 0x4a, 0x83};
};
