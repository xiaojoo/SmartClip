import QtQuick
import QtQuick.Controls
import QtQuick.Effects
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

    /*
     * 窗口本身必须透明，整窗的圆角才画得出来。
     *
     * 无边框（FramelessWindowHint）之后四角就是方角，
     * 所以要自己圆：真正负责裁剪的是下面 interfaceMask 那一层
     * （白底圆角矩形只当 alpha 遮罩，不参与显示）。
     * 窗口这一层留透明，遮罩裁掉的四角才会露出桌面，
     * 而不是露出一块方形的底色。
     *
     * 注意 palette.window 一并删掉：Fusion 样式会照着它刷一层
     * 不透明窗口底，那样四角又会被这块底色填回方形。
     */
    color: "transparent"

    // 整窗圆角半径（和 FolderTree / EditorArea 卡片的 radius: 10 同一套观感）
    readonly property real cornerRadius: 12

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

    property string searchText: ""
    property var selectedItem: null
    property string activeFolder: "today"
    property var treeRows: []
    property bool showWhatTab: true

    // ---- 左侧列表宽度（可由中间间隙拖动调整） ----
    property real folderTreeWidth: 300
    readonly property real folderTreeMinWidth: 180
    readonly property real folderTreeMaxWidth: 600

    /*
     * 全局强调色（#4c96d8）。
     *
     * 顶栏右侧的窗口按钮和左侧导航条的 hover 底色都用它，
     * 和编辑区 / 选中态本来就是同一个蓝，只在这里写一次值。
     */
    readonly property color accentColor: "#4c96d8"

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

    /*
     * 圆角遮罩的形状（白 = 保留，透明 = 挖掉）。
     *
     * 白色本身不会被画到屏幕上：visible: false 只是让它不参与正常显示，
     * 下面 maskSource 会单独把它渲染成一张纹理当蒙版用。
     */
    Rectangle {
        id: maskShape

        visible: false
        width: window.width; height: window.height
        radius: window.cornerRadius
        color: "#ffffff"
    }

    /*
     * 把遮罩形状渲染成纹理。
     *
     * MultiEffect 的 maskSource 必须是一个能提供纹理的源
     * （ShaderEffectSource），直接塞一个普通 Item 进去
     * 会拿不到纹理，结果整个窗口被乘成黑色 —— 实测就是这样。
     */
    ShaderEffectSource {
        id: maskTexture

        sourceItem: maskShape
        hideSource: true
        live: true
        smooth: true
        width: window.width; height: window.height
    }

    /*
     * 整个界面套一层圆角容器。
     *
     * 为什么不直接把 ApplicationWindow 自己的 color 设成圆角矩形：
     * 窗口的 color 只画在窗口最底层，上层任何铺满的矩形
     * （顶栏、状态栏、编辑区）都会把四角重新盖成方角出来。
     * 所以改成“先正常画完整个界面，再按圆角形状裁一刀”。
     *
     * 做法是 MultiEffect 的蒙版：它把 contentRoot 整棵子树
     * 渲染成一张纹理，再用 maskTexture 的 alpha 通道去裁。
     * 白 = 保留，透明 = 挖掉，于是四角被啃掉、露出透明窗口，
     * 圆角才真正成立（clip: true 做不到这件事 —— Qt Quick 的
     * clip 只认矩形，radius 不参与裁剪）。
     */
    Item {
        id: interfaceRoot

        anchors.fill: parent

        Rectangle {
            id: contentRoot

            anchors.fill: parent

            // 卡片之外那圈底（编辑区右侧 5px 间隙、左树面板左侧的留白）
            color: "#313335"

            layer.enabled: true
            layer.effect: MultiEffect {

                /*
                 * 蒙版纹理和 contentRoot 位置一致（同为 0,0 且同尺寸），
                 * 否则这条圆角裁剪会整体错位。
                 */
                maskEnabled: true
                maskSource: maskTexture

                // 圆角边缘抗锯齿，否则斜边会有台阶
                antialiasing: true
            }

            ColumnLayout {
                anchors.fill: parent; spacing: 0

        /*
         * 顶部这一行（原生标题栏去掉后它就是窗口最顶上的一行）：
         * 应用图标 / 应用名 / 菜单 tab 在左，搜索框和
         * 缩小 / 放大 / 关闭 三个窗口按钮在最右边。
         *
         * 原来左边还有 ☰ 和「main」分支选择器，已经去掉，
         * 详见 qml/components/TopBar.qml 开头。
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
                            id: navCell
                            required property var modelData
                            width: 26; height: 26; x: 4; radius: 5

                            /*
                             * 悬停态：整格填强调蓝 + 图标转白。
                             *
                             * 选中那一格原本是 #3a4a5a 的浅蓝底，
                             * 鼠标压上去时也让位给同一片蓝色 ——
                             * 否则 hover 在选中的格子上完全没反馈。
                             *
                             * 这里用绑定而不是 onEntered/onExited 手动改色：
                             * 绑定是幂等的，鼠标快速划过多格也不会串色。
                             */
                            readonly property bool hot: navHit.containsMouse
                            color: hot ? window.accentColor
                                       : (modelData.active ? "#3a4a5a" : "transparent")

                            AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: modelData.k
                                      tint: navCell.hot ? "#ffffff"
                                                        : (modelData.active ? window.accentColor : "#9aa0a8")
                                      size: 16 }
                            MouseArea { id: navHit; anchors.fill: parent; hoverEnabled: true }
                        }
                    }
                    Item { width: 1; height: Math.max(1, parent.height - 300) }

                    // 底部齿轮：导航条的收尾格子，同样给 hover（它不接点击）
                    Rectangle {
                        id: gearCell
                        width: 26; height: 26; x: 4; radius: 5
                        readonly property bool hot: gearHit.containsMouse
                        color: hot ? window.accentColor : "transparent"
                        AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: "gear"
                                  tint: gearCell.hot ? "#ffffff" : "#9aa0a8"; size: 16 }
                        MouseArea { id: gearHit; anchors.fill: parent; hoverEnabled: true }
                    }
                }
            }

            FolderTree {
                id: folderTree
                Layout.fillHeight: true
                Layout.preferredWidth: window.folderTreeWidth

                /*
                 * 面板右边缘在窗口里的 x。
                 *
                 * 这里用实际几何反推，不手算常量：布局槽位的右边界
                 * 正好就是那条 5px 间隙的左边界（实测 raw=334，
                 * 间隙在 334..338），所以直接取它。
                 * FolderTree 自己的 anchors 边距不算进槽位里，
                 * 之前写 folderTreeWidth + 12 就是差了这一段。
                 */
                readonly property real panelRight: mapToItem(window.contentItem, width, 0).x

                rows: window.treeRows
                activeKey: window.activeFolder
                selected: window.selectedItem
                onFolderClicked: (key) => window.toggleFolder(key)
                onItemClicked: (item) => window.selectItem(item)
            }

            /*
             * FolderTree / EditorArea 中间的间隙。
             *
             * 拖动的热区不在这里：它挪到窗口级了（见文件末尾的
             * splitterMouse）——放在布局里会被四边 resize 热区压住，
             * 收不到 hover，光标也就一直是箭头。
             * 这里只负责占住那 5px 的间隙。
             */
            Item {
                id: splitterGap

                Layout.preferredWidth: 5
                Layout.fillHeight: true
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
        }
    }

    /*
     * 圆角外侧那一圈描边。
     *
     * 窗口底色已经是透明的，深灰界面直接贴到桌面上会显得“糊”，
     * 压一条比底色亮一点点的细线，边界才立得住。
     * 它和遮罩用同一个半径，所以描边是贴着裁剪边缘走的。
     */
    Rectangle {
        anchors.fill: parent
        z: 10
        color: "transparent"
        radius: window.cornerRadius
        border.width: 1
        border.color: "#4b4d4f"
    }

    /*
     * 无边框窗口的四边 / 四角拖动改变大小。
     *
     * 拆成四条贴着窗口外沿的长条热区（上下各 6px 高、左右各 6px 宽），
     * 用 Qt 系统级的 startSystemResize 交给窗口管理器处理，
     * 比自己算增量更跟手。
     *
     * 为什么不再是原来那个 anchors.fill 的整窗热区：
     * 它 hoverEnabled: true，会把整个窗口的 hover 事件全吃掉 ——
     * 鼠标不管落在哪里，事件都先到它这一层（z:1000）并被 accept，
     * 下层的顶栏菜单 tab、左侧图标条、右上角三个窗口按钮
     * 就永远收不到 hover，hover 底色也就永远不亮。
     * （同一个原因，中间那条分隔线的光标以前也被压成箭头，
     *   当时是把分隔线单独抬到 z:2000 绕过去的，见下面 splitterMouse。）
     *
     * 只贴四条边，窗口内部就还给下面的控件；
     * 四角的行为和以前一致：按下时把相邻的那条边一起带进 startSystemResize。
     */
    component ResizeEdge: MouseArea {
        // 这条热区代表哪条边（Qt.LeftEdge / Qt.TopEdge / …）
        required property int edge
        // 要操作的窗口
        property var host: null

        // 四角 10px 内同时带上相邻的边
        readonly property int corner: 10

        hoverEnabled: true
        acceptedButtons: Qt.LeftButton

        /*
         * 光标只用「左右」和「上下」两种双向箭头，不用斜箭头。
         *
         * 原因是四角的光标会盖住旁边的东西：左右两条竖边正好从
         * 中间那条可拖动的分隔线（splitter）两头经过，
         * 顶到角落时如果显示斜箭头，那条分隔线上也会跟着变成斜的，
         * 看着像在拖角。所以左右边缘一律优先给 SizeHorCursor。
         * 功能不变，startSystemResize 里还是照旧带上角。
         */
        cursorShape: (edge === Qt.LeftEdge || edge === Qt.RightEdge)
                     ? Qt.SizeHorCursor : Qt.SizeVerCursor

        onPressed: (mouse) => {
            // 热区自身坐标 -> 窗口坐标：四角的判定要按整窗尺寸来算
            var p = mapToItem(parent, mouse.x, mouse.y)

            var e = edge
            if (p.y <= corner) e |= Qt.TopEdge
            if (p.y >= parent.height - corner) e |= Qt.BottomEdge
            if (p.x <= corner) e |= Qt.LeftEdge
            if (p.x >= parent.width - corner) e |= Qt.RightEdge

            host.startSystemResize(e)
            mouse.accepted = true
        }
    }

    ResizeEdge {
        edge: Qt.LeftEdge; host: window; z: 1000
        width: 6
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
    }
    ResizeEdge {
        edge: Qt.RightEdge; host: window; z: 1000
        width: 6
        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
    }
    ResizeEdge {
        edge: Qt.TopEdge; host: window; z: 1000
        height: 6
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
    }
    ResizeEdge {
        edge: Qt.BottomEdge; host: window; z: 1000
        height: 6
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
    }

    /*
     * FolderTree / EditorArea 中间那条可拖动的分隔线。
     *
     * 原来是塞在布局里的一个 Item，问题是它会被当时那层
     * 整窗 resize 热区（anchors.fill + 更高的 z）压住：
     * 鼠标移上去 hover 事件全被 resize 层吃掉，
     * 分隔线自己的 cursorShape 根本不会生效，一直是箭头；
     * 靠上/靠下时还会显示 resize 层的斜箭头。
     *
     * 所以改成窗口级的独立热区，z 比 resize 层更高，
     * 位置跟着树面板的右边缘走。这样 hover 一定是它先收到，
     * 光标稳定显示 Qt.SplitHCursor（左右两个箭头），
     * 而且落点正好在那条 5px 的间隙上。
     */
    MouseArea {
        id: splitterMouse

        /*
         * 位置直接取树面板的右边缘（= 间隙左边界），
         * 往左外扩 4px、往右盖住 5px 的间隙，落点就是那条缝。
         */
        x: folderTree.panelRight - 4
        width: 9
        height: parent.height

        z: 2000

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

            // 用场景坐标算位移，热区自身随宽度移动也不受影响
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
