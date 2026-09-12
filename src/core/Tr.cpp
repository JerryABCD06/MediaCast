#include "Tr.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>

namespace {

/**
 * 中划线换成下划线。
 *
 * 同一个语言有两种写法：Windows 和 Qt 说 `zh-Hans-CN`，我们文件里写
 * `zh_Hans_CN`。对的时候一律换成下划线，省得填 meta 的人还得记住该写哪种。
 */
QString normalizeLocaleName(const QString &name)
{
    QString out = name;
    out.replace(QLatin1Char('-'), QLatin1Char('_'));
    return out;
}

/** 读一个 JSON 对象出来。读不动就填 error 并返回 false。 */
bool readJson(const QString &path, QJsonObject *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("打不开（%1）").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("不是合法的 JSON：%1（第 %2 个字符）")
                     .arg(parseError.errorString())
                     .arg(parseError.offset);
        return false;
    }
    if (!doc.isObject()) {
        *error = QStringLiteral("最外层不是一个对象");
        return false;
    }

    *out = doc.object();
    return true;
}

/** 从 {"strings": {...}} 里把那堆键值抠出来。 */
QHash<QString, QString> stringsFrom(const QJsonObject &root)
{
    QHash<QString, QString> out;
    const QJsonObject strings = root.value(QStringLiteral("strings")).toObject();
    for (auto it = strings.constBegin(); it != strings.constEnd(); ++it) {
        if (it.value().isString())
            out.insert(it.key(), it.value().toString());
    }
    return out;
}

} // namespace

Tr::Tr(QObject *parent)
    : QTranslator(parent)
{
    // 基准语言先装上 —— 后面不管怎么折腾，兜底的那份总是在手上。
    //
    // 磁盘上有就用磁盘上那份（改英文的错别字不用重新编译），没有就用编进
    // exe 里的那份。这一条是"整个 lang 目录被删了也不会满屏键名"的保证。
    const QString onDisk = langDir() + QStringLiteral("/") + baseCode() + QStringLiteral(".json");
    const QString embedded = QStringLiteral(":/lang/") + baseCode() + QStringLiteral(".json");

    QJsonObject root;
    QString error;
    if (readJson(onDisk, &root, &error)) {
        m_base = stringsFrom(root);
    } else if (readJson(embedded, &root, &error)) {
        m_base = stringsFrom(root);
    } else {
        log(QStringLiteral("基准语言读不出来（%1）—— 界面上的文字会显示成键名")
                .arg(error));
    }
}

Tr::~Tr()
{
    // QCoreApplication 存的是裸指针，不摘的话它那儿留着一个已经没了的对象。
    QCoreApplication::removeTranslator(this);
}

QString Tr::baseCode()
{
    return QStringLiteral("en_US");
}

QString Tr::langDir() const
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/lang");
}

void Tr::log(const QString &text)
{
    emit logMessage(text);
}

void Tr::scan()
{
    m_languages.clear();

    const QDir dir(langDir());
    if (!dir.exists()) {
        log(QStringLiteral("语言目录不存在（%1）—— 只有基准语言 %2 可用")
                .arg(dir.absolutePath(), baseCode()));
        return;
    }

    // 按文件名排序，列表的顺序就是稳的。
    const QStringList files =
        dir.entryList({ QStringLiteral("*.json") }, QDir::Files, QDir::Name);

    // 文件名只允许英文字母和下划线 —— 这是唯一的格式要求。
    //
    // 为什么卡这一条：文件名会被拿来当语言代码用（写进设置文件、和系统语言
    // 比对），里头混进空格、点、中文的时候，出问题的往往是别的地方而不是这里。
    static const QRegularExpression validName(QStringLiteral("^[A-Za-z_]+$"));

    for (const QString &fileName : files) {
        const QString code = fileName.chopped(5);   // 去掉 ".json"
        if (!validName.match(code).hasMatch()) {
            log(QStringLiteral("跳过 %1：文件名只能有英文字母和下划线").arg(fileName));
            continue;
        }

        QJsonObject root;
        QString error;
        if (!readJson(dir.filePath(fileName), &root, &error)) {
            log(QStringLiteral("跳过 %1：%2").arg(fileName, error));
            continue;
        }

        const QJsonObject meta = root.value(QStringLiteral("meta")).toObject();
        const QString name = meta.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            log(QStringLiteral("跳过 %1：meta.name 没写").arg(fileName));
            continue;
        }

        LanguageInfo info;
        info.code = code;
        info.name = name;
        const QJsonArray systemNames = meta.value(QStringLiteral("system")).toArray();
        for (const QJsonValue &value : systemNames) {
            if (value.isString())
                info.systemNames.append(normalizeLocaleName(value.toString()));
        }

        m_languages.append(info);
    }

    std::sort(m_languages.begin(), m_languages.end(),
              [](const LanguageInfo &a, const LanguageInfo &b) { return a.code < b.code; });

    // 基准语言永远在列表里 —— 哪怕它的文件被删了，它也一直是能用的那个，
    // 列表里少了它反而说不通。
    LanguageInfo base;
    if (!find(baseCode(), &base)) {
        base.code = baseCode();
        base.name = QStringLiteral("English");
        m_languages.prepend(base);
    }

    QStringList names;
    names.reserve(m_languages.size());
    for (const LanguageInfo &info : m_languages)
        names.append(info.code);

    log(QStringLiteral("语言目录：找到 %1 个语言（%2）")
            .arg(m_languages.size())
            .arg(names.join(QStringLiteral("、"))));
}

bool Tr::find(const QString &code, LanguageInfo *out) const
{
    for (const LanguageInfo &info : m_languages) {
        if (info.code == code) {
            if (out)
                *out = info;
            return true;
        }
    }
    return false;
}

QVariantList Tr::languages() const
{
    QVariantList out;
    out.reserve(m_languages.size());
    for (const LanguageInfo &info : m_languages) {
        QVariantMap item;
        item.insert(QStringLiteral("code"), info.code);
        item.insert(QStringLiteral("name"), info.name);
        out.append(item);
    }
    return out;
}

QString Tr::matchSystemLanguage() const
{
    // 系统给的是一个**按用户偏好排过序**的候选清单，比如中文机器上是
    //   zh-Hans-CN | zh-CN | zh-Hans | zh | en-Latn-US | en-US | ...
    //
    // 整串都要看，不能只看第一个：第一个是 "zh-Hans-CN"，而文件里多半写的是
    // "zh_CN" —— 那是清单里的第二个。顺序本身有意义（用户把中文排第一），
    // 所以按清单顺序找，谁先对上就用谁。
    const QStringList candidates = QLocale::system().uiLanguages();

    for (const QString &candidate : candidates) {
        const QString normalized = normalizeLocaleName(candidate);
        for (const LanguageInfo &info : m_languages) {
            for (const QString &systemName : info.systemNames) {
                if (systemName.compare(normalized, Qt::CaseInsensitive) == 0)
                    return info.code;
            }
        }
    }

    return QString();
}

void Tr::setLanguage(const QString &requested)
{
    m_requested = requested;

    QString code = requested;

    if (code.isEmpty()) {
        // 跟随系统。对不上就用基准 —— 这时候说一声，因为用户多半会奇怪
        // "我系统明明是中文"。原因通常是 meta.system 里没写他那个写法。
        code = matchSystemLanguage();
        if (code.isEmpty()) {
            log(QStringLiteral("系统语言（%1）在语言目录里没有对应的文件，改用 %2")
                    .arg(QLocale::system().uiLanguages().join(QStringLiteral("、")),
                         baseCode()));
        }
    } else if (!find(code, nullptr)) {
        // 设置文件里选的那个语言，文件没了（被删了、改名了）。
        log(QStringLiteral("设置里选的语言「%1」在语言目录里找不到，改用 %2")
                .arg(code, baseCode()));
    }

    if (code.isEmpty() || !find(code, nullptr))
        code = baseCode();

    m_effective = code;
    loadActiveTable(code);
}

void Tr::loadActiveTable(const QString &code)
{
    m_active.clear();

    // 基准语言就是基准语言，不用再读一遍文件 —— 常驻的那份就是它。
    if (code == baseCode()) {
        m_active = m_base;
        log(QStringLiteral("语言 -> %1（基准）").arg(code));
        return;
    }

    QJsonObject root;
    QString error;
    const QString path = langDir() + QStringLiteral("/") + code + QStringLiteral(".json");
    if (!readJson(path, &root, &error)) {
        // 刚才扫的时候还在，现在读不到 —— 文件被删了或者改坏了。
        // 不把界面弄成空的：落回基准，如实说一声。
        log(QStringLiteral("读不到 %1（%2），先用 %3 顶上")
                .arg(code + QStringLiteral(".json"), error, baseCode()));
        m_active = m_base;
        m_effective = baseCode();
        return;
    }

    m_active = stringsFrom(root);
    reportTableDifferences();
    log(QStringLiteral("语言 -> %1（%2 条）").arg(code).arg(m_active.size()));
}

void Tr::reportTableDifferences()
{
    // 缺的键：会落回英文，界面不会空着，但多半不是想要的结果。
    // 多的键：拼错了，或者语言文件比基准旧 —— 两种都值得看一眼。
    //
    // 只在这儿报一次，不在 translate() 里报：那边是每查一句都要走的路径，
    // 而且同一个键会因为界面重画被反复查到，日志会被刷爆。
    QStringList missing;
    for (auto it = m_base.constBegin(); it != m_base.constEnd(); ++it) {
        if (!m_active.contains(it.key()))
            missing.append(it.key());
    }

    QStringList extra;
    for (auto it = m_active.constBegin(); it != m_active.constEnd(); ++it) {
        if (!m_base.contains(it.key()))
            extra.append(it.key());
    }

    // 列表可能很长，日志里只摆前几条。
    const auto preview = [](const QStringList &list) {
        constexpr int kMaxShown = 8;
        if (list.size() <= kMaxShown)
            return list.join(QStringLiteral("、"));
        return list.mid(0, kMaxShown).join(QStringLiteral("、"))
             + QStringLiteral(" …（共 %1 条）").arg(list.size());
    };

    if (!missing.isEmpty())
        log(QStringLiteral("有 %1 条没翻（会用英文）：%2").arg(missing.size()).arg(preview(missing)));
    if (!extra.isEmpty())
        log(QStringLiteral("有 %1 条是英文基准里没有的键（已忽略）：%2")
                .arg(extra.size())
                .arg(preview(extra)));
}

QString Tr::translate(const char *context, const char *sourceText,
                      const char *disambiguation, int n) const
{
    Q_UNUSED(context);
    Q_UNUSED(disambiguation);
    Q_UNUSED(n);

    if (!sourceText || !*sourceText)
        return QString();

    const QString key = QString::fromUtf8(sourceText);

    const auto active = m_active.constFind(key);
    if (active != m_active.constEnd())
        return active.value();

    const auto base = m_base.constFind(key);
    if (base != m_base.constEnd())
        return base.value();

    // 两边都没有就返回空串，让 Qt 继续往下走（别的翻译器、最后是源码原文）。
    //
    // 这里**不能**返回 key 本身：旧界面那些还没搬过来的中文（`tr("退出")` 之类）
    // 就是靠"查不到 -> 显示源码原文"这条路保持原样的。迁移期间这两类字符串
    // 混在一起，全靠这个返回值区分。
    return QString();
}
