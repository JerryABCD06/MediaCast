#include "AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonParseError>

namespace {

constexpr const char *kUiLanguage        = "ui.language";
constexpr const char *kUiDarkMode        = "ui.darkmode";
constexpr const char *kCastNewCast       = "cast.newcast";
constexpr const char *kCastBroadcast     = "cast.broadcast";
constexpr const char *kCastInterval      = "cast.broadcast_interval";

// 默认值只有这一处。load() 生成的那份文件、和读不到时的兜底，用的都是它。
constexpr const char *kDefaultLanguage   = "System";
constexpr const char *kDefaultDarkMode   = "System";
constexpr bool        kDefaultNewCast    = true;
constexpr bool        kDefaultBroadcast  = true;
constexpr int         kDefaultIntervalMs = 10000;

} // namespace

AppSettings::AppSettings(QObject *parent)
    : QObject(parent)
{
    // **放在主程序同级目录。**
    //
    // 注意：装到 C:\Program Files\ 那种地方的话这儿是写不进去的。那时
    // isPersistent() 会是 false，用户在界面上改的东西不会保留 —— 但程序照常跑。
    // 真到要打包安装的那一步再考虑退回 %LOCALAPPDATA%。
    m_path = QCoreApplication::applicationDirPath() + QStringLiteral("/MediaCast.json");

    load();
}

// ── 读 / 写 ──────────────────────────────────────────────────────────────

void AppSettings::load()
{
    QFile file(m_path);

    if (file.exists() && file.open(QIODevice::ReadOnly)) {
        QJsonParseError error{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
        file.close();

        if (error.error == QJsonParseError::NoError && doc.isObject()) {
            m_root = doc.object();
            m_persistent = true;
            return;
        }

        // 文件在，但读不动（改坏了、或者被别的程序占着）。
        // **不覆盖它** —— 里面可能有用户手改的东西，毁掉就找不回来了。
        emit logMessage(QStringLiteral("设置文件读不动（%1），这次先用默认值：%2")
                            .arg(m_path, error.errorString()));
    }

    // 没有文件（或者刚才读坏了）：按默认值建一份出来。
    //
    // 注意要**把默认值一条条写进去**，不是存一个空对象 —— 空文件用户打开
    // 什么都看不到，也就没法照着改。这份文件的一个用处就是"给用户看有哪些
    // 项可以改"。
    QJsonObject ui;
    ui.insert(QStringLiteral("language"), QString::fromLatin1(kDefaultLanguage));
    ui.insert(QStringLiteral("darkmode"), QString::fromLatin1(kDefaultDarkMode));

    QJsonObject cast;
    cast.insert(QStringLiteral("newcast"), kDefaultNewCast);
    cast.insert(QStringLiteral("broadcast"), kDefaultBroadcast);
    cast.insert(QStringLiteral("broadcast_interval"), kDefaultIntervalMs);

    m_root = QJsonObject{ { QStringLiteral("ui"), ui }, { QStringLiteral("cast"), cast } };
    save();
}

void AppSettings::save()
{
    QFile file(m_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (m_persistent) {
            // 之前能写、现在不能了 —— 说一声，别默默丢设置。
            emit logMessage(QStringLiteral("设置存不回去（%1）：%2")
                                .arg(m_path, file.errorString()));
        }
        m_persistent = false;
        return;
    }

    file.write(QJsonDocument(m_root).toJson(QJsonDocument::Indented));
    file.close();
    m_persistent = true;
}

// ── 嵌套 JSON 的取值 / 存值 ───────────────────────────────────────────────

QString AppSettings::stringValue(const QString &path, const QString &fallback) const
{
    const QStringList parts = path.split(QLatin1Char('.'));
    QJsonValue node = m_root;
    for (const QString &part : parts) {
        if (!node.isObject())
            return fallback;
        node = node.toObject().value(part);
    }
    return node.isString() ? node.toString() : fallback;
}

bool AppSettings::boolValue(const QString &path, bool fallback) const
{
    const QStringList parts = path.split(QLatin1Char('.'));
    QJsonValue node = m_root;
    for (const QString &part : parts) {
        if (!node.isObject())
            return fallback;
        node = node.toObject().value(part);
    }
    return node.isBool() ? node.toBool() : fallback;
}

int AppSettings::intValue(const QString &path, int fallback) const
{
    const QStringList parts = path.split(QLatin1Char('.'));
    QJsonValue node = m_root;
    for (const QString &part : parts) {
        if (!node.isObject())
            return fallback;
        node = node.toObject().value(part);
    }
    return node.isDouble() ? node.toInt() : fallback;
}

namespace {

/**
 * 往嵌套的 JSON 对象里按路径写一个值，返回改完的那一份。
 *
 * 为什么是"返回一份新的"而不是"改原地"：QJsonObject 是**值语义** —— 从
 * `value("ui")` 拿出来的是个副本，改它不会影响原对象。所以只能一路重新装配
 * 回去，最里面那一层改完，一层层往外套。写成递归比手写循环难出错。
 */
QJsonObject insertAt(QJsonObject object, const QStringList &parts, int index,
                     const QJsonValue &value)
{
    if (index == parts.size() - 1) {
        object.insert(parts.at(index), value);
        return object;
    }

    const QJsonObject child = object.value(parts.at(index)).toObject();
    object.insert(parts.at(index), insertAt(child, parts, index + 1, value));
    return object;
}

} // namespace

void AppSettings::setValue(const QString &path, const QJsonValue &value)
{
    const QStringList parts = path.split(QLatin1Char('.'));
    if (parts.isEmpty())
        return;

    m_root = insertAt(m_root, parts, 0, value);
    save();
}

// ── 界面 ─────────────────────────────────────────────────────────────────

QString AppSettings::language() const
{
    return stringValue(kUiLanguage, QString::fromLatin1(kDefaultLanguage));
}

void AppSettings::setLanguage(const QString &value)
{
    if (language() == value)
        return;

    setValue(QString::fromLatin1(kUiLanguage), value);
    emit languageChanged(value);
}

QString AppSettings::darkMode() const
{
    return stringValue(kUiDarkMode, QString::fromLatin1(kDefaultDarkMode));
}

void AppSettings::setDarkMode(const QString &value)
{
    if (darkMode() == value)
        return;

    setValue(QString::fromLatin1(kUiDarkMode), value);
    emit darkModeChanged(value);
}

// ── 投送 ─────────────────────────────────────────────────────────────────

bool AppSettings::acceptNewCast() const
{
    return boolValue(kCastNewCast, kDefaultNewCast);
}

void AppSettings::setAcceptNewCast(bool value)
{
    if (acceptNewCast() == value)
        return;

    setValue(QString::fromLatin1(kCastNewCast), value);
    emit acceptNewCastChanged(value);
}

bool AppSettings::broadcast() const
{
    return boolValue(kCastBroadcast, kDefaultBroadcast);
}

void AppSettings::setBroadcast(bool value)
{
    if (broadcast() == value)
        return;

    setValue(QString::fromLatin1(kCastBroadcast), value);
    emit broadcastChanged(value);
}

int AppSettings::broadcastIntervalMs() const
{
    return intValue(kCastInterval, kDefaultIntervalMs);
}

void AppSettings::setBroadcastIntervalMs(int ms)
{
    if (ms <= 0 || broadcastIntervalMs() == ms)
        return;

    setValue(QString::fromLatin1(kCastInterval), ms);
    emit broadcastIntervalChanged(ms);
}
