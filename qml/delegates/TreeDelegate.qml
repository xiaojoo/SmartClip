pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

/*
 * 左树的一行。
 *
 * 现在只有两种行（见 js/FolderManager.js 拍出来的 rows）：
 *   folder —— 日期文件夹（2026-09-13）/ 导入的文件夹 / 导入目录里的子目录
 *   file   —— 日期文件夹里的一个 md（073100.md），或者导入目录里的 md / txt
 *
 * 缩进按 depth 算，所以"日期 -> 文件"是两级、导入的目录能一层层往里套，
 * 同一个委托就够了。
 */
Rectangle {
    id: root

    required property var modelData
    property bool rowHighlight: false
    signal rowClicked()
    /* 右键：把"哪一行"和鼠标坐标报给 Main，由它弹菜单（见 FolderTree） */
    signal rowContextMenu(real x, real y)

    readonly property color selColor:    "#214283"
    readonly property color textBright:  "#e8e8e8"
    readonly property color textColor:   "#bbbbbb"
    readonly property color textMuted:   "#7d7d7d"
    readonly property color accentColor: "#4c96d8"
    readonly property color chevronDim:  "#6f767e"
    readonly property color folderColor: "#c8b74f"
    readonly property color importColor: "#7fa8c8"

    readonly property bool isFolder: modelData && modelData.kind === "folder"
    readonly property bool isDate: isFolder && modelData.folderKind === "date"
    readonly property bool isImportRoot: isFolder && modelData.folderKind === "imported"
    readonly property int  depth: modelData && modelData.depth !== undefined ? modelData.depth : 0

    IconProvider { id: icons }

    /*
     * 行高：分组和文件一样高（24）。
     *
     * 分组原来是 28：内容只有 16px 高的图标 + 12px 的文字，多出来的 4px 就
     * 全变成行内的空白，几个分组摞在一起时上下各空 6px、再加上列表那 2px 的
     * 行距，看着有十几 px 那么散。压到 24 之后分组之间和文件之间一样紧。
     */
    implicitHeight: 24
    radius: 3

    /*
     * 只有当前选中的文件有蓝色高亮，鼠标悬停不再变色。
     *
     * 文件夹行一律不亮：一级菜单是容器，点它只是展开 / 收起。
     */
    color: rowHighlight ? selColor : "transparent"

    /*
     * 行内元素之间的间隙。
     *
     * 图标尺寸**分级**：文件夹图标 15px，比正文字号（12）大一档；文件 13px ——
     * 层级在尺寸上也分得开。缩进按 depth 走，一眼能看出谁套在谁里面。
     */
    RowLayout {
        anchors.fill: parent
        spacing: 3
        anchors.leftMargin: 6 + Math.min(root.depth, 6) * 14
        anchors.rightMargin: 6

        AppIcon {
            visible: root.isFolder
            provider: icons
            kind: modelData.expanded ? "chevron-down" : "chevron-right"
            tint: rowHighlight ? textBright : chevronDim
            size: 13
            Layout.alignment: Qt.AlignVCenter
        }

        AppIcon {
            provider: icons
            kind: root.isFolder ? (root.isImportRoot ? "open" : "folder") : "file"
            tint: root.isFolder ? (root.isImportRoot ? importColor : folderColor) : accentColor
            size: root.isFolder ? 15 : 13
            Layout.alignment: Qt.AlignVCenter
        }

        Label {
            text: modelData.label !== undefined ? modelData.label : ""
            color: root.isFolder ? textBright : textColor
            font.pixelSize: 12
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        /*
         * 右边的计数：文件夹给"里面几个文件"，文件给"里面几条内容"。
         * 数字后面那个小字是单位，只给文件夹写（一行里已经够挤了）。
         */
        Label {
            text: root.isFolder ? (modelData.files + " 个文件")
                                : (modelData.entries + " 条")
            color: textMuted
            font.pixelSize: 11
            Layout.alignment: Qt.AlignVCenter
        }
    }

    MouseArea {
        // hoverEnabled 关掉：这个委托已经没有悬停样式了，
        // 留着它只会不断产生 containsMouse 变化通知
        id: mouse
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onClicked: (event) => {
            if (event.button === Qt.RightButton) {
                /*
                 * 菜单是独立的原生弹窗，位置要的是**窗口内容区坐标**，
                 * 所以这里先把委托内坐标换算过去（见 DropdownMenu.openAtPoint）。
                 */
                var p = mapToItem(null, event.x, event.y)
                root.rowContextMenu(p.x, p.y)
            } else {
                root.rowClicked()
            }
        }
    }

    /*
     * 悬停说明：文件名带完整路径，文件夹带"几个文件 / 几条内容"。
     * 提示框自己是个原生弹窗（见 AppToolTip.qml），跟着这一行放。
     */
    AppToolTip {
        hovered: mouse.containsMouse
        text: {
            if (!root.modelData)
                return ""
            if (root.isFolder)
                return root.modelData.path + "\n" + root.modelData.files + " 个文件 · "
                       + root.modelData.entries + " 条内容"
            return root.modelData.path + "\n" + root.modelData.entries + " 条内容 · "
                   + Math.max(1, Math.round(root.modelData.size / 1024)) + " KB"
        }
    }
}
