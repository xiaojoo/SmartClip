pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

/*
 * 顶部这一行（原生标题栏去掉后它就是窗口最顶上的一行）：
 *
 *   [S] SmartClip │ 文件 编辑 视图 … 帮助        [🔍 搜索剪贴内容]  [− □ ×]
 *
 * 最左边的 ☰ 汉堡键和「main」分支选择器已经去掉：
 * 汉堡键弹出的那组菜单和菜单栏 tab 是同一份东西，重复；
 * 分支名是从 IDE 抄来的装饰，这个应用不做版本控制。
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
    function toolItems() { return [{ label: "设置", act: "none" }, { label: "关于 SmartClip", act: "none" }] }

    /*
     * 锚点用左边的应用图标。
     *
     * 原来锚在 ☰ 汉堡键上，那个键已经删掉；
     * 现在唯一还会调到这里的是 Main.qml 的 "menu:" 命令，
     * 用它当锚点菜单会从这一行最左边弹出，位置仍然合理。
     */
    function openGroup(label) { root.openMenu(appBadge, root.menuItems(label)) }
    function clearSearch() { field.text = "" }
    function tabLabels() {
        return ["文件", "编辑", "视图", "导航", "代码", "运行", "工具", "VCS", "窗口", "帮助"]
    }

    IconProvider { id: icons }

    RowLayout {
        anchors.fill: parent
        spacing: 8

        /*
         * 左边这一组（应用图标 / 标题 / 菜单 tab）自带左内边距。
         *
         * 原来的写法是整行 RowLayout 加 anchors.leftMargin: 10 /
         * rightMargin: 10，简单，但右边那 10px 会一起把窗口按钮
         * 从窗口右边缘推开 —— 关闭键右边就会留出一条缝，
         * 鼠标滑到窗口最右上角点不到关闭（原生标题栏里是能点到的）。
         * 所以改成左右两组各自带边距：左边这组留 10，右边那组不留。
         */
        RowLayout {
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
            Layout.leftMargin: 10
            spacing: 8

            // ---- 左：应用图标 / 标题 ----
            Rectangle { id: appBadge
                Layout.preferredWidth: 20; Layout.preferredHeight: 20; radius: 4
                gradient: Gradient {
                    GradientStop { position: 0.0; color: "#f7971e" }
                    GradientStop { position: 1.0; color: "#ff6b6b" }
                }
                Text { anchors.centerIn: parent; text: "S"; color: "#ffffff"; font.pixelSize: 12; font.bold: true }
            }

            Label { text: "SmartClip"; color: root.textBright; font.pixelSize: 12; font.bold: true }

            // 竖分隔线
            Rectangle { Layout.preferredWidth: 1; Layout.preferredHeight: 16; color: root.borderColor
                        Layout.leftMargin: 2; Layout.rightMargin: 2 }

            // ---- 菜单 tab ----
            Repeater {
                id: tabRepeater

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
        }

        Item { Layout.fillWidth: true }

        /*
         * 右边这一组：搜索框 + 缩小 / 放大 / 关闭。
         *
         * 整组不带右边距，且高度撑满这一行，所以最右边的关闭键
         * 能一路顶到窗口的右上角（和原生标题栏一致），
         * 整窗圆角那一刀会把它右上角跟着修圆。
         */
        RowLayout {
            Layout.fillHeight: true
            Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            Layout.rightMargin: 0
            spacing: 6

            /*
             * 搜索框：宽度固定，高度自己定，所以外面再套一层
             * 撑满高度的 Item 让它垂直居中（直接给 Layout.preferredHeight
             * 在 fillHeight 的组里会被拉伸，边框就变成整行高了）。
             */
            Item {
                id: searchSlot

                Layout.preferredWidth: 300
                Layout.fillHeight: true
                Layout.rightMargin: 6

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    height: 24; radius: 5
                    color: root.fieldBg; border.color: root.borderColor
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 9; anchors.rightMargin: 9; spacing: 6
                        AppIcon { provider: icons; kind: "search"; tint: root.iconColor; size: 13 }
                        TextField {
                            id: field
                            Layout.fillWidth: true; Layout.fillHeight: true
                            placeholderText: "搜索剪贴内容"; placeholderTextColor: root.textMuted; color: root.textColor
                            font.pixelSize: 12; background: Item {}
                            verticalAlignment: TextInput.AlignVCenter
                            onTextChanged: root.searchChanged(text)
                        } }
                }
            }

            /*
             * 窗口按钮：缩小 / 放大(还原) / 关闭。
             *
             * 原生标题栏去掉后由这里接管，贴在这一行最右边，
             * 高度撑满整行，鼠标滑到最右边就是关闭。
             */
            WindowControls {
                id: controls

                host: root.host
                Layout.fillHeight: true
                Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
            }
        }
    }

    /*
     * 顶栏空白处的拖动移动窗口。
     *
     * 原生标题栏去掉以后，窗口只能靠边缘拉伸或者窗口按钮动，
     * 想挪个位置没地方下手。这里把顶栏的空白当标题栏用：
     * 按住拖动就带动整个窗口（和原生标题栏的手感一致）。
     *
     * 两个关键点：
     *
     * 1) 命中判断在 onPressed 里做。压在应用图标 / 菜单 tab /
     *    搜索框 / 窗口按钮上的按下，直接不接受，事件继续往下走，
     *    该开菜单开菜单、该打字打字；只有落在真正的空白处才开始拖窗。
     *    菜单 tab 自己是 MouseArea，会先把 pressed 吃掉，所以这里
     *    只需要额外避开图标、搜索框和右边那三个窗口按钮。
     *
     * 2) 移动交给系统的 startSystemMove()，不是自己算增量改
     *    window.x/y。这样贴边吸附、多屏 DPI 切换、最大化状态下
     *    拖动还原都归窗口管理器管，跟手程度和原生标题栏一样。
     *    它是阻塞调用，接管鼠标后由系统接管整个拖动过程，
     *    期间不会再给我们事件，所以用 started 兜一下重复进入。
     */
    MouseArea {
        id: dragArea

        anchors.fill: parent
        acceptedButtons: Qt.LeftButton

        /*
         * 千万不要开 hoverEnabled。
         *
         * 这一层铺满整行，一旦开了 hover，顶栏里所有控件的
         * hover 都会被它截住：菜单 tab 的高亮不再亮，左侧图标条
         * 和窗口按钮的底色点过一次之后就卡住不变 —— 因为它们的
         * containsMouse 收不到"鼠标离开"的事件了。
         *
         * 这里只要 pressed / released / doubleClicked 三个信号，
         * hover 一律让给下面的控件自己去接。
         *
         * preventStealing: 拖窗期间别让父级 Flickable 之类的容器
         * 把这次按下抢走（顶栏目前没有这种父级，留着是防御性的）。
         */
        preventStealing: true

        // startSystemMove() 已经发起过（同一个按下不重复发起）
        property bool started: false

        /*
         * 顶栏里需要让开鼠标的区域（要拖动时先避开它们）。
         *
         * 关键：it.x / it.y 是**相对各自父项**的坐标，不是顶栏坐标！
         * 实测（qmltestrunner 里量过）：
         *   appBadge   在顶栏里 x=10，但 it.x 报 0（父行有 leftMargin）
         *   searchSlot 在顶栏里 x≈1110，但 it.x 报 0（父行是右对齐的）
         *   controls   在顶栏里 x≈1322，但 it.x 报 312
         *   tab 的父行原点恰好就是顶栏原点，所以 tab 的 x 是对的
         * 之前直接拿 it.x / it.y 去比，算出来全是错的：搜索框和三个
         * 窗口按钮被判成"空白"，第一次按下就被拖窗吃掉，于是要点两次；
         * 点 tab 左边时又被 appBadge 那个假矩形（0..20）误判。
         * 所以必须用 mapToItem 换算到拖动层自己的坐标系 ——
         * mouse.x / mouse.y 正好就是拖动层坐标，两边就统一了。
         *
         * 原来这里写的是 mapToItem(null, 0, 0)，运行时报
         * "Cannot read property 'x' of undefined"，异常抛在 onPressed
         * 里直接把整个拖动打断，所以换成 mapToItem(dragArea, 0, 0)
         * 并且对返回值做判空。
         */
        function hitRect(it) {
            var tl = it.mapToItem(dragArea, 0, 0)
            if (!tl || tl.x === undefined)
                return null
            return { x: tl.x, y: tl.y, w: it.width, h: it.height }
        }

        // 只在"按下"时要判断：压在交互控件上的按下必须放行
        function overInteractive(mouse) {
            // 和 hitRect 一样，统一用拖动层自己的局部坐标
            var px = mouse.x
            var py = mouse.y

            var hits = [appBadge, searchSlot, controls]
            for (var i = 0; i < hits.length; ++i) {
                var it = hits[i]
                if (!it || !it.visible)
                    continue
                var r = hitRect(it)
                if (r && px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h)
                    return true
            }

            // 菜单 tab 是 Repeater 生成的，逐个比
            for (var j = 0; j < tabRepeater.count; ++j) {
                var tab = tabRepeater.itemAt(j)
                if (!tab || !tab.visible)
                    continue
                var tr = hitRect(tab)
                if (tr && px >= tr.x && px <= tr.x + tr.w && py >= tr.y && py <= tr.y + tr.h)
                    return true
            }

            return false
        }

        onPressed: (mouse) => {
            started = false

            if (!root.host || overInteractive(mouse)) {
                // 压在交互控件上：放行，让下面的控件处理
                mouse.accepted = false
                return
            }

            /*
             * 空白处：把拖动交给窗口管理器。
             *
             * 先 accepted 再调 startSystemMove()：后者是阻塞的，
             * 进去以后要等这次拖动结束才返回，写在它后面的赋值
             * 这一轮根本执行不到。
             */
            mouse.accepted = true
            started = true

            /*
             * startSystemMove() 不是所有平台 / 窗口类型都支持
             * （不支持时 Qt 会告警或直接抛错）。这里兜一层，
             * 免得一个异常把整个 onPressed 打断 —— 之前
             * overInteractive 抛 "Cannot read property 'x' of undefined"
             * 就是这个下场：报错刷屏，而且窗口完全拖不动。
             */
            try {
                root.host.startSystemMove()
            } catch (e) {
                console.warn("TopBar: startSystemMove 不可用：", e)
            }
        }

        onReleased: (mouse) => { started = false }

        // 双击空白处 = 放大 / 还原，和原生标题栏的习惯一致
        onDoubleClicked: (mouse) => {
            if (!root.host || overInteractive(mouse))
                return
            if (root.host.visibility === Window.Maximized)
                root.host.showNormal()
            else
                root.host.showMaximized()
        }
    }
}
