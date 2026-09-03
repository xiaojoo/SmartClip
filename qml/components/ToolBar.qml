pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

// Main toolbar: run-config box, actions, and the global search field.
Rectangle {
    id: root

    implicitHeight: 38
    color: "#3c3f41"; border.color: "#4b4d4f"; border.width: 1

    signal searchChanged(string text)
    signal requestRefresh()
    signal openMenu(Item anchor, var items)

    readonly property color borderColor: "#4b4d4f"
    readonly property color iconColor:   "#9aa0a8"
    readonly property color accentColor: "#4c96d8"
    readonly property color textColor:   "#bbbbbb"
    readonly property color fieldBg:     "#3a3c3f"
    readonly property color boxBg:       "#3f4144"
    readonly property color boxHover:    "#454749"

    IconProvider { id: icons }

    function folderItems() {
        return [ { label: "今天",  act: "folder:today" },
                 { label: "昨天",  act: "folder:yesterday" },
                 { label: "近 7 天", act: "folder:week" },
                 { label: "更早",  act: "folder:older" } ]
    }
    function toolItems() {
        return [{ label: "设置", act: "none" }, { label: "关于 SmartClip", act: "none" }]
    }
    function clearSearch() { field.text = "" }

    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 8
        AppIcon { provider: icons; kind: "chevron-left";  tint: iconColor; size: 15 }
        AppIcon { provider: icons; kind: "chevron-right"; tint: iconColor; size: 15 }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; Layout.alignment: Qt.AlignVCenter; color: borderColor }

        // module / run-config box (opens date-folder menu)
        Rectangle {
            id: combo
            Layout.preferredWidth: 168; Layout.preferredHeight: 28; radius: 4
            color: boxBg; border.color: borderColor
            RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 6
                AppIcon { provider: icons; kind: "chevron-down"; tint: accentColor; size: 12 }
                Label { text: "SmartClip"; color: textColor; font.pixelSize: 12; Layout.fillWidth: true }
            }
            MouseArea { anchors.fill: parent; hoverEnabled: true
                onEntered: parent.color = boxHover; onExited: parent.color = boxBg
                onClicked: root.openMenu(combo, root.folderItems()) }
        }
        Rectangle { Layout.preferredWidth: 26; Layout.preferredHeight: 26; radius: 4; color: "#2f6f4f"
            AppIcon { anchors.centerIn: parent; provider: icons; kind: "play"; tint: "#c8f2d8"; size: 12 }
            MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: root.requestRefresh() } }
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; Layout.alignment: Qt.AlignVCenter; color: borderColor }
        AppIcon { provider: icons; kind: "refresh"; tint: iconColor; size: 15
            MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: root.requestRefresh() } }
        AppIcon { provider: icons; kind: "close"; tint: iconColor; size: 15
            MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: root.clearSearch() } }

        Item { Layout.fillWidth: true }

        // search field
        Rectangle {
            Layout.preferredWidth: 300; Layout.preferredHeight: 28; radius: 4
            color: fieldBg; border.color: borderColor
            RowLayout { anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 9; spacing: 6
                AppIcon { provider: icons; kind: "search"; tint: iconColor; size: 13 }
                TextField {
                    id: field; Layout.fillWidth: true; Layout.fillHeight: true
                    placeholderText: "搜索剪贴内容"; placeholderTextColor: "#6d737a"; color: textColor
                    font.pixelSize: 12
                    background: Item {}
                    verticalAlignment: TextInput.AlignVCenter
                    onTextChanged: root.searchChanged(text)
                }
            }
        }
        Rectangle { Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 4; color: boxBg; border.color: borderColor
            AppIcon { anchors.centerIn: parent; provider: icons; kind: "gear"; tint: iconColor; size: 16 }
            MouseArea { anchors.fill: parent; hoverEnabled: true
                onEntered: parent.color = boxHover; onExited: parent.color = boxBg
                onClicked: root.openMenu(parent, root.toolItems()) } }
    }
}
