#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTranslator>
#include <QVariantList>

/**
 * Tr —— 语言：**有哪些、现在用哪个、哪个键对应哪句话。**
 *
 * 语言文件是 `lang/*.json`，明文，不编译（见 lang/README.md）。加一种语言只要
 * 往那个目录里丢一个文件，不用改代码、不用重新编译。
 *
 * ── 为什么它是 QTranslator ───────────────────────────────────────────────
 *
 * 因为这样**代码里还是写 `tr("tray_quit")` / `qsTr("ui_settings_language")`**，
 * 只不过引号里从中文换成了键名。剩下的全归 Qt：装上它之后，Qt 每查一句都会走到
 * 我们的 `translate()`，而"语言变了把整个界面重算一遍"这件事也有现成的机制
 * （`QQmlEngine::retranslate()`），一行都不用自己造。
 *
 * 反过来说，如果自己搞一套 `Tr.t("键")` 的接口，就得自己解决"QML 的绑定怎么知道
 * 该重算"——那才是这类改造里最容易翻车的地方。
 *
 * ── 查不到的时候 ─────────────────────────────────────────────────────────
 *
 *   当前语言里没有 -> 用基准语言（英文，跟着 exe 编进去的）
 *   基准里也没有   -> 返回空串，让 Qt 继续往下走，最后显示源码里那串东西本身
 *                     （也就是键名）。看到界面上一串 `ui_xxx` 就说明这个键
 *                     漏了 —— 不过缺键在装载语言表的时候已经报过日志了。
 *
 * ── 什么时候读文件 ───────────────────────────────────────────────────────
 *
 * 启动时扫目录**只读每个文件的 meta**（名字 + 对应哪些系统语言），字符串表当场
 * 丢掉；真正切到某个语言，才把那个文件完整读一遍。所以常驻内存里只有当前语言
 * 那一张表。
 *
 * 注意：JSON 是整篇解析的，"只读开头的几行"这件事格式上做不到 —— 扫的时候
 * 也得把文件整个读进来，只是读完只留 meta 而已。
 */
class Tr : public QTranslator
{
    Q_OBJECT

public:
    explicit Tr(QObject *parent = nullptr);
    ~Tr() override;

    /** 基准语言的代码。它的表跟着 exe 一起编进去，**永远可用**。 */
    static QString baseCode();

    /** 扫 lang/ 目录。启动时调一次就够 —— 运行中加文件要重启才认。 */
    void scan();

    /** 扫到的语言：`[{ code: "zh_CN", name: "简体中文" }, ...]`，按代码排序。 */
    Q_INVOKABLE QVariantList languages() const;

    /**
     * 用哪个语言。**空串 = 跟随系统。**
     *
     * 选的语言文件不存在、或者跟随系统没对上，都回落到基准语言 —— 不是让界面
     * 空着，也不是显示键名。
     */
    void setLanguage(const QString &requested);

    /** 用户选的那个（空串 = 跟随系统）。用于写回设置文件。 */
    QString requestedLanguage() const { return m_requested; }

    /** 实际在用哪个 —— 永远是有效的。 */
    QString effectiveLanguage() const { return m_effective; }

    // 名字冲突是有意的：Qt 自己去调它，谁来调、什么时候调都不是我们的事。
    QString translate(const char *context, const char *sourceText,
                      const char *disambiguation = nullptr, int n = -1) const override;

signals:
    void logMessage(const QString &text);

private:
    /** 扫目录时留下的那点信息 —— 只有这些，字符串表不留。 */
    struct LanguageInfo {
        QString code;              // 文件名去掉 .json，比如 "zh_CN"
        QString name;              // meta.name，比如 "简体中文"
        QStringList systemNames;   // meta.system，比如 ["zh_CN", "zh_Hans_CN"]
    };

    QString langDir() const;
    bool find(const QString &code, LanguageInfo *out) const;

    /** 系统的语言清单里，有没有对得上的。对不上返回空串。 */
    QString matchSystemLanguage() const;

    /** 把某个语言的完整表读进来，放进 m_active。 */
    void loadActiveTable(const QString &code);

    /** 和基准比一比，缺了什么、多了什么 —— 只在日志里说。 */
    void reportTableDifferences();

    // 不是 const：里面要发信号，而信号是非 const 的成员函数。
    void log(const QString &text);

    QHash<QString, QString> m_base;     // 基准语言（英文）
    QHash<QString, QString> m_active;   // 当前语言
    QList<LanguageInfo> m_languages;

    QString m_requested;   // 空 = 跟随系统
    QString m_effective;
};
