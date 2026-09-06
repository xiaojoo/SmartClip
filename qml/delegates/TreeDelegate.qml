pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

// 树行（模版样式：直角行、蓝色文件夹、柔和蓝选中）
Rectangle {
    id: root
    required property var modelData
    property bool rowHighlight: false
    signal rowClicked()

    readonly property color selColor:    "#2e436e"
    readonly property color hoverColor:  "#34363a"
    readonly property color textBright:  "#ced0d6"
    readonly property color textColor:   "#b4b8bf"
    readonly property color textMuted:   "#6f737a"
    readonly property color chevronDim:  "#6f737a"
    readonly property color folderColor: "#549bf5"
    readonly property color fileColor:   "#8b929e"
    readonly property color imageColor:  "#d7a85b"

    readonly property bool isFolder: modelData && modelData.kind === "folder"

    IconProvider { id: icons }

    implicitHeight: isFolder ? 26 : 24
    radius: 0
    color: rowHighlight ? selColor : (mouse.containsMouse ? hoverColor : "transparent")

    RowLayout {
        anchors.fill: parent; spacing: 4
        anchors.leftMargin: isFolder ? 6 : 24
        anchors.rightMargin: 8
        AppIcon { visible: isFolder; provider: icons
                  kind: modelData.expanded ? "chevron-down" : "chevron-right"
                  tint: rowHighlight ? textBright : chevronDim; size: 11
                  Layout.alignment: Qt.AlignVCenter }
        Item { width: 2; height: 1 }
        AppIcon { provider: icons
                  kind: isFolder ? "folder" : (modelData.item.type === "image" ? "image" : "file")
                  tint: isFolder ? folderColor : (modelData.item.type === "image" ? imageColor : fileColor)
                  size: 13; Layout.alignment: Qt.AlignVCenter }
        Label { text: isFolder ? modelData.label
                  : (modelData.item.type === "image" ? "图片 · " + modelData.item.title : modelData.item.title)
                color: isFolder ? textBright : textColor
                font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
        Label { text: isFolder ? modelData.count : ""
                color: textMuted; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter }
    }
    MouseArea { id: mouse; anchors.fill: parent; hoverEnabled: true; onClicked: root.rowClicked() }
}