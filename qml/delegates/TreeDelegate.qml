pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0

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
    /*
     * 右键菜单正指着这一行（见 FolderTree.contextPath）。
     *
     * 和 rowHighlight（蓝底 = "这份文件开在编辑器里"）是两件事，所以用两个颜色：
     * 蓝底是"打开的是它"，这块灰黑底是"菜单要动的是它"。右键一个没打开的
     * 文件时，光看菜单看不出动的是哪一行 —— 用户要的就是这一眼。
     */
    property bool rowContext: false
    signal rowClicked()
    /* 右键：把"哪一行"和鼠标坐标报给 Main，由它弹菜单（见 FolderTree） */
    signal rowContextMenu(real x, real y)

    readonly property color selColor:    Theme.c("#214283", Theme.rev)
    /* 右键那一行的底色：灰黑，比面板底色（#1e1f22）亮一档就够，不抢蓝底的风头 */
    readonly property color contextColor: Theme.c("#34373b", Theme.rev)

    /*
     * 自检用：图标那一格在场景里的 x。
     *
     * 一级（文件夹）和二级（文件）的图标必须落在同一列上 —— 这条是画出来的
     * 几何，不是某个属性值，所以让委托自己报坐标，自检比对两级的 x 是否相等
     * （见 FolderTree.iconColumnXs）。
     */
    readonly property real iconCellX: iconCellItem ? iconCellItem.mapToItem(null, 0, 0).x : -1

    /*
     * 自检用：**展开箭头**那一格在场景里的 x（文件行没有箭头，给 -1）。
     *
     * 标题「项目」的左边缘要对齐的就是它（用户要的"树往左靠、标题别动"），
     * 见 FolderTree.firstRowChevronPanelX。
     */
    readonly property real chevronCellX: chevronItem ? chevronItem.mapToItem(null, 0, 0).x : -1
    readonly property color textBright:  Theme.c("#e8e8e8", Theme.rev)
    readonly property color textColor:   Theme.c("#bbbbbb", Theme.rev)
    readonly property color textMuted:   Theme.c("#7d7d7d", Theme.rev)
    readonly property color accentColor: "#4c96d8"
    readonly property color chevronDim:  Theme.c("#6f767e", Theme.rev)
    readonly property color folderColor: Theme.c("#c8b74f", Theme.rev)
    readonly property color importColor: Theme.c("#7fa8c8", Theme.rev)

    readonly property bool isFolder: modelData && modelData.kind === "folder"
    readonly property bool isDate: isFolder && modelData.folderKind === "date"
    readonly property bool isImportRoot: isFolder && modelData.folderKind === "imported"
    /* 依赖 / 构建目录（没进去扫，计数那栏写"未索引"，见 ClipboardStore::scanFolder） */
    readonly property bool skipped: isFolder && modelData.skipped === true
    readonly property int  depth: modelData && modelData.depth !== undefined ? modelData.depth : 0

    /*
     * 图标落在哪一列上（就是 RowLayout 的左边距）。
     *
     * 文件夹行是 [缩进][展开箭头 13 + 间距 3][图标]；文件行没有箭头那一格，
     * 但得把上级文件夹的缩进**和那一格**一起补上（16 - 14 = 2px 的差值就是
     * 这么来的），二级图标才和一级图标对齐 —— 少补这 2px 的话，一行行看下来
     * 那条图标列是歪的（用户报的"一级图标和二级图标没对齐"）。
     *
     * 缩进仍然封顶 6 层：导入目录能套很深，再往里就不该一直往右跑了。
     */
    readonly property int indentStep: 14
    readonly property int chevronCell: 16   /* 13 的箭头 + 3 的间距 */
    /* 两种图标共用的一格（文件夹图标本来就是 15，文件图标 13 靠左放） */
    readonly property int iconCell: 15
    readonly property int iconInset: {
        var d = Math.min(Math.max(root.depth, 0), 6)
        return root.isFolder ? 6 + d * indentStep
                             : 6 + Math.max(0, d - 1) * indentStep + chevronCell
    }

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
     * 右键菜单指着的那一行另给一层灰黑底（文件夹也给 —— 菜单对文件夹也有话可说）。
     */
    color: rowHighlight ? selColor : (rowContext ? contextColor : "transparent")

    /*
     * 行内元素之间的间隙。
     *
     * 图标尺寸**分级**：文件夹图标 15px，比正文字号（12）大一档；文件 13px ——
     * 层级在尺寸上也分得开。缩进按 depth 走，一眼能看出谁套在谁里面。
     */
    RowLayout {
        anchors.fill: parent
        spacing: 3
        anchors.leftMargin: root.iconInset
        anchors.rightMargin: 6

        AppIcon {
            id: chevronItem
            visible: root.isFolder
            provider: icons
            kind: modelData.expanded ? "chevron-down" : "chevron-right"
            tint: rowHighlight ? textBright : chevronDim
            size: 13
            Layout.alignment: Qt.AlignVCenter
        }

        /*
         * 图标那一格：文件夹图标 15、文件图标 13，都给同样宽的一格、图标靠左放。
         * 这样两级图标不但左边缘对齐，后面那个名字也跟着对齐（不然名字会差 2px）。
         */
        Item {
            id: iconCellItem
            Layout.preferredWidth: root.iconCell
            Layout.preferredHeight: root.iconCell
            Layout.alignment: Qt.AlignVCenter

            AppIcon {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                provider: icons
                kind: root.isFolder ? (root.isImportRoot ? "open" : "folder") : "file"
                tint: root.isFolder ? (root.isImportRoot ? importColor : folderColor) : accentColor
                size: root.isFolder ? 15 : 13
            }
        }

        Label {
            text: modelData.label !== undefined ? modelData.label : ""
            /*
             * 蓝底那一行（rowHighlight = 这份文件正开在编辑器里）的字单独一档：
             * #214283 上灰字 #bbbbbb 对比只有 5.04，分组行的 #e8e8e8 是 7.89，
             * 2026-09-24 他要"选中改成白色"—— 白字压上去 9.67。
             * 这里用字面量 #ffffff 而不走 Theme.c：白色是这套里**故意不进查表**的
             * 那几个值之一（浅色档选中底是 #1a4d8f，一样是深底，白字两边都成立）。
             */
            color: root.rowHighlight ? "#ffffff" : (root.isFolder ? textBright : textColor)
            font.pixelSize: 12
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        /*
         * 右边的计数：文件夹给"里面几个文件"，文件给"里面几条内容"。
         * 数字后面那个小字是单位，只给文件夹写（一行里已经够挤了）。
         *
         * 依赖 / 构建目录（node_modules、target…）没进去扫，计数无从谈起 ——
         * 那里写"未索引"，比写个"0 个文件"诚实（见 ClipboardStore::scanFolder）。
         */
        Label {
            text: root.isFolder ? (root.skipped ? "未索引" : (modelData.files + " 个文件"))
                                : (modelData.entries + " 条")
            color: root.rowHighlight ? "#ffffff" : textMuted
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
