pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Thin, dark scrollbar: fully transparent track, only a slim dark line handle.
ScrollBar {
    id: root

    policy: ScrollBar.AsNeeded

    implicitWidth: orientation === Qt.Vertical ? 5 : 0
    implicitHeight: orientation === Qt.Horizontal ? 5 : 0

    background: Item {}

    contentItem: Rectangle {
        implicitWidth: 5
        implicitHeight: 5
        radius: 2.5
        color: "#494d51"
        opacity: 0.8
    }
}
