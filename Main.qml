import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "qml/components"
import "qml/models"
import "qml/utils"
import "js/FolderManager.js" as Folders
import "js/TimeUtils.js" as Time

ApplicationWindow {
    id: window
    width: 1460; height: 900; minimumWidth: 1000; minimumHeight: 640; visible: true
    title: "SmartClip — 剪贴板"
    color: "#2b2d30"

    property string searchText: ""
    property var selectedItem: null
    property string activeFolder: "today"
    property var treeRows: []
    property bool showWhatTab: true

    readonly property var folders: [
        { key: "today",     label: "今天" },
        { key: "yesterday", label: "昨天" },
        { key: "week",      label: "近 7 天" },
        { key: "older",     label: "更早" }
    ]
    property var expanded: ({ "today": true, "yesterday": false, "week": false, "older": false })

    ClipboardModel { id: cbm; onChanged: window.rebuild() }

    function rebuild() { treeRows = Folders.buildTree(cbm.entries, folders, expanded, Time.periodFor) }
    function refresh() { cbm.reload(searchText) }
    function toggleFolder(key) {
        var e = ({})
        for (var k in expanded) e[k] = expanded[k]
        e[key] = !e[key]
        expanded = e; activeFolder = key; selectedItem = null; showWhatTab = true
        rebuild()
    }
    function activateFolder(key) {
        var e = ({})
        for (var k in expanded) e[k] = expanded[k]
        e[key] = true
        expanded = e; activeFolder = key; selectedItem = null; showWhatTab = true
        rebuild()
    }
    function selectItem(item) {
        selectedItem = item; showWhatTab = false
        if (item) Store.copyItem(item.id)
    }
    function handleCommand(act) {
        if (act === "refresh") refresh()
        else if (act === "quit") window.close()
        else if (act === "copy") { if (selectedItem) Store.copyItem(selectedItem.id) }
        else if (act === "clearsearch") { searchText = ""; toolBar.clearSearch() }
        else if (act.indexOf("folder:") === 0) activateFolder(act.substring(7))
        else if (act.indexOf("menu:") === 0) toolBar.openGroup(act.substring(5))
    }

    Component.onCompleted: refresh()
    Connections { target: clipboardStore; function onChanged() { window.refresh() } }

    DropdownMenu { id: ddMenu; anchors.fill: parent; onSelected: (act) => window.handleCommand(act) }

    ColumnLayout {
        anchors.fill: parent; spacing: 0

        MainToolBar {
            id: toolBar
            Layout.fillWidth: true
            onOpenMenu: (anchor, items) => ddMenu.openFor(anchor, items)
            onRequestRefresh: () => window.refresh()
            onSearchChanged: (text) => { window.searchText = text; window.refresh() }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0

            // ---- 左侧工具窗口图标条（保持原样） ----
            Rectangle {
                Layout.fillHeight: true; Layout.preferredWidth: 34
                color: "#3c3f41"; border.color: "#4b4d4f"; border.width: 1
                IconProvider { id: stripIcons }
                Column {
                    anchors.fill: parent; anchors.topMargin: 8; anchors.bottomMargin: 8; spacing: 6
                    Repeater {
                        model: [ { k: "folder", active: true }, { k: "file", active: false },
                                 { k: "search", active: false }, { k: "play", active: false },
                                 { k: "branch", active: false } ]
                        delegate: Rectangle {
                            required property var modelData
                            width: 26; height: 26; x: 4; radius: 5
                            color: modelData.active ? "#3a4a5a" : "transparent"
                            AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: modelData.k
                                      tint: modelData.active ? "#4c96d8" : "#9aa0a8"; size: 16 }
                            MouseArea { anchors.fill: parent; hoverEnabled: true
                                onEntered: if (!modelData.active) parent.color = "#454749"
                                onExited: if (!modelData.active) parent.color = "transparent" }
                        }
                    }
                    Item { width: 1; height: Math.max(1, parent.height - 300) }
                    Rectangle { width: 26; height: 26; x: 4; radius: 5; color: "transparent"
                        AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: "gear"; tint: "#9aa0a8"; size: 16 } }
                }
            }

            FolderTree {
                id: folderTree
                Layout.fillHeight: true; Layout.preferredWidth: 300
                Layout.topMargin: 8
                Layout.bottomMargin: 8
                rows: window.treeRows
                activeKey: window.activeFolder
                selected: window.selectedItem
                onFolderClicked: (key) => window.toggleFolder(key)
                onItemClicked: (item) => window.selectItem(item)
            }

            EditorArea {
                id: editor
                Layout.fillWidth: true; Layout.fillHeight: true
                Layout.leftMargin: 8
                Layout.topMargin: 8
                Layout.bottomMargin: 8
                item: window.selectedItem
                showWelcome: window.showWhatTab
            }
        }

        StatusBar {
            Layout.fillWidth: true
            title: window.selectedItem ? window.selectedItem.title : "README.md"
            count: cbm.entries.length
            copied: window.selectedItem !== null
        }
    }
}