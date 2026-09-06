pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

Rectangle {
    id: root

    implicitHeight: 26
    color: "#313335"; border.color: "#4b4d4f"; border.width: 1

    property string title: ""
    property int count: 0
    property bool copied: false

    readonly property color mutedColor: "#77808c"
    readonly property color textColor:  "#7d7d7d"

    IconProvider { id: icons }

    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 12
        Text { text: "SmartClip"; color: mutedColor; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; Layout.alignment: Qt.AlignVCenter; color: "#4b4d4f" }
        Label { text: root.title; color: textColor; font.pixelSize: 11 }
        Item { Layout.fillWidth: true }
        Label { text: root.copied ? "已复制" : "自动采集"; color: textColor; font.pixelSize: 11 }
        Label { text: root.count + " 项"; color: mutedColor; font.pixelSize: 11 }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; Layout.alignment: Qt.AlignVCenter; color: "#4b4d4f" }
        AppIcon { provider: icons; kind: "branch"; tint: "#8b929e"; size: 12 }
        Label { text: "分支: main"; color: textColor; font.pixelSize: 11 }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; Layout.alignment: Qt.AlignVCenter; color: "#4b4d4f" }
        Label { text: "UTF-8"; color: textColor; font.pixelSize: 11 }
    }
}