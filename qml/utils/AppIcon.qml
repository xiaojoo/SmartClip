pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts

// Tinted vector icon: renders IconProvider.svg(kind, tint) as an Image.
// (IconImage is only available as a private QtQuick.Controls.impl type.)
Image {
    id: root

    property var provider: null
    property string kind: ""
    property color tint: "#9aa0a8"
    property int size: 14

    width: size; height: size
    Layout.preferredWidth: size
    Layout.preferredHeight: size

    source: root.provider && root.kind ? root.provider.svg(root.kind, root.tint) : ""
    sourceSize.width: size * 2
    sourceSize.height: size * 2
    fillMode: Image.PreserveAspectFit
    antialiasing: true
}
