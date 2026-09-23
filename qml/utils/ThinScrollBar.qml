pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import SmartClip.Globals 1.0

// Thin, dark scrollbar: fully transparent track, only a slim dark line handle.
ScrollBar {
    id: root

    policy: ScrollBar.AsNeeded

    implicitWidth: orientation === Qt.Vertical ? 8 : 0
    implicitHeight: orientation === Qt.Horizontal ? 8 : 0

    // Hide when there is nothing to scroll (content fully visible).
    visible: root.size < 1.0

    background: Item {}

    contentItem: Rectangle {
        implicitWidth: 8
        implicitHeight: 8
        radius: 4
        color: Theme.c("#565a60", Theme.light)
        opacity: 0.9
    }
}

