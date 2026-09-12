pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

/*
 * 工具栏按钮。
 *
 * 主流编辑器工具栏的按钮就三件事：图标、悬停高亮、工具提示带快捷键。
 * 再多的（文字标签 / 下拉箭头 / 选中态）都是可选的开关，这里都留了属性。
 *
 * 注意 enabled 用的是 Item 自带的那个 —— 子项会一起禁用，MouseArea
 * 收不到点击，光标也自动回到箭头。
 */
Rectangle {
    id: root

    property var provider: null
    property string kind: ""
    property string label: ""
    property string tip: ""
    property string shortcut: ""
    property bool checked: false
    property bool showArrow: false

    signal clicked()

    readonly property bool hot: hit.containsMouse && root.enabled
    readonly property color idleIcon: root.enabled ? "#9aa0a8" : "#5c6066"
    readonly property color hotIcon: root.checked ? "#ffffff" : "#e8e8e8"

    implicitWidth: content.implicitWidth + 16
    implicitHeight: 28
    radius: 4

    color: !root.enabled ? "transparent"
                          : root.checked ? "#3d78b8"
                                         : (root.hot ? "#45484c" : "transparent")

    RowLayout {
        id: content

        anchors.centerIn: parent
        spacing: 5

        AppIcon {
            visible: root.kind !== ""
            provider: root.provider
            kind: root.kind
            size: 16
            tint: (root.hot || root.checked) ? root.hotIcon : root.idleIcon
        }

        Label {
            visible: root.label !== ""
            text: root.label
            font.pixelSize: 11
            color: (root.hot || root.checked) ? root.hotIcon : "#b4b8bf"
        }

        AppIcon {
            visible: root.showArrow
            provider: root.provider
            kind: "chevron-down"
            size: 9
            tint: (root.hot || root.checked) ? root.hotIcon : "#7d838c"
        }
    }

    MouseArea {
        id: hit
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: root.clicked()
    }

    ToolTip.visible: hit.containsMouse && root.tip !== ""
    ToolTip.text: root.shortcut !== "" ? root.tip + "  (" + root.shortcut + ")" : root.tip
    ToolTip.delay: 420
}
