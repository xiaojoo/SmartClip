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
    /*
     * 当前选中的**文件路径**（不是对象、也不是 id）。
     *
     * 树上现在没有"条目"这个概念了：一行要么是文件夹、要么是磁盘上真实存在的
     * 一个 md 文件，所以"选中的是谁"用路径认最稳 —— 重命名、刷新都还在。
     */
    property string selectedPath: ""

    /*
     * 右键菜单正指着哪一行（路径；空串 = 没有）。
     *
     * Main 弹出那一份行菜单时写进来、菜单收起时清掉（见 Main.qml 的
     * openTreeRowMenu / ddMenu.onClosed）。委托据此给那一行画一层灰黑底
     * （见 TreeDelegate.rowContext）—— 右键一个没打开的文件时，光看菜单
     * 看不出动的是哪一行。
     */
    property string contextPath: ""

    signal folderClicked(string key)
    signal fileClicked(var row)
    /* 行上按右键：Main 那边据此弹"打开 / 重命名 / 删除"那一份菜单 */
    signal rowContextMenuRequested(var row, real x, real y)

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
    /* 标题栏右边那个 ⋯ 弹的操作菜单；anchor 传被点的那个控件 */
    signal menuRequested(var anchor)
    /* 标题「项目 ∨」弹的"看哪一份"（项目 / 项目文件 / 打开的文件）；同上 */
    signal scopeMenuRequested(var anchor)

    /* 标题栏到底摆了几个按钮（自检用，见 src/SelfTest.cpp） */
    readonly property int toolbarButtonCount: toolRow.children.length

    /*
     * 标题栏那排按钮的尺寸：按钮 20×20、里面图标 13×13。
     *
     * 只在这里写一遍值，七个按钮都读它 —— 想再调大小改这两行就够。
     */
    readonly property int toolButtonSize: 20
    readonly property int toolIconSize: 13

    /*
     * 一级行的**展开箭头**落在面板的哪一列上 —— 标题「项目」的左边缘要对齐它
     * （用户报的"树往左靠、标题别动"，观感照 PyCharm：项目那一行正好在标题下面）。
     *
     *   5  ListView 的 leftMargin（见下面 view 里那一条）
     *   6  一级行的缩进（TreeDelegate.iconInset 的底数）
     * 两个数加起来是 11，和标题文字的左边缘一样 —— 这个"凑上"是**故意**的：
     * 改任一处，另一处都要跟着改（自检里钉了这条，见 SelfTest.cpp）。
     */
    readonly property int treeChevronInset: 5 + 6

    /*
     * 准星按钮能不能点（由 Main 按"当前标签是不是左树里的文件"给）。
     * 当前标签是未命名空白文档、或从菜单打开的其他文件时没什么可定位的，那就置灰。
     */
    property bool locateEnabled: true

    /*
     * 某个文件现在是不是落在可视区里（自检量"定位真的把目标滚进来了"用）。
     *
     * 列表是虚拟化的：行太远时委托根本没被创建，itemAtIndex() 直接给 null ——
     * 那也算"不在可视区里"。拿到了就比 y 和 viewport 的上下边界
     * （y 是内容坐标，要加上 contentY 才是屏幕上的位置）。
     */
    function rowVisible(path) {
        if (!path)
            return false
        for (var i = 0; i < rows.length; ++i) {
            var r = rows[i]
            if (r.kind !== "file" || r.path !== path)
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
     * 把某个文件滚进可视区（定位当前标签的最后一步，见 Main.locateCurrentFile）。
     *
     * 按**文件路径**找行，不按下标：树是每次重建的，下标随时会变。
     * 返回有没有找到那一行（没找到通常是搜索框把它过滤掉了）。
     */
    function scrollToPath(path) {
        for (var i = 0; i < rows.length; ++i) {
            var r = rows[i]
            if (r.kind === "file" && r.path === path) {
                view.positionViewAtIndex(i, ListView.Contain)
                return true
            }
        }
        return false
    }

    /*
     * 现在亮着蓝底的行有几个（自检用）。
     *
     * 直接问**委托自己**的 rowHighlight —— 量的是界面上真画出来的高亮，
     * 不是把 delegate 里那个表达式在这里再算一遍（那种断言自己证明自己，
     * 表达式改了它跟着改，什么也钉不住）。
     *
     * 只数已经实例化的行（列表是虚拟化的，屏幕外的行没有委托）——
     * 对"文件夹那一级不许亮"这条足够：分组行就在列表顶部，永远看得见。
     */
    function highlightCounts() {
        var folderRows = 0
        var fileRows = 0
        var contextRows = 0
        for (var i = 0; i < rows.length; ++i) {
            var it = view.itemAtIndex(i)
            if (!it)
                continue
            if (it.rowContext === true)
                ++contextRows
            if (it.rowHighlight !== true)
                continue
            if (rows[i].kind === "folder")
                ++folderRows
            else
                ++fileRows
        }
        return { folders: folderRows, files: fileRows, context: contextRows }
    }

    /*
     * 一级（文件夹）和二级（文件）的图标各落在哪一列上（自检用）。
     *
     * 两级必须一样 —— 这是"画出来"的几何，不是某个属性值，所以问委托自己
     * （TreeDelegate.iconCellX）。没实例化的行（列表是虚拟化的）跳过。
     */
    function iconColumnXs() {
        var folderX = -1
        var fileX = -1
        for (var i = 0; i < rows.length; ++i) {
            var it = view.itemAtIndex(i)
            if (!it)
                continue
            if (rows[i].kind === "folder" && folderX < 0)
                folderX = it.iconCellX
            else if (rows[i].kind === "file" && fileX < 0)
                fileX = it.iconCellX
        }
        return { folder: folderX, file: fileX }
    }

    /*
     * 标题「项目」那两个字的左边缘在场景里的 x（自检用）。
     *
     * 和 iconColumnXs 同一套口径（都是 mapToItem(null, …) 的场景坐标），
     * 自检直接比这两个数 —— 量的是**画出来**的位置，不是把常数再抄一遍。
     */
    function titleTextX() {
        return titleLabel.mapToItem(null, 0, 0).x
    }

    /*
     * 一级那一行的**展开箭头**在面板坐标里的 x（自检用）。
     *
     * 列表能横向滚（leftMargin 那几 px 就体现在 contentX 上）：内容跟着 contentX
     * 左右挪，而标题在列表外面不动 —— 所以要折回"没滚动时"的那一列。
     * contentItem.x = -contentX，所以**静止值 = 现在量到的 + contentX**
     * （实测量过：自检里 contentX=-5、箭头在 17，静止就是那 12）。
     */
    function firstRowChevronPanelX() {
        var it = view.itemAtIndex(0)
        if (!it)
            return -1
        return it.chevronCellX - root.mapToItem(null, 0, 0).x + view.contentX
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
         * 新建文件 / 刷新 / 定位当前文件 / 全部折叠 / 全部展开 / 更多 / 收起面板。
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
                /*
                 * 左边缘让**文字**落在一级行展开箭头那一列上（root.treeChevronInset）。
                 *
                 * 这个胶囊左右各留 5px（宽度 = 内容 + 10、里面那行居中），
                 * 所以左边距要减掉左边那 5px —— 写成算式而不是写死数字：
                 * 哪天改胶囊的留白，对齐不会跟着坏。
                 */
                anchors.leftMargin: root.treeChevronInset - (width - titleRow.implicitWidth) / 2
                anchors.verticalCenter: parent.verticalCenter
                height: 22
                width: titleRow.implicitWidth + 10
                radius: 4
                color: titleHit.containsMouse ? root.hoverColor : "transparent"

                RowLayout {
                    id: titleRow
                    anchors.centerIn: parent
                    spacing: 4
                    Label {
                        id: titleLabel
                        text: "项目"
                        color: root.textBright
                        font.pixelSize: 12
                        font.bold: true
                    }
                    AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 10 }
                }

                MouseArea {
                    id: titleHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    /*
                     * 标题弹的是"看哪一份"（项目 / 项目文件 / 打开的文件，
                     * 见 Menus.treeScopeMenu）；右边那个 ⋯ 弹的还是那排操作
                     * （menuRequested）—— 和 PyCharm 一样，标题管视图、
                     * 工具按钮管动作。
                     */
                    onClicked: root.scopeMenuRequested(titleButton)
                }

                AppToolTip { hovered: titleHit.containsMouse; text: "看哪一份文件" }
            }

            RowLayout {
                id: toolRow
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1

                /*
                 * 按钮 20×20、里面图标 13×13（宽度只在 root 上写一次）。
                 *
                 * 图标比 ToolButton 的默认 16 小一档：标题栏只有 35px 高，
                 * 七个 16px 的图标挤在一起比"项目"那行字还抢眼，
                 * 缩到 13 之后这一排才像工具的辅助按钮，不像主操作。
                 * 按钮本身也跟着从 22 收到 20 —— 图标四周留白比例不变。
                 */
                ToolButton {
                    implicitWidth: root.toolButtonSize
                    implicitHeight: root.toolButtonSize
                    iconSize: root.toolIconSize
                    provider: icons
                    kind: "plus"
                    tip: "新建文件（今天这一组里的一份 md）"
                    onClicked: root.newEntryRequested()
                }
                ToolButton {
                    implicitWidth: root.toolButtonSize
                    implicitHeight: root.toolButtonSize
                    iconSize: root.toolIconSize
                    provider: icons
                    kind: "refresh"
                    tip: "刷新列表"
                    onClicked: root.refreshRequested()
                }
                ToolButton {
                    implicitWidth: root.toolButtonSize
                    implicitHeight: root.toolButtonSize
                    iconSize: root.toolIconSize
                    provider: icons
                    kind: "locate"
                    enabled: root.locateEnabled
                    tip: root.locateEnabled ? "定位当前文件"
                                            : "当前标签不在左侧列表里"
                    onClicked: root.locateRequested()
                }
                ToolButton {
                    implicitWidth: root.toolButtonSize
                    implicitHeight: root.toolButtonSize
                    iconSize: root.toolIconSize
                    provider: icons
                    kind: "collapse-all"
                    tip: "全部折叠"
                    onClicked: root.collapseAllRequested()
                }
                ToolButton {
                    implicitWidth: root.toolButtonSize
                    implicitHeight: root.toolButtonSize
                    iconSize: root.toolIconSize
                    provider: icons
                    kind: "expand-all"
                    tip: "全部展开"
                    onClicked: root.expandAllRequested()
                }
                ToolButton {
                    id: moreButton
                    implicitWidth: root.toolButtonSize
                    implicitHeight: root.toolButtonSize
                    iconSize: root.toolIconSize
                    provider: icons
                    kind: "more"
                    tip: "更多选项"
                    onClicked: root.menuRequested(moreButton)
                }
                ToolButton {
                    implicitWidth: root.toolButtonSize
                    implicitHeight: root.toolButtonSize
                    iconSize: root.toolIconSize
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

                /*
                 * 左内边距：**5**（原来是 10）。
                 *
                 * 一级行的展开箭头 = 这个 5 + 行内缩进 6 = 面板 x=11，正好和标题
                 * 「项目」那两个字的左边缘对齐（用户要的"树往左靠、标题别动"，
                 * 观感照 PyCharm）。改这个数要连 root.treeChevronInset 一起看。
                 */
                leftMargin: 5
                topMargin: 8
                bottomMargin: 8

                /*
                 * 行与行之间留 2px 间隙。
                 *
                 * 原来留 4px 是因为"每行都有自己的圆角高亮块（选中 / 悬停）"，
                 * 贴在一起会连成一整片。现在悬停底色已经取消、蓝底只剩选中的
                 * 那一条，这个理由不成立了 —— 4px 反而把四个分组撑得很散，
                 * 只留一条细缝，行与行还认得出是独立条目就行。
                 */
                spacing: 2

                ScrollBar.vertical: ThinScrollBar {
                    anchors.right: parent.right
                    // anchors.rightMargin: 0
                    // topPadding: 8
                    // bottomPadding: 8
                }

            delegate: TreeDelegate {
                x: 12
                width: view.width - 18

                /*
                 * 蓝底只给**选中的文件**。
                 *
                 * 日期文件夹（一级菜单）和导入的文件夹原来也会跟着亮，实际看着
                 * 太抢眼：文件夹是容器，点它只是展开 / 收起，不该比里面选中的
                 * 那个文件还显眼。现在文件夹一律不亮，"打开的是哪一份"一眼可见。
                 */
                rowHighlight: modelData.kind === "file" && root.selectedPath !== ""
                              && modelData.path === root.selectedPath

                /*
                 * 右键菜单指着的那一行：文件夹也给（菜单对文件夹也有条目）。
                 * Main 弹菜单时写进 contextPath，菜单收起时清掉。
                 */
                rowContext: root.contextPath !== "" && modelData.path === root.contextPath

                onRowClicked: modelData.kind === "folder"
                              ? root.folderClicked(modelData.key)
                              : root.fileClicked(modelData)

                onRowContextMenu: (x, y) => root.rowContextMenuRequested(modelData, x, y)
                }
            }
        }

        /*
         * 面包屑里原来还画过一块"外部库 / 回收站"（一直是注释掉的装饰）。
         * 现在"外部库"这件事真的做了：导入的文件夹会和日期文件夹一起挂在
         * 同一个列表里（见 ClipboardStore::tree 与"更多"菜单的"导入文件夹…"），
         * 所以这块占位注释也没必要留了。
         */
    }
}