pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../delegates"
import "../utils"

Rectangle {
    id: root
    color: "#1e1f22"
    radius: 10
    clip: true
    border.width: 0

    property var rows: []
    property string activeKey: "today"
    property var selected: null

    signal folderClicked(string key)
    signal itemClicked(var item)

    readonly property color borderColor: "#43454a"
    readonly property color textBright:  "#ced0d6"
    readonly property color textMuted:   "#6f737a"

    IconProvider { id: icons }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            color: "transparent"
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                spacing: 6
                Label { text: "项目"; color: root.textBright; font.pixelSize: 12; font.bold: true }
                AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 10 }
                Item { Layout.fillWidth: true }
                AppIcon { provider: icons; kind: "grid"; tint: root.textMuted; size: 13 }
                AppIcon { provider: icons; kind: "more"; tint: root.textMuted; size: 14 }
            }
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: root.borderColor
            }
        }

        ListView {
            id: view
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.rows
            ScrollBar.vertical: ThinScrollBar {}
            delegate: TreeDelegate {
                width: view.width
                rowHighlight: modelData.kind === "folder"
                              ? root.activeKey === modelData.key
                              : (root.selected && root.selected.id === modelData.item.id)
                onRowClicked: modelData.kind === "folder"
                              ? root.folderClicked(modelData.key)
                              : root.itemClicked(modelData.item)
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: borderColor }

        Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 8
            spacing: 6
            Label { text: "外部库"; color: root.textMuted; font.pixelSize: 12; Layout.fillWidth: true }
            AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 10 }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 24
            Layout.leftMargin: 34
            Layout.rightMargin: 8
            spacing: 6
            AppIcon { provider: icons; kind: "trash"; tint: root.textMuted; size: 12 }
            Label { text: "回收站"; color: root.textMuted; font.pixelSize: 12; Layout.fillWidth: true }
        }

        Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }
    }
}