pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SmartClip.Terminal 1.0
import SmartClip.Globals 1.0
import "../utils"

/*
 * 底部终端面板（照 VS Code / Windows Terminal 那一套）。
 *
 * 只有"外壳"在这儿：标签条、新建/关闭、拖高度、收起、右侧回滚条。
 * 正文（字符网格、键盘、选区）全在 C++ 的 TerminalView 里（src/TerminalView.h），
 * 这一层不碰任何转义序列。
 *
 * 三条约定，改之前先看：
 *  1. 高度由外面给（root.panelHeight），本组件只发"拖到了多少像素"和"拖完了"
 *     两个信号 —— 记不记进设置、收到 0 还是收起来，是主窗口的事。
 *  2. 每个标签 = 一个独立会话 = 一个 TerminalView。切标签只是换 visible，
 *     后台那一拍的输出照样进来（不会"切走就丢字"）。
 *  3. 标签上的名字优先用 shell 自己报的（OSC 标题，cd 之后会变），
 *     没报过就退化成 powershell / cmd 这个程序名。
 */
Rectangle {
    id: root

    /* 开合和最大化由主窗口持有（它要拿这两个值算布局槽位） */
    required property bool opened
    required property bool maximized

    /* 拖高度 / 收起 / 最大化，都由外面落实 */
    signal draggedTo(real height)
    signal dragFinished()
    signal closeRequested()
    signal maximizeToggled()

    /* 底色交给里面那张卡片（card）—— 根要透明，Layout 让出来的那三条边距才露出窗口底色 */
    color: "transparent"
    /*
     * 只认 opened，**不要再加 `&& height > 0`**。
     *
     * 那样写是个死结：Quick Layouts 把不可见的项整个跳过（不给高度），
     * 而高度为 0 又让它不可见 —— 于是外面把槽位算成 260，这里永远停在 0
     * （实测 wanted=260 / height=0）。收起靠布局槽位给 0 就够了。
     */
    visible: root.opened

    readonly property real headerHeight: 30
    readonly property color borderColor: "#3c3f41"
    /* 和左树 / 编辑区那两张卡片同一套（实测 FolderTree、EditorArea 都是 10 + #1e1f22） */
    readonly property real cardRadius: 10
    readonly property color cardColor: "#1e1f22"
    readonly property color textColor: "#cccccc"
    readonly property color mutedColor: "#8a9199"
    readonly property color accentColor: "#0e639c"

    /*
     * 会话表：只存界面要显示的东西，正文在 itemAt 那一侧。
     *
     * 开局这一条是**声明出来的**，不是在 Component.onCompleted 里 append 的：
     * 在父组件补完之前往模型里加行，Repeater 会按 QQmlIncubator::AsynchronousIfNested
     * 走异步孵化，实测那条 delegate 一直孵不出来（views.count=1 而 itemAt(0) 永远为空、
     * 又不报任何错）。声明式给初值就没这个问题，语义上也更直白：面板一开就该有一条。
     */
    ListModel {
        id: sessionModel
        ListElement { title: "终端" }
    }
    property int current: 0
    /* 自检要读：面板有没有真被开出来、槽位要多高（红的时候靠这两个值定位） */
    readonly property int sessionCount: sessionModel.count
    readonly property real wantedHeight: Layout.preferredHeight

    IconProvider { id: icons }

    function newSession() {
        sessionModel.append({ title: qsTr("终端") })
        root.current = sessionModel.count - 1
    }

    function closeSession(index) {
        if (index < 0 || index >= sessionModel.count)
            return
        sessionModel.remove(index)
        if (root.current >= sessionModel.count)
            root.current = Math.max(0, sessionModel.count - 1)
    }

    /*
     * itemAt() 给的是委托**外层那个 Item**（里面才是 TerminalView + 位置条），
     * 所以两个取视图的口子都要解一层引用。
     */
    function currentView() {
        const d = views.itemAt(root.current)
        return d ? d.terminal : null
    }

    /* 自检用：Repeater 长出来的那条视图（findChild 够不到它，见 SelfTestTerminal.cpp） */
    function firstView() {
        const d = views.itemAt(0)
        return d ? d.terminal : null
    }

    /* 自检用：当前会话那条位置条（同样在委托里，C++ 抓不到） */
    function currentTrack() {
        const d = views.itemAt(root.current)
        return d ? d.scrollTrack : null
    }

    /* 自检用：顶边那条拖高度的把手（按住期间光标要钉在上下拉伸上） */
    function stripItem() {
        return resizeStrip
    }

    /*
     * 自检用：标签条和右边那一排按钮在**场景**里的矩形。
     *
     * 为什么要交出去：编辑区是 createWindowContainer 出来的原生子窗，它永远画在 QML
     * 上面。它要是往面板这边伸过去，右边那几颗按钮就被盖掉了（用户报的"打开文档之后
     * 添加/删除的气泡不见了"）。盖住这件事只有在"按钮的全局矩形 vs 那块原生窗的
     * 全局矩形"上才量得出来，光看 QML 自己的几何永远是"没重叠"。
     */
    function headerRect() {
        const a = header.mapToItem(null, 0, 0)
        return Qt.rect(a.x, a.y, header.width, header.height)
    }

    function headerButtons() {
        const out = []
        for (let i = 0; i < headerButtonsRepeater.count; ++i) {
            const it = headerButtonsRepeater.itemAt(i)
            if (!it)
                continue
            const p = it.mapToItem(null, 0, 0)
            out.push(Qt.rect(p.x, p.y, it.width, it.height))
        }
        return out
    }

    /*
     * 自检用：那 4 颗按钮的悬停气泡（Popup）。
     * 判据要真 open 一个出来、抓真实桌面看它落在的那几像素是不是气泡自己的底色 ——
     * 属性读起来永远是对的，被原生控件盖住这件事只有在图上才看得见。
     */
    function headerTips() {
        const out = []
        for (let i = 0; i < headerButtonsRepeater.count; ++i) {
            const it = headerButtonsRepeater.itemAt(i)
            if (it && it.tipItem)
                out.push(it.tipItem)
        }
        return out
    }

    function focusTerminal() {
        const v = currentView()
        if (v)
            v.forceActiveFocus()
    }

    /*
     * 一张卡片，不是通栏面板。
     *
     * 边距、圆角、底色全部对齐左树和编辑区那两张卡片（实测：圆角 10、卡片色
     * #1e1f22、无边框、树与内容之间那条缝是 3px 窗口底色）—— 见 Main.qml 里
     * TerminalPanel 那三行 Layout margin。
     */
    Rectangle {
        id: card

        anchors.fill: parent
        radius: root.cardRadius
        color: root.cardColor
        clip: true

            Column {
                anchors.fill: parent
                spacing: 0

                /* ---------------- 标签条 ---------------- */
                Rectangle {
                    id: header

                    width: parent.width
                    height: root.headerHeight
                    color: "#252526"
                    /*
                     * 只有上面两个角要圆 —— 它压在卡片的圆角上，直角会把卡片顶上那两个角
                     * 盖成方的（卡片的 clip 只裁矩形，不裁圆角）。
                     *
                     * 下面那半边用同色补平：Qt 里 `topLeft.radius` 这种分组属性在本工程
                     * 编译不过（报 Cannot assign to non-existent property "topRight"），
                     * 而"整块圆角 + 下半截盖平"视觉上完全等价，还不用碰那个语法。
                     */
                    radius: root.cardRadius

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: root.cardRadius
                        color: header.color
                    }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 4
                    spacing: 8

                    Label {
                        text: qsTr("终端")
                        color: root.mutedColor
                        font.pixelSize: 12
                        Layout.alignment: Qt.AlignVCenter
                    }

                    /* ---- 标签本体（横向可滚，超出不出滚动条，滚轮直接翻） ---- */
                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        Row {
                            id: tabRow

                            anchors.left: parent.left
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 2

                            Repeater {
                                model: sessionModel

                                Rectangle {
                                    id: tab

                                    required property int index
                                    required property var model

                                    readonly property bool active: index === root.current
                                    readonly property bool hot: hit.containsMouse

                                    /*
                                     * 宽度要给够：文字左边 9px，右边还要留关闭按钮那 20px
                                     * （按钮是 16 宽 + 4 右边距）。只加 22 的话按钮会压在
                                     * 标签最后两个字母上（实测 "powershell" 被盖成 "powersh??l"）。
                                     */
                                    width: Math.min(tabLabel.implicitWidth + 29, 220)
                                    height: 24
                                    radius: 3
                                    color: active ? "#1e1f21" : (hot ? "#2d2d30" : "transparent")

                                    Label {
                                        id: tabLabel

                                        anchors.left: parent.left
                                        anchors.leftMargin: 9
                                        anchors.verticalCenter: parent.verticalCenter
                                        /* 标题可能是 shell 报的一长串路径：超出就省略号，别把关闭按钮挤走 */
                                        width: Math.min(implicitWidth, tab.width - 29)
                                        elide: Text.ElideRight
                                        text: tab.model.title
                                        color: tab.active ? root.textColor : root.mutedColor
                                        font.pixelSize: 12
                                    }

                                    /* 关闭：只在"这条是当前标签"或悬停时出现（VS Code 的规矩） */
                                    Rectangle {
                                        id: closeButton

                                        anchors.right: parent.right
                                        anchors.rightMargin: 4
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 16
                                        height: 16
                                        radius: 3
                                        color: closeHit.containsMouse ? "#404043" : "transparent"
                                        visible: tab.active || tab.hot

                                        AppIcon {
                                            anchors.centerIn: parent
                                            provider: icons
                                            kind: "close"
                                            size: 10
                                            tint: closeHit.containsMouse ? "#ffffff" : root.mutedColor
                                        }

                                        MouseArea {
                                            id: closeHit

                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.closeSession(tab.index)
                                        }
                                    }

                                    MouseArea {
                                        id: hit

                                        anchors.fill: parent
                                        anchors.rightMargin: closeButton.visible ? 20 : 0
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                        onClicked: (mouse) => {
                                            if (mouse.button === Qt.MiddleButton)
                                                root.closeSession(tab.index)
                                            else
                                                root.current = tab.index
                                        }
                                    }
                                }
                            }
                        }
                    }

                    /* ---- 右边那一排：新建 / 清空 / 最大化 / 关闭 ---- */
                    Repeater {
                        id: headerButtonsRepeater

                        model: [
                            { kind: "plus",       tip: qsTr("新建终端"),        on: () => root.newSession() },
                            { kind: "trash",      tip: qsTr("清屏（含回滚）"),  on: () => { const v = root.currentView(); if (v) v.clearBuffer() } },
                            { kind: root.maximized ? "chevron-down" : "chevron-up",
                              tip: root.maximized ? qsTr("还原面板") : qsTr("最大化面板"),
                              on: () => root.maximizeToggled() },
                            { kind: "win-min",    tip: qsTr("关闭面板"),        on: () => root.closeRequested() },
                        ]

                        delegate: Rectangle {
                            id: cell

                            required property var modelData
                            readonly property bool hot: cellHit.containsMouse
                            /* 自检用：把这颗按钮的气泡交出去（见 headerTips()） */
                            readonly property var tipItem: headerTip

                            Layout.alignment: Qt.AlignVCenter
                            width: 24
                            height: 22
                            radius: 4
                            color: hot ? "#3a3a3d" : "transparent"

                            AppIcon {
                                anchors.centerIn: parent
                                provider: icons
                                kind: cell.modelData.kind
                                size: 14
                                tint: hot ? "#ffffff" : root.mutedColor
                            }

                            MouseArea {
                                id: cellHit

                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: cell.modelData.on()
                            }

                            AppToolTip {
                                id: headerTip

                                text: cell.modelData.tip
                                hovered: cell.hot
                                /*
                                 * 往下弹：往上弹就落进编辑区那块矩形里被盖掉
                                 * （编辑区不是 QML 画的，压不过 —— 见 AppToolTip.qml
                                 * 的 preferBelow 那段）。标签条在面板最上面，
                                 * 下面有的是终端正文那一块我们自己的地方。
                                 */
                                preferBelow: true
                            }
                        }
                    }
                }
            }

            /* ---------------- 正文 ---------------- */
            Item {
                id: body

                width: parent.width
                height: Math.max(0, parent.height - header.height)

                Repeater {
                    id: views

                    model: sessionModel

                    /*
                     * 一个会话 = 一块正文 + 它自己那条位置条。
                     * 位置条**必须跟着会话走**：以前它挂在 Repeater 外面、靠
                     * root.currentView() 拿视图，而 QML 的绑定会把**函数的结果缓存住** ——
                     * 第一次求值是 null 就永远是 null，条子从此再也不出现（实测
                     * "history=598 可见=0"）。放进委托里直接写 view.xxx 就没这个坑。
                     */
                    Item {
                        id: cell

                        required property int index
                        required property var model

                        /* 交给 root.currentView()/firstView() 解引用用 */
                        readonly property var terminal: view
                        /* 自检用：位置条也在委托里，C++ 那边 findChild 够不到（见 currentTrack） */
                        readonly property var scrollTrack: sb

                        anchors.fill: parent
                        visible: index === root.current

                        TerminalView {
                            id: view

                            /*
                             * 四周让开 2px：正文自己会铺满矩形底色，直角压在卡片的 radius 10
                             * 上会把下面那两个圆角盖成方的。让开 2px 之后露出来的是卡片自己
                             * 的底色（和正文同一个色），圆角就保住了 —— 和编辑区那套同一招
                             * （见 EditorArea 的 cardLeftInset，那边实测 2px 和 10px 画出来一样）。
                             * 上面不让：那一头接的是标签条，不是卡片的角。
                             */
                            anchors.leftMargin: 2
                            anchors.rightMargin: 2
                            anchors.bottomMargin: 2
                            anchors.fill: parent
                            focus: visible

                            /* 字号跟主窗口那条"编辑器字号"不是一回事，这里给一个终端常用的档 */
                            fontFamily: "Cascadia Mono"
                            fontSize: 13
                            foregroundColor: "#d4d4d4"
                            backgroundColor: root.cardColor
                            padding: 6
                            cornerRadius: root.cardRadius - 2

                            Component.onCompleted: {
                                /* 空 = 系统默认 shell；工作目录空 = 用户主目录 */
                                start("", "")
                            }
                            onTitleChanged: sessionModel.setProperty(index, "title", title)
                            onRunningChanged: if (!running)
                                                  sessionModel.setProperty(index, "title",
                                                                           qsTr("已退出"))
                        }

                        /*
                         * 回滚位置条：**内容一超出视口就常驻**（history > 0），贴底也留着、
                         * 窗口停在最下面；只有"没得滚"才藏。点/拖这条都直接写 scrollUp，
                         * 不给 thumb 挂 drag.target —— 一挂上去位置绑定就被抢走。
                         *
                         * `visible` 这个绑定要求 historyRows 变化时**有信号发出来**：它的
                         * NOTIFY 挂的是 gridChanged，而回滚行数在输出滚动时就会变、行列数
                         * 不动 —— 那一条补发在 TerminalView 的构造函数里（src/TerminalView.cpp）。
                         * 少了它，这条子就只在改尺寸时才重新求值，看着像"时隐时现"。
                         */
                        Rectangle {
                            id: sb

                            width: 8
                            /* 让开卡片右下那个圆角（正文让开 2px，见上面 view 的边距） */
                            height: parent.height - 2
                            anchors.right: parent.right
                            anchors.rightMargin: 3
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 2
                            visible: view.historyRows > 0
                            /* 8 宽 → 半径 4 = 整个短边，两端才是真半圆（不圆就是方块） */
                            radius: width / 2
                            color: "#2a2d2e"

                            Rectangle {
                                id: thumb

                                width: 6
                                x: 1
                                height: Math.max(24, sb.height * view.rows
                                                        / (view.historyRows + view.rows))
                                /* 贴底（scrollUp=0）时窗口在最下面 */
                                y: (sb.height - height) * (view.historyRows - view.scrollUp)
                                   / Math.max(1, view.historyRows)
                                radius: width / 2
                                color: "#4a4d50"
                            }

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor

                                /* 屏幕上这一个 y → 该往回翻多少行（越往上翻得越早） */
                                function upAt(y) {
                                    const travel = Math.max(1, sb.height - thumb.height)
                                    const frac = Math.max(0, Math.min(1,
                                                                      (y - thumb.height / 2) / travel))
                                    return Math.round(view.historyRows * (1 - frac))
                                }

                                /* 点哪儿窗口就跳到哪儿（不然只有按住拖能生效，点一下没反应） */
                                /*
                                 * 按住拖的这一段把光标钉住：轨道只有 8px 宽，手指抖一点就
                                 * 跑到正文里，光标闪回箭头 —— 和面板顶边那条拖高度的把手
                                 * 同一个毛病、同一个修法（见文件末尾 resizeStrip）。
                                 */
                                onPressed: (mouse) => {
                                    Win.pushResizeCursor(Qt.PointingHandCursor)
                                    view.scrollUp = upAt(mouse.y)
                                }
                                onPositionChanged: (mouse) => {
                                    if (pressed)
                                        view.scrollUp = upAt(mouse.y)
                                }
                                onReleased: Win.popResizeCursor()
                                onCanceled: Win.popResizeCursor()

                                onWheel: (wheel) => {
                                    /* 往前滚 = 看更早的，所以传负数（scrollLines 正方向是"往新内容"） */
                                    view.scrollLines(wheel.angleDelta.y > 0 ? -3 : 3)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* ---------------- 顶边拖高度 ---------------- */
    MouseArea {
        id: resizeStrip

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 6
        z: 10

        hoverEnabled: true
        cursorShape: Qt.SizeVerCursor
        acceptedButtons: Qt.LeftButton

        property real pressY: 0
        property real pressHeight: 0

        onPressed: (mouse) => {
            pressY = mapToItem(null, 0, mouse.y).y
            pressHeight = root.height
            /*
             * 按住这一段把光标钉成上下拉伸。
             *
             * cursorShape 只在鼠标**停在这 6px 上**时生效，而这条把手就 6px 高：
             * 拖得比面板长得快一点点，指针就跑进正文 / 标签条里，光标当场闪回箭头
             * （用户 2026-09-23 报的那条）。QML 的 cursorShape 管不住这种情况，
             * 得用应用级 override（见 src/WindowHelper.h 的 pushResizeCursor，
             * 左树和编辑区那条缝用的是同一招）。
             */
            Win.pushResizeCursor(Qt.SizeVerCursor)
            mouse.accepted = true
        }
        onPositionChanged: (mouse) => {
            if (!(mouse.buttons & Qt.LeftButton))
                return
            const y = mapToItem(null, 0, mouse.y).y
            root.draggedTo(pressHeight + (pressY - y))
            mouse.accepted = true
        }
        onReleased: (mouse) => {
            Win.popResizeCursor()
            root.dragFinished()
            mouse.accepted = true
        }
        onCanceled: {
            Win.popResizeCursor()
            root.dragFinished()
        }
    }
}
