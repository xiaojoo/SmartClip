pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

/*
 * IDE 风格下拉菜单。
 *
 * 这里刻意不用 Popup：底部 tabbar 的菜单需要向上弹，
 * 而 Popup 的位置会走平台层（Windows 上还会被屏幕可用区域
 * 二次调整），实测在贴边的菜单栏上会算错、被切一半。
 * 改成纯场景内的 Item 覆盖层，坐标就是窗口坐标，
 * 上下左右都能自己夹紧在窗口内，位置完全可控。
 */
Item {
    id: root

    /*
     * 之前的 Popup 是独立图层，永远画在最上面；
     * 换成普通 Item 之后就必须自己抬 z，
     * 否则会被后面声明的工具栏 / 列表 / 底部栏盖住（菜单“打不开”）。
     * 比 Main.qml 里四边 resize 热区的 z:1000 低即可。
     */
    z: 900

    property var entries: []
    signal selected(string act)

    readonly property color bgColor:     "#3c3f41"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor:   "#bbbbbb"
    readonly property color hoverColor:  "#46484a"

    // 菜单在窗口内的位置与尺寸
    readonly property real menuWidth: 200
    readonly property real menuHeight: (entries ? entries.length : 0) * 30 + 8
    property real menuX: 0
    property real menuY: 0

    // 覆盖层是否可见（等价于原来 Popup 的 opened）
    property bool opened: false

    /*
     * 打开菜单的那一下点击会带来一个“鼠标被夺走”的合成事件，
     * 直接落在这个闭合层上，会把刚打开的菜单瞬间关掉
     * （表现为点了菜单栏没反应）。所以闭合层在收到事件后
     * 延后一帧再真正关闭，让那次合成事件自己过期。
     */
    Timer {
        id: closeGuard
        interval: 0
        onTriggered: root.opened = false
    }

    function openFor(anchor, items) {
        entries = items

        var pos = anchor.mapToItem(root, 0, 0)

        // 贴着底部菜单栏时下面没地方，向上弹；两边都放不下就夹在窗口内
        var below = pos.y + anchor.height + 3
        var above = pos.y - menuHeight - 3
        menuY = (below + menuHeight + 4 <= root.height) ? below
              : (above >= 4 ? above : Math.max(4, root.height - menuHeight - 4))

        menuX = Math.max(2, Math.min(pos.x, root.width - menuWidth - 4))
        opened = true
        root.forceActiveFocus()
    }

    function close() { closeGuard.stop(); opened = false }

    // 菜单打开时按 Esc 关闭
    Keys.onEscapePressed: (event) => { if (opened) { close(); event.accepted = true } }

    // 点击别处关闭。注意这一层会吃掉那次点击（和 Popup 的
    // CloseOnPressOutside 一样），不会穿透到下面的列表。
    MouseArea {
        anchors.fill: parent
        visible: root.opened
        acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
        onPressed: closeGuard.restart()
    }

    Rectangle {
        id: dd
        visible: root.opened
        x: root.menuX
        y: root.menuY
        width: root.menuWidth
        height: root.menuHeight
        color: root.bgColor
        radius: 5
        border.color: root.borderColor

        Column {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 2
            Repeater {
                model: root.entries
                delegate: Rectangle {
                    required property var modelData
                    width: parent.width; height: 28; radius: 4
                    color: dh.containsMouse ? root.hoverColor : "transparent"
                    Label {
                        anchors.fill: parent; anchors.leftMargin: 10
                        verticalAlignment: Text.AlignVCenter
                        text: modelData.label; color: root.textColor; font.pixelSize: 13
                    }
                    MouseArea {
                        id: dh; anchors.fill: parent; hoverEnabled: true
                        onClicked: { root.selected(modelData.act); root.close() }
                    }
                }
            }
        }
    }
}
