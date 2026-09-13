// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "LegalDocs.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTextStream>

namespace {

QVariantMap doc(const QString &file, const QString &canonical, const QString &key)
{
    return QVariantMap{
        { QStringLiteral("file"), file },
        { QStringLiteral("canonical"), canonical },
        { QStringLiteral("key"), key },
    };
}

QVariantMap component(const QString &name, const QString &copyright, const QString &license,
                      const QString &licenseFile, const QString &source)
{
    return QVariantMap{
        { QStringLiteral("name"), name },
        { QStringLiteral("copyright"), copyright },
        { QStringLiteral("license"), license },
        { QStringLiteral("licenseFile"), licenseFile },
        { QStringLiteral("source"), source },
    };
}

} // namespace

LegalDocs::LegalDocs(QObject *parent)
    : QObject(parent)
{
    // ── 我们自己的声明 ────────────────────────────────────────────────────
    //
    // 顺序是"对最终用户的相关度"，不是字母序 —— 用户第一个想问的永远是
    // "这软件联网吗、留下什么"，所以隐私排最上。
    m_documents = QVariantList{
        doc(QStringLiteral("PRIVACY.md"), QStringLiteral("Privacy Notice"),
            QStringLiteral("ui_legal_doc_privacy")),
        doc(QStringLiteral("LEGAL-NOTICES.md"), QStringLiteral("Legal Notices"),
            QStringLiteral("ui_legal_doc_notices")),
        doc(QStringLiteral("TRADEMARKS.md"), QStringLiteral("Trademark Notice"),
            QStringLiteral("ui_legal_doc_trademarks")),
        doc(QStringLiteral("CODECS.md"), QStringLiteral("Codec and Patent Notice"),
            QStringLiteral("ui_legal_doc_codecs")),
    };

    // ── 本项目自己那一条 ──────────────────────────────────────────────────
    m_project = component(QStringLiteral("Media Cast Receiver"),
                          QStringLiteral("Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>"),
                          QStringLiteral("GPL-3.0-or-later"),
                          QStringLiteral("GPL-3.0.txt"),
                          QStringLiteral("https://github.com/"));
    m_project.insert(QStringLiteral("isSelf"), true);

    // ── 第三方组件（首字母序）────────────────────────────────────────────
    //
    // 这份表要和 licenses/THIRD-PARTY-NOTICES.md 一致 —— 那边是权威版本，
    // 这儿只是它的结构化副本。加依赖时两处一起改。
    m_components = QVariantList{
        component(QStringLiteral("FluentUI"),
                  QStringLiteral("Copyright (c) 2023 zhuzichu"),
                  QStringLiteral("MIT"),
                  QStringLiteral("MIT-FluentUI.txt"),
                  QStringLiteral("https://github.com/zhuzichu520/FluentUI")),
        component(QStringLiteral("libmpv / FFmpeg"),
                  QStringLiteral("mpv 项目与 FFmpeg 项目及各位贡献者"),
                  QStringLiteral("GPL-2.0-or-later"),
                  QStringLiteral("GPL-2.0.txt"),
                  QStringLiteral("https://github.com/mpv-player/mpv")),
        component(QStringLiteral("Qt 6"),
                  QStringLiteral("The Qt Company Ltd. and contributors"),
                  QStringLiteral("LGPL-3.0"),
                  QStringLiteral("LGPL-3.0.txt"),
                  QStringLiteral("https://download.qt.io/official_releases/qt/6.11/6.11.2/")),
    };

}

void LegalDocs::scan()
{
    // 发布包缺了许可文本是**合规问题**，不是显示问题。所以别等用户点开才发现 ——
    // 启动就查一遍，缺了直接在日志里点名。
    QStringList missing;
    const QDir base(QCoreApplication::applicationDirPath());
    for (const QVariant &v : m_documents) {
        const QString f = v.toMap().value(QStringLiteral("file")).toString();
        if (!base.exists(QStringLiteral("legal/") + f))
            missing << QStringLiteral("legal/") + f;
    }
    for (const QVariant &v : m_components) {
        const QString f = v.toMap().value(QStringLiteral("licenseFile")).toString();
        if (!base.exists(QStringLiteral("licenses/") + f))
            missing << QStringLiteral("licenses/") + f;
    }

    emit logMessage(QStringLiteral("法律文本：%1 份声明、%2 个第三方组件")
                        .arg(m_documents.size())
                        .arg(m_components.size()));
    if (!missing.isEmpty()) {
        emit logMessage(QStringLiteral("**许可文件缺失 %1 个**（发布包不完整）：%2")
                            .arg(missing.size())
                            .arg(missing.join(QStringLiteral("、"))));
    }
}

QString LegalDocs::readDocument(const QString &file)
{
    return readFromDir(QStringLiteral("legal"), file);
}

QString LegalDocs::readLicense(const QString &file)
{
    return readFromDir(QStringLiteral("licenses"), file);
}

QString LegalDocs::readFromDir(const QString &subDir, const QString &file)
{
    m_lastError.clear();

    // 文件名来自界面，别放任它往上跳目录。
    if (file.contains('/') || file.contains('\\') || file.contains(QStringLiteral(".."))) {
        m_lastError = QStringLiteral("文件名不合法：%1").arg(file);
        return QString();
    }

    const QString path = QCoreApplication::applicationDirPath()
                         + QLatin1Char('/') + subDir + QLatin1Char('/') + file;

    QFile f(path);
    if (!f.exists()) {
        m_lastError = QStringLiteral("文件不存在：%1").arg(path);
        return QString();
    }
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_lastError = QStringLiteral("打不开：%1").arg(path);
        return QString();
    }

    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    return in.readAll();
}
