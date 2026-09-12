#pragma once

#include <QString>
#include <QObject>

class QTranslator;
class AppSettings;

// UiState —— 界面的**唯一状态源**：现在是哪种语言、要深色还是浅色。
//
// 为什么需要它：这个程序里同时住着两套界面 —— QML 那套（FluentUI 组件）和
// 原生 Qt 控件那套（文件对话框、消息框、右键菜单）。这两套各有一套自己的
// 语言和外观来源，谁也不知道谁。用户看到的结果就是：QML 界面切成了英文，
// 弹出的文件对话框还是中文；QML 切成深色，原生控件还是白的。
//
// 所以规矩定成一条：**语言和深浅只在这里存一份**，两边都向它看齐。
//
// 三个设计约束，都是为了让这个文件保持"干净"：
//
//   一、**不认识 FluentUI。** 它不知道 FluTheme 存在，也不 include 它的头文件。
//      QML 那边自己把 FluTheme.darkMode 绑到 UiState.themeMode 上。
//      反过来的话，C++ 层就被一个第三方 QML 库绑住了。
//
//   二、**不认识任何窗口或 QML 引擎。** 它只发信号。谁想跟着变，谁自己连。
//      语言变化要重算 qsTr() 的话，得由持有引擎的那一方去调 retranslate()。
//
//   三、**原生 Qt 控件是重点。** 只装自己的 .qm 是不够的 —— 文件对话框上
//      那些"打开/取消"是 Qt 自己画的，得装 Qt 自带的 qtbase_*.qm 才会变语言；
//      深色也不是设个 QML 属性就完事，得动 QStyleHints 的 colorScheme。
class UiState : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
    Q_PROPERTY(bool dark READ dark NOTIFY darkChanged)
    Q_PROPERTY(QString language READ language WRITE setLanguage NOTIFY languageChanged)

public:
    /** 外观模式。语义和 Qt::ColorScheme 大致对应，但多一个"跟随系统"。 */
    enum ThemeMode {
        System = 0,
        Light  = 1,
        Dark   = 2,
    };
    Q_ENUM(ThemeMode)

    /**
     * 语言和深浅的初始值从设置文件里读，之后用户一改就写回去。
     *
     * **这里是"唯一的那一份"。** 界面不直接改外观 —— 它只是把"用户选了什么"
     * 告诉这里（写属性），真正的改变由这里的信号广播出去，每个界面再跟着变。
     * 这条链路是单向的，界面在收到广播之前不会自己动。
     */
    explicit UiState(AppSettings *settings, QObject *parent = nullptr);
    ~UiState() override;

    int themeMode() const { return m_themeMode; }

    /** 当前**实际**是不是深色。System 模式下它跟着 Windows 走。 */
    bool dark() const;

    /** 语言代码，比如 "zh_CN"、"en_US"。空字符串 = 跟随系统。 */
    QString language() const { return m_language; }

    void setThemeMode(int mode);
    void setLanguage(const QString &code);

signals:
    void themeModeChanged();
    void darkChanged();
    void languageChanged();

private:
    void applyTheme();
    void applyLanguage();

    /** 当前该用哪个 locale。只看系统语言清单里的**第一个**，原因见 .cpp。 */
    static QString systemLanguageCode();

    int m_themeMode = System;
    QString m_language;   // 空 = 跟随系统

    QTranslator *m_appTranslator = nullptr;   // 我们自己的 MCast_*.qm
    QTranslator *m_qtTranslator  = nullptr;   // Qt 自带原生控件的 qtbase_*.qm

    /** 设置文件。初始值从它读，改动写回它。可以为空（没有设置也能跑）。 */
    AppSettings *m_settings = nullptr;
};
