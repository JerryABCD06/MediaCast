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

    // 产品名，不翻译 —— 所以**不套 qsTr**。套了的话哪天有人做了个语言文件
    // 顺手把它翻成中文，任务栏和窗口标题上的名字就跟着变了。
    title: "Media Cast"
    width: 1000
    height: 740

    // ── 系统那个 backdrop：开关还管着它，但它现在是看不见的 ───────────────
    //
    // 这是 FluentUI 自带的（`FluFrameless` 把它暴露成 effect 属性），内部走的是
    // `DwmSetWindowAttribute(hwnd, 38, DWMSBT_MAINWINDOW)`。可选值还有
    // "mica-alt"（资源管理器那种）、"acrylic"、"dwm-blur"（Win10 也有）、
    // "normal"（关，也是原来的默认值）。
    //
    // **底色早就不是它给的了** —— 底色是我们自己画的（见下面那个 MicaBackdrop），
    // DWM 那张被结结实实盖住。开关还是设置里那个（读同一个 `Settings.uiMica`），
    // 所以"关掉云母"连系统这边也一起停 —— 但它现在看不出任何区别。
    //
    // 这一条量过两次，**以第二次为准**：
    //
    //   · 第一次（两个构建各抓一张）看着像"写死成 normal 会让窗口边上多出一道
    //     1px 的线"。**那次对比不干净**：两次截图之间桌面上别的东西也变了，
    //     窗口也没有挪到同一个位置。
    //   · 第二次（两个构建、都投同一张图、窗口都挪到 (335,18)，只比窗口内部
    //     的像素）：窗口内部**逐像素 0 差别**，边缘那几列也一致；整屏找出来的
    //     差异全部落在窗口外面（别的东西在动）。
    //
    // 所以它现在只剩一个语义问题：关掉开关 = 连系统那边也不算了。哪天真要
    // "开关只管我们画的那层"，把这里写死成 "mica" 就行 —— 但那之前先想想值不值：
    // 写死之后，用户关了云母，DWM 还在白算一张他看不见的底。
    effect: Settings.uiMica ? "mica" : "normal"

    // ── 窗口的底：我们自己画 ──────────────────────────────────────────────
    //
    // 换掉 FluWindow 自带的那层（它只会在"窗口透明"和"画一块灰"之间二选一），
    // 改成我们自己那份云母底 —— 和各个页面用的是**同一个组件、同一套参数**。
    //
    // 为什么非要自己画：系统云母只能从"没画东西的地方"透出来，于是每个要挡住
    // 下层内容的页面都得自己想一遍"我底下是什么、它会不会漏上来"。换成自己画
    // 之后，规则简单了 —— **谁要底谁摆一块，摆上去就是实心的**。
    //
    // 上面那行 effect 还留着，理由见它自己的注释（管窗口边框，不管底色）。
    background: Component {
        MicaBackdrop {
            id: windowBackdrop
            // 全窗口唯一一处"造云母"的地方（整屏算一次），别处的底都从它裁。
            isSource: true
            Component.onCompleted: window.backdropItem = windowBackdrop
            Component.onDestruction: window.backdropItem = null
        }
    }

    /**
     * 窗口那层云母（就是上面 background 里那个实例）。
     *
     * 页面上的底**不自己造壁纸图**，都借它这张（见 MicaBackdrop 头注释里那段
     * 为什么）。所以全窗口只有一张壁纸图、一套参数，各页面的底算出来才和它
     * 逐像素一致、拼起来没有接缝。
     */
    property Item backdropItem: null

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
        // 判据是**有没有投送方连着**，不是"mpv 手里有没有东西"。
        //
        // 三件事要同时成立：
        //   · 正在放、暂停着 —— 要问（关了手机那边就断了）
        //   · 片子播完了但手机还挂着"正在投屏" —— 也要问
        //   · mpv 里还留着一条、可并没有谁连着（本机试放、或者对方早就走了）
        //     —— **不该问**。那种时候界面上什么也没在投送，弹一个"关闭窗口将
        //     结束本次投送"只会让人莫名其妙。
        //
        // 早先这里判的是 hasMedia（"手里有没有东西"），第三条就会误报。
        // 现在这个判据和顶栏那个「断开连接」按钮是同一条 —— 它显示的时候，
        // 就是关窗口该问的时候。
        if (Playback && Playback.peerConnected) {
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
        pageTitle: qsTr("ui_nav_settings")

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

        // 设置页：盖满整页，所以下面那些控件点不到；自己带一块实心底，
        // 否则画面会从卡片缝里透出来。
        Item {
            anchors.fill: parent
            visible: window.page === 1

            // **这一页的底：自己画一块。**
            //
            // 以前这里是不透明的灰、或者"有云母时干脆透明去借窗口的"，那条路要求
            // 投屏页把看得见的东西全收起来（CastPage.covered）—— 加一页就要多想
            // 一层"我底下压着什么"。换成实心的底之后，底下压着什么、它在不在放
            // 片子，全都不用管了。
            //
            // 摆在最下面：它只是底，上面的字和控件都得压在它上面。
            MicaBackdrop {
                anchors.fill: parent
                // 从窗口那层算好的整屏云母上裁一块 —— 见 MicaBackdrop 头注释。
                canvasItem: window.backdropItem ? window.backdropItem.ownCanvas : null
            }

            // **先吃掉这一页上的点击。**
            //
            // 光有底色的 Item **不吃鼠标事件** —— 没有这一层的话，点在设置页的空白处
            // （四周留白、左边那栏下方的空区）会**穿到下面投屏页**，落到那个
            // "点画面 = 播放/暂停"的 MouseArea 上：看着在设置页里，片子却被暂停了。
            //
            // 空实现就够。声明在 SettingsPage **之前**（后声明的在上面），
            // 所以页面里的开关、下拉框照样点得到。
            MouseArea {
                anchors.fill: parent
            }

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

        // 库自带的宽度是 400（逻辑像素），那是按中文短句定的。英文那句
        // "Closing the window will end this cast..." 在 400 里要折成两行，
        // 而这个对话框的高度是按"一行"算出来的 —— 折行之后**第二行会被切掉**，
        // 按钮上的 "Close and disconnect" 也会被挤到框外。
        //
        // 加宽到 560 之后两边都能容下。以后加语言时如果哪句更长，先看这儿。
        width: 560

        title: qsTr("ui_close_dialog_title")
        message: qsTr("ui_close_dialog_text")
        negativeText: qsTr("ui_common_cancel")
        positiveText: qsTr("ui_close_dialog_confirm")
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
