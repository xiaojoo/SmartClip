pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

Rectangle {
    id: root
    implicitHeight: 40
    color: "#313335"

    signal searchChanged(string text)
    signal requestRefresh()
    signal openMenu(Item anchor, var items)

    readonly property color borderColor: "#43454a"
    readonly property color iconColor:   "#8b929e"
    readonly property color textColor:   "#b4b8bf"
    readonly property color textBright:  "#ced0d6"
    readonly property color textMuted:   "#6f737a"
    readonly property color hoverColor:  "#34363a"
    readonly property color fieldBg:     "#2b2d30"
    readonly property color green:       "#499c54"
    readonly property color red:         "#db5860"

    IconProvider { id: icons }

    function folderItems() {
        return [ { label: "今天",  act: "folder:today" }, { label: "昨天", act: "folder:yesterday" },
                 { label: "近 7 天", act: "folder:week" }, { label: "更早", act: "folder:older" } ]
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
    function menuGroups() {
        return [ { label: "文件", act: "menu:文件" }, { label: "编辑", act: "menu:编辑" },
                 { label: "视图", act: "menu:视图" }, { label: "导航", act: "menu:导航" },
                 { label: "代码", act: "menu:代码" }, { label: "运行", act: "menu:运行" },
                 { label: "工具", act: "menu:工具" }, { label: "VCS", act: "menu:VCS" },
                 { label: "窗口", act: "menu:窗口" }, { label: "帮助", act: "menu:帮助" } ]
    }
    function toolItems() { return [{ label: "设置", act: "none" }, { label: "关于 SmartClip", act: "none" }] }
    function openGroup(label) { root.openMenu(burger, root.menuItems(label)) }
    function clearSearch() { field.text = "" }

    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 10; spacing: 8

        Rectangle {
            id: burger
            Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 5; color: "transparent"
            Column { anchors.centerIn: parent; spacing: 3
                Repeater { model: 3; delegate: Rectangle { width: 14; height: 2; radius: 1; color: root.iconColor } } }
            MouseArea { anchors.fill: parent; hoverEnabled: true
                onEntered: burger.color = root.hoverColor
                onExited: burger.color = "transparent"
                onClicked: root.openMenu(burger, root.menuGroups()) }
        }

        Rectangle { Layout.preferredWidth: 20; Layout.preferredHeight: 20; radius: 4
            gradient: Gradient {
                GradientStop { position: 0.0; color: "#f7971e" }
                GradientStop { position: 1.0; color: "#ff6b6b" }
            }
            Text { anchors.centerIn: parent; text: "S"; color: "#ffffff"; font.pixelSize: 12; font.bold: true }
        }

        Label { text: "SmartClip"; color: textBright; font.pixelSize: 12; font.bold: true }
        AppIcon { provider: icons; kind: "chevron-right"; tint: textMuted; size: 10 }

        Rectangle {
            id: branchBox
            Layout.preferredHeight: 26; Layout.preferredWidth: branchRow.implicitWidth + 16
            radius: 5; color: "transparent"
            RowLayout { id: branchRow; anchors.centerIn: parent; spacing: 5
                AppIcon { provider: icons; kind: "branch"; tint: root.iconColor; size: 13 }
                Label { text: "main"; color: root.textColor; font.pixelSize: 12 }
                AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 10 } }
            MouseArea { anchors.fill: parent; hoverEnabled: true
                onEntered: branchBox.color = root.hoverColor
                onExited: branchBox.color = "transparent"
                onClicked: root.openMenu(branchBox, root.folderItems()) }
        }

        Item { Layout.fillWidth: true }

        Rectangle {
            Layout.preferredWidth: 320; Layout.preferredHeight: 28; radius: 5
            color: fieldBg; border.color: borderColor
            RowLayout { anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 9; spacing: 6
                AppIcon { provider: icons; kind: "search"; tint: root.iconColor; size: 13 }
                TextField {
                    id: field
                    Layout.fillWidth: true; Layout.fillHeight: true
                    placeholderText: "搜索剪贴内容"; placeholderTextColor: "#6f737a"; color: root.textColor
                    font.pixelSize: 12; background: Item {}
                    verticalAlignment: TextInput.AlignVCenter
                    onTextChanged: root.searchChanged(text)
                } }
        }

        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; color: borderColor }

        Rectangle { Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 5; color: "transparent"
            AppIcon { anchors.centerIn: parent; provider: icons; kind: "play"; tint: root.green; size: 14 }
            MouseArea { anchors.fill: parent; hoverEnabled: true
                onEntered: parent.color = root.hoverColor; onExited: parent.color = "transparent"
                onClicked: root.requestRefresh() } }

        Rectangle { Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 5; color: "transparent"
            Rectangle { anchors.centerIn: parent; width: 10; height: 10; radius: 5; color: root.red }
            MouseArea { anchors.fill: parent; hoverEnabled: true
                onEntered: parent.color = root.hoverColor; onExited: parent.color = "transparent"
                onClicked: root.clearSearch() } }

        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 20; color: borderColor }

        Rectangle { Layout.preferredWidth: 28; Layout.preferredHeight: 28; radius: 5; color: "transparent"
            AppIcon { anchors.centerIn: parent; provider: icons; kind: "gear"; tint: root.iconColor; size: 15 }
            MouseArea { anchors.fill: parent; hoverEnabled: true
                onEntered: parent.color = root.hoverColor; onExited: parent.color = "transparent"
                onClicked: root.openMenu(parent, root.toolItems()) } }
    }
}