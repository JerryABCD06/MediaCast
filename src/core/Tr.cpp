// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "Tr.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>

namespace {

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

void mergeStringTable(
    const QString &path, QHash<QString, QString> &table)
{
    QJsonObject root;
    QString error;
    if (!readJson(path, &root, &error))
        return;
    const QHash<QString, QString> extra = stringsFrom(root);
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        table.insert(it.key(), it.value());
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
        mergeStringTable(
            langDir() + QStringLiteral("/en_US/media.json"), m_base);
        return true;
    }
    if (readJson(embedded, &root, &error)) {
        m_base = stringsFrom(root);
        mergeStringTable(
            QStringLiteral(":/lang/en_US/media.json"), m_base);
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
            mergeStringTable(
                QStringLiteral(":/lang/en_US/media.json"), m_active);
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
    const QFileInfo activeInfo(info.uiFile);
    mergeStringTable(
        activeInfo.absolutePath() + QStringLiteral("/media.json"),
        m_active);
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
