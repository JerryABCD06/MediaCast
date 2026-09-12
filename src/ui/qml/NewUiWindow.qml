import QtQuick
import FluentUI
import MediaCast 1.0
import "components"

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

    // ── 导航：设置是**二级页**，不是平级页签 ─────────────────────────────
    //
    // 照 Windows 11「照片」的做法：齿轮点进去，顶栏左边变成 [←][设置]，
    // 点返回回到主界面。
    //
    // 为什么不做成平级页签（原来的 FluPivot）：平级页签和二级页是两种导航模型，
    // 混在一起用户会分不清自己在哪一层 —— 尤其"设置"里以后会长出子页面。
    property int page: 0        // 0 = 投屏，1 = 设置

    // ── 顶栏 ─────────────────────────────────────────────────────────────
    //
    // 整条换掉 FluWindow 自带的那条（自带那条只有一个图标和一个标题）。
    // 高度、透明背景、三按钮怎么摆，都在 AppTopBar 里 —— 见那个文件的头注释，
    // 里面有"为什么按钮不能混在一个容器里"的原因。
    appBar: AppTopBar {
        id: topBar

        mode: window.page === 0 ? 0 : 1
        pageTitle: qsTr("设置")

        onBackClicked: window.page = 0
        onSettingsClicked: window.page = 1
        onDisconnectClicked: {
            // 和"关窗口时确认断开"走的是同一条路：Shell 把意思发出去，main() 把它
            // 接在 DlnaRenderer::endSession 上（结束会话、推事件、让设备在网络里
            // 消失一下再回来）。界面不认识 DLNA，也不该认识。
            Shell.endCasting()
        }
        onInfoClicked: {
            // TODO：「关于」那一类信息。先留空壳。
        }

        // 顶栏上**我们自己的**按钮要登记成"不算标题栏"，否则按下去是拖窗口。
        // 窗口那三个按钮 FluWindow 已经替我们登记过了，不用管。
        //
        // 用 ready 信号而不是窗口的 Component.onCompleted：FluWindow 自己
        // 用了那个处理函数（居中、登记、显示），实例上再写一个有可能把它顶掉。
        onReady: {
            window.setHitTestVisible(topBar.backButton)
            window.setHitTestVisible(topBar.disconnectButton)
            window.setHitTestVisible(topBar.infoButton)
            window.setHitTestVisible(topBar.settingsButton)
        }
    }

    // ── 页面 ─────────────────────────────────────────────────────────────
    //
    // **投屏页一直存在、一直可见**，切设置页只是**盖上去**，不是换掉它。
    //
    // 这不是偷懒：mpv 在 render API 模式下，画面是靠我们每帧调一次渲染推着走的。
    // 一旦把那个画面 item 藏起来（或者销毁），mpv 的视频输出就停了 —— 再开也
    // 起不来（实测表现：状态显示在播、位置一直是 0、画面全黑）。所以"切页 =
    // 藏起投屏页"这条路走不通，只能盖。
    //
    // 代价：在设置页里的时候，画面还在底下照常渲染（白烧一点显卡）。换来的是
    // "边看边改设置、切回来接着播" —— 值。
    //
    // **不留边距**：顶栏底下整块都归页面用 —— 投屏页要的是"画面铺满"，
    // 设置页要的是"盖满、别从缝里漏出主页面的东西"。谁需要内边距谁自己加
    // （设置页在内容外面加了一圈，见那个文件）。
    Item {
        anchors.fill: parent
        anchors.margins: 0

        CastPage {
            anchors.fill: parent
        }

        // 设置页：盖满整页，所以下面那些控件点不到；自己带一层不透明底色，
        // 否则画面会从卡片缝里透出来。
        Rectangle {
            anchors.fill: parent
            visible: window.page === 1
            color: FluTheme.backgroundColor

            SettingsPage {
                anchors.fill: parent
            }
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
