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
    anchors.leftMargin: 22
    anchors.rightMargin: 6

    property var rows: []
    property string activeKey: "today"
    property var selected: null

    signal folderClicked(string key)
    signal itemClicked(var item)

    /*
     * 标题栏那排工具按钮（对齐 PyCharm 的项目面板）。
     *
     * 按钮只把"被点了"这件事发出来，具体做什么全在 Main.qml ——
     * 数据（expanded / 排序 / 面板宽度）都在那边，这里只管画。
     */
    signal newEntryRequested()
    signal refreshRequested()
    signal collapseAllRequested()
    signal expandAllRequested()
    signal hideRequested()
    /* 定位当前标签（准星按钮）：在哪一组、第几行由 Main 那边算 */
    signal locateRequested()
    /* 标题 / "更多"都要弹出同一份菜单；anchor 传被点的那个控件 */
    signal menuRequested(var anchor)

    /* 标题栏到底摆了几个按钮（自检用，见 src/SelfTest.cpp） */
    readonly property int toolbarButtonCount: toolRow.children.length

    /*
     * 准星按钮能不能点（由 Main 按"当前标签是不是列表里的条目"给）。
     * 当前标签是磁盘文件或未命名空白文档时没什么可定位的，那就置灰。
     */
    property bool locateEnabled: true

    /*
     * 某一条现在是不是落在可视区里（自检量"定位真的把目标滚进来了"用）。
     *
     * 列表是虚拟化的：行太远时委托根本没被创建，itemAtIndex() 直接给 null ——
     * 那也算"不在可视区里"。拿到了就比 y 和 viewport 的上下边界
     * （y 是内容坐标，要加上 contentY 才是屏幕上的位置）。
     */
    function rowVisible(id) {
        for (var i = 0; i < rows.length; ++i) {
            var r = rows[i]
            if (r.kind !== "item" || r.item.id !== id)
                continue
            var it = view.itemAtIndex(i)
            if (!it)
                return false
            var top = it.y - view.contentY
            return top >= -0.5 && top + it.height <= view.height + 0.5
        }
        return false
    }

    /*
     * 把某一条滚进可视区（定位当前标签的最后一步，见 Main.locateCurrentItem）。
     *
     * 按**条目 id** 找行，不按下标：树是每次重建的，下标随时会变。
     * 返回有没有找到那一行（没找到通常是搜索框把它过滤掉了）。
     */
    function scrollToItem(id) {
        for (var i = 0; i < rows.length; ++i) {
            var r = rows[i]
            if (r.kind === "item" && r.item.id === id) {
                view.positionViewAtIndex(i, ListView.Contain)
                return true
            }
        }
        return false
    }

    readonly property color borderColor: "#43454a"
    readonly property color textBright:  "#ced0d6"
    readonly property color textMuted:   "#6f737a"
    /* 标题 / 按钮的悬停底色：比面板底色亮一点点就够，深色界面上不抢眼 */
    readonly property color hoverColor:  "#2b2d30"

    IconProvider { id: icons }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        /*
         * 面板标题栏。
         *
         * 左边是标题（点一下弹"项目树"菜单，和右边那个 ⋯ 是同一份），
         * 右边一排工具按钮 —— PyCharm 项目面板那几件事：
         * 新建条目 / 刷新 / 定位当前文件 / 全部折叠 / 全部展开 / 更多 / 收起面板。
         *
         * 按钮只有图标，说明文字走 AppToolTip（ToolButton 自带）。
         * 宽度不够时标题先被挤掉：按钮那排锚在右边，标题锚在左边，
         * 面板宽度见 Main.qml 的 folderTreeMinWidth（按这排按钮的宽度定的）。
         */
        Rectangle {
            id: header
            Layout.fillWidth: true
            Layout.preferredHeight: 35
            color: "transparent"

            Rectangle {
                id: titleButton
                anchors.left: parent.left
                anchors.leftMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                height: 22
                width: titleRow.implicitWidth + 10
                radius: 4
                color: titleHit.containsMouse ? root.hoverColor : "transparent"

                RowLayout {
                    id: titleRow
                    anchors.centerIn: parent
                    spacing: 4
                    Label { text: "项目"; color: root.textBright; font.pixelSize: 12; font.bold: true }
                    AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 10 }
                }

                MouseArea {
                    id: titleHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.menuRequested(titleButton)
                }

                AppToolTip { hovered: titleHit.containsMouse; text: "项目树选项" }
            }

            RowLayout {
                id: toolRow
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                /*
                 * 22×22 是照着标题栏的高度挑的：再大这一排就比"项目"那行字
                 * 高出一截，七个挤在一起也放不下（面板最窄只到 240）。
                 */
                ToolButton {
                    implicitWidth: 22
                    implicitHeight: 22
                    provider: icons
                    kind: "plus"
                    tip: "新建条目"
                    onClicked: root.newEntryRequested()
                }
                ToolButton {
                    implicitWidth: 22
                    implicitHeight: 22
                    provider: icons
                    kind: "refresh"
                    tip: "刷新列表"
                    onClicked: root.refreshRequested()
                }
                ToolButton {
                    implicitWidth: 22
                    implicitHeight: 22
                    provider: icons
                    kind: "locate"
                    enabled: root.locateEnabled
                    tip: root.locateEnabled ? "定位当前文件"
                                            : "当前标签不在左侧列表里"
                    onClicked: root.locateRequested()
                }
                ToolButton {
                    implicitWidth: 22
                    implicitHeight: 22
                    provider: icons
                    kind: "collapse-all"
                    tip: "全部折叠"
                    onClicked: root.collapseAllRequested()
                }
                ToolButton {
                    implicitWidth: 22
                    implicitHeight: 22
                    provider: icons
                    kind: "expand-all"
                    tip: "全部展开"
                    onClicked: root.expandAllRequested()
                }
                ToolButton {
                    id: moreButton
                    implicitWidth: 22
                    implicitHeight: 22
                    provider: icons
                    kind: "more"
                    tip: "更多选项"
                    onClicked: root.menuRequested(moreButton)
                }
                ToolButton {
                    implicitWidth: 22
                    implicitHeight: 22
                    provider: icons
                    kind: "minus"
                    tip: "收起面板"
                    onClicked: root.hideRequested()
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Qt.rgba(root.borderColor.r,
                               root.borderColor.g,
                               root.borderColor.b,
                               0.5)
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: view

                anchors.fill: parent

                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: root.rows

                leftMargin: 10
                topMargin: 8
                bottomMargin: 8

                /*
                 * 行与行之间留 4px 间隙。
                 *
                 * 每行都有自己的圆角高亮块（选中 / 悬停），
                 * 贴在一起时这些色块会连成一整片，
                 * 看不出是独立条目；留一点缝之后层次才清楚。
                 */
                spacing: 4

                ScrollBar.vertical: ThinScrollBar {
                    anchors.right: parent.right
                    // anchors.rightMargin: 0
                    // topPadding: 8
                    // bottomPadding: 8
                }

            delegate: TreeDelegate {
                x: 12
                width: view.width - 18

                rowHighlight: modelData.kind === "folder"
                              ? root.activeKey === modelData.key
                              : (root.selected &&
                                 root.selected.id === modelData.item.id)

                onRowClicked: modelData.kind === "folder"
                              ? root.folderClicked(modelData.key)
                              : root.itemClicked(modelData.item)
                }
            }
        }

        // Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: borderColor }

        // Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }

        // RowLayout {
        //     Layout.fillWidth: true
        //     Layout.leftMargin: 12
        //     Layout.rightMargin: 8
        //     spacing: 6
        //     Label { text: "外部库"; color: root.textMuted; font.pixelSize: 12; Layout.fillWidth: true }
        //     AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 10 }
        // }

        // RowLayout {
        //     Layout.fillWidth: true
        //     Layout.preferredHeight: 24
        //     Layout.leftMargin: 34
        //     Layout.rightMargin: 8
        //     spacing: 6
        //     AppIcon { provider: icons; kind: "trash"; tint: root.textMuted; size: 12 }
        //     Label { text: "回收站"; color: root.textMuted; font.pixelSize: 12; Layout.fillWidth: true }
        // }

        // Item { Layout.fillWidth: true; Layout.preferredHeight: 8 }
    }
}