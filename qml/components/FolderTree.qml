pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../delegates"
import "../utils"

// Left "Project" panel: header + date-folder tree + 外部库/回收站 footer.
Rectangle {
    id: root

    color: "#3c3f41"; border.color: "#4b4d4f"; border.width: 1

    property var rows: []
    property string activeKey: "today"
    property var selected: null

    signal folderClicked(string key)
    signal itemClicked(var item)

    readonly property color borderColor: "#4b4d4f"
    readonly property color headerColor:  "#3a3c3f"
    readonly property color accentColor:  "#4c96d8"
    readonly property color textColor:    "#bbbbbb"
    readonly property color textMuted:    "#7d7d7d"

    IconProvider { id: icons }

    ColumnLayout {
        anchors.fill: parent; spacing: 0

        // panel header
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 30; color: headerColor; border.color: borderColor; border.width: 1
            RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                IconImage { source: icons.path("chevron-down"); color: accentColor; width: 12; height: 12 }
                Label { text: "项目"; color: textColor; font.pixelSize: 12; font.bold: true }
                Item { Layout.fillWidth: true }
                IconImage { source: icons.path("grid"); color: textMuted; width: 13; height: 13 }
                IconImage { source: icons.path("more"); color: "#d4b652"; width: 14; height: 14 }
            }
        }

        // tree
        ListView {
            id: view
            Layout.fillWidth: true; Layout.fillHeight: true; clip: true
            model: root.rows
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            delegate: Component {
                TreeDelegate {
                    rowData: modelData
                    width: view.width
                    rowHighlight: modelData.kind === "folder"
                                  ? root.activeKey === modelData.key
                                  : (root.selected && root.selected.id === modelData.item.id)
                    onRowClicked: modelData.kind === "folder"
                                  ? root.folderClicked(modelData.key)
                                  : root.itemClicked(modelData.item)
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: borderColor }
        Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }
        RowLayout { Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 8; spacing: 8
            IconImage { source: icons.path("chevron-down"); color: accentColor; width: 11; height: 11 }
            Label { text: "外部库"; color: textColor; font.pixelSize: 12 }
            Item { Layout.fillWidth: true }
        }
        RowLayout { Layout.fillWidth: true; Layout.preferredHeight: 24; Layout.leftMargin: 34; Layout.rightMargin: 8; spacing: 8
            IconImage { source: icons.path("trash"); color: textMuted; width: 12; height: 12 }
            Label { text: "回收站"; color: textColor; font.pixelSize: 12; Layout.fillWidth: true }
        }
        Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }
    }
}
