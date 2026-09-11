pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

Rectangle {
    id: root

    required property var modelData
    property bool rowHighlight: false
    signal rowClicked()

    readonly property color selColor:    "#214283"
    readonly property color textBright:  "#e8e8e8"
    readonly property color textColor:   "#bbbbbb"
    readonly property color textMuted:   "#7d7d7d"
    readonly property color accentColor: "#4c96d8"
    readonly property color chevronDim:  "#6f767e"
    readonly property color folderColor: "#c8b74f"
    readonly property color imageColor:  "#d7a85b"

    readonly property bool isFolder: modelData && modelData.kind === "folder"

    IconProvider { id: icons }

    implicitHeight: isFolder ? 28 : 24
    radius: 3

    /*
     * 只有当前选中项有蓝色高亮，鼠标悬停不再变色。
     *
     * 原来的 hoverColor 灰底有两个问题：
     * 一是窗口被缩放 / 最小化 / 鼠标移出窗口时，containsMouse
     * 有时不会被重置，灰色的悬停块就留在列表里好几个；
     * 二是列表本身已经有蓝色选中条，再多一种底色反而乱。
     */
    color: rowHighlight ? selColor : "transparent"

    RowLayout {
        anchors.fill: parent; spacing: 4
        anchors.leftMargin: isFolder ? 6 : 22
        anchors.rightMargin: 6
        AppIcon {
            visible: isFolder
            provider: icons
            kind: modelData.expanded ? "chevron-down" : "chevron-right"
            tint: rowHighlight ? textBright : chevronDim
            size: 11
            Layout.alignment: Qt.AlignVCenter
        }
        Item { width: 2; height: 1 }
        AppIcon {
            provider: icons
            kind: isFolder ? "folder" : (modelData.item.type === "image" ? "image" : "file")
            tint: isFolder ? folderColor : (modelData.item.type === "image" ? imageColor : accentColor)
            size: 13
            Layout.alignment: Qt.AlignVCenter
        }
        Label {
            text: isFolder
                  ? modelData.label
                  : (modelData.item.type === "image" ? "图片 · " + modelData.item.title : modelData.item.title)
            color: isFolder ? textBright : textColor
            font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true
        }
        Label {
            text: isFolder ? modelData.count : ""
            color: textMuted; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter
        }
    }
    MouseArea {
        // hoverEnabled 关掉：这个委托已经没有悬停样式了，
        // 留着它只会不断产生 containsMouse 变化通知
        id: mouse; anchors.fill: parent
        onClicked: root.rowClicked()
    }
}