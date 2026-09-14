import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtQuick.Layouts
import FluentUI

Rectangle{
    property string title: ""
    property string darkText : qsTr("Dark")
    property string lightText : qsTr("Light")
    property string minimizeText : qsTr("Minimize")
    property string restoreText : qsTr("Restore")
    property string maximizeText : qsTr("Maximize")
    property string closeText : qsTr("Close")
    property string stayTopText : qsTr("Sticky on Top")
    property string stayTopCancelText : qsTr("Sticky on Top cancelled")
    property color textColor: FluTheme.fontPrimaryColor
    property color minimizeNormalColor: FluTheme.itemNormalColor
    property color minimizeHoverColor: FluTheme.itemHoverColor
    property color minimizePressColor: FluTheme.itemPressColor
    property color maximizeNormalColor: FluTheme.itemNormalColor
    property color maximizeHoverColor: FluTheme.itemHoverColor
    property color maximizePressColor: FluTheme.itemPressColor
    property color closeNormalColor: Qt.rgba(0,0,0,0)
    // 关闭键那两档红：Windows 11 的标准值，由他实测给出（2026-09-14）。
    // 悬停 #C42B1C、按下 #C53D30，两档都是不透明的，字形转白（见下面 btn_close）。
    // 这个红是**固定值**：深色主题下 Windows 也用同一个红，不跟着主题走。
    property color closeHoverColor: "#C42B1C"
    property color closePressColor: "#C53D30"
    property bool showDark: false
    property bool showClose: true
    property bool showMinimize: true
    property bool showMaximize: true
    property bool showStayTop: true
    property bool titleVisible: true
    property url icon
    property int iconSize: 20
    property bool isMac: FluTools.isMacos()
    property color borerlessColor : FluTheme.primaryColor
    property alias buttonStayTop: btn_stay_top
    property alias buttonMinimize: btn_minimize
    property alias buttonMaximize: btn_maximize
    property alias buttonClose: btn_close
    property alias buttonDark: btn_dark
    property alias layoutMacosButtons: layout_macos_buttons
    property alias layoutStandardbuttons: layout_standard_buttons
    property var maxClickListener : function(){
        if(FluTools.isMacos()){
            if (d.win.visibility === Window.FullScreen || d.win.visibility === Window.Maximized)
                d.win.showNormal()
            else
                d.win.showFullScreen()
        }else{
            if (d.win.visibility === Window.Maximized || d.win.visibility === Window.FullScreen)
                d.win.showNormal()
            else
                d.win.showMaximized()
            d.hoverMaxBtn = false
        }
    }
    property var minClickListener: function(){
        if(d.win.transientParent != null){
            d.win.transientParent.showMinimized()
        }else{
            d.win.showMinimized()
        }
    }
    property var closeClickListener : function(){
        d.win.close()
    }
    property var stayTopClickListener: function(){
        if(d.win instanceof FluWindow){
            d.win.stayTop = !d.win.stayTop
        }
    }
    property var darkClickListener: function(){
        if(FluTheme.dark){
            FluTheme.darkMode = FluThemeType.Light
        }else{
            FluTheme.darkMode = FluThemeType.Dark
        }
    }
    id:control
    color: Qt.rgba(0,0,0,0)
    // 标准标题栏高度是 **32**（微软那篇《标题栏设计》："标准标题栏的高度为 32px"）。
    // 各窗口可以自己改（我们的主界面是 48，照 Windows「照片」定的）——
    // 下面那几个按钮**按这个高度铺满**，所以改这儿它们自动跟着变。
    height: visible ? 32 : 0
    opacity: visible
    z: 65535
    Item{
        id:d
        property var hitTestList: []
        property bool hoverMaxBtn: false
        property var win: Window.window
        property bool stayTop: {
            if(d.win instanceof FluWindow){
                return d.win.stayTop
            }
            return false
        }
        property bool isRestore: win && (Window.Maximized === win.visibility || Window.FullScreen === win.visibility)
        property bool resizable: win && !(win.height === win.maximumHeight && win.height === win.minimumHeight && win.width === win.maximumWidth && win.width === win.minimumWidth)
        function containsPointToItem(point,item){
            var pos = item.mapToGlobal(0,0)
            var rect = Qt.rect(pos.x,pos.y,item.width,item.height)
            if(point.x>rect.x && point.x<(rect.x+rect.width) && point.y>rect.y && point.y<(rect.y+rect.height)){
                return true
            }
            return false
        }
    }
    Row{
        anchors{
            verticalCenter: parent.verticalCenter
            left: isMac ? undefined : parent.left
            leftMargin: isMac ? undefined : 10
            horizontalCenter: isMac ? parent.horizontalCenter : undefined
        }
        spacing: 10
        Image{
            width: control.iconSize
            height: control.iconSize
            visible: status === Image.Ready ? true : false
            source: control.icon
            anchors.verticalCenter: parent.verticalCenter
        }
        FluText {
            text: title
            visible: control.titleVisible
            color:control.textColor
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    Component{
        id:com_macos_buttons
        RowLayout{
            FluImageButton{
                Layout.preferredHeight: 12
                Layout.preferredWidth: 12
                normalImage: "../Image/btn_close_normal.png"
                hoveredImage: "../Image/btn_close_hovered.png"
                pushedImage: "../Image/btn_close_pushed.png"
                visible: showClose
                onClicked: closeClickListener()
            }
            FluImageButton{
                Layout.preferredHeight: 12
                Layout.preferredWidth: 12
                normalImage: "../Image/btn_min_normal.png"
                hoveredImage: "../Image/btn_min_hovered.png"
                pushedImage: "../Image/btn_min_pushed.png"
                onClicked: minClickListener()
                visible: showMinimize
            }
            FluImageButton{
                Layout.preferredHeight: 12
                Layout.preferredWidth: 12
                normalImage: "../Image/btn_max_normal.png"
                hoveredImage: "../Image/btn_max_hovered.png"
                pushedImage: "../Image/btn_max_pushed.png"
                onClicked: maxClickListener()
                visible: d.resizable && showMaximize
            }
        }
    }
    RowLayout{
        id:layout_standard_buttons
        // ── 那三个标准按钮：尺寸和图标照微软的规范 ──────────────────────────
        //
        // 来源是微软《标题栏设计》那篇 + 他实测的真机：
        //
        //   · 宽度 **46**（文章没写宽度；100% 缩放的截图里，三个字形中心间距
        //     46.0 / 46.0，悬停背板 x 96..141 = 宽 46）；
        //   · 高度**跟着标题栏走**（文章："标题按钮有完整的出血背板" ——
        //     背板铺满、上下不留空，也不留缝）。所以这里不是写死高度，而是
        //     `Layout.fillHeight`，标题栏改高改矮它们自己跟上；
        //   · 图标 **10 × 10**（同上一张图里量的：三个字形都是 10 高）；
        //   · 字形用文章点名的那四个：E921 ChromeMinimize / E922 ChromeMaximize /
        //     E923 ChromeRestore / E8BB ChromeClose（库里本来就有，且码位一致）；
        //   · 悬停/按下背板用主题那套 `itemHoverColor` / `itemPressColor`
        //     （= 和别的无边框图标按钮完全一致）。Windows 那边量出来是约 4.8%
        //     的黑，主题这套是 6% —— 差 1.2%（约三个色阶），但它是**半透明**的，
        //     压在云母、深色主题上都自然；写死一个 #E9E9E9 只在浅色纯色底上对。
        //   · 只有关闭键那两档红是固定值，见上面 closeHoverColor。
        height: parent.height
        anchors.right: parent.right
        spacing: 0
        FluIconButton{
            id:btn_dark
            Layout.preferredWidth: 40
            Layout.fillHeight: true
            padding: 0
            verticalPadding: 0
            horizontalPadding: 0
            rightPadding: 2
            iconSource: FluTheme.dark ? FluentIcons.Brightness : FluentIcons.QuietHours
            Layout.alignment: Qt.AlignVCenter
            iconSize: 15
            visible: showDark
            text: FluTheme.dark ? control.lightText : control.darkText
            radius: 0
            iconColor:control.textColor
            onClicked:()=> darkClickListener(btn_dark)
        }
        FluIconButton{
            id:btn_stay_top
            Layout.preferredWidth: 40
            Layout.fillHeight: true
            padding: 0
            verticalPadding: 0
            horizontalPadding: 0
            iconSource : FluentIcons.Pinned
            Layout.alignment: Qt.AlignVCenter
            iconSize: 14
            visible: {
                if(!(d.win instanceof FluWindow)){
                    return false
                }
                return showStayTop
            }
            text:d.stayTop ? control.stayTopCancelText : control.stayTopText
            radius: 0
            iconColor: d.stayTop ? FluTheme.primaryColor : control.textColor
            onClicked: stayTopClickListener()
        }
        FluIconButton{
            id:btn_minimize
            Layout.preferredWidth: 46
            Layout.fillHeight: true
            padding: 0
            verticalPadding: 0
            horizontalPadding: 0
            iconSource : FluentIcons.ChromeMinimize
            Layout.alignment: Qt.AlignVCenter
            iconSize: 10
            text:minimizeText
            radius: 0
            visible: !isMac && showMinimize
            iconColor: control.textColor
            color: {
                if(pressed){
                    return minimizePressColor
                }
                return hovered ? minimizeHoverColor : minimizeNormalColor
            }
            onClicked: minClickListener()
        }
        FluIconButton{
            id:btn_maximize
            property bool hover: btn_maximize.hovered
            Layout.preferredWidth: 46
            Layout.fillHeight: true
            padding: 0
            verticalPadding: 0
            horizontalPadding: 0
            iconSource : d.isRestore  ? FluentIcons.ChromeRestore : FluentIcons.ChromeMaximize
            color: {
                if(down){
                    return maximizePressColor
                }
                return btn_maximize.hover ? maximizeHoverColor : maximizeNormalColor
            }
            Layout.alignment: Qt.AlignVCenter
            visible: d.resizable && !isMac && showMaximize
            radius: 0
            iconColor: control.textColor
            text:d.isRestore?restoreText:maximizeText
            iconSize: 10
            onClicked: maxClickListener()
        }
        FluIconButton{
            id:btn_close
            Layout.preferredWidth: 46
            Layout.fillHeight: true
            padding: 0
            verticalPadding: 0
            horizontalPadding: 0
            iconSource : FluentIcons.ChromeClose
            Layout.alignment: Qt.AlignVCenter
            text:closeText
            visible: !isMac && showClose
            radius: 0
            iconSize: 10
            // 悬停时字形转白；**按下时再暗一档，不是纯白** —— 这是 Windows 的标准行为，
            // 由他悬停/按住分别截图量出来的：
            //   悬停：接近纯白（量到 #F4F6FC，≈96% 白）
            //   按下：白色约 67% 不透明 —— 压在按下态那个红(#C53D30)上就是 #EDBEBB，
            //         三个通道分别反算出来的比例都是 0.67，所以这个数不是猜的。
            // 平时（没悬停没按下）跟标题栏文字走。
            iconColor: pressed     ? Qt.rgba(1, 1, 1, 0.67)
                     : hovered     ? Qt.rgba(1, 1, 1, 1)
                                   : control.textColor
            color:{
                if(pressed){
                    return closePressColor
                }
                return hovered ? closeHoverColor : closeNormalColor
            }
            onClicked: closeClickListener()
        }
    }
    FluLoader{
        id:layout_macos_buttons
        anchors{
            verticalCenter: parent.verticalCenter
            left: parent.left
            leftMargin: 10
        }
        sourceComponent: isMac ? com_macos_buttons : undefined
    }
}
