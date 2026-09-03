pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// IDE-style menu bar. Each entry can open a DropdownMenu (via openMenu signal).
Rectangle {
    id: root

    implicitHeight: 28
    color: "#313335"

    signal openMenu(Item anchor, var items)

    readonly property color textColor: "#bbbbbb"
    readonly property color textHot:   "#e8e8e8"

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

    RowLayout {
        anchors.fill: parent; anchors.leftMargin: 6; spacing: 10
        Repeater {
            model: ["文件", "编辑", "视图", "导航", "代码", "运行", "工具", "VCS", "窗口", "帮助"]
            delegate: Label {
                required property string modelData
                text: modelData; color: textColor; font.pixelSize: 12
                Layout.alignment: Qt.AlignVCenter
                MouseArea {
                    anchors.fill: parent; hoverEnabled: true
                    onEntered: parent.color = textHot
                    onExited: parent.color = textColor
                    onClicked: if (root.hasMenu(modelData)) root.openMenu(parent, root.menuItems(modelData))
                }
            }
        }
    }
}
