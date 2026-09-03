pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../utils"

// One row of the folder/date tree: a folder header or a clipboard item.
Rectangle {
    id: root

    property var rowData: null
    property bool rowHighlight: false
    signal rowClicked()

    readonly property color selColor:    "#214283"
    readonly property color hoverColor:  "#46484a"
    readonly property color textBright:  "#e8e8e8"
    readonly property color textColor:   "#bbbbbb"
    readonly property color textMuted:   "#7d7d7d"
    readonly property color accentColor: "#4c96d8"
    readonly property color chevronDim:  "#6f767e"
    readonly property color folderColor: "#c8b74f"
    readonly property color imageColor:  "#d7a85b"

    IconProvider { id: icons }

    implicitHeight: rowData && rowData.kind === "folder" ? 28 : 24
    radius: 3
    color: rowHighlight ? selColor : (mouse.containsMouse ? hoverColor : "transparent")

    RowLayout {
        anchors.fill: parent; spacing: 4
        anchors.leftMargin: rowData && rowData.kind === "folder" ? 6 : 24
        anchors.rightMargin: 8
        IconImage {
            visible: rowData && rowData.kind === "folder"
            source: rowData && rowData.expanded ? icons.path("chevron-down") : icons.path("chevron-right")
            color: rowHighlight ? textBright : chevronDim
            width: 11; height: 11
            Layout.alignment: Qt.AlignVCenter
        }
        Item { width: 2; height: 1 }
        IconImage {
            source: rowData && rowData.kind === "folder" ? icons.path("folder")
                  : rowData && rowData.item && rowData.item.type === "image" ? icons.path("image")
                  : icons.path("file")
            color: rowData && rowData.kind === "folder" ? folderColor
                 : rowData && rowData.item && rowData.item.type === "image" ? imageColor
                 : accentColor
            width: 13; height: 13
            Layout.alignment: Qt.AlignVCenter
        }
        Label {
            text: rowData && rowData.kind === "folder"
                  ? rowData.label
                  : (rowData.item && rowData.item.type === "image" ? "图片 · " + rowData.item.title : rowData.item.title)
            color: rowData && rowData.kind === "folder" ? textBright : textColor
            font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true
        }
        Label {
            text: rowData && rowData.kind === "folder" ? rowData.count : ""
            color: textMuted; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter
        }
    }
    MouseArea {
        id: mouse; anchors.fill: parent; hoverEnabled: true
        onClicked: root.rowClicked()
    }
}
