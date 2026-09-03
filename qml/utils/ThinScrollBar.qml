pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

// Thin, dark scrollbar: transparent track, only a slim dark line as handle.
ScrollBar {
    id: root

    policy: ScrollBar.AsNeeded

    implicitWidth: orientation === Qt.Vertical ? 6 : 0
    implicitHeight: orientation === Qt.Horizontal ? 6 : 0

    background: Rectangle { color: "transparent" }

    contentItem: Rectangle {
        implicitWidth: 6
        implicitHeight: 6
        radius: 3
        color: "#565a60"
        opacity: 0.9
    }
}
