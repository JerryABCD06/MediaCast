import QtQuick
import FluentUI
import MediaCast 1.0

// 一块**自己画的云母底**。
//
// ── 为什么要自己画 ──────────────────────────────────────────────────────
//
// 系统那套云母（DWM）只能从"窗口上没画东西的地方"透出来。于是只要有一页必须
// 挡住下层的内容（设置页压在投屏页上，投屏页里有 mpv 的画面），这一页就只剩
// 两条路：画一块死灰（等于这一页没有云母），或者干脆不画底、透明（下层的东西
// 从卡片缝里透上来）。
//
// 之前选的是"透明 + 要求下面的页面自己把东西收起来"，那是把耦合埋进每一页：
// 加一页就得想一遍"我底下压着什么、它会不会漏上来"。
//
// 这里换一条路：**谁要底，谁自己画一块实心的。**
//
// ── 用法 ────────────────────────────────────────────────────────────────
//
//     // 窗口那层（**全窗口只有一个**，它负责真正去画云母）
//     FluWindow { background: Component { MicaBackdrop { } } }
//
//     // 页面上要挡住下层内容的那一块
//     MicaBackdrop {
//         anchors.fill: parent
//         wallpaperItem: windowBackdropItem.ownWallpaperItem   // 见下
//     }
//
// 关掉云母、机器不认云母、壁纸读不到、壁纸还没解码完 —— 一律退回窗口那块灰
// （#F3F3F3 / #1A1A1A）。所以调用方不用自己判断开关，也不用管"我底下有没有
// 东西"：摆一块底，就一定是一块实心的底。
//
// ── 这不是系统云母 ──────────────────────────────────────────────────────
//
// 差别只在取不到的场合：桌面背景是纯色、幻灯片、或者 Wallpaper Engine 那种
// 第三方动态壁纸时，系统从桌面上取色，我们只能退回那块灰。云母的定义本来就
// 是"取壁纸"，所以正常情况下两者看起来是一回事。
//
// 还有一条：壁纸的铺法我们按"填充"（PreserveAspectCrop）算，这也是 Windows
// 默认的铺法。要是把桌面设成"平铺 / 居中 / 适应"，这份底和真实桌面对不上 ——
// 但和窗口里其他几块底仍然是对得上的（它们走同一套算法）。
Item {
    id: control

    /** 云母开关。默认跟设置走。 */
    property bool micaEnabled: Settings.uiMica

    /**
     * 共用的壁纸图。**空 = 自己造一张**（只有窗口背景那一层是这样）。
     *
     * 页面上的底一律把窗口那层的那张借过来用，不各造各的。原因是踩过的一个坑：
     * 页面活在**带裁剪的内容区**里（FluWindow 的 layout_content 是 clip: true），
     * 而 Qt 把源 item 渲染进纹理时会连这层裁剪一起算 —— 页面那块底顶边用来当
     * "模糊邻域"的像素就成了空的，模糊出来的颜色和隔壁（不被裁的窗口那层）对
     * 不上，接缝上一条横线，怎么调都对不齐。
     *
     * 借同一张图之后，两边用的是同一批像素、同一套参数，算出来就是同一张底。
     */
    property Item wallpaperItem: null

    /** 窗口那层把这张借出去（`backdrop.ownWallpaperItem`）。 */
    readonly property alias ownWallpaperItem: img_wall

    /** 自己造壁纸图的那一层。 */
    readonly property bool generating: wallpaperItem === null

    /**
     * 模糊半径、染色浓度。
     *
     * **各块底必须一致**，否则拼起来会露出接缝。默认值抄的是 FluWindow 那套
     * （浅色 0.75 / 深色 0.80，半径 60）。
     */
    property int blurRadius: 128
    property real tintOpacity: FluTheme.dark ? 0.80 : 0.75

    /**
     * 往外多取一圈再模糊。
     *
     * **没有这一圈就会在接缝上露出一条横线**：模糊要采样周围的像素，只取自己
     * 这一块的话，边缘那几像素是拿"夹住"的边界值算出来的，和隔壁那块用真实
     * 邻域算出来的结果对不上。多取 blurRadius 这么宽就够了。
     */
    readonly property real overdraw: blurRadius

    /** 壁纸文件路径。由下面那个定时器去问系统，改了会自己跟上。 */
    property string wallpaperPath: ""

    // ── 坐标 ────────────────────────────────────────────────────────────
    //
    // 这份底的来源是一张**和整块屏幕一样大**的壁纸图，所以要算的是：
    // "我这一块，对应壁纸上的哪一块？"
    //
    // 壁纸图的 (0,0) 钉在窗口的 (0,0) 上（见 img_wall 的 x/y），所以：
    //     壁纸坐标 = 屏幕坐标 - 屏幕原点
    //
    readonly property var win: Window.window

    /**
     * 这块屏幕在整个虚拟桌面里的位置和大小。
     *
     * **别去问 Window.screen.geometry** —— Qt 6.11 的 Screen 类型上根本没有
     * geometry / virtualX / virtualY（那是 Qt 5 的东西）。写了一不会报错，只会
     * 安安静静地拿到 undefined，然后整块底变成一张空图（踩过，排查了两轮）。
     * 库自己那份 com_background 里还在用 window.screen.virtualX，所以那条路在
     * Qt 6 下本来就是坏的。
     *
     * 能拿到的是这些：
     *   · 尺寸 —— 贴在本 item 上的 Screen（`Screen.width/height`），精确；
     *   · 原点 —— 只能走库的 desktopAvailableGeometry（它带屏幕偏移）。
     *     任务栏在屏幕右侧 / 下侧时它是准的；在左侧 / 上侧时会差一个任务栏的
     *     厚度。那点差别本来就要过一遍半径 60 的模糊，看不出来。
     */
    readonly property rect screenGeometry: {
        var origin = control.win ? FluTools.desktopAvailableGeometry(control.win)
                                 : Qt.rect(0, 0, 0, 0)
        return Qt.rect(origin.x, origin.y, Screen.width, Screen.height)
    }

    /**
     * 这块底在**窗口**里的位置（左上角）。
     *
     * 两个坑叠在一起，这里都绕开了：
     *
     * 一、**别用 mapToItem(win.contentItem, 0, 0)**。实测它在这个窗口里恒返回
     *     (0,0)，而把父级的 x/y 一层层加起来得到的是正确的 48 —— 设置页那块底
     *     因此整整偏了一个顶栏的高度，接缝就在顶栏下面露出来。
     *
     * 二、**别把位置算在一个"只在 win 变化时才重算"的绑定里**。绑定只认被读到的
     *     属性；而锚点是创建之后才落的，写成 `mapToItem(...)` 这种形式的话，
     *     引擎看不见 item 自己的 x/y，算一次就定了。
     *
     * 下面这个写法两条都躲开：自己沿 parent 累加，每一层的 x/y / parent 都是
     * 明确读到的属性，任何一个变了都会重算。
     */
    readonly property point originInWindow: {
        var px = 0
        var py = 0
        var item = control
        while (item) {
            px += item.x
            py += item.y
            item = item.parent
        }
        return Qt.point(px, py)
    }

    /**
     * 画的是一张**整个窗口那么大**的图（不是"自己这么大"）。
     *
     * 这样各块底取的源矩形完全一样，出来的像素自然一样，各自只是把它裁到自己
     * 身上。按"自己这么大"去算的话，两块相邻的底在交界处各自缺一段邻域，拼起来
     * 就是一条色带。
     */
    readonly property real backdropWidth: (win ? win.width : width)
    readonly property real backdropHeight: (win ? win.height : height)

    readonly property rect sourceRect: Qt.rect(
                                           (win ? win.x : 0) - screenGeometry.x - overdraw,
                                           (win ? win.y : 0) - screenGeometry.y - overdraw,
                                           backdropWidth + overdraw * 2,
                                           backdropHeight + overdraw * 2)

    /** 云母开关开着，而且这台机器认云母（Win10 的 availableEffects 里没有它）。 */
    readonly property bool micaOn: micaEnabled
                                   && win !== null
                                   && win.availableEffects.indexOf("mica") >= 0

    /** 壁纸解码完了没有。没解码完就还是那块灰顶着。 */
    readonly property bool wallpaperReady: {
        var item = control.wallpaperItem !== null ? control.wallpaperItem : img_wall
        return item !== null && item.status === Image.Ready
    }

    // ── 底色 ────────────────────────────────────────────────────────────
    //
    // 垫在最下面。**它让这块底永远是实心的** —— 上面那几个条件里只要有一个不
    // 成立（关了云母、机器不认、壁纸没读到），露出来的就是它。
    //
    // 用 windowActiveBackgroundColor 而不是 FluTheme.backgroundColor：后者是
    // 纯白，和 Windows 那块 #F3F3F3 不是一回事。
    Rectangle {
        anchors.fill: parent
        color: (control.win && !control.win.active) ? FluTheme.windowBackgroundColor
                                                    : FluTheme.windowActiveBackgroundColor
    }

    // ── 壁纸 ────────────────────────────────────────────────────────────
    //
    // 钉在 -originInWindow 上，等于把它的 (0,0) 对齐到窗口的 (0,0)。
    //
    // **它必须是 visible 的，哪怕不该被看见。** 这一点踩过：写 visible: false
    // 的时候，ShaderEffectSource 采到的是一张**空图** —— 模糊出来什么都没有，
    // 屏幕上也就不见云母（只剩那层染色盖在底色上，看着像一块发白的灰）。
    // Qt 只把"可见"的 item 渲染进那层 FBO。
    //
    // 那就让它可见：**它整个被压在下面的 FluAcrylic 底下**，而 FluAcrylic 是
    // 不透明的（模糊结果 + 染色），所以原始壁纸一像素也露不出来。（两者的可见
    // 条件是同一套，不存在"亚克力没画而壁纸露出来"的缝。）
    //
    // sourceSize 取一半：模糊之后根本看不出细节，解码小一半省内存也省一次采样。
    Image {
        id: img_wall
        visible: control.generating && control.micaOn
        cache: true
        asynchronous: true
        fillMode: Image.PreserveAspectCrop
        x: -control.originInWindow.x
        y: -control.originInWindow.y
        width: control.screenGeometry.width
        height: control.screenGeometry.height
        sourceSize: Qt.size(Math.max(1, Math.round(width / 2)),
                            Math.max(1, Math.round(height / 2)))
        source: (control.generating && control.micaOn && control.wallpaperPath !== "")
                ? FluTools.getUrlByFilePath(control.wallpaperPath) : ""
    }

    // ── 模糊 + 染色 + 噪点 ──────────────────────────────────────────────
    //
    // 库自带的亚克力：取 target 的这一块 → FastBlur → 染色 → 叠一层噪点。
    // 它比本块大一圈（上下左右各一圈 overdraw），那一圈是喂给模糊器当邻域用的。
    //
    // **这里故意不裁。** 裁过的版本试过，接缝反而更明显：Qt 把源 item 渲染进
    // 纹理的时候会连祖先的 clip 一起算，于是顶边那圈"邻域"是空的，模糊出来的
    // 颜色和隔壁对不上。不裁的话，多出来的那圈画的是**和窗口那层一模一样的
    // 内容**（同一张图、同一套参数），压在顶栏底下根本看不出来；而整个模糊是在
    // 完整的邻域上算出来的，和窗口那层逐像素一致。
    FluAcrylic {
        x: -control.originInWindow.x - control.overdraw
        y: -control.originInWindow.y - control.overdraw
        width: control.backdropWidth + control.overdraw * 2
        height: control.backdropHeight + control.overdraw * 2
        visible: control.micaOn && control.wallpaperReady
        // 页面上的底借窗口那层那张图（见上面 wallpaperItem 那段的"为什么"）。
        target: control.wallpaperItem !== null ? control.wallpaperItem : img_wall
        targetRect: control.sourceRect
        blurRadius: control.blurRadius
        tintOpacity: control.tintOpacity
        tintColor: FluTheme.dark ? Qt.rgba(0, 0, 0, 1) : Qt.rgba(1, 1, 1, 1)
    }

    // ── 壁纸换了要跟上 ──────────────────────────────────────────────────

    //
    // **不能问 FluTheme.desktopImagePath**：那个属性只在 blurBehindWindowEnabled
    // 为真的时候才更新，而我们开的是系统云母 —— 库里会把它关掉（见 FluWindow
    // 的 onEffectiveChanged），于是那个路径在我们的场景里永远是空的。
    //
    // 直接问 FluTools 要，顺带自己盯着它变。轮询很便宜（一次
    // SystemParametersInfoW），3 秒一次，比为此动 C++ 加监视器划算。
    //
    // 只有窗口那层要轮询 —— 别的底用的是它那张图，跟着它变。
    Timer {
        interval: 3000
        repeat: true
        running: control.generating && control.micaOn
        triggeredOnStart: true
        onTriggered: control.wallpaperPath = FluTools.getWallpaperFilePath()
    }
}
