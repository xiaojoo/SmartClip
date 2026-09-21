pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0
import "../../js/EditorMenus.js" as Menus

/*
 * 顶部这一行（原生标题栏去掉后它就是窗口最顶上的一行）：
 *
 *   [S] SmartClip │ 文件 编辑 搜索 … 帮助        [🔍 搜索剪贴内容]  [− □ ×]
 *
 * 菜单项和工具栏按钮是同一套命令（见 js/EditorMenus.js 与 Main.qml 的
 * dispatch），菜单里额外显示快捷键、勾选状态和禁用状态。
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
    // 编辑器本体，菜单里的勾选 / 禁用状态要看它
    property var view: null
    /*
     * 当前生效的快捷键清单（Cmd.shortcutItems，见 src/EditorController.h）。
     * 菜单条目里硬编码的那份只是出厂默认，用户改过键之后要以这份为准。
     */
    property var shortcuts: []

    signal openMenu(Item anchor, var items)
    signal searchChanged(string text)
    /*
     * 点了「帮助」那一栏。
     *
     * 它**没有下拉菜单**（见下面 navHit 的 onClicked 和 js/EditorMenus.js 的
     * helpMenu 说明）—— 点一下直接开"关于 SmartClip"。所以不走 openMenu，
     * 单独一个信号让 Main.qml 去 dispatch("about")。
     */
    signal aboutRequested()

    readonly property color borderColor: "#43454a"
    readonly property color iconColor:   "#8b929e"
    readonly property color textColor:   "#b4b8bf"
    readonly property color textBright:  "#ced0d6"
    readonly property color textMuted:   "#6f737a"
    readonly property color fieldBg:     "#2b2d30"

    /*
     * 这一栏点了有没有反应。
     *
     * 「帮助」也算有 —— 它虽然没有下拉菜单，但点一下直接开"关于"（见下面
     * onClicked）。不把它算进来的话那一栏没有 hover 反馈，看着像坏的。
     */
    function hasMenu(label) { return Menus.hasMenu(label) }
    /* 这一栏点下去是"直接执行"，不是"弹下拉菜单" */
    function isDirect(label) { return label === "帮助" }

    /*
     * 点某一栏。**鼠标和自检都走这一个函数** —— 自检要是自己另写一套触发方式，
     * 它验的就不是用户真走的那条路了（这个项目里栽过：自检 new 了一个影子
     * DocImport，结果验了个寂寞）。
     *
     * anchor 是**被点的那一栏**（菜单就挂在它正下方）。这里踩过一次：原来写死
     * 用 appBadge（左边那个应用图标）当锚点，于是点「设置」菜单从最左边弹出来，
     * 整条偏移到应用图标底下去了（用户报的"全部偏移菜单了"）。鼠标点的时候
     * 锚点只能是那一栏自己。
     *
     * anchor 传 null 时（比如 Main.qml 的 "menu:设置" 那条命令、自检）退回
     * appBadge —— 那种情况没有"被点的那一栏"可言。
     */
    function activateTab(label, anchor) {
        if (root.isDirect(label)) {
            root.aboutRequested()
        } else if (root.hasMenu(label)) {
            root.openMenu(anchor ? anchor : appBadge, root.menuItems(label))
        }
    }

    /*
     * 菜单条目。
     *
     * 第三个参数把"用户改过的快捷键"覆盖表带进去（name -> 组合键），
     * 没改过的动作不在表里，菜单就用 EditorMenus.js 里的出厂默认值。
     */
    function menuItems(label) {
        var ov = ({})
        for (var i = 0; i < (root.shortcuts ? root.shortcuts.length : 0); ++i) {
            var item = root.shortcuts[i]
            if (item && item.name)
                ov[item.name] = item.shortcut
        }
        return Menus.menuItems(label, root.view, ov)
    }

    /*
     * 按名字开某一栏的菜单，锚点用左边的应用图标。
     *
     * 走这条的是 Main.qml 的 "menu:xx" 命令（快捷键 / 自检调的那条路）——
     * 那种情况手里没有"被点的那一栏"，只能拿应用图标当锚点，菜单会从这一行
     * 最左边弹出。**鼠标点那一栏不走这里**，走 activateTab(label, 那一栏自己)，
     * 菜单挂在那一栏正下方。
     */
    function openGroup(label) { root.openMenu(appBadge, root.menuItems(label)) }
    function clearSearch() { field.text = "" }
    function tabLabels() { return Menus.tabLabels() }

    /*
     * 找某一栏那个方块（自检用）。
     *
     * 鼠标点的时候是 MouseArea 把 `parent`（那一栏自己）传给 activateTab 的；
     * 自检要走**同一条路**，就得先拿到那个方块。找不到返回 null。
     */
    function tabItem(label) {
        for (var i = 0; i < tabRepeater.count; ++i) {
            var cell = tabRepeater.itemAt(i)
            if (cell && cell.children[0] && cell.children[0].text === label)
                return cell
        }
        return null
    }

    /*
     * 某一栏在宿主窗口里的左边（自检用）。
     *
     * 自检拿它验"菜单挂在被点的那一栏正下方"—— 锚点写错时（比如写死用 appBadge）
     * 菜单会整条偏到窗口最左边，一比就露馅。找不到那一栏返回 -1。
     */
    function tabLeft(label) {
        var cell = root.tabItem(label)
        return cell ? cell.mapToItem(root, 0, 0).x : -1
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
            /*
             * 原来这里是一块橙色渐变圆角方块 + 白色 "S"（应用还没有图标时的占位）。
             * 换成随包的折带 S 裸图版：顶栏底色本来就是 #313335，再套一层深色瓦片
             * 等于白画一笔，所以用不带底的那一份。
             * sourceSize 取 2x 和 AppIcon 一个规矩（顶栏这一格 20 逻辑 px）。
             */
            Image { id: appBadge
                Layout.preferredWidth: 20; Layout.preferredHeight: 20
                source: "qrc:/brand/smartclip-bare.svg"
                sourceSize.width: 40; sourceSize.height: 40
                fillMode: Image.PreserveAspectFit
                antialiasing: true
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

                    /*
                     * 这一栏鼠标压上去有没有反馈。除了"有下拉菜单的"，还有
                     * 「帮助」—— 它一按就直接开"关于"，没有下拉菜单但也得亮起来。
                     */
                    readonly property bool live: root.hasMenu(modelData)
                                                 || root.isDirect(modelData)

                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredHeight: 22
                    Layout.preferredWidth: tabLabel.implicitWidth + 14
                    radius: 4
                    color: tabHover.containsMouse && live ? "#3a3d41" : "transparent"

                    Label {
                        id: tabLabel
                        anchors.centerIn: parent
                        text: modelData
                        font.pixelSize: 12
                        color: tabHover.containsMouse && live ? "#e8e8e8" : root.textColor
                    }

                    MouseArea {
                        id: tabHover
                        anchors.fill: parent
                        hoverEnabled: true
                        /* 锚点传**这一栏自己**（parent 就是那个 Rectangle）——
                           菜单要挂在被点的那一栏正下方，不是窗口最左边。 */
                        onClicked: root.activateTab(modelData, parent)
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

                            /*
                             * 编辑区是原生 QScintilla 子窗口，它拿着键盘焦点时
                             * 打字进不了 QML。所以这个输入框拿到焦点时，先把原生
                             * 控件的焦点交还给 QQuickWidget（见
                             * EditorViewItem::releaseEditorFocus）。
                             */
                            onActiveFocusChanged: {
                                if (activeFocus && root.view)
                                    root.view.releaseEditorFocus()
                            }
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
                Win.startSystemMove()
            } catch (e) {
                console.warn("TopBar: startSystemMove 不可用：", e)
            }
        }

        onReleased: (mouse) => { started = false }

        // 双击空白处 = 放大 / 还原，和原生标题栏的习惯一致
        // （展开 / 收拢的动画在 winHelper 里，见 src/WindowHelper.cpp）
        onDoubleClicked: (mouse) => {
            if (overInteractive(mouse))
                return
            /* 窗口操作统一走 WinHelper（见 src/WindowHelper.cpp） */
            Win.toggleMaximize()
        }
    }
}
