import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "qml/components"
import "qml/models"
import "qml/utils"
import "js/FolderManager.js" as Folders
import "js/TimeUtils.js" as Time
/*
 * 点击条目 / 菜单里的“复制”都要回填系统剪贴板，
 * 走的是 js/ClipboardManager.js 里包了一层 clipboardStore 的 Store。
 *
 * QML 的 JS import 是按文件生效的：
 * ClipboardModel.qml 里那份 `as Store` 不会外泄到这里，
 * 所以这个文件必须自己 import 一次，否则 selectItem() 里
 * 的 Store.copyItem() 会直接抛 ReferenceError（剪贴板也回填不了）。
 */
import "js/ClipboardManager.js" as Store

ApplicationWindow {
    id: window
    width: 1460; height: 900; minimumWidth: 1000; minimumHeight: 640; visible: true
    title: "SmartClip — 剪贴板"
    color: "#313335"

    /*
     * 去掉系统原生标题栏（截图里顶上那条白底、带图标 / 标题 /
     * 最小化 / 最大化 / 关闭的栏）。
     *
     * 三个窗口按钮因此改由底部栏最右侧的 WindowControls 接管
     * （见 qml/components/WindowControls.qml）。
     * 没有边框了，四边和四角的拖动改变大小也要自己补
     * （见文件末尾的 resizeHandles），否则窗口只能靠按钮最大化。
     */
    flags: Qt.Window | Qt.FramelessWindowHint

    // 原生标题栏没了，Fusion 样式下窗口背景仍可能闪白，这里直接压成深灰
    palette.window: "#313335"

    property string searchText: ""
    property var selectedItem: null
    property string activeFolder: "today"
    property var treeRows: []
    property bool showWhatTab: true

    // ---- 左侧列表宽度（可由中间间隙拖动调整） ----
    property real folderTreeWidth: 300
    readonly property real folderTreeMinWidth: 180
    readonly property real folderTreeMaxWidth: 600

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
        else if (act === "clearsearch") { searchText = ""; topBar.clearSearch() }
        else if (act.indexOf("folder:") === 0) activateFolder(act.substring(7))
        else if (act.indexOf("menu:") === 0) topBar.openGroup(act.substring(5))
    }

    Component.onCompleted: refresh()
    Connections { target: clipboardStore; function onChanged() { window.refresh() } }

    DropdownMenu { id: ddMenu; anchors.fill: parent; onSelected: (act) => window.handleCommand(act) }

    ColumnLayout {
        anchors.fill: parent; spacing: 0

        /*
         * 顶部这一行（原生标题栏去掉后它就是窗口最顶上的一行）：
         * 应用图标 / 分支 / 菜单 tab 在左，搜索框和
         * 缩小 / 放大 / 关闭 三个窗口按钮在最右边。
         */
        TopBar {
            id: topBar
            Layout.fillWidth: true
            host: window
            onOpenMenu: (anchor, items) => ddMenu.openFor(anchor, items)
            onSearchChanged: (text) => { window.searchText = text; window.refresh() }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0

            // ---- 左侧工具窗口图标条（已取消边框） ----
            Rectangle {
                Layout.fillHeight: true; Layout.preferredWidth: 34
                color: "#313335"
                // 已删除 border.color 和 border.width

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
                Layout.fillHeight: true; Layout.preferredWidth: window.folderTreeWidth
                rows: window.treeRows
                activeKey: window.activeFolder
                selected: window.selectedItem
                onFolderClicked: (key) => window.toggleFolder(key)
                onItemClicked: (item) => window.selectItem(item)
            }

            /*
             * FolderTree / EditorArea 中间的透明拖动热区。
             *
             * 原先这里是 EditorArea 的 Layout.leftMargin: 5，
             * 现在由这个完全透明的矩形占据同样的 5px；
             * 该区域本来就是窗口背景色 #313335，
             * 所以视觉样式没有任何改变。
             */
            Item {
                id: splitterHandle

                Layout.preferredWidth: 5
                Layout.fillHeight: true

                z: 100

                MouseArea {
                    id: splitterMouse

                    // 比间隙略宽，方便抓取（左右各外扩 1px）
                    x: -1
                    width: 7
                    height: parent.height

                    hoverEnabled: true
                    cursorShape: Qt.SplitHCursor
                    acceptedButtons: Qt.LeftButton

                    property real pressSceneX: 0
                    property real pressWidth: 0

                    onPressed: (mouse) => {
                        pressSceneX = mapToItem(null, mouse.x, 0).x
                        pressWidth = window.folderTreeWidth
                        mouse.accepted = true
                    }

                    onPositionChanged: (mouse) => {
                        if (!(mouse.buttons & Qt.LeftButton))
                            return

                        // 用场景坐标计算位移，热区自身随宽度移动也不受影响
                        var delta = mapToItem(null, mouse.x, 0).x - pressSceneX

                        window.folderTreeWidth =
                            Math.max(window.folderTreeMinWidth,
                                     Math.min(window.folderTreeMaxWidth,
                                              pressWidth + delta))
                        mouse.accepted = true
                    }

                    onReleased: (mouse) => { mouse.accepted = true }
                }
            }

            EditorArea {
                id: editor
                Layout.fillWidth: true;
                Layout.fillHeight: true
                // 原来的 Layout.leftMargin: 5 已由上面的透明热区占据

                /*
                 * 右侧留出和左侧热区等宽的 5px。
                 *
                 * 不留的话卡片右边缘会紧贴窗口边框：
                 * 右上 / 右下的圆角虽然画出来了，
                 * 但紧挨着窗口那圈浅色边框，看着就像被切掉的方角，
                 * 和左边（splitter 5px 背景间隙）不是一套。
                 */
                Layout.rightMargin: 5

                item: window.selectedItem
                showWelcome: window.showWhatTab
            }
        }

        /*
         * 底部：原来的状态栏保持不变。
         *
         * 缩小 / 放大 / 关闭 不放这里 —— 它们和搜索框一起
         * 在顶部那一行的最右边（见 TopBar.qml 末尾的 WindowControls）。
         */
        StatusBar {
            Layout.fillWidth: true
            title: window.selectedItem ? window.selectedItem.title : "README.md"
            count: cbm.entries.length
            copied: window.selectedItem !== null
        }
    }

    /*
     * 无边框窗口的四边 / 四角拖动改变大小。
     *
     * 热区贴在窗口最外圈、盖在内容之上（z 调高），
     * 用 Qt 系统级的 startSystemResize 交给窗口管理器处理，
     * 比自己算增量更跟手，也不会和中间的 splitter 抢事件。
     */
    MouseArea {
        id: resizeHandles
        anchors.fill: parent
        z: 1000

        // 四边 6px、四角 10px：够抓，又不至于挡住底栏的按钮
        property int edge: 6
        property int corner: 10

        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: {
            var x = mouseX, y = mouseY
            var l = x <= corner, r = x >= width - corner
            var t = y <= corner, b = y >= height - corner
            if ((l && t) || (r && b)) return Qt.SizeFDiagCursor
            if ((r && t) || (l && b)) return Qt.SizeBDiagCursor
            if (x <= edge || x >= width - edge) return Qt.SizeHorCursor
            if (y <= edge || y >= height - edge) return Qt.SizeVerCursor
            return Qt.ArrowCursor
        }

        onPressed: (mouse) => {
            var x = mouse.x, y = mouse.y
            var l = x <= corner, r = x >= width - corner
            var t = y <= corner, b = y >= height - corner
            var e = 0
            if (t) e |= Qt.TopEdge
            if (b) e |= Qt.BottomEdge
            if (l) e |= Qt.LeftEdge
            if (r) e |= Qt.RightEdge
            if (e === 0) {
                // 窗口内部：不接管，交给下面的按钮 / 列表
                mouse.accepted = false
                return
            }
            window.startSystemResize(e)
            mouse.accepted = true
        }
    }
}
