pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: root

    property var entries: []
    signal selected(string act)

    readonly property color bgColor:     "#3c3f41"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor:   "#bbbbbb"
    readonly property color hoverColor:  "#46484a"

    function openFor(anchor, items) {
        entries = items
        dd.width = 200
        dd.height = (items ? items.length : 0) * 30 + 8
        var pos = anchor.mapToItem(root, 0, anchor.height)
        dd.x = Math.max(2, Math.min(pos.x, root.width - dd.width - 4))
        dd.y = pos.y + 3
        dd.open()
    }

    Popup {
        id: dd
        x: 0; y: 0
        padding: 4
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
        background: Rectangle { color: bgColor; radius: 5; border.color: borderColor }
        Column {
            anchors.fill: parent; spacing: 2
            Repeater {
                model: root.entries
                delegate: Rectangle {
                    required property var modelData
                    width: parent.width; height: 28; radius: 4
                    color: dh.containsMouse ? hoverColor : "transparent"
                    Label {
                        anchors.fill: parent; anchors.leftMargin: 10
                        verticalAlignment: Text.AlignVCenter
                        text: modelData.label; color: textColor; font.pixelSize: 13
                    }
                    MouseArea {
                        id: dh; anchors.fill: parent; hoverEnabled: true
                        onClicked: { root.selected(modelData.act); dd.close() }
                    }
                }
            }
        }
    }
}