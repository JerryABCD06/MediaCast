// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#pragma once

#include <QObject>
#include <QVariantList>
#include <QVariantMap>

/**
 * 随程序分发的那些"法律文本"的入口 —— 界面要什么，它给什么，不碰界面。
 *
 * 两类东西，来自 exe 旁边两个目录：
 *
 *     legal/      我们自己那几份声明（隐私 / 法律 / 商标 / 编解码器）
 *     licenses/   第三方组件的许可全文，以及组件清单
 *
 * **为什么从文件读，而不是编进 exe（qrc）：**
 * 许可文本只应该存在**一份** —— 就是随包发出去的那份。编一份进 exe 就有了两个
 * 来源，迟早会出现"软件里显示的"和"实际分发的"不一致，而那正是合规上最忌讳的
 * 事。顺带还有两个好处：文件缺失时能明确报出来（构造时就检查并写日志，等于给
 * 发布包加了一道自检），以及许可文本永远保持纯文本、不需要 QML 去解析。
 *
 * **排序规则放在这里，不放界面：**
 * "本项目置顶、第三方按首字母"是一条规矩，不是排版偏好，所以它只有一处来源。
 * 界面拿到 self 就画在最上面，拿到 components 就直接按顺序排下去。
 */
class LegalDocs : public QObject
{
    Q_OBJECT

public:
    explicit LegalDocs(QObject *parent = nullptr);

    /**
     * 自检一遍发布内容：legal/ 和 licenses/ 下的文件齐不齐，然后写日志。
     *
     * **单独一个方法，不放在构造函数里** —— 日志是构造之后才接上的（和 Tr::scan()
     * 同一个道理），在构造函数里发就等于白喊。
     */
    void scan();

    /**
     * 我们自己的声明，已经按"对最终用户的相关度"排好：
     * 隐私在最上（那是用户真正会问的），然后是法律、商标、编解码器。
     *
     * 每一项：{ file, canonical, key }
     *   file      文件名（也是它的权威标识）
     *   canonical 英文名 —— **界面必须显示它**，本地化名只是别名
     *   key       语言文件里的键名；查不到就只显示 canonical，不加括号
     */
    Q_PROPERTY(QVariantList documents READ documents CONSTANT)

    /** 第三方组件，已按名称首字母排序。每一项：{ name, copyright, license, licenseFile, source } */
    Q_PROPERTY(QVariantList components READ components CONSTANT)

    /** 本项目自己那一条，界面上单独放在最上面并与下面留间隔 */
    Q_PROPERTY(QVariantMap project READ project CONSTANT)

    /** 读 legal/ 下的某一份（纯文本返回；读不到返回空串并记下原因） */
    Q_INVOKABLE QString readDocument(const QString &file);

    /** 读 licenses/ 下的某一份（许可全文） */
    Q_INVOKABLE QString readLicense(const QString &file);

    /** 最近一次读取失败的原因，供界面显示（成功时为空串） */
    Q_INVOKABLE QString lastError() const { return m_lastError; }

    // Q_PROPERTY 的 READ 要的就是这几个 —— 上面写了 READ xxx 却没声明，
    // moc 生成的代码里就找不到成员（编译期会报 C2039）。
    QVariantList documents() const { return m_documents; }
    QVariantList components() const { return m_components; }
    QVariantMap  project() const { return m_project; }

signals:
    void logMessage(const QString &text);

private:
    QVariantList m_documents;
    QVariantList m_components;
    QVariantMap m_project;
    QString m_lastError;

    QString readFromDir(const QString &subDir, const QString &file);
};
