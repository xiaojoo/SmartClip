pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

Rectangle {
    id: root
    implicitHeight: 26
    color: "#3c3f41"; border.color: "#43454a"; border.width: 1

    property string title: ""
    property int count: 0
    property bool copied: false

    readonly property color borderColor: "#43454a"
    readonly property color mutedColor:  "#6f737a"
    readonly property color textColor:   "#8b929e"

    IconProvider { id: icons }

    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 10
        AppIcon { provider: icons; kind: "branch"; tint: root.mutedColor; size: 12 }
        Label { text: "main"; color: root.textColor; font.pixelSize: 11 }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; color: root.borderColor }
        Label { text: root.title; color: root.mutedColor; font.pixelSize: 11
                elide: Text.ElideRight; Layout.fillWidth: true }
        Label { text: root.copied ? "已复制" : "自动采集"; color: root.textColor; font.pixelSize: 11 }
        Label { text: root.count + " 项"; color: root.mutedColor; font.pixelSize: 11 }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; color: root.borderColor }
        Label { text: "UTF-8"; color: root.mutedColor; font.pixelSize: 11 }
    }
    Rectangle { anchors.left: parent.left; anchors.right: parent.right
                anchors.top: parent.top; height: 1; color: borderColor }
}