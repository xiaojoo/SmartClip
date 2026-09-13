pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SmartClip.Globals 1.0

/*
 * 一块桌面便签的界面（由 src/StickyNotes.cpp 用 QQuickWidget 装进一个
 * 置顶无边框小窗里）。
 *
 * 三块：
 *
 *   ┌───────────────────────────────┐
 *   │ 便签 3        ● ● ● ● ⊞ ⇧ ✕  │  头部（按住拖动 / 换色 / 排列 / 置顶 / 关掉）
 *   ├───────────────────────────────┤
 *   │ 正文（TextEdit，纯文本）        │  随手记的地方，占满剩下的高度
 *   ├───────────────────────────────┤
 *   │ [链接卡片] [链接卡片] …         │  正文里的链接 —— 缩略图 + 域名，
 *   └───────────────────────────────┘  点一下进网站（见下面 linksStrip）
 *
 * 配色全部跟着便签纸（noteData.color）走：文字 / 次要文字 / 头部底色都是
 * C++ 侧算好的（StickyNote::textColor / mutedTextColor），因为便签纸有深有浅
 * （柠檬黄到石墨黑），这里再写一套就会两边不一致。
 *
 * 由 C++ 传进来的三个属性（QQuickWidget::setInitialProperties，见
 * StickyNoteWindow 的构造函数）：
 *   noteData      这条便签（正文 text / 底色 color / 链接清单 links / 派生色）
 *   noteWindow    这个窗口（拖动 / 改大小 / 关闭 / 置顶 / 删除 / 取色）
 *   windowNumber  便签编号（"便签 3"里的那个 3）
 */
Rectangle {
    id: root

    /*
     * C++ 传进来的三个对象 / 值（见 StickyNoteWindow 的构造函数）。
     *
     * **必须在 QML 侧声明**：QQuickWidget::setInitialProperties 只往
     * "已经声明过的属性"上写，没声明就报
     * "Setting initial properties failed: … does not have a property called …"，
     * 然后整份界面里到处 ReferenceError。声明成 var 就是"运行时填进来"的
     * 意思（C++ 填进来之前是 undefined，所以下面几个派生值都做了判空）。
     */
    property var noteData
    property var noteWindow
    property int windowNumber: 0

    /*
     * 便签纸本身。
     *
     * 圆角 + 描边：窗口是无边框透明的，圆角要自己画；外面那圈描边和主窗口
     * 一个做法（见 Main.qml 末尾那条 border 矩形），否则深色纸贴在深色桌面上
     * 边界会糊掉。真正的透明裁剪由 QWidget 的 WA_TranslucentBackground 负责。
     */
    color: root.noteData ? root.noteData.color : "transparent"
    radius: 8
    border.width: 1
    border.color: root.softInkColor

    /*
     * 整块便签的透明度（菜单里「透明度」那条，见 StickyNote::opacity）。
     *
     * 连文字一起透：半透明的便签本来就是"盖在文档上看"的，只透纸不透字会
     * 看着像错位。注意窗口本身是 WA_TranslucentBackground 的，所以这里降的
     * 是整块内容的 alpha，桌面会从底下透上来。
     */
    opacity: root.noteData ? root.noteData.opacity : 1.0

    /*
     * 便签纸上的三档颜色，全部由底色推出来（C++ 侧算的，见 StickyNote 的
     * textColor / mutedTextColor）：
     *
     *   inkColor      正文和图标的主色（浅纸深字、深纸浅字）
     *   softInkColor  描边 / 分隔这种"轻一点"的地方（主色带透明度）
     *   washColor     正文区那块底的底色（主色极淡的一层）
     *
     * 为什么在这几个派生组件（Repeater 的委托里那种）里不能直接写
     * `noteData.textColor`：那是**方法**在 QML 里当属性读会拿到函数对象
     * （报 "Unable to assign a function to a property of any type other than var"），
     * 而 Qt.rgba 这类函数直接赋值给 color 也报同一个错。统一走根上这几个
     * 已经算好的色值，两件事一起解决。
     */
    readonly property color inkColor: root.noteData ? root.noteData.textColor : "#1e2024"
    readonly property color softInkColor: root.noteData ? root.noteData.mutedTextColor
                                                        : Qt.rgba(0, 0, 0, 0.55)
    readonly property color washColor: Qt.rgba(root.inkColor.r, root.inkColor.g,
                                               root.inkColor.b, 0.07)
    readonly property color washBorderColor: Qt.rgba(0, 0, 0, 0.12)
    /*
     * 链接栏 / 头部那一条的底色。
     *
     * 这里不再用 C++ 那个 shadeColor（它得先知道纸的深浅），改成"主色加一层
     * 极淡的底"：浅纸上是一层灰、深纸上是一层亮 —— 和 shadeColor 起的是同一
     * 个作用（和便签纸拉开层次），但少一次跨语言取值，也就少一处创建期求值。
     */
    readonly property color stripColor: Qt.rgba(root.inkColor.r, root.inkColor.g,
                                                root.inkColor.b, 0.10)
    readonly property color stripBorderColor: Qt.rgba(0, 0, 0, 0.14)

    /*
     * noteData / noteWindow / windowNumber 是**根上的属性**，由 C++ 在创建
     * 这块窗口时用 QQuickWidget::setInitialProperties 给进来
     * （见 StickyNoteWindow 的构造函数）。
     *
     * 为什么不用 rootContext()->setContextProperty()：上下文属性在"对象构造
     * 期间"求值的绑定里读到的是 null —— QML 那边会报
     * "TypeError: Cannot read property 'palette' of null"，因为 Repeater 的
     * 委托是**创建期**就求值的（不是点击时才求值）。main.cpp 里 winHelper
     * 那个坑是同一回事，那边的解法是改注册成单例。
     *
     * 下面两个是把它们再摊平成"创建期一定算得出来"的值，给嵌套的委托 /
     * 内联 component 用。
     */
    readonly property bool pinned: noteWindow.staysOnTop

    /* 界面自检要量的几个值（见 StickyNotes::windowState） */
    readonly property int cardCount: linkRepeater.count
    readonly property int editorFontPx: editor.font.pixelSize
    readonly property real headerHeight: header.height
    readonly property real noteMargin: 10
    readonly property real editorWidth: editor.width
    /* 头部那一条的鼠标形状（拖动把手）：自检拿它确认"按住头部能搬窗口" */
    readonly property string headerCursor: "open-hand"
    /* 正文里链接那一栏是不是展开着（自检看这个 + cardCount） */
    property bool linksExpanded: true
    /* 头部那个「⋯」菜单开着没（自检看它，见 windowState） */
    readonly property bool menuOpened: noteMenu.opened
    /* 「⋯」按钮在便签窗口里的位置（自检量"菜单贴着按钮右下角"） */
    readonly property real menuButtonX: menuButton.mapToItem(null, 0, 0).x
    readonly property real menuButtonY: menuButton.mapToItem(null, 0, 0).y
    readonly property real menuButtonW: menuButton.width
    readonly property real menuButtonH: menuButton.height
    /* 色板里有几格（自检确认菜单里那个颜色面板真的摆出来了）。菜单还没开过的
       时候 swatches 是空的，所以这里要判空 —— 直接取 .length 会报
       "Cannot read property 'length' of undefined"（创建期就求值一次）。 */
    readonly property int menuSwatchCount: noteMenu.swatches ? noteMenu.swatches.length : 0

    /* 正文是否为空（决定要不要显示"随手写点什么…"那句提示） */
    readonly property bool emptyText: !root.noteData || root.noteData.text.length === 0
    readonly property bool hasLinks: cardCount > 0

    /*
     * 点到便签身子上的时候，如果菜单开着就先把菜单收掉。
     *
     * 菜单是个**不接焦点的独立窗口**（见 NoteMenu 的 flags），它自己收不到
     * "外面被点了"这件事。用一个 TapHandler 而不是盖一层 MouseArea：
     *   * MouseArea 会把 hover / 拖动全吃掉（头部拖动把手、编辑区选择都完蛋）；
     *   * TapHandler 只在"按下又抬起、没怎么动"时触发，而且不抢别人的事件。
     *
     * 菜单本身不在这个窗口里（它是独立的 Window），所以点在菜单上不会走到
     * 这儿 —— 不用担心把菜单自己的点击吃掉。
     */
    TapHandler {
        gesturePolicy: TapHandler.DragThreshold
        onTapped: (eventPoint) => {
            if (!noteMenu.opened)
                return
            /* 头部那个「⋯」按钮自己会开/收，交给它，别在这儿抢先收掉 */
            var local = eventPoint.position
            var inButton = local.x >= menuButton.x && local.x <= menuButton.x + menuButton.width
                           && local.y >= menuButton.y && local.y <= menuButton.y + menuButton.height
            if (!inButton)
                noteMenu.closeMenu()
        }
    }

    function setBackground(hex) {
        noteWindow.setNoteColor(hex)
    }

    /* 菜单里「更多颜色…」：开系统取色框（取消返回空串，颜色不动） */
    function pickCustomColor() {
        var picked = noteWindow.pickColor()
        if (picked !== "")
            setBackground(picked)
    }

    /*
     * 打开头部那个「⋯」菜单（界面上点按钮走这里，自检里也调它 ——
     * 走的是同一条路，量到的就是用户点出来的那份菜单）。
     *
     * (screenX, screenY) 是点击那一下鼠标在屏幕上的位置：菜单左上角就落在
     * 那儿（见 NoteMenu 开头"摆位：贴着鼠标"）。不给就用按钮右下角兜底。
     */
    function openNoteMenu(screenX, screenY) {
        var at = menuButton.mapToGlobal(menuButton.width, menuButton.height)
        var px = (screenX === undefined || screenX < 0) ? at.x : screenX
        var py = (screenY === undefined || screenY < 0) ? at.y : screenY
        noteMenu.openAt(px, py, noteWindow)
        return noteMenu.opened
    }

    /* 收起菜单（自检收尾用；界面上是点别处 / Esc / 选一条命令） */
    function closeNoteMenu() { noteMenu.closeMenu() }

    /*
     * 按动作名执行一条菜单命令。
     *
     * 菜单里绝大多数条目自己就做完了（见 NoteMenu 的 fire()），但"透明度和
     * 锁定"这两类要带参数、而且要把状态回写到数据上，所以留这一个口子：
     * 自检用 "opacity:70" / "pin" / "lock" / "hide" 这样的动作名驱动，
     * 走的是和菜单条目完全同一条路。
     */
    function handleMenuAct(act) {
        if (!act)
            return false
        if (act.indexOf("opacity:") === 0) {
            noteWindow.setOpacityPercent(parseInt(act.substring(8)))
            return true
        }
        if (act === "pin") { noteWindow.toggleStaysOnTop(); return true }
        if (act === "lock") { noteWindow.setLocked(!noteWindow.locked); return true }
        if (act === "hide") { noteWindow.closeNote(); return true }
        if (act === "copy") { noteWindow.copyText(); return true }
        if (act === "delete") { noteWindow.deleteNote(); return true }
        if (act === "pickColor") { pickCustomColor(); return true }
        return false
    }

    /* 在正文里定位第 index 条链接：把光标挪到那一行（卡片右下角那个小箭头） */
    function revealLink(index) {
        if (!root.noteData || !root.noteData.links || index < 0 || index >= root.noteData.links.count)
            return
        /* role 号从 C++ 那边读（NoteLinkModel::lineRole），别在这里写魔数 */
        var links = root.noteData.links
        var row = links.data(links.index(index, 0), links.lineRole)
        if (row === undefined || row < 0)
            return
        /*
         * 把光标挪到那一行并让它可见：TextEdit 拿到光标就会把那一行滚进
         * 可视区（前提是它是当前焦点项）。
         */
        var pos = editor.positionAt(row, 0)
        editor.cursorPosition = pos
        editor.forceActiveFocus()
    }

    /*
     * 链接卡片用 Repeater + Flow，不用 ListView。
     *
     * 卡片是"从正文里抽出来的几条"（通常两三张、最多十几张），而且位置跟着
     * 正文重排 —— Repeater + Flow 一次性算完布局，比 ListView 的视口 / 复用
     * 逻辑更直白，也不会在正文高频改动时和视图复用打架。
     */

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.noteMargin
        spacing: 8

        /* ------------------------------------------------------------------
         * 头部：编号 + 拖动条 + 一排按键
         * ---------------------------------------------------------------- */
        Item {
            id: header
            Layout.fillWidth: true
            Layout.preferredHeight: 22

            /* 整条都是拖动把手（按住就搬窗口） */
            MouseArea {
                id: headerDrag
                anchors.fill: parent
                cursorShape: Qt.OpenHandCursor
                acceptedButtons: Qt.LeftButton
                onPressed: noteWindow.beginDrag()
            }

            RowLayout {
                anchors.fill: parent
                spacing: 4

                Label {
                    text: "便签 " + root.windowNumber
                    font.pixelSize: 11
                    font.bold: true
                    color: root.softInkColor
                    Layout.alignment: Qt.AlignVCenter
                }

                /* 保存状态：这一条不是"文件"，改动是自动落盘的（见 StickyNoteStore） */
                Label {
                    text: root.noteData && root.noteData.linkCount > 0 ? "· " + root.noteData.linkCount + " 个链接" : ""
                    font.pixelSize: 10
                    color: root.softInkColor
                    Layout.alignment: Qt.AlignVCenter
                }

                Item { Layout.fillWidth: true }

                /*
                 * 唯一的按钮：「⋯」。
                 *
                 * 原来这里是一排 8 个色块 + 一个"更多颜色" + 排列 / 置顶 / 关闭
                 * 三个图标（共 12 个可点的小格子），在 330px 宽的便签上撞得
                 * 很满、也没法再加东西。现在全部收进这一个菜单里
                 * （见 qml/notes/NoteMenu.qml，照 Windows 便签那套排的）。
                 */
                Rectangle {
                    id: menuButton
                    /* 自检按名字找它（见 StickyNoteWindow 的 menuButtonX/Y） */
                    objectName: "noteMenuButton"
                    readonly property bool active: noteMenu.opened
                    readonly property bool hot: menuHit.containsMouse || active

                    implicitWidth: 22
                    implicitHeight: 18
                    radius: 4
                    color: hot ? Qt.rgba(root.inkColor.r, root.inkColor.g,
                                         root.inkColor.b, 0.16) : "transparent"
                    Layout.alignment: Qt.AlignVCenter

                    /* 三个点：竖排（Windows 便签那个 "…" 就是这个样子） */
                    Canvas {
                        id: dotsCanvas
                        anchors.centerIn: parent
                        width: 14
                        height: 14
                        antialiasing: true

                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.reset()
                            ctx.fillStyle = root.inkColor
                            for (var i = 0; i < 3; ++i) {
                                ctx.beginPath()
                                ctx.arc(7, 3 + i * 4, 1.5, 0, Math.PI * 2)
                                ctx.fill()
                            }
                        }

                        Connections {
                            target: root
                            function onInkColorChanged() { dotsCanvas.requestPaint() }
                        }
                        Component.onCompleted: dotsCanvas.requestPaint()
                    }

                    MouseArea {
                        id: menuHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: (mouse) => {
                            if (noteMenu.opened) {
                                noteMenu.close()
                                return
                            }
                            /* 把鼠标那一点（屏幕坐标）交给菜单：左上角贴鼠标 */
                            var p = menuHit.mapToGlobal(mouse.x, mouse.y)
                            root.openNoteMenu(p.x, p.y)
                        }
                    }

                    NoteTip {
                        hovered: menuHit.containsMouse && !noteMenu.opened
                        text: "便签菜单（颜色 / 透明度 / 置顶 / 删除…）"
                    }
                }
            }
        }

        /*
         * 头部那个「⋯」的菜单。
         *
         * 放在便签窗口的 QML 里（不是 C++）：条目都是"这一刻便签的状态"
         * （颜色、透明度、勾选、链接条数），由菜单自己在 openAt 里现搭，
         * 见 NoteMenu.qml 的 rebuild()。
         *
         * customColorRequested 那条只有"开系统取色框"要绕回本文件 —— 取色框
         * 得对着这块便签开（见 root.pickCustomColor）。
         */
        NoteMenu {
            id: noteMenu
            /* 自检要按名字找到它（见 StickyNotes::menuState） */
            objectName: "noteMenu"
            paper: root
            notes: Notes
            onCustomColorRequested: root.pickCustomColor()
        }
        /* ------------------------------------------------------------------
         * 正文
         * ---------------------------------------------------------------- */
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
            /* 正文只占"卡片栏之外"的那部分高度，见 linksStrip 的 preferredHeight */
            Layout.minimumHeight: 40

            Rectangle {
                anchors.fill: parent
                radius: 5
                /*
                 * 正文底：便签纸本身稍微亮一点点，让它看着像"写在纸上的一块
                 * 区域"，而不是浮在纸上的字。色的分量都在根上算好了
                 * （见 washColor / washBorderColor），这里只做赋值。
                 */
                color: root.washColor
                border.width: 1
                border.color: root.washBorderColor
            }

            Flickable {
                id: editorFlick
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                /* 让 TextEdit 自己长高，滚动交给这一层（便签正文通常不长） */
                contentWidth: width
                contentHeight: Math.max(height, editor.contentHeight + 2)
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: NoteScrollBar { }

                TextEdit {
                    id: editor
                    /*
                     * objectName 是给自检用的：它要能拿到这个编辑区、把内容
                     * 写进去（走的就是用户敲字那条路，见 StickyNoteWindow::typeText）。
                     */
                    objectName: "noteEditor"
                    width: editorFlick.width
                    /*
                     * 便签里写的是"字"，不是代码：等宽字体会让中文和英文行距
                     * 都变怪，用系统的界面字体。字号 13 是试出来的 ——
                     * 便签窗口就那么宽，再大一点一行放不下几个字。
                     */
                    font.pixelSize: 13
                    color: root.inkColor
                    selectionColor: Qt.rgba(0.2, 0.45, 0.8, 0.45)
                    selectedTextColor: root.inkColor
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    persistentSelection: true
                    textFormat: TextEdit.PlainText
                    text: noteData.text

                    /*
                     * 用户敲字 -> 回写数据（C++ 侧顺手重抽链接、排一次落盘）。
                     *
                     * 注意不要写 `onTextChanged: noteData.text = text` 之外的东西：
                     * noteData.text 变了会绕回来再设一次 text（绑定被打破），
                     * Qt 这边同一份内容再赋一次是空操作，不会死循环。
                     */
                    onTextChanged: noteData.text = text

                    /*
                     * 键盘：Esc 先收菜单（菜单不接焦点，键盘事件都落在编辑区
                     * 这儿），然后是"粘贴走纯文本"（便签不引外来样式）。
                     */
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Escape && noteMenu.opened) {
                            noteMenu.closeMenu()
                            event.accepted = true
                            return
                        }
                        if (event.matches(StandardKey.Paste)) {
                            editor.insert(Clipboard.text)
                            event.accepted = true
                        }
                    }

                    Text {
                        /* 空的时候那句提示；有字了就没了 */
                        visible: root.emptyText
                        text: "随手写点什么…\n\n" +
                              "（正文里的网址会自动变成下面的缩略图卡片）"
                        font.pixelSize: 12
                        color: root.softInkColor
                        wrapMode: Text.WordWrap
                        width: editor.width
                        topPadding: 2
                    }
                }
            }
        }

        /* ------------------------------------------------------------------
         * 链接卡片：正文里的网址 -> 网站缩略图，点一下进网站
         * ---------------------------------------------------------------- */
        Rectangle {
            id: linksStrip
            Layout.fillWidth: true
            /* 收起来时只留底下那条细杠（还能再展开） */
            Layout.preferredHeight: root.hasLinks
                                 ? (root.linksExpanded
                                    ? Math.min(linksFlow.height + 4, 150) : 1)
                                 : 0
            visible: root.hasLinks
            radius: 5
            color: root.stripColor
            border.width: 1
            border.color: root.stripBorderColor
            clip: true

            MouseArea {
                /* 收起来的那条细杠：点一下再展开 */
                anchors.fill: parent
                enabled: !root.linksExpanded
                cursorShape: Qt.PointingHandCursor
                onClicked: root.linksExpanded = true
            }

            Flickable {
                id: linksFlick
                anchors.fill: parent
                anchors.margins: 2
                clip: true
                contentWidth: width
                contentHeight: linksFlow.height
                boundsBehavior: Flickable.StopAtBounds
                visible: root.linksExpanded
                ScrollBar.vertical: NoteScrollBar { }

                Flow {
                    id: linksFlow
                    width: linksFlick.width
                    spacing: 6

                    Repeater {
                        id: linkRepeater
                        model: root.noteData ? root.noteData.links : null

                        delegate: Rectangle {
                            id: card
                            required property int index
                            required property string url
                            required property string title
                            required property string host
                            required property int line
                            required property string thumbSource
                            required property bool ready

                            width: 128
                            height: 86
                            radius: 5
                            color: Qt.rgba(1, 1, 1, 0.55)
                            border.width: cardHit.containsMouse ? 2 : 1
                            border.color: cardHit.containsMouse ? "#4c96d8"
                                                                : Qt.rgba(0, 0, 0, 0.18)

                            Column {
                                anchors.fill: parent
                                anchors.margins: 4
                                spacing: 3

                                /* ---- 缩略图 ---- */
                                Item {
                                    width: parent.width
                                    height: 52
                                    clip: true

                                    Image {
                                        id: thumb
                                        anchors.fill: parent
                                        /*
                                         * thumbSource 里带着一个版本号（抓图
                                         * 成功 / 失败各 +1）—— 不带着它的话
                                         * QQuickPixmapCache 会一直用"还没有图"
                                         * 那次的缓存，卡片永远是占位块
                                         * （见 NoteLinkModel::onThumbnailReady）。
                                         *
                                         * 但光有版本号还不够：正在联网取图的那几秒
                                         * 里 URL 有效、图还没有，Qt 会报一句
                                         * "Failed to get image from provider"。
                                         * ready 为假就先不给 source，取到图那一下
                                         * ready 变真、这条绑定重算，图才第一次被
                                         * 请求 —— 那句告警也就没有了。
                                         */
                                        source: card.ready ? card.thumbSource : ""
                                        sourceSize.width: 160
                                        sourceSize.height: 104
                                        fillMode: Image.PreserveAspectCrop
                                        asynchronous: true
                                        cache: false        // 版本号已经管了缓存，别让它再存一份
                                        visible: card.ready && status === Image.Ready

                                        /* 图来到之前淡入一下，别"啪"地跳出来 */
                                        opacity: visible ? 1 : 0
                                        Behavior on opacity {
                                            NumberAnimation { duration: 140 }
                                        }
                                    }

                                    /* 还没图 / 取不到图：一个占位块（带上域名首字母） */
                                    Rectangle {
                                        anchors.fill: parent
                                        visible: !thumb.visible
                                        color: Qt.rgba(0, 0, 0, 0.06)
                                        radius: 3

                                        Column {
                                            anchors.centerIn: parent
                                            spacing: 2
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: card.host.length > 0
                                                      ? card.host.charAt(0).toUpperCase() : "?"
                                                font.pixelSize: 18
                                                font.bold: true
                                                color: Qt.rgba(0, 0, 0, 0.42)
                                            }
                                            Label {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: card.ready ? "无预览图" : "载入中…"
                                                font.pixelSize: 9
                                                color: Qt.rgba(0, 0, 0, 0.38)
                                            }
                                        }
                                    }
                                }

                                /* ---- 标题 / 域名 ---- */
                                Label {
                                    width: parent.width
                                    text: card.title.length > 0 ? card.title : card.host
                                    font.pixelSize: 10
                                    font.bold: true
                                    color: "#22262b"
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                }
                                Label {
                                    width: parent.width
                                    text: card.host
                                    font.pixelSize: 9
                                    color: "#5a6068"
                                    elide: Text.ElideMiddle
                                    maximumLineCount: 1
                                }
                            }

                            MouseArea {
                                id: cardHit
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                onClicked: noteWindow.openLink(card.index)
                            }

                            /* 悬停时右上角那个小箭头：把正文滚到这条链接上 */
                            Rectangle {
                                width: 16
                                height: 16
                                radius: 8
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 3
                                visible: cardHit.containsMouse
                                color: "#4c96d8"
                                Label {
                                    anchors.centerIn: parent
                                    text: "↵"
                                    font.pixelSize: 10
                                    color: "#ffffff"
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.revealLink(card.index)
                                }
                            }

                            NoteTip {
                                hovered: cardHit.containsMouse
                                text: card.url
                            }
                        }
                    }
                }
            }
        }
    }

    /* ------------------------------------------------------------------
     * 右下角：拖边改大小（和主窗口四边的 resize 热区同一个做法）
     * ---------------------------------------------------------------- */
    MouseArea {
        width: 14
        height: 14
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        cursorShape: Qt.SizeFDiagCursor
        acceptedButtons: Qt.LeftButton
        onPressed: noteWindow.beginResize()

        /* 三道小斜线，示意"这里能拖" */
        Repeater {
            model: 3
            delegate: Rectangle {
                required property int index
                width: 5 - index
                height: 1
                color: root.softInkColor
                rotation: -45
                x: 12 - index * 4
                y: 12 - index * 0.5
            }
        }
    }


    /*
     * 便签上的小提示。
     *
     * 不直接用 AppToolTip（那个是主窗口那套深色卡片，尺寸和出现时机都是给
     * 主界面调的）：这里只要一个跟手的短提示。
     */
    component NoteTip: Rectangle {
        id: noteTip
        required property bool hovered
        property string text: ""

        visible: hovered && text !== ""
        z: 50
        radius: 4
        color: "#2b2d30"
        border.width: 1
        border.color: "#4b4d4f"
        implicitWidth: tipLabel.implicitWidth + 12
        implicitHeight: tipLabel.implicitHeight + 8
        /* 贴着鼠标那一格的下沿（别盖住图标自己） */
        x: Math.max(2, Math.min(parent.width - width - 2, 0))
        y: parent.height + 4

        Label {
            id: tipLabel
            anchors.centerIn: parent
            text: noteTip.text
            font.pixelSize: 10
            color: "#d6d7da"
        }
    }

    /* 便签窗口里的滚动条（正文 / 链接栏共用）：细一点，别抢戏 */
    component NoteScrollBar: ScrollBar {
        id: noteBar
        policy: ScrollBar.AsNeeded
        width: 6
        padding: 0

        contentItem: Rectangle {
            implicitWidth: 6
            radius: 3
            color: Qt.rgba(root.inkColor.r, root.inkColor.g, root.inkColor.b, 0.35)
            opacity: noteBar.pressed ? 1.0 : 0.7
        }

        background: Item { }
    }
}
