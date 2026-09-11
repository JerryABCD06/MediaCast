import QtQuick
import FluentUI

// TopNav —— 顶部的多页签导航条（带图标、支持左右分列）。
//
// ⚠️ **目前停用中。** 现役的顶栏是 NewUiWindow 里那个 FluPivot。
//
// "停用"的意思是：这个文件留着，但**没有列进 res/QmlUi.qrc**，所以不会被
// 编进 exe、也不会被 QML 引擎看到 —— 放在那儿纯粹是备查和以后复用。
//
// 要用的时候三件事：
//   一、在 res/QmlUi.qrc 里加一行
//       <file alias="TopNav.qml">../src/ui/qml/TopNav.qml</file>
//   二、在 CMakeLists.txt 的 qt_add_translations SOURCES 里加一行
//       （这个文件本身不含字符串，但 NewUiWindow 到时候会多出模型里的条目）
//   三、把 NewUiWindow 里的 FluPivot 换成它 —— 接口是照着 FluPivot 的
//       意思设计的：给一串 model，读写 currentIndex
//
// 留着的理由：FluPivot 有两个做不到的事，而这个实现两个都做到了 ——
//
//   一、**图标。** FluPivot 的 delegate 只读 modelData.title，塞不进图标。
//   二、**左右分列。** FluPivot 内部是一个横向 ListView，条目只能挨着排；
//       这个实现能把一部分条目标到左边、一部分标到右边（就是当初要的
//       「左边投屏、右边设置」）。
//
// 它只做两件事：把条目画出来、报告"现在选了第几个"。
// **它不认识任何页面**，页面也不认识它 —— 两边只通过 currentIndex 说话。
Item {
    id: root

    /// 条目列表：[{ title, icon, align }]
    ///   align —— "left"（默认）或 "right"
    ///   icon  —— FluentIcons 的枚举值；0 或不写就是不画图标
    ///           （FluIcon 在 iconSource 为 0 时自己就是透明的）
    property var model: []

    /// 当前选中的下标。外部读写它就行，不用管里面怎么画的。
    property int currentIndex: 0

    property color normalColor: FluTheme.fontSecondaryColor
    property color hoverColor: FluTheme.fontPrimaryColor
    property color highlightColor: FluTheme.fontPrimaryColor

    /// 条目离窗口左右边的距离。分隔线是通到底的，只有条目往里缩。
    property int horizontalPadding: 16

    implicitHeight: 48
    implicitWidth: 320

    // 把 model 分成左右两组，并且**把原始下标带上**。
    // 不带的话 delegate 里就不知道自己对应哪一项，点谁都选不中。
    function itemsFor(side) {
        var out = []
        for (var i = 0; i < model.length; ++i) {
            var item = model[i]
            var align = item.align === undefined ? "left" : item.align
            if (align === side)
                out.push({ index: i, title: item.title, icon: item.icon })
        }
        return out
    }

    Row {
        anchors.left: parent.left
        anchors.leftMargin: root.horizontalPadding
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        Repeater {
            model: root.itemsFor("left")
            delegate: navItem
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: root.horizontalPadding
        anchors.verticalCenter: parent.verticalCenter
        spacing: 8
        Repeater {
            model: root.itemsFor("right")
            delegate: navItem
        }
    }

    // 底下一条分隔线，和 FluentUI 其它地方用的分隔线一致。
    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: FluTheme.dividerColor
    }

    Component {
        id: navItem

        Item {
            id: itemRoot
            height: root.height
            width: content.width + 32

            readonly property bool selected: root.currentIndex === modelData.index

            // 选中、悬停、普通三档颜色。图标和文字共用这一个 ——
            // 只改文字不改图标的话，看起来像没做完。
            readonly property color effectiveColor:
                selected ? root.highlightColor
                : mouse.containsMouse ? root.hoverColor
                : root.normalColor

            Row {
                id: content
                anchors.centerIn: parent
                spacing: 8

                FluIcon {
                    anchors.verticalCenter: parent.verticalCenter
                    iconSource: modelData.icon === undefined ? 0 : modelData.icon
                    iconSize: 16
                    iconColor: itemRoot.effectiveColor
                }

                FluText {
                    anchors.verticalCenter: parent.verticalCenter
                    text: modelData.title
                    font: FluTextStyle.Body
                    textColor: itemRoot.effectiveColor
                }
            }

            // 当前项下面那道线。3 像素圆角、主题强调色 —— 和 FluPivot 一致。
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 3
                radius: 1.5
                color: FluTheme.primaryColor
                opacity: itemRoot.selected ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: FluTheme.animationEnabled ? 120 : 0 }
                }
            }

            MouseArea {
                id: mouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.currentIndex = modelData.index
            }
        }
    }
}
