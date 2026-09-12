#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

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

public:
    explicit AppSettings(QObject *parent = nullptr);

    /** 设置文件在哪儿。 */
    QString filePath() const { return m_path; }

    /** 文件写不进去时它是 false —— 那种情况下用户改的设置不会保留。 */
    bool isPersistent() const { return m_persistent; }

    // ── 界面 ─────────────────────────────────────────────────────────────

    QString language() const;
    void setLanguage(const QString &value);

    QString darkMode() const;
    void setDarkMode(const QString &value);

    // ── 投送 ─────────────────────────────────────────────────────────────

    bool acceptNewCast() const;
    void setAcceptNewCast(bool value);

    bool broadcast() const;
    void setBroadcast(bool value);

    int broadcastIntervalMs() const;
    void setBroadcastIntervalMs(int ms);

signals:
    void languageChanged(const QString &value);
    void darkModeChanged(const QString &value);
    void acceptNewCastChanged(bool value);
    void broadcastChanged(bool value);
    void broadcastIntervalChanged(int ms);

    void logMessage(const QString &text);

private:
    /** 读文件。读不到就按默认值来（并顺手写一份出来）。 */
    void load();
    void save();

    /** 从嵌套的 JSON 里按点分路径取值，取不到就用 fallback。 */
    QString stringValue(const QString &path, const QString &fallback) const;
    bool    boolValue(const QString &path, bool fallback) const;
    int     intValue(const QString &path, int fallback) const;

    void setValue(const QString &path, const QJsonValue &value);

    QString m_path;
    bool    m_persistent = false;
    QJsonObject m_root;
};
