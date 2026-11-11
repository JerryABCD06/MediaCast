// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "Tr.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLocale>
#include <QRegularExpression>

#include <algorithm>

namespace {

QString normalizeLocaleName(const QString &name)
{
    QString out = name;
    out.replace(QLatin1Char('-'), QLatin1Char('_'));
    return out;
}

bool readJson(const QString &path, QJsonObject *out, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("打不开（%1）").arg(file.errorString());
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        *error = QStringLiteral("不是合法的 JSON：%1（第 %2 个字符）")
                     .arg(parseError.errorString())
                     .arg(parseError.offset);
        return false;
    }
    if (!document.isObject()) {
        *error = QStringLiteral("最外层不是一个对象");
        return false;
    }
    *out = document.object();
    return true;
}

QHash<QString, QString> stringsFrom(const QJsonObject &root)
{
    QHash<QString, QString> out;
    const QJsonObject strings =
        root.value(QStringLiteral("strings")).toObject();
    for (auto it = strings.constBegin(); it != strings.constEnd(); ++it) {
        if (it.value().isString())
            out.insert(it.key(), it.value().toString());
    }
    return out;
}

QString readLocaleAliases(const QString &path, QStringList *aliases)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return file.errorString();
    while (!file.atEnd()) {
        QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        aliases->append(normalizeLocaleName(line));
    }
    return {};
}

} // namespace

Tr::Tr(QObject *parent)
    : QTranslator(parent)
{
    m_effective = baseCode();
    if (!loadBaseTable()) {
        log(QStringLiteral("内置英文语言表读取失败；界面可能显示键名"));
    }
    m_active = m_base;
}

Tr::~Tr()
{
    QCoreApplication::removeTranslator(this);
}

QString Tr::baseCode()
{
    return QStringLiteral("en_US");
}

bool Tr::loadBaseTable()
{
    const QString onDisk =
        langDir() + QStringLiteral("/en_US/ui.json");
    const QString embedded =
        QStringLiteral(":/lang/en_US/ui.json");

    QJsonObject root;
    QString error;
    if (readJson(onDisk, &root, &error)) {
        m_base = stringsFrom(root);
        return true;
    }
    if (readJson(embedded, &root, &error)) {
        m_base = stringsFrom(root);
        return true;
    }
    log(QStringLiteral("基准语言读不出来（%1）").arg(error));
    return false;
}

QString Tr::langDir() const
{
    return QCoreApplication::applicationDirPath()
        + QStringLiteral("/lang");
}

bool Tr::applyCatalog(const QByteArray &json, QString *error)
{
    QJsonParseError parseError{};
    const QJsonDocument document =
        QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError
        || !document.isObject()) {
        if (error)
            *error = parseError.errorString();
        return false;
    }

    const QJsonObject root = document.object();
    const QString base =
        root.value(QStringLiteral("baseLanguage")).toString();
    const QString resolved =
        root.value(QStringLiteral("resolvedLanguage")).toString();
    const QJsonArray packs =
        root.value(QStringLiteral("languages")).toArray();
    if (base != baseCode() || resolved.isEmpty() || packs.isEmpty()) {
        if (error)
            *error = QStringLiteral("语言目录字段不完整");
        return false;
    }

    QList<LanguageInfo> candidate;
    for (const QJsonValue &value : packs) {
        const QJsonObject object = value.toObject();
        LanguageInfo info;
        info.code = object.value(QStringLiteral("code")).toString();
        info.name = object.value(QStringLiteral("name")).toString().trimmed();
        info.uiFile =
            object.value(QStringLiteral("uiFile")).toString();
        if (info.code.isEmpty() || info.name.isEmpty()
            || info.uiFile.isEmpty()) {
            if (error)
                *error = QStringLiteral("语言目录里有不完整条目");
            return false;
        }
        candidate.append(info);
    }
    if (std::none_of(
            candidate.cbegin(), candidate.cend(),
            [](const LanguageInfo &info) {
                return info.code == QStringLiteral("en_US");
            })) {
        if (error)
            *error = QStringLiteral("Shell 语言目录缺少 en_US");
        return false;
    }
    std::sort(
        candidate.begin(), candidate.end(),
        [](const LanguageInfo &left, const LanguageInfo &right) {
            return left.code < right.code;
        });

    m_languages = std::move(candidate);
    log(QStringLiteral("Shell 语言目录：%1 个语言")
            .arg(m_languages.size()));
    QString resolveError;
    if (!setResolvedLanguage(resolved, &resolveError) && error)
        *error = resolveError;
    return true;
}

void Tr::scan()
{
    m_languages.clear();

    const QDir dir(langDir());
    if (!dir.exists()) {
        log(QStringLiteral("语言目录不存在（%1）—— 只有基准语言 %2 可用")
                .arg(dir.absolutePath(), baseCode()));
    } else {
        static const QRegularExpression validCode(
            QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
        const QStringList directories = dir.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QString &code : directories) {
            if (!validCode.match(code).hasMatch()) {
                log(QStringLiteral("跳过语言目录 %1：名称不合法").arg(code));
                continue;
            }
            const QDir packDir(dir.filePath(code));
            const QString manifestPath =
                packDir.filePath(QStringLiteral("manifest.json"));
            const QString uiPath =
                packDir.filePath(QStringLiteral("ui.json"));
            const QString shellPath =
                packDir.filePath(QStringLiteral("shell.json"));
            const QString aliasesPath =
                packDir.filePath(QStringLiteral("locale-aliases.txt"));
            if (!QFileInfo::exists(manifestPath)
                || !QFileInfo::exists(uiPath)
                || !QFileInfo::exists(shellPath)
                || !QFileInfo::exists(aliasesPath)) {
                log(QStringLiteral("跳过 %1：语言包文件不完整").arg(code));
                continue;
            }

            QJsonObject manifest;
            QString error;
            if (!readJson(manifestPath, &manifest, &error)) {
                log(QStringLiteral("跳过 %1：manifest.json：%2")
                        .arg(code, error));
                continue;
            }
            if (manifest.value(QStringLiteral("type")).toString()
                    != QStringLiteral("MediaCast.LanguagePack")
                || manifest.value(QStringLiteral("schemaVersion")).toInt()
                    != 1
                || manifest.value(QStringLiteral("language")).toString()
                    != code) {
                log(QStringLiteral("跳过 %1：manifest 标识不匹配").arg(code));
                continue;
            }
            const QString name =
                manifest.value(QStringLiteral("name")).toString().trimmed();
            QJsonObject unused;
            if (name.isEmpty() || !readJson(uiPath, &unused, &error)
                || !readJson(shellPath, &unused, &error)) {
                log(QStringLiteral("跳过 %1：语言包 JSON 无效").arg(code));
                continue;
            }

            LanguageInfo info;
            info.code = code;
            info.name = name;
            info.uiFile = uiPath;
            const QString aliasError =
                readLocaleAliases(aliasesPath, &info.systemNames);
            if (!aliasError.isEmpty()) {
                log(QStringLiteral("跳过 %1：locale-aliases.txt：%2")
                        .arg(code, aliasError));
                continue;
            }
            m_languages.append(info);
        }
    }

    std::sort(
        m_languages.begin(), m_languages.end(),
        [](const LanguageInfo &left, const LanguageInfo &right) {
            return left.code < right.code;
        });
    if (!find(baseCode(), nullptr)) {
        LanguageInfo base;
        base.code = baseCode();
        base.name = QStringLiteral("English");
        base.uiFile = QStringLiteral(":/lang/en_US/ui.json");
        m_languages.prepend(base);
    }

    QStringList names;
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
    const QStringList candidates = QLocale::system().uiLanguages();
    for (const QString &candidate : candidates) {
        const QString normalized = normalizeLocaleName(candidate);
        for (const LanguageInfo &info : m_languages) {
            for (const QString &systemName : info.systemNames) {
                if (systemName.compare(
                        normalized, Qt::CaseInsensitive) == 0)
                    return info.code;
            }
        }
    }
    return {};
}

void Tr::setLanguage(const QString &requested)
{
    m_requested = requested;
    QString code = requested;
    if (code.isEmpty()) {
        code = matchSystemLanguage();
        if (code.isEmpty()) {
            log(QStringLiteral("系统语言（%1）没有匹配语言包，改用 %2")
                    .arg(QLocale::system().uiLanguages().join(
                        QStringLiteral("、")), baseCode()));
        }
    } else if (!find(code, nullptr)) {
        log(QStringLiteral("设置里的语言「%1」不存在，改用 %2")
                .arg(code, baseCode()));
    }
    if (code.isEmpty() || !find(code, nullptr))
        code = baseCode();
    QString error;
    if (!setResolvedLanguage(code, &error) && !error.isEmpty())
        log(error);
}

void Tr::setRequestedLanguage(const QString &requested)
{
    m_requested = requested;
}

bool Tr::setResolvedLanguage(const QString &code, QString *error)
{
    const QString requested =
        code.isEmpty() ? baseCode() : code;
    if (requested == m_effective && !m_active.isEmpty())
        return true;
    if (!find(requested, nullptr)) {
        if (error)
            *error = QStringLiteral("语言包不存在：%1").arg(requested);
        loadActiveTable(baseCode());
        return false;
    }
    loadActiveTable(requested);
    if (m_effective != requested) {
        if (error)
            *error = QStringLiteral("无法读取语言包：%1").arg(requested);
        return false;
    }
    return true;
}

void Tr::loadActiveTable(const QString &code)
{
    m_active.clear();
    LanguageInfo info;
    if (!find(code, &info)) {
        m_active = m_base;
        m_effective = baseCode();
        return;
    }

    QJsonObject root;
    QString error;
    if (!readJson(info.uiFile, &root, &error)) {
        if (code == baseCode()
            && readJson(
                QStringLiteral(":/lang/en_US/ui.json"),
                &root, &error)) {
            m_active = stringsFrom(root);
            m_effective = baseCode();
            return;
        }
        log(QStringLiteral("读不到 %1（%2），先用 %3 顶上")
                .arg(info.uiFile, error, baseCode()));
        m_active = m_base;
        m_effective = baseCode();
        return;
    }

    m_active = stringsFrom(root);
    m_effective = code;
    reportTableDifferences();
    log(QStringLiteral("语言 -> %1（%2 条）")
            .arg(code).arg(m_active.size()));
}

void Tr::reportTableDifferences()
{
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

    const auto preview = [](const QStringList &list) {
        constexpr int kMaxShown = 8;
        if (list.size() <= kMaxShown)
            return list.join(QStringLiteral("、"));
        return list.mid(0, kMaxShown).join(QStringLiteral("、"))
            + QStringLiteral(" …（共 %1 条）").arg(list.size());
    };

    if (!missing.isEmpty()) {
        log(QStringLiteral("有 %1 条没翻（会用英文）：%2")
                .arg(missing.size()).arg(preview(missing)));
    }
    if (!extra.isEmpty()) {
        log(QStringLiteral("有 %1 条是英文基准里没有的键（已忽略）：%2")
                .arg(extra.size()).arg(preview(extra)));
    }
}

QString Tr::translate(
    const char *context, const char *sourceText,
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
    return QString();
}

void Tr::log(const QString &text)
{
    emit logMessage(text);
}
