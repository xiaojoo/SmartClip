pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1460; height: 900; minimumWidth: 1000; minimumHeight: 640; visible: true
    title: "SmartClip — 剪贴板"

    // ---------- IntelliJ "Darcula"-style palette ----------
    readonly property color editorBg:  "#2b2b2b"
    readonly property color panelBg:   "#3c3f41"
    readonly property color barBg:     "#313335"
    readonly property color border:    "#4b4d4f"
    readonly property color textMain:  "#bbbbbb"
    readonly property color textBright:"#e8e8e8"
    readonly property color textMuted: "#7d7d7d"
    readonly property color accent:    "#4c96d8"
    readonly property color selBg:     "#214283"
    readonly property color hoverBg:   "#46484a"
    readonly property color codeText:  "#a9b7c6"
    readonly property color iconMuted: "#9aa0a8"
    readonly property color iconFolder:"#c8b74f"
    readonly property color iconImage: "#d7a85b"

    // ---------- vector icons (inline SVG, tinted by color) ----------
    function iconSvg(kind, color) {
        var c = color
        function p(d) { return '<path d="' + d + '" fill="none" stroke="' + c + '" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round"/>' }
        function f(d) { return '<path d="' + d + '" fill="' + c + '"/>' }
        function ci(x, y, r) { return '<circle cx="' + x + '" cy="' + y + '" r="' + r + '" fill="' + c + '"/>' }
        var s = ""
        if (kind === "chevron-down")  s = p('M4.4 6.4 L8 10 L11.6 6.4')
        else if (kind === "chevron-right") s = p('M6.4 4.4 L10 8 L6.4 11.6')
        else if (kind === "chevron-left")  s = p('M9.6 4.4 L6 8 L9.6 11.6')
        else if (kind === "chevron-up")    s = p('M4.4 9.6 L8 6 L11.6 9.6')
        else if (kind === "play")          s = f('M5.8 3.4 L13.4 8 L5.8 12.6 Z')
        else if (kind === "close")         s = p('M4.6 4.6 L11.4 11.4 M11.4 4.6 L4.6 11.4')
        else if (kind === "plus")          s = p('M8 3.6 L8 12.4 M3.6 8 L12.4 8')
        else if (kind === "refresh")       s = p('M14.4 9.4 A6.4 6.4 0 1 1 12.9 4.6') + p('M15.2 3.4 L15.2 6.8 L11.7 6.3')
        else if (kind === "search")        s = '<circle cx="6.2" cy="6.2" r="4.1" fill="none" stroke="' + c + '" stroke-width="1.7"/>' + p('M9.4 9.4 L13.8 13.8')
        else if (kind === "gear")          s = p('M8 4.9 A3.1 3.1 0 1 0 8 11.1 A3.1 3.1 0 1 0 8 4.9') + (function(){ var t = ""; for (var a = 0; a < 360; a += 45) t += '<line x1="13.2" y1="8" x2="15.4" y2="8" stroke="' + c + '" stroke-width="1.6" stroke-linecap="round" transform="rotate(' + a + ' 8 8)"/>'; return t })()
        else if (kind === "more")          s = ci(8, 3.2, 1.5) + ci(8, 8, 1.5) + ci(8, 12.8, 1.5)
        else if (kind === "dot")           s = ci(8, 8, 2)
        else if (kind === "grid")          s = p('M3 3 H7 V7 H3 Z M9 3 H13 V7 H9 Z M3 9 H7 V13 H3 Z M9 9 H13 V13 H9 Z')
        else if (kind === "branch")        s = ci(5.2, 4.8, 1.7) + ci(5.2, 11.2, 1.7) + ci(11.6, 8, 1.7) + p('M5.2 6.5 L5.2 9.5') + p('M5.2 9.5 C5.2 12 11.6 10.4 11.6 8.4')
        else if (kind === "folder")        s = f('M2.4 4.4 A1.6 1.6 0 0 1 4 2.8 H6.2 L7.6 4.6 H11.8 A1.6 1.6 0 0 1 13.4 6.2 V11 A1.6 1.6 0 0 1 11.8 12.6 H4 A1.6 1.6 0 0 1 2.4 11 Z')
        else if (kind === "file")          s = p('M4 2.6 H8.9 L13.2 6.9 V12.8 A1.2 1.2 0 0 1 12 14 H4 A1.2 1.2 0 0 1 2.8 12.8 V3.8 A1.2 1.2 0 0 1 4 2.6 Z') + p('M8.9 2.6 V6.9 H13.2') + p('M5.8 10.1 H10.2 M5.8 12.1 H10.2')
        else if (kind === "image")         s = p('M2.6 3.6 H13.4 A1.2 1.2 0 0 1 14.6 4.8 V11.2 A1.2 1.2 0 0 1 13.4 12.4 H2.6 A1.2 1.2 0 0 1 1.4 11.2 V4.8 A1.2 1.2 0 0 1 2.6 3.6 Z') + ci(6, 7.1, 1.2) + p('M2 12.3 L6.3 8 L8.8 10.2 L11 8.4 L14 11.2')
        else if (kind === "trash")         s = p('M2.8 4.6 H13.2') + p('M5.3 4.6 V3.4 A1.2 1.2 0 0 1 6.5 2.2 H9.5 A1.2 1.2 0 0 1 10.7 3.4 V4.6') + p('M4.2 4.6 L4.9 12.9 A1.2 1.2 0 0 0 6.1 14 H9.9 A1.2 1.2 0 0 0 11.1 12.9 L11.8 4.6') + p('M6.6 7.6 V11.4 M9.4 7.6 V11.4')
        return "data:image/svg+xml;charset=utf-8," + encodeURIComponent('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">' + s + '</svg>')
    }

    // ---------- clipboard data ----------
    property var entries: []
    property string searchText: ""
    property var selectedItem: null
    property string activeFolder: "today"
    property var treeRows: []
    property bool showWhatTab: true          // true = welcome/README tab, false = selected item

    readonly property var folders: [
        { key: "today",     label: "今天" },
        { key: "yesterday", label: "昨天" },
        { key: "week",      label: "近 7 天" },
        { key: "older",     label: "更早" }
    ]
    property var expanded: ({ "today": true, "yesterday": false, "week": false, "older": false })

    function periodFor(timestamp) {
        var then = new Date(timestamp), today = new Date()
        today.setHours(0, 0, 0, 0); then.setHours(0, 0, 0, 0)
        var days = Math.floor((today.getTime() - then.getTime()) / 86400000)
        if (days <= 0) return "today"
        if (days === 1) return "yesterday"
        if (days <= 7) return "week"
        return "older"
    }
    function itemsInPeriod(key) {
        return entries.filter(function(item) { return periodFor(item.createdAt) === key })
    }
    function periodCount(key) { return itemsInPeriod(key).length }
    function visibleCount() { return entries.length }
    function displayTime(timestamp) { return Qt.formatDateTime(new Date(timestamp), "yyyy-MM-dd HH:mm") }
    function tabTitle() { return selectedItem ? selectedItem.title : "README.md" }

    function buildTree() {
        var rows = []
        for (var i = 0; i < folders.length; i++) {
            var f = folders[i]
            rows.push({ kind: "folder", key: f.key, label: f.label, count: periodCount(f.key), expanded: expanded[f.key] })
            if (expanded[f.key]) {
                var list = itemsInPeriod(f.key)
                for (var j = 0; j < list.length; j++)
                    rows.push({ kind: "item", item: list[j] })
            }
        }
        return rows
    }
    function refresh() {
        entries = clipboardStore.items(searchText)
        treeRows = buildTree()
    }
    function activateFolder(key) {
        var e = ({})
        for (var k in expanded) e[k] = expanded[k]
        e[key] = true
        expanded = e
        activeFolder = key
        selectedItem = null
        showWhatTab = true
        treeRows = buildTree()
    }
    function toggleFolder(key) {
        var e = ({})
        for (var k in expanded) e[k] = expanded[k]
        e[key] = !e[key]
        expanded = e
        activeFolder = key
        selectedItem = null
        showWhatTab = true
        treeRows = buildTree()
    }
    function selectItem(item) {
        selectedItem = item
        showWhatTab = false
        if (item) clipboardStore.copyItem(item.id)
    }

    // ---------- top-bar dropdown menus ----------
    property var ddItems: []

    function hasMenu(label) {
        return label === "文件" || label === "编辑" || label === "视图" ||
               label === "运行" || label === "工具" || label === "帮助"
    }
    function folderItems() {
        return [ { label: "今天",  act: "folder:today" },
                 { label: "昨天",  act: "folder:yesterday" },
                 { label: "近 7 天", act: "folder:week" },
                 { label: "更早",  act: "folder:older" } ]
    }
    function menuItems(label) {
        if (label === "文件") return [{ label: "刷新剪贴板", act: "refresh" }, { label: "退出", act: "quit" }]
        if (label === "编辑") return [{ label: "复制所选", act: "copy" }, { label: "清空搜索", act: "clearsearch" }]
        if (label === "视图") return folderItems()
        if (label === "运行") return [{ label: "重新采集剪贴板", act: "refresh" }]
        if (label === "工具") return [{ label: "设置", act: "none" }, { label: "关于 SmartClip", act: "none" }]
        if (label === "帮助") return [{ label: "使用说明", act: "none" }, { label: "关于", act: "none" }]
        return [{ label: "（暂无）", act: "none" }]
    }
    function showDropdown(anchor, items) {
        ddItems = items
        dd.width = 200
        dd.height = ddItems.length * 30 + 8
        var pos = anchor.mapToItem(window.contentItem, 0, anchor.height)
        dd.x = Math.max(2, Math.min(pos.x, window.width - dd.width - 4))
        dd.y = pos.y + 3
        dd.open()
    }
    function ddTrigger(act) {
        if (act === "refresh") refresh()
        else if (act === "quit") window.close()
        else if (act === "copy") { if (selectedItem) clipboardStore.copyItem(selectedItem.id) }
        else if (act === "clearsearch") { search.text = "" }
        else if (act.indexOf("folder:") === 0) activateFolder(act.substring(7))
    }

    Component.onCompleted: refresh()
    Connections { target: clipboardStore; function onChanged() { window.refresh() } }

    // ==================== root vertical shell ====================
    ColumnLayout {
        anchors.fill: parent; spacing: 0

        // ======================= MENU BAR =======================
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 28; color: barBg
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 8; spacing: 8
                Repeater {
                    model: ["文件", "编辑", "视图", "导航", "代码", "运行", "工具", "VCS", "窗口", "帮助"]
                    delegate: Label {
                        required property string modelData
                        text: modelData; color: textMain; font.pixelSize: 12
                        Layout.alignment: Qt.AlignVCenter
                        MouseArea { anchors.fill: parent; hoverEnabled: true
                            onEntered: parent.color = textBright
                            onExited: parent.color = textMain
                            onClicked: if (window.hasMenu(modelData)) window.showDropdown(parent, window.menuItems(modelData)) }
                    }
                }
            }
        }
        // ======================= TOOLBAR =======================
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 38; color: panelBg; border.color: border; border.width: 1
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 6; spacing: 3
                Image { source: iconSvg("chevron-left", iconMuted); width: 15; height: 15; sourceSize.width: 30; sourceSize.height: 30 }
                Image { source: iconSvg("chevron-right", iconMuted); width: 15; height: 15; sourceSize.width: 30; sourceSize.height: 30 }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; Layout.alignment: Qt.AlignVCenter; color: border }

                // module / run-config combobox (visual, opens folder menu)
                Rectangle {
                    Layout.preferredWidth: 168; Layout.preferredHeight: 28; radius: 4
                    color: "#3f4144"; border.color: border
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 6
                        Image { source: iconSvg("chevron-down", accent); width: 12; height: 12; sourceSize.width: 24; sourceSize.height: 24 }
                        Label { text: "SmartClip"; color: textMain; font.pixelSize: 12; Layout.fillWidth: true }
                    }
                    MouseArea { anchors.fill: parent; hoverEnabled: true
                        onEntered: parent.color = "#454749"; onExited: parent.color = "#3f4144"
                        onClicked: window.showDropdown(parent, window.folderItems()) }
                }
                Rectangle { Layout.preferredWidth: 26; Layout.preferredHeight: 26; radius: 4; color: "#2f6f4f"
                    Image { anchors.centerIn: parent; source: iconSvg("play", "#c8f2d8"); width: 13; height: 13; sourceSize.width: 26; sourceSize.height: 26 }
                    MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: window.refresh() } }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; Layout.alignment: Qt.AlignVCenter; color: border }
                Image { source: iconSvg("refresh", iconMuted); width: 15; height: 15; sourceSize.width: 30; sourceSize.height: 30 }
                Image { source: iconSvg("close", iconMuted); width: 15; height: 15; sourceSize.width: 30; sourceSize.height: 30 }

                Item { Layout.fillWidth: true }

                // search field
                Rectangle {
                    Layout.preferredWidth: 300; Layout.preferredHeight: 28; radius: 4; color: "#3a3c3f"; border.color: border
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 9; spacing: 6
                        Image { source: iconSvg("search", iconMuted); width: 13; height: 13; sourceSize.width: 26; sourceSize.height: 26 }
                        TextField { id: search; Layout.fillWidth: true; Layout.fillHeight: true
                            placeholderText: "搜索剪贴内容"; placeholderTextColor: "#6d737a"; color: textMain
                            font.pixelSize: 12
                            background: Item {}
                            verticalAlignment: TextInput.AlignVCenter
                            onTextChanged: { window.searchText = text; window.refresh() } }
                    }
                }
                Rectangle { Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 4; color: "#3f4144"; border.color: border
                    Image { anchors.centerIn: parent; source: iconSvg("gear", iconMuted); width: 16; height: 16; sourceSize.width: 32; sourceSize.height: 32 }
                    MouseArea { anchors.fill: parent; hoverEnabled: true
                        onEntered: parent.color = "#454749"; onExited: parent.color = "#3f4144"
                        onClicked: window.showDropdown(parent, window.menuItems("工具")) } }
            }
        }
        // ======================= MAIN ROW =======================
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0

            // ---- left tool-window icon strip ----
            Rectangle {
                Layout.fillHeight: true; Layout.preferredWidth: 34; color: panelBg; border.color: border; border.width: 1
                Column {
                    anchors.fill: parent; anchors.topMargin: 8; anchors.bottomMargin: 8; spacing: 6
                    Repeater {
                        model: [
                            { k: "folder", active: true }, { k: "file", active: false }, { k: "search", active: false },
                            { k: "play", active: false }, { k: "branch", active: false }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            width: 26; height: 26; x: 4; radius: 5
                            color: modelData.active ? "#3a4a5a" : "transparent"
                            Image { anchors.centerIn: parent; source: iconSvg(modelData.k, modelData.active ? accent : iconMuted); width: 16; height: 16; sourceSize.width: 32; sourceSize.height: 32 }
                            MouseArea { anchors.fill: parent; hoverEnabled: true
                                onEntered: if (!modelData.active) parent.color = "#454749"
                                onExited: if (!modelData.active) parent.color = "transparent" }
                        }
                    }
                    Item { width: 1; height: Math.max(1, parent.height - 300) }
                    Rectangle { width: 26; height: 26; x: 4; radius: 5; color: "transparent"
                        Image { anchors.centerIn: parent; source: iconSvg("gear", iconMuted); width: 16; height: 16; sourceSize.width: 32; sourceSize.height: 32 } }
                }
            }
            // ---- project / clipboard tree panel ----
            Rectangle {
                Layout.fillHeight: true; Layout.preferredWidth: 300; color: panelBg; border.color: border; border.width: 1
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 30; color: "#3a3c3f"; border.color: border; border.width: 1
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                            Image { source: iconSvg("chevron-down", accent); width: 12; height: 12; sourceSize.width: 24; sourceSize.height: 24 }
                            Label { text: "项目"; color: textMain; font.pixelSize: 12; font.bold: true }
                            Item { Layout.fillWidth: true }
                            Image { source: iconSvg("grid", textMuted); width: 13; height: 13; sourceSize.width: 26; sourceSize.height: 26 }
                            Image { source: iconSvg("more", "#d4b652"); width: 14; height: 14; sourceSize.width: 28; sourceSize.height: 28 }
                        }
                    }
                    ListView {
                        id: tree
                        Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                        model: window.treeRows
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        delegate: Rectangle {
                            required property var modelData
                            width: ListView.view.width
                            height: modelData.kind === "folder" ? 28 : 24
                            radius: 3
                            color: modelData.kind === "folder"
                                   ? (window.activeFolder === modelData.key ? window.selBg : (row.containsMouse ? window.hoverBg : "transparent"))
                                   : (window.selectedItem && window.selectedItem.id === modelData.item.id ? window.selBg : (row.containsMouse ? window.hoverBg : "transparent"))
                            RowLayout {
                                anchors.fill: parent; spacing: 4
                                anchors.leftMargin: modelData.kind === "folder" ? 6 : 24
                                anchors.rightMargin: 8
                                Image {
                                    visible: modelData.kind === "folder"
                                    source: iconSvg(modelData.expanded ? "chevron-down" : "chevron-right", window.activeFolder === modelData.key ? window.textBright : iconMuted)
                                    width: 11; height: 11; sourceSize.width: 22; sourceSize.height: 22
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                Item { width: 2; height: 1 }
                                Image {
                                    source: modelData.kind === "folder" ? iconSvg("folder", iconFolder)
                                          : modelData.item.type === "image" ? iconSvg("image", iconImage)
                                          : iconSvg("file", accent)
                                    width: 13; height: 13; sourceSize.width: 26; sourceSize.height: 26
                                    Layout.alignment: Qt.AlignVCenter
                                }
                                Label {
                                    text: modelData.kind === "folder" ? modelData.label :
                                          (modelData.item.type === "image" ? "图片 · " + modelData.item.title : modelData.item.title)
                                    color: modelData.kind === "folder" ? textBright : textMain
                                    font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true
                                }
                                Label {
                                    text: modelData.kind === "folder" ? modelData.count : ""
                                    color: textMuted; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter
                                }
                            }
                            MouseArea {
                                id: row; anchors.fill: parent; hoverEnabled: true
                                onClicked: modelData.kind === "folder"
                                           ? window.toggleFolder(modelData.key)
                                           : window.selectItem(modelData.item)
                            }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: border }
                    Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }
                    RowLayout { Layout.fillWidth: true; Layout.leftMargin: 12; Layout.rightMargin: 8; spacing: 8
                        Image { source: iconSvg("chevron-down", accent); width: 11; height: 11; sourceSize.width: 22; sourceSize.height: 22 }
                        Label { text: "外部库"; color: textMain; font.pixelSize: 12 }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout { Layout.fillWidth: true; Layout.preferredHeight: 24; Layout.leftMargin: 34; Layout.rightMargin: 8; spacing: 8
                        Image { source: iconSvg("trash", textMuted); width: 12; height: 12; sourceSize.width: 24; sourceSize.height: 24 }
                        Label { text: "回收站"; color: textMain; font.pixelSize: 12; Layout.fillWidth: true }
                    }
                    Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }
                }
            }
            // ---- editor area ----
            ColumnLayout { Layout.fillWidth: true; Layout.fillHeight: true; spacing: 0
                // tab bar
                Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 34; color: barBg; border.color: border; border.width: 1
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 4; anchors.rightMargin: 6; spacing: 4
                        Rectangle { Layout.preferredWidth: 240; Layout.preferredHeight: 28; radius: 4; color: "#45484c"
                            RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                                Image { source: selectedItem && selectedItem.type === "image" ? iconSvg("image", iconImage) : iconSvg("file", accent); width: 12; height: 12; sourceSize.width: 24; sourceSize.height: 24 }
                                Label { text: tabTitle(); color: textBright; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                                Image { source: iconSvg("close", textMuted); width: 11; height: 11; sourceSize.width: 22; sourceSize.height: 22 }
                            }
                            MouseArea { anchors.fill: parent; hoverEnabled: true }
                        }
                        Rectangle { Layout.preferredWidth: 30; Layout.preferredHeight: 28; radius: 4; color: "transparent"
                            Image { anchors.centerIn: parent; source: iconSvg("plus", textMuted); width: 15; height: 15; sourceSize.width: 30; sourceSize.height: 30 }
                            MouseArea { anchors.fill: parent; hoverEnabled: true }
                        }
                        Item { Layout.fillWidth: true }
                        Image { source: iconSvg("grid", accent); width: 12; height: 12; sourceSize.width: 24; sourceSize.height: 24 }
                        Image { source: iconSvg("chevron-down", textMuted); width: 11; height: 11; sourceSize.width: 22; sourceSize.height: 22 }
                    }
                }
                // ---- editor content ----
                Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: editorBg; border.color: border; border.width: 1

                    // welcome / README tab
                    Column {
                        visible: window.showWhatTab
                        anchors.fill: parent; anchors.margins: 28; spacing: 8
                        Label { text: "SmartClip — 剪贴板"; color: textBright; font.pixelSize: 22; font.bold: true }
                        Label { text: "从剪贴板复制的文本和图片会自动保存为条目，点击左侧条目即可回填到剪贴板。"
                            color: textMain; font.pixelSize: 13; wrapMode: Text.Wrap; width: parent.width }
                        Rectangle { width: parent.width; height: 1; color: border }
                        Label { text: "## 功能"; color: accent; font.pixelSize: 15; font.bold: true }
                        Label { text: "• 自动采集：监听系统剪贴板，文本与图片自动存入本地数据库\n• 分类视图：按今天 / 昨天 / 近 7 天 / 更早 分组展示\n• 快速回填：单击任意条目即可复制回剪贴板\n• 全文搜索：按标题或内容即时过滤"
                            color: codeText; font.pixelSize: 13; font.family: "Consolas"; lineHeight: 1.5; width: parent.width }
                        Rectangle { width: parent.width; height: 1; color: border }
                        Label { text: "## 使用"; color: accent; font.pixelSize: 15; font.bold: true }
                        Label { text: "1. 复制任意内容，SmartClip 自动记录\n2. 在左侧展开对应日期文件夹，点选条目\n3. 预览区显示内容，同时已复制回剪贴板"
                            color: codeText; font.pixelSize: 13; font.family: "Consolas"; lineHeight: 1.5; width: parent.width }
                    }

                    // selected image preview
                    Column {
                        visible: !window.showWhatTab && selectedItem && selectedItem.type === "image"
                        anchors.fill: parent; anchors.margins: 20; spacing: 10
                        RowLayout { width: parent.width; spacing: 8
                            Image { source: iconSvg("image", iconImage); width: 14; height: 14; sourceSize.width: 28; sourceSize.height: 28 }
                            Label { text: selectedItem ? selectedItem.title : ""; color: textMain; font.pixelSize: 13; font.bold: true; Layout.fillWidth: true }
                        }
                        Label { text: selectedItem ? displayTime(selectedItem.createdAt) : ""; color: textMuted; font.pixelSize: 12 }
                        Rectangle { width: parent.width; height: 1; color: border }
                        Item { width: parent.width; height: parent.height - 90
                            Image {
                                anchors.fill: parent
                                source: selectedItem ? "file:///" + selectedItem.content.replace(/\\/g, "/") : ""
                                fillMode: Image.PreserveAspectFit
                                horizontalAlignment: Image.AlignHCenter; verticalAlignment: Image.AlignVCenter
                            }
                        }
                    }

                    // selected text preview (code editor look)
                    ScrollView {
                        visible: !window.showWhatTab && selectedItem && selectedItem.type === "text"
                        anchors.fill: parent; clip: true
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
                        TextArea {
                            readOnly: true
                            text: selectedItem ? selectedItem.content : ""
                            color: codeText; font.family: "Consolas"; font.pixelSize: 13
                            wrapMode: TextEdit.Wrap; selectByMouse: true
                            topPadding: 18; leftPadding: 22; rightPadding: 22; bottomPadding: 18
                            background: Rectangle { color: editorBg }
                        }
                    }
                }
            }
        }
        // ======================= STATUS BAR (full width) =======================
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 26; color: barBg; border.color: border; border.width: 1
            RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 12
                Text { text: "SmartClip"; color: "#77808c"; font.pixelSize: 11; Layout.alignment: Qt.AlignVCenter }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; Layout.alignment: Qt.AlignVCenter; color: border }
                Label { text: tabTitle(); color: textMuted; font.pixelSize: 11 }
                Item { Layout.fillWidth: true }
                Label { text: selectedItem ? "已复制" : "自动采集"; color: textMuted; font.pixelSize: 11 }
                Label { text: visibleCount() + " 项"; color: "#77808c"; font.pixelSize: 11 }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; Layout.alignment: Qt.AlignVCenter; color: border }
                Image { source: iconSvg("branch", "#8b929e"); width: 12; height: 12; sourceSize.width: 24; sourceSize.height: 24 }
                Label { text: "分支: main"; color: textMuted; font.pixelSize: 11 }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; Layout.alignment: Qt.AlignVCenter; color: border }
                Label { text: "UTF-8"; color: textMuted; font.pixelSize: 11 }
            }
        }
    }

    // ======================= DROPDOWN MENU POPUP =======================
    Popup {
        id: dd
        x: 0; y: 0
        width: 200; height: 40
        padding: 4
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape
        background: Rectangle { color: "#3c3f41"; radius: 5; border.color: "#4b4d4f" }
        Column {
            anchors.fill: parent; spacing: 2
            Repeater {
                model: window.ddItems
                delegate: Rectangle {
                    required property var modelData
                    width: parent.width; height: 28; radius: 4
                    color: ddh.containsMouse ? "#46484a" : "transparent"
                    Label {
                        anchors.fill: parent; anchors.leftMargin: 10
                        verticalAlignment: Text.AlignVCenter
                        text: modelData.label; color: window.textMain; font.pixelSize: 13
                    }
                    MouseArea {
                        id: ddh; anchors.fill: parent; hoverEnabled: true
                        onClicked: { window.ddTrigger(modelData.act); dd.close() }
                    }
                }
            }
        }
    }
}
