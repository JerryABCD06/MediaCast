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
// ── 怎么画：整屏算一次，各处只裁一块 ────────────────────────────────────
//
// 分两步，**这一步是这套东西的关键**：
//
//   一、**造**：整块屏幕那么大的一张云母 —— 壁纸模糊 + 染色 + 噪点，算一次，
//       钉在屏幕坐标系上。全窗口只有一处（窗口背景那层）。
//   二、**裁**：谁要底，就从那张算好的图上裁自己那一块，贴在自己身上。
//
// 为什么不各画各的（试过，就是那么错的）：模糊里的噪点层是按各自的左上角
// 平铺的，窗口那层和页面那层因此差两三个色阶 —— 接缝上一道淡淡的横线，
// 时有时无；而且每动一下窗口、每换一页都要重算一遍模糊。
//
// 裁切是纯取像素：**所有底都来自同一张图上的同一批像素**，接缝在结构上不可能
// 存在；窗口移动缩放也只改"裁哪儿"，不动那张图。
//
// ── 用法 ────────────────────────────────────────────────────────────────
//
//     // 窗口那层：全窗口唯一一处"造"（`isSource`）
//     FluWindow {
//         background: Component { MicaBackdrop { id: src; isSource: true } }
//         property Item backdropItem: src      // 给别人借
//     }
//
//     // 页面上要挡住下层内容的那一块
//     MicaBackdrop {
//         anchors.fill: parent
//         canvasItem: window.backdropItem ? window.backdropItem.ownCanvas : null
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
// 但窗口里各块底之间仍然对得上（它们本来就是同一张图上裁下来的）。
Item {
    id: control

    /** 云母开关。默认跟设置走。 */
    property bool micaEnabled: Settings.uiMica

    /**
     * 这一块是不是"造那张图"的那一块。**全窗口只有一个**（窗口背景那层）。
     *
     * 写成显式开关而不是"canvasItem 为空就算造"，是为了别出现这种尴尬：
     * 借画布的那一方在窗口刚建起来、还没拿到引用时，会自己造一张 ——
     * 而它活在带裁剪的内容区里，造出来的那张是残的。宁可空着（露底色）。
     */
    property bool isSource: false

    /** 借来的画布（`窗口那层.ownCanvas`）。自己造的那一块不用填。 */
    property Item canvasItem: null

    /** 窗口那层把画布借出去。 */
    readonly property alias ownCanvas: canvas

    /** 模糊半径、染色浓度。**只有造的那一层用得上**（裁切不重算）。 */
    property int blurRadius: 128
    property real tintOpacity: FluTheme.dark ? 0.80 : 0.75

    /** 壁纸文件路径。由下面那个定时器去问系统，改了会自己跟上。 */
    property string wallpaperPath: ""

    // ── 坐标 ────────────────────────────────────────────────────────────
    //
    // 画布的坐标系 = **屏幕坐标系**：画布自己的 (0,0) 就是屏幕的 (0,0)，
    // 所以"裁哪儿"算的就是"这一块在屏幕上占哪儿"：
    //
    //     屏幕坐标 = 窗口位置 + 这一块在窗口里的偏移
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
     *     厚度。那点差别本来就要过一遍模糊，看不出来。
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

    /** 裁切用：这一块在**屏幕**上占的那一格。 */
    readonly property rect onScreen: Qt.rect(
                                         (win ? win.x : 0) - screenGeometry.x + originInWindow.x,
                                         (win ? win.y : 0) - screenGeometry.y + originInWindow.y,
                                         width,
                                         height)

    /**
     * 开着没有。
     *
     * **这里不再问 `win.availableEffects`（"这台机器认不认云母"）** —— 那是底色
     * 还靠系统云母给的时候留下的门槛（只有 Win11 认）。现在底色是我们自己拿壁纸
     * 算的，Win10 一样算得出来，再拿这个挡着就等于白做一个功能。
     */
    readonly property bool micaOn: micaEnabled && win !== null

    /** 拿来裁的那张图算好了没有（壁纸解码完没有）。 */
    readonly property bool canvasReady: {
        var item = control.canvasItem !== null ? control.canvasItem : canvas
        return item !== null && item.ready === true
    }

    // ── 底色 ────────────────────────────────────────────────────────────
    //
    // 垫在最下面。**它让这块底永远是实心的** —— 上面那几个条件里只要有一个不
    // 成立（关了云母、机器不认、壁纸没读到、还没算好），露出来的就是它。
    //
    // 用 windowActiveBackgroundColor 而不是 FluTheme.backgroundColor：后者是
    // 纯白，和 Windows 那块 #F3F3F3 不是一回事。
    Rectangle {
        anchors.fill: parent
        color: (control.win && !control.win.active) ? FluTheme.windowBackgroundColor
                                                    : FluTheme.windowActiveBackgroundColor
    }

    // ── 一、造：整屏算一次 ──────────────────────────────────────────────
    //
    // 只有窗口背景那一层会建这个东西。它的坐标系就是屏幕坐标系，
    // 所以裁切那边只要说"我要屏幕上哪一格"就行。
    //
    // 它自己画在窗口里是**多余**的（裁出来的那块会整个盖住它），但必须
    // `visible` —— 理由见下面 img_wall 那段，Qt 采不到不可见的 item。
    Item {
        id: canvas
        visible: control.isSource && control.micaOn
        x: 0
        y: 0
        width: control.screenGeometry.width
        height: control.screenGeometry.height

        /** 别处借它之前先问一句：算好了没有。 */
        readonly property bool ready: control.isSource && control.micaOn
                                      && img_wall.status === Image.Ready

        // 壁纸。钉在画布的 (0,0) 上，也就是屏幕的 (0,0)。
        //
        // **它必须是 visible 的，哪怕不该被看见。** 这一点踩过：写 visible: false
        // 的时候，ShaderEffectSource 采到的是一张**空图** —— 模糊出来什么都没有，
        // 屏幕上也就不见云母（只剩那层染色盖在底色上，看着像一块发白的灰）。
        // Qt 只把"可见"的 item 渲染进那层 FBO。
        //
        // sourceSize 取一半：模糊之后根本看不出细节，解码小一半省内存也省一次采样。
        Image {
            id: img_wall
            visible: canvas.visible
            cache: true
            // **同步解码。** 异步的话，窗口露面时这张图还没好，用户会先看到
            // 一小会儿底色（灰）、再变成云母（实测约 0.3 秒）。同步解码把这点
            // 开销挪到"窗口还没露面"的时候 —— 反正那时候窗口本来就在等第一帧
            // （见 NewUiWindow::show() 里那句 grabWindow）。
            asynchronous: false
            fillMode: Image.PreserveAspectCrop
            x: 0
            y: 0
            width: parent.width
            height: parent.height
            sourceSize: Qt.size(Math.max(1, Math.round(width / 2)),
                                Math.max(1, Math.round(height / 2)))
            source: (canvas.visible && control.wallpaperPath !== "")
                    ? FluTools.getUrlByFilePath(control.wallpaperPath) : ""
        }

        // 库自带的亚克力：模糊 + 染色 + 叠一层噪点。**整屏算这一次**，
        // 之后所有底都是从这里裁的。
        FluAcrylic {
            anchors.fill: parent
            visible: canvas.ready
            target: img_wall
            targetRect: Qt.rect(0, 0, canvas.width, canvas.height)
            blurRadius: control.blurRadius
            tintOpacity: control.tintOpacity
            tintColor: FluTheme.dark ? Qt.rgba(0, 0, 0, 1) : Qt.rgba(1, 1, 1, 1)
        }
    }

    // ── 二、裁：从那张图上取自己这一格 ──────────────────────────────────
    //
    // 纯取像素，不重算、也不多取邻域（模糊在造的那一步就做完了）。
    ShaderEffectSource {
        anchors.fill: parent
        visible: control.micaOn && control.canvasReady
        sourceItem: control.isSource ? canvas : control.canvasItem
        sourceRect: control.onScreen
        live: true
    }

    // 造的那一层自己也是"要底"的一方，所以上面那块裁切它也要用（sourceItem 是
    // 它自己的画布）。窗口那块的底色就是这一裁。

    // ── 壁纸换了要跟上 ──────────────────────────────────────────────────
    //
    // **不能问 FluTheme.desktopImagePath**：那个属性只在 blurBehindWindowEnabled
    // 为真的时候才更新，而我们开的是系统云母 —— 库里会把它关掉（见 FluWindow
    // 的 onEffectiveChanged），于是那个路径在我们的场景里永远是空的。
    //
    // 直接问 FluTools 要，顺带自己盯着它变。轮询很便宜（一次
    // SystemParametersInfoW），3 秒一次，比为此动 C++ 加监视器划算。
    //
    // 只有造的那一层要轮询 —— 别处的底是从它那张图上裁的，跟着它变。
    Timer {
        interval: 3000
        repeat: true
        running: control.isSource && control.micaOn
        triggeredOnStart: true
        onTriggered: control.wallpaperPath = FluTools.getWallpaperFilePath()
    }
}
