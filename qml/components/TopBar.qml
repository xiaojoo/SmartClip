pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

/*
 * 顶部这一行（原生标题栏去掉后它就是窗口最顶上的一行）：
 *
 *   [☰] [S] SmartClip › main │ 文件 编辑 视图 … 帮助        [🔍 搜索剪贴内容]
 *
 * 搜索框在这一行的最右边（菜单右边）。
 * 最小化 / 最大化(还原) / 关闭 三个按钮在搜索框右边，也在这同一行。
 */
Rectangle {
    id: root

    implicitHeight: 34
    color: "#313335"

    // 要操作的窗口（Main.qml 的 window），转给右侧的窗口按钮
    property var host: null

    signal openMenu(Item anchor, var items)
    signal searchChanged(string text)

    readonly property color borderColor: "#43454a"
    readonly property color iconColor:   "#8b929e"
    readonly property color textColor:   "#b4b8bf"
    readonly property color textBright:  "#ced0d6"
    readonly property color textMuted:   "#6f737a"
    readonly property color hoverColor:  "#34363a"
    readonly property color fieldBg:     "#2b2d30"

    function folderItems() {
        return [ { label: "今天",  act: "folder:today" }, { label: "昨天", act: "folder:yesterday" },
                 { label: "近 7 天", act: "folder:week" }, { label: "更早", act: "folder:older" } ]
    }
    function hasMenu(label) {
        return label === "文件" || label === "编辑" || label === "视图" ||
               label === "运行" || label === "工具" || label === "帮助"
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
    function tabLabels() {
        return ["文件", "编辑", "视图", "导航", "代码", "运行", "工具", "VCS", "窗口", "帮助"]
    }

    IconProvider { id: icons }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 8

        // ---- 左：汉堡键 / 应用图标 / 标题 / 分支 ----
        Rectangle {
            id: burger
            Layout.preferredWidth: 26; Layout.preferredHeight: 26; radius: 5; color: "transparent"
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

        Label { text: "SmartClip"; color: root.textBright; font.pixelSize: 12; font.bold: true }
        AppIcon { provider: icons; kind: "chevron-right"; tint: root.textMuted; size: 10 }

        Rectangle {
            id: branchBox
            Layout.preferredHeight: 24; Layout.preferredWidth: branchRow.implicitWidth + 16
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

        // 竖分隔线
        Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 16; color: root.borderColor
                    Layout.leftMargin: 2; Layout.rightMargin: 2 }

        // ---- 菜单 tab ----
        Repeater {
            model: root.tabLabels()

            delegate: Rectangle {
                required property string modelData

                Layout.alignment: Qt.AlignVCenter
                Layout.preferredHeight: 22
                Layout.preferredWidth: tabLabel.implicitWidth + 14
                radius: 4
                color: tabHover.containsMouse && root.hasMenu(modelData) ? "#3a3d41" : "transparent"

                Label {
                    id: tabLabel
                    anchors.centerIn: parent
                    text: modelData
                    font.pixelSize: 12
                    color: tabHover.containsMouse && root.hasMenu(modelData)
                           ? "#e8e8e8" : root.textColor
                }

                MouseArea {
                    id: tabHover
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: if (root.hasMenu(modelData))
                                   root.openMenu(parent, root.menuItems(modelData))
                }
            }
        }

        Item { Layout.fillWidth: true }

        /*
         * 搜索框：在这一行的最右边，菜单 tab 的右侧。
         * 后面紧挨着缩小 / 放大 / 关闭 三个窗口按钮。
         */
        Rectangle {
            Layout.preferredWidth: 300; Layout.preferredHeight: 24; radius: 5
            Layout.alignment: Qt.AlignVCenter
            Layout.rightMargin: 6
            color: root.fieldBg; border.color: root.borderColor
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

        /*
         * 窗口按钮：缩小 / 放大(还原) / 关闭。
         *
         * 原生标题栏去掉后由这里接管，贴在这一行最右边，
         * 高度撑满整行，鼠标滑到最右边就是关闭。
         */
        WindowControls {
            host: root.host
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
        }
    }
}
