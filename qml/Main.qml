pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: window
    width: 1460; height: 900; minimumWidth: 1000; minimumHeight: 640; visible: true
    title: "SmartClip — 剪贴板"

    // ---------- IntelliJ "Darcula"-style palette ----------
    readonly property color winBg:     "#202329"
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

    Component.onCompleted: refresh()
    Connections { target: clipboardStore; function onChanged() { window.refresh() } }

    // ==================== root vertical shell ====================
    ColumnLayout {
        anchors.fill: parent; spacing: 0

        // ======================= MENU BAR =======================
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 28; color: barBg
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 8; spacing: 18
                Repeater {
                    model: ["文件", "编辑", "视图", "导航", "代码", "运行", "工具", "VCS", "窗口", "帮助"]
                    delegate: Label {
                        required property string modelData
                        text: modelData; color: textMain; font.pixelSize: 12
                        Layout.alignment: Qt.AlignVCenter
                        MouseArea { anchors.fill: parent; hoverEnabled: true
                            onEntered: parent.color = textBright
                            onExited: parent.color = textMain }
                    }
                }
            }
        }
        // ======================= TOOLBAR =======================
        Rectangle {
            Layout.fillWidth: true; Layout.preferredHeight: 38; color: panelBg; border.color: border; border.width: 1
            RowLayout {
                anchors.fill: parent; anchors.leftMargin: 8; anchors.rightMargin: 8; spacing: 6
                Text { text: "◀"; color: textMuted; font.pixelSize: 13; Layout.alignment: Qt.AlignVCenter }
                Text { text: "▶"; color: textMuted; font.pixelSize: 13; Layout.alignment: Qt.AlignVCenter }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; Layout.alignment: Qt.AlignVCenter; color: border }

                // module / run-config combobox (visual)
                Rectangle {
                    Layout.preferredWidth: 168; Layout.preferredHeight: 28; radius: 4
                    color: "#3f4144"; border.color: border
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 6
                        Text { text: "▾"; color: accent; font.pixelSize: 11 }
                        Label { text: "SmartClip"; color: textMain; font.pixelSize: 12; Layout.fillWidth: true }
                    }
                    MouseArea { anchors.fill: parent; hoverEnabled: true
                        onEntered: parent.color = "#454749"; onExited: parent.color = "#3f4144" }
                }
                Rectangle { Layout.preferredWidth: 26; Layout.preferredHeight: 26; radius: 4; color: "#2f6f4f"
                    Text { anchors.centerIn: parent; text: "▶"; color: "#c8f2d8"; font.pixelSize: 12 }
                    MouseArea { anchors.fill: parent; hoverEnabled: true; onClicked: window.refresh() } }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; Layout.alignment: Qt.AlignVCenter; color: border }
                Text { text: "🔄"; color: textMuted; font.pixelSize: 13; Layout.alignment: Qt.AlignVCenter }
                Text { text: "🧹"; color: textMuted; font.pixelSize: 13; Layout.alignment: Qt.AlignVCenter }

                Item { Layout.fillWidth: true }

                // search field
                Rectangle {
                    Layout.preferredWidth: 300; Layout.preferredHeight: 28; radius: 4; color: "#3a3c3f"; border.color: border
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 9; spacing: 6
                        Text { text: "⌕"; color: textMuted; font.pixelSize: 15; Layout.alignment: Qt.AlignVCenter }
                        TextField { id: search; Layout.fillWidth: true; Layout.fillHeight: true
                            placeholderText: "搜索剪贴内容"; placeholderTextColor: "#6d737a"; color: textMain
                            font.pixelSize: 12
                            background: Item {}
                            verticalAlignment: TextInput.AlignVCenter
                            onTextChanged: { window.searchText = text; window.refresh() } }
                    }
                }
                Rectangle { Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 4; color: "#3f4144"; border.color: border
                    Text { anchors.centerIn: parent; text: "⚙"; color: textMuted; font.pixelSize: 14 }
                    MouseArea { anchors.fill: parent; hoverEnabled: true
                        onEntered: parent.color = "#454749"; onExited: parent.color = "#3f4144" } }
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
                            { t: "▣", active: true }, { t: "◧", active: false }, { t: "⌘", active: false },
                            { t: "▤", active: false }, { t: "☰", active: false }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            width: 26; height: 26; x: 4; radius: 5
                            color: modelData.active ? "#3a4a5a" : "transparent"
                            Text { anchors.centerIn: parent; text: modelData.t
                                color: modelData.active ? accent : "#9aa0a8"; font.pixelSize: 15 }
                            MouseArea { anchors.fill: parent; hoverEnabled: true
                                onEntered: if (!modelData.active) parent.color = "#454749"
                                onExited: if (!modelData.active) parent.color = "transparent" }
                        }
                    }
                    Item { width: 1; height: Math.max(1, parent.height - 300) }
                    Rectangle { width: 26; height: 26; x: 4; radius: 5; color: "transparent"
                        Text { anchors.centerIn: parent; text: "⚙"; color: "#9aa0a8"; font.pixelSize: 14 } }
                }
            }
            // ---- project / clipboard tree panel ----
            Rectangle {
                Layout.fillHeight: true; Layout.preferredWidth: 300; color: panelBg; border.color: border; border.width: 1
                ColumnLayout {
                    anchors.fill: parent; spacing: 0
                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 30; color: "#3a3c3f"; border.color: border; border.width: 1
                        RowLayout { anchors.fill: parent; anchors.leftMargin: 12; anchors.rightMargin: 8; spacing: 8
                            Text { text: "⌄"; color: accent; font.pixelSize: 13; Layout.alignment: Qt.AlignVCenter }
                            Label { text: "项目"; color: textMain; font.pixelSize: 12; font.bold: true }
                            Item { Layout.fillWidth: true }
                            Text { text: "⊞"; color: textMuted; font.pixelSize: 13; Layout.alignment: Qt.AlignVCenter }
                            Text { text: "⋮"; color: "#d4b652"; font.pixelSize: 16; Layout.alignment: Qt.AlignVCenter }
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
                                anchors.fill: parent; spacing: 6
                                anchors.leftMargin: modelData.kind === "folder" ? 6 : 26
                                anchors.rightMargin: 8
                                Text {
                                    text: modelData.kind === "folder" ? (modelData.expanded ? "⌄" : "›") : "·"
                                    color: modelData.kind === "folder" ? accent : "#6f767e"; font.pixelSize: 13
                                    Layout.preferredWidth: 14; Layout.alignment: Qt.AlignVCenter
                                }
                                Text {
                                    text: modelData.kind === "folder" ? "📁" : (modelData.item.type === "image" ? "▧" : "≡")
                                    color: modelData.kind === "folder" ? "#c8b74f" : (modelData.item.type === "image" ? "#d7a85b" : accent)
                                    font.pixelSize: 13; Layout.preferredWidth: 18; Layout.alignment: Qt.AlignVCenter
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
                        Text { text: "⌄"; color: accent; font.pixelSize: 12; Layout.alignment: Qt.AlignVCenter }
                        Label { text: "外部库"; color: textMain; font.pixelSize: 12 }
                        Item { Layout.fillWidth: true }
                    }
                    RowLayout { Layout.fillWidth: true; Layout.preferredHeight: 24; Layout.leftMargin: 34; Layout.rightMargin: 8; spacing: 8
                        Text { text: "⌫"; color: textMuted; font.pixelSize: 12; Layout.alignment: Qt.AlignVCenter }
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
                                Text { text: selectedItem && selectedItem.type === "image" ? "▧" : "≡"; color: accent; font.pixelSize: 13; Layout.alignment: Qt.AlignVCenter }
                                Label { text: tabTitle(); color: textBright; font.pixelSize: 12; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: "×"; color: textMuted; font.pixelSize: 14; Layout.alignment: Qt.AlignVCenter }
                            }
                            MouseArea { anchors.fill: parent; hoverEnabled: true }
                        }
                        Rectangle { Layout.preferredWidth: 34; Layout.preferredHeight: 28; radius: 4; color: "transparent"
                            Text { anchors.centerIn: parent; text: "+"; color: textMuted; font.pixelSize: 18 }
                            MouseArea { anchors.fill: parent; hoverEnabled: true }
                        }
                        Item { Layout.fillWidth: true }
                        Text { text: "▣"; color: accent; font.pixelSize: 12; Layout.alignment: Qt.AlignVCenter }
                        Text { text: "⌄"; color: textMuted; font.pixelSize: 12; Layout.alignment: Qt.AlignVCenter }
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
                        Label { text: selectedItem ? selectedItem.title : ""; color: textMain; font.pixelSize: 13; font.bold: true }
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
                Text { text: "⎇"; color: textMuted; font.pixelSize: 12; Layout.alignment: Qt.AlignVCenter }
                Label { text: "分支: main"; color: textMuted; font.pixelSize: 11 }
                Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 12; Layout.alignment: Qt.AlignVCenter; color: border }
                Label { text: "UTF-8"; color: textMuted; font.pixelSize: 11 }
            }
        }
    }
}
