import QtQuick
import FluentUI
import MediaCast 1.0

// 新界面的窗口外壳。它只做三件事：
//   一、把 FluentUI 的窗口撑起来；
//   二、把界面状态（语言 / 深浅）接到 UiState 上；
//   三、摆好顶栏和内容区。
//
// **它不认识任何一个具体页面。** 页面清单在下面 FluPivotItem 那几行里，
// 页面本身是 CastPage.qml / SettingsPage.qml —— 加一页就是加一个 FluPivotItem，
// 别的地方不用动。
FluWindow {
    id: window

    title: qsTr("Media Cast")
    width: 1000
    height: 740

    // **关掉窗口只是把它藏起来，不要销毁。**
    //
    // FluWindow 默认 autoDestroy: true，那意味着点了关闭按钮它就把自己从
    // FluRouter 里摘掉、整个窗口对象销毁。而这个窗口是我们常年持有的一个
    // 裸指针（NewUiWindow::m_window）：窗口一销毁，指针就野了，下一次投屏
    // 进来调 m_window->show() 直接崩 —— 崩在 Qt6Gui 内部读一个非法地址，
    // 栈上什么都看不出来，查了很久。
    //
    // 而且这个程序本来就不该"关掉界面就退出"：它是常驻托盘收投送的。
    // 所以关窗 = 藏起来，之后需要时再 show() 出来，是它该有的行为。
    autoDestroy: false

    // 标题栏用 FluentUI 自绘的这条（默认行为），三个按钮也是它画的。
    //
    // 底座仍然是 DWM 的：FluFrameless 保留了 WS_CAPTION / WS_THICKFRAME，
    // 所以系统级的窗口行为都在 —— 贴靠布局、拖边缩放、开合动画、投影。
    // 差别只在"谁画那几个字形"。
    //
    // 想让系统画按钮（整条原生标题栏）的话：把 FluApp.useSystemAppBar 设成 true，
    // 而且必须**抢在窗口出生之前**设。写在下面的 Component.onCompleted 里是
    // 无效的 —— FluWindow 自己的 onCompleted 里有一句
    //     useSystemAppBar = FluApp.useSystemAppBar
    // 会把它覆盖掉，更麻烦的是无边框助手已经在窗口创建时改过样式位了，
    // 事后关掉它原生边框也不会自己长回来。

    // 深浅只有 UiState 一个源头，FluentUI 的 FluTheme 是**跟着它走**的，
    // 不是反过来。这里用 Binding 而不是赋值，这样 UiState 一变它就跟着变。
    //
    // 两边的枚举名不一样，所以要做一次映射：UiState 是我们自己的（不依赖
    // FluentUI），FluThemeType 是 FluentUI 的。这个映射只能写在使用方，
    // 写进 UiState 就把它和第三方库绑死了。
    Binding {
        target: FluTheme
        property: "darkMode"
        value: UiState.themeMode === UiState.Dark ? FluThemeType.Dark
             : UiState.themeMode === UiState.Light ? FluThemeType.Light
                                                   : FluThemeType.System
    }

    // 顶栏 + 内容区都由 FluPivot 一个控件包办：
    // 上面那排是它的 header，下面那块是当前 item 的 contentItem。
    //
    // 用库自带的控件而不是自己写一个，是为了跟这个框架保持一套观感 ——
    // 下划线、悬停色、动画时长、字体层级全是它的。
    //
    // 已知取舍：FluPivot 的 delegate 只读 modelData.title，**不支持图标**。
    // 所以"设置"现在没有齿轮。想加的话得在自己的组件里把它的 header 换成
    // 自己的 ListView，那是另一件事。
    FluPivot {
        anchors.fill: parent
        // FluPivot 自己没有内边距属性，页签文字会贴着窗口左边。
        // 整块往里收一点 —— 收在这里而不是收在页面里，是因为页面的内边距
        // 管不到顶栏那条。
        anchors.margins: 20

        FluPivotItem {
            title: qsTr("投屏")
            contentItem: CastPage {}
        }

        FluPivotItem {
            title: qsTr("设置")
            contentItem: SettingsPage {}
        }
    }
}
