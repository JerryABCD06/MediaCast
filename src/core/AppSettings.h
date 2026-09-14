// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

/**
 * AppSettings —— 用户设置。
 *
 * 存在**主程序旁边**的一个 JSON 文件里，程序自己生成、自己维护。没有就在
 * 第一次启动时按默认值写一份出来 —— 用户想改直接拿记事本改，不用翻界面。
 *
 * ── 键名就是文件里的路径 ──────────────────────────────────────────────────
 *
 *     文件里                     代码里
 *     { "ui": { "language": … } }   language()
 *
 * 用点分是**为了和用户对上**：他打开文件看到的就是 `ui.language`，
 * 我们文档里说的也是 `ui.language`，不用再翻译一遍。
 *
 *   ui.language               "System" / "zh_CN" / "en_US" …
 *   ui.darkmode               "System" / "Light" / "Dark"
 *   cast.newcast              要不要接收新的投送
 *   cast.broadcast            要不要定期对外广播（关了就只响应搜索）
 *   cast.broadcast_interval   广播间隔，毫秒
 *   picture.brightness        ┐
 *   picture.contrast          │ 画面调节（「显示效果」那十来项），
 *   …                         ┘ 键名 = 后端那张表里的短名
 *
 * ── 它是"唯一的那一份"，不是"一份副本" ────────────────────────────────────
 *
 * 这个类的每一个值变都会发信号。谁想跟着变就自己去连 —— 界面、DLNA、
 * 以后别的模块都一样。**别在别处再存一份**，那样迟早对不上。
 */
class AppSettings : public QObject
{
    Q_OBJECT

    // ── 给 QML 的属性 ────────────────────────────────────────────────────
    //
    // 设置页要能读、能改这五项，而 QML 读不了 C++ 的方法 —— 只能是属性。
    // 每一项的 READ/WRITE 就是下面那几个 getter/setter，NOTIFY 是它们本来就
    // 在发的信号，一个都没新增。
    //
    // 名字和文件里的键名一一对应（ui.language → uiLanguage …），这样哪天
    // 排查"界面显示的和文件里的对不上"，两边能直接对着看。
    Q_PROPERTY(QString uiLanguage READ language WRITE setLanguage
                   NOTIFY languageChanged)
    Q_PROPERTY(QString uiDarkMode READ darkMode WRITE setDarkMode
                   NOTIFY darkModeChanged)
    Q_PROPERTY(bool castNewCast READ acceptNewCast WRITE setAcceptNewCast
                   NOTIFY acceptNewCastChanged)
    Q_PROPERTY(bool castBroadcast READ broadcast WRITE setBroadcast
                   NOTIFY broadcastChanged)
    Q_PROPERTY(int castBroadcastInterval READ broadcastIntervalMs
                   WRITE setBroadcastIntervalMs NOTIFY broadcastIntervalChanged)
    Q_PROPERTY(bool uiMica READ mica WRITE setMica NOTIFY micaChanged)

public:
    explicit AppSettings(QObject *parent = nullptr);
    ~AppSettings() override;

    /** 设置文件在哪儿。 */
    QString filePath() const { return m_path; }

    /** 文件写不进去时它是 false —— 那种情况下用户改的设置不会保留。 */
    bool isPersistent() const { return m_persistent; }

    // ── 界面 ─────────────────────────────────────────────────────────────

    QString language() const;
    void setLanguage(const QString &value);

    QString darkMode() const;
    void setDarkMode(const QString &value);

    /**
     * 窗口底色要不要跟着桌面壁纸取色（云母）。
     *
     * 开了之后窗口底色跟着桌面壁纸走；关掉就是普通的纯色窗口（浅色 #F3F3F3）。
     *
     * **这份底色是我们自己画的**（QML 里的 MicaBackdrop：拿桌面壁纸模糊 + 染色），
     * 不依赖系统的云母，所以 Win10 一样有效。窗口上还留着的
     * `effect: "mica"` 管的是**窗口的边框和圆角**，不是底色。
     */
    bool mica() const;
    void setMica(bool value);

    // ── 投送 ─────────────────────────────────────────────────────────────

    bool acceptNewCast() const;
    void setAcceptNewCast(bool value);

    bool broadcast() const;
    void setBroadcast(bool value);

    int broadcastIntervalMs() const;
    void setBroadcastIntervalMs(int ms);

    // ── 画面调节（「显示效果」那十来项）──────────────────────────────────
    //
    // **这里存的只是"备忘"，权威始终在播放器手里。** 界面读的是播放器、不读
    // 这份文件；这份唯一的用处是**开机时把上次的值放回去**（`main()` 里
    // `player->start()` 之后那几行）。所以方向只有一条：播放器变了 → 写文件。
    //
    // 为什么不做成 Q_PROPERTY 给界面读：那等于给"同一样东西有两份状态"开口子 ——
    // 一份在 mpv 手里、一份在配置文件里，谁说了算迟早要吵。
    //
    // 存过的项才在文件里（改过哪几项就有哪几行）。删掉整段 `picture` 就等于
    // "全部回到没调过的样子"。

    /** 存下来的那些值（短名 → 整数）。没存过就是空的。 */
    QVariantMap pictureValues() const;

    /**
     * 记一项。
     *
     * **不立刻写盘**：拖一次滑块能发几十条变化，一条写一次盘太浪费。合并到
     * 800 毫秒之后写一次；万一用户拖完就退出，析构里会把没落盘的补上。
     */
    void setPictureValue(const QString &name, int value);

signals:
    void languageChanged(const QString &value);
    void darkModeChanged(const QString &value);
    void acceptNewCastChanged(bool value);
    void broadcastChanged(bool value);
    void broadcastIntervalChanged(int ms);
    void micaChanged(bool value);

    void logMessage(const QString &text);

private:
    /** 读文件。读不到就按默认值来（并顺手写一份出来）。 */
    void load();
    void save();

    /** 画面调节那几项攒着一起写盘用（见 setPictureValue）。 */
    QTimer m_pictureSaveTimer;

    /** 从嵌套的 JSON 里按点分路径取值，取不到就用 fallback。 */
    QString stringValue(const QString &path, const QString &fallback) const;
    bool    boolValue(const QString &path, bool fallback) const;
    int     intValue(const QString &path, int fallback) const;

    void setValue(const QString &path, const QJsonValue &value);

    QString m_path;
    bool    m_persistent = false;
    QJsonObject m_root;
};
