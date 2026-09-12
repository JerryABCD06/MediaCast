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

    // ── 关窗策略 ─────────────────────────────────────────────────────────
    //
    // FluWindow 自带一个 closeListener：autoDestroy 为真就把窗口销毁，为假就
    // 改成"藏起来"。这里把它整个换掉，因为我们要三件事：
    //
    //   一、**关掉就真的关掉**（窗口对象销毁）。这个程序是常驻托盘的接收器，
    //       但界面不该赖着不走；下次要用时（有人投屏、或者托盘点「打开主界面」）
    //       再建一个新的。
    //   二、**正在投送的时候先问一句**。关掉界面确实会把这边的投送断掉，
    //       用户应该知道，而不是点完 X 才发现手机上的投屏没了。
    //   三、真关的时候走 FluRouter.removeWindow(window) —— 那是 FluentUI 自己的
    //       关窗方式（从它的窗口表里摘掉 + deleteLater）。直接调 destroy() 等于
    //       在自己的信号处理里把自己拆掉；只调 FluWindow 那个 deleteLater() 又
    //       漏了摘登记。
    //
    // 早先这里是 autoDestroy: false（关窗只藏起来），原因是窗口一销毁，
    // NewUiWindow 里那个裸指针就野了，下一次投屏进来会崩。那个问题已经在
    // C++ 那边解决 —— 引擎只建一次、窗口可以反复建。
    closeListener: function(event) {
        // 用 hasMedia 而不是 idle：**停着的东西也算一个没结束的会话**。
        // 用 idle 的话，片子播完之后用户能一声不响地把窗口关掉，而手机那边
        // 还挂着"正在投屏到这台电脑"。
        if (Playback && Playback.hasMedia) {
            // 把这次关闭拦下来。窗口不走，等用户在对话框里选。
            event.accepted = false
            dialog_end_cast.open()
            return
        }

        event.accepted = true
        FluRouter.removeWindow(window)
    }

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

    // 正在投送时点关闭 -> 先问一句。
    //
    // 文案要说清楚**代价是什么**，而不是含糊的"确定要关闭吗" —— 用户关心的是
    // "手机上的投屏会不会断"，不是窗口关不关。
    FluContentDialog {
        id: dialog_end_cast

        title: qsTr("正在投送")
        message: qsTr("关掉窗口会结束这次投送，手机上也会断开。确定要关吗？")
        negativeText: qsTr("取消")
        positiveText: qsTr("关闭并断开")
        buttonFlags: FluContentDialogType.NegativeButton | FluContentDialogType.PositiveButton

        // 取消：什么都不做。窗口还在原地，投送照常。
        onNegativeClicked: {
        }

        onPositiveClicked: {
            // 顺序要紧：先让协议层把这次投送收干净（结束会话、推事件、让设备在
            // 网络里消失一下再回来），再把窗口关掉。反过来的话，窗口一没，
            // 界面上就没有东西去触发这一句了。
            Shell.endCasting()
            FluRouter.removeWindow(window)
        }
    }
}
