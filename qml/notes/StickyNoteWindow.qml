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
     * 窗口这一层**整块透明**，便签纸是下面那张 paper。
     *
     * 为什么把"纸"从根上拆下来：一摞便签左边那排切换色块要画在**卡片外面**
     * （贴着桌面，见 tabStrip），窗口左边给色块让出的那一条就必须透出桌面 ——
     * 根这一层要是还刷着纸色，色块看着就是"画在卡片里"（用户截图报的就是这个）。
     *
     * 圆角 / 描边跟着挪到 paper 上（原来画在这一层）。真正的透明裁剪由 QWidget
     * 的 WA_TranslucentBackground 负责（见 src/StickyNotes.cpp 的构造函数）。
     */
    color: "transparent"

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
    /*
     * 一摞便签那几个状态（转发 noteWindow 上的属性）：自检量"这一块在不在
     * 一摞里、是不是露头那张"直接读根对象就行，不用再去 C++ 里绕。
     */
    readonly property bool inGroup: noteWindow ? noteWindow.inGroup : false
    readonly property bool groupActive: noteWindow ? noteWindow.groupActive : false
    readonly property int groupSize: noteWindow ? noteWindow.groupSize : 0
    /*
     * 这一块**锁着没**。锁定 = 整块不响应鼠标（正文 / 拖动 / 改大小 / 标签条都
     * 让开），只剩头部那排按钮还能点 —— 好让用户解锁（见 StickyNotes::setLocked
     * 里那段"为什么不再用窗口级鼠标穿透"）。
     */
    readonly property bool locked: noteWindow ? noteWindow.locked : false
    /* 有没有一块便签正被拖到这一块身上（头部那条会亮，见下面头部里的提示块） */
    readonly property bool dropPreview: noteWindow ? noteWindow.dropPreview : false
    /*
     * 左边那条"文件夹标签"上画什么：**它所在那一摞（组合）里的每一块**一个
     * 色块（自己也在里面）。
     *
     * 只有**组合过**的便签才有这条标签条（用户明确要求："这个左边的 tab 不是
     * 每个卡片都有，只有组合的才有"）—— 没组合过的单独一块便签左边干干净净。
     * 归成一摞之后，界面上只摆一张纸（露头那块），其余几块收成这排色块，
     * 点一下换上来（见下面 tabStrip）。清单是 C++ 算好给的（窗口自己翻不到
     * 别的便签的数据），这里只负责画。
     */
    readonly property var groupTabs: noteWindow ? noteWindow.groupTabs : []
    /*
     * 这一块是摞里"收起来的那张纸"：界面上它整块**不画**，只在标签条上留一个
     * 色块。用 collapsed 而不是把窗口藏掉 —— 藏窗口那一下整摞会闪，而且
     * "露着的那一块"和"标签条上选中的那一个"就对不上了。
     */
    readonly property bool collapsed: noteWindow ? noteWindow.tabbed : false
    /*
     * 要不要画标签条 / 那条占多宽 —— **由 C++ 明确推过来**（见下面
     * Connections 里的 syncTabStrip），不写成"groupTabs.length > 1"那种绑定。
     *
     * 为什么不用绑定：`groupTabs` 是 C++ 的属性，它的 NOTIFY 到了 QML 这边
     * 有时候推不动那几个派生值（实测：窗口刚建时是"没组合"-> 算出
     * tabStripWidth = 0 / hasTabStrip = false，之后组合好了、C++ 那边
     * groupTabs 已经是 3 项，这几个值却还停在 0/false —— 标签条整条不画，
     * 屏幕上就是一张空白卡片，用户报的就是这个）。
     *
     * 现在这两个值只有一处会改：C++ 的 tabsChanged 一到，就在那个槽里现算
     * 一次（colorChanged / 建组 / 拆组 / 换纸都会发 tabsChanged，见
     * StickyNoteWindow::notifyTabsChanged）。窗口左边那条宽度的"真相"本来也
     * 在 C++（frameRectFor 按 tabStripWidth() 算），这里跟着它走就一致了。
     */
    property bool hasTabStrip: false
    property real tabStripWidth: 0
    /* 自检用：标签条上有几个色块（自己那块也在里面） */
    readonly property int tabCount: groupTabs.length
    /*
     * 那一排色块**是不是由这一块画**。
     *
     * 一摞便签是各自独立的窗口、每块都拿着同一份 groupTabs —— 同组几块要是都
     * 画一遍，后面几块的色块会正好落在前面那块**透明的那一条**上，斜着叠成
     * 一串。所以只让**露头那块**（完整露在桌面上的那张纸）画；收起来的那块
     * 整块不画、只在别人的标签条上占一个色块（见 collapsed）。
     */
    readonly property bool chipStripDrawn: hasTabStrip && !collapsed

    /*
     * C++ 那边"标签条状态变了"的通知：现读一次 groupTabs 并把
     * hasTabStrip / tabStripWidth 定下来（读的时候用 noteWindow.groupTabs
     * 而不是上面那个只读绑定，免得又踩到绑定不刷新的坑）。
     */
    function syncTabStrip() {
        var tabs = noteWindow ? noteWindow.groupTabs : []
        var on = tabs ? tabs.length > 1 : false
        if (hasTabStrip !== on)
            hasTabStrip = on
        var w = on ? 34 : 0
        if (tabStripWidth !== w)
            tabStripWidth = w
    }

    Connections {
        target: noteWindow
        function onTabsChanged() { root.syncTabStrip() }
    }
    Component.onCompleted: root.syncTabStrip()
    /*
     * 自检用：便签纸的左边缘 / 色块的右边缘（都是窗口坐标系里的 x）。
     *
     * 规矩是**色块整块落在纸外面**（chipRight < paperLeft）—— 这就是"切换 tab
     * 在卡片外边"唯一能量出来的形式。只量 tabStripWidth > 0 是不够的：纸要是
     * 还刷满整个窗口（色块压在纸上），那个断言照样是绿的（用户截图报的正是这个）。
     */
    readonly property real paperLeft: paper.x
    readonly property real chipRight: tabStrip.x + tabColumn.x + tabColumn.width
    /* 正文里链接那一栏是不是展开着（自检看这个 + cardCount） */
    property bool linksExpanded: true
    /* 便签菜单开着没（自检看它，见 windowState） */
    readonly property bool menuOpened: noteMenu.opened

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
     *
     * 组合那一句同理：这一块要是叠在下面（露着一条标题栏），点它就是
     * "把这张纸抽到最上面"。onPressed 而不是 onTapped —— 像 Windows 便签那样
     * 按下去就换到前面，不用等抬手（抬手那一下多半已经落在编辑区里在选字了）。
     * 不抢鼠标：TapHandler 只是旁听，TextEdit 该收的按下事件照收。
     */
    TapHandler {
        gesturePolicy: TapHandler.DragThreshold
        /* 锁定之后整块不响应鼠标（只剩头部那排按钮，好解锁，见 setLocked） */
        enabled: !root.locked
        onTapped: (eventPoint) => {
            /*
             * 点这一块 = 把它抽到最上面（叠着的纸里点了下面那张）。
             *
             * 用 onTapped（按下 + 抬起、没怎么动）而不是 onPressed：TapHandler
             * 是**旁听**鼠标事件的（不能盖 MouseArea，否则编辑区选字、拖动把手
             * 全完蛋），而 Qt 会把鼠标事件发给所有收到它的窗口 —— 别处点一下、
             * 这一块刚好在那时候露出来，也会顺带触发。真点一下有完整的
             * 按下 + 抬起，onTapped 不会被那种误触带上；promoteInGroup 自己
             * 还会再确认一次"这一块是当前活动窗口"（见那里的说明）。
             */
            noteWindow.promoteInGroup()
            /* 左键点便签 = 顺手把菜单收掉（右键才是弹菜单，见下面那个 TapHandler） */
            if (noteMenu.opened)
                noteMenu.closeMenu()
        }
    }

    /*
     * **便签上任意位置右键 -> 弹便签菜单**（用户要求："右键点便签的任意位置
     * （正文 / 头部 / 标签条）也弹这个菜单"，并且"⋯ 不要了" —— 头上那个三点
     * 按钮已经撤掉，右键就是唯一的入口）。
     *
     * 还是用 TapHandler，不盖 MouseArea：
     *   * 旁听，不抢鼠标 —— 正文选字、拖动把手、色块点击、链接卡片全都照旧；
     *   * 不给光标设形状（MouseArea 的 cursorShape 默认是箭头，盖在正文上会把
     *     那个 I 型光标弄没）。
     *
     * 只认右键，左键那条走上面那个 TapHandler（抽到最上面 + 收菜单）。
     * 锁定的便签不认鼠标 —— 要解锁点头上那个锁按钮（见 setLocked）。
     */
    TapHandler {
        objectName: "noteRightClick"
        acceptedButtons: Qt.RightButton
        enabled: !root.locked
        onTapped: (eventPoint) => {
            if (noteMenu.opened) {
                noteMenu.close()
                return
            }
            /* 菜单要的是**屏幕坐标**（它是一块独立窗口，见 NoteMenu::openAt） */
            var at = root.mapToGlobal(eventPoint.position.x, eventPoint.position.y)
            root.openNoteMenu(at.x, at.y)
        }
    }

    function setBackground(hex) {
        noteWindow.setNoteColor(hex)
    }

    /*
     * 注意这里**没有**"开 Qt 取色框"那条路了：原来便签右键菜单里那条「更多颜色」
     * 连它飞出的色板一起删掉了（用户要求），C++ 那套 pickColor / colorDialogFor
     * 也一并删了。换底色的入口只剩头上那个颜色弹窗（见下面 palettePopup）。
     */

    /*
     * 打开便签菜单（界面上是**在便签上点右键**走这里，见上面那个 TapHandler；
     * 自检也调它 —— 走的是同一条路，量到的就是用户点出来的那份菜单）。
     *
     * (screenX, screenY) 是点击那一下鼠标在屏幕上的位置：菜单左上角就落在
     * 那儿（见 NoteMenu 开头"摆位：贴着鼠标"）。不给就用便签左上角兜底
     * （以前兜底是"⋯ 按钮右下角"，那个按钮已经撤了）。
     */
    function openNoteMenu(screenX, screenY) {
        var at = root.mapToGlobal(0, 0)
        var px = (screenX === undefined || screenX < 0) ? at.x : screenX
        var py = (screenY === undefined || screenY < 0) ? at.y : screenY
        noteMenu.openAt(px, py, noteWindow)
        return noteMenu.opened
    }

    /* 收起菜单（自检收尾用；界面上是点别处 / Esc / 选一条命令） */
    function closeNoteMenu() { noteMenu.closeMenu() }

    /*
     * 颜色弹窗（便签头上那个色块按钮点开的调色板）。
     *
     * 用户的要求是"颜色放到外面来、做成弹窗的形式"：不再钻进「⋯」菜单的子面板
     * 里，而是便在签头上的一个按钮，点开就是一份调色板。
     *
     * 窗口本身见下面 palettePopup：Popup.Window（和 AskCard / 下拉菜单同一类，
     * 只占自己那一小块原生窗），所以弹窗能探到便签外面去 —— 便签只有 330 宽，
     * 挤在窗口里会被裁掉。
     *
     * 开完必须**把弹窗那块原生窗顶到最上面**（见 raisePopupWindow）：便签自己
     * 是置顶的，点按钮那一下被激活、抬上去的是便签 —— 不顶这一下，第二次点开
     * 弹窗就压在卡片底下（用户报的"再次点击会被卡片遮挡"，实测弹窗被夹进卡片
     * 范围里时整块都看不见）。
     */
    function openPalette() {
        palettePopup.openAt(colorButton)
        noteWindow.raisePopupWindow(palettePopup.objectName)
        return palettePopup.opened
    }
    function closePalette() { palettePopup.close() }

    /*
     * 颜色按钮那一下该"开"还是该"收"。
     *
     * 判据是 palettePopup.closedByButton：弹窗**刚才那一下是不是被这个按钮按关的**
     * （在弹窗的 onClosed 里当场判，见 palettePopup 里那段说明）。
     *
     * 为什么不在这一下现读 palettePopup.opened：弹窗带着 CloseOnPressOutside，按在
     * 按钮上那一下它先收到事件、先关，之后这次按下才被重放给便签窗口 —— 实测事件
     * 顺序是 `onClosed -> colorHit.onPressed -> colorHit.onClicked`，等到 onClicked
     * 再问"开着没"永远是 false，于是这一下永远走"开"，按钮就再也关不掉弹窗
     * （用户看到的"再次点击"没反应就是这么来的）。
     */
    function paletteButtonClicked() {
        if (palettePopup.closedByButton) {
            /* 这一下按在按钮上、已经把弹窗收掉了：这个标记用掉，别再开一次 */
            palettePopup.closedByButton = false
            return false
        }
        return openPalette()
    }

    /* 自检用：色板有哪几格 / 按第几格换个色（走的都是界面上那条路） */
    function paletteSwatchAt(index) { return palettePopup.swatchAt(index) }
    function pickPaletteSwatch(index) { return palettePopup.pickAt(index) }

    /* 锁定按钮：和菜单里那条「锁定（鼠标穿透）」同一条路（handleMenuAct） */
    function toggleLock() { return handleMenuAct("lock") }

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
        /*
         * 组合：菜单里"与…组合"那一条点下去就是它（act 形如
         * "group:<便签 id>"）。走的是和 C++ 直调同一条路 —— 菜单只负责把
         * "和哪一块"告诉这边，怎么摆那一摞是 StickyNotes 的事。
         */
        if (act.indexOf("group:") === 0) {
            var mate = findNoteById(act.substring(6))
            if (!mate)
                return false
            return Notes.groupWith(noteData, [mate])
        }
        if (act === "ungroup") { return Notes.ungroup(noteData) }
        if (act === "pin") { noteWindow.toggleStaysOnTop(); return true }
        if (act === "lock") { noteWindow.setLocked(!noteWindow.locked); return true }
        if (act === "hide") { noteWindow.closeNote(); return true }
        if (act === "copy") { noteWindow.copyText(); return true }
        if (act === "delete") { noteWindow.deleteNote(); return true }
        return false
    }

    /* 按便签 id 找到那条数据（菜单里"与…组合"点了一条，拿到的就是这个 id） */
    function findNoteById(id) {
        if (!Notes || !id)
            return null
        var all = Notes.noteList()
        for (var i = 0; i < all.length; ++i) {
            if (all[i] && all[i].id === id)
                return all[i]
        }
        return null
    }

    /* 在正文里定位第 index 条链接：把光标挪到那一行（卡片右下角那个小箭头） */    function revealLink(index) {
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

    /*
     * 便签纸（卡片本体）。
     *
     * 左边那条 tabStripWidth 是**留给标签条**的：色块画在那条里，是露在卡片
     * **外面**的一张张"文件夹标签"（见下面 tabStrip）；所以这张纸从色块右边
     * 开始，窗口左边那一条留透明、让桌面透上来 —— 用户要的就是这个观感：
     * 色块贴着桌面，纸是一张独立的卡片。
     *
     * 便签纸的**尺寸不变**，只是整张往右挪了标签条那一条：窗口 = 标签条 + 纸
     * （C++ 的 frameRectFor 就是按这个把窗口往左长出来的，便签自己的几何
     * 从头到尾都是这张纸），纸右边 / 上边 / 下边都还贴着窗口边（见 content 的
     * leftMargin 也是同一个数，正文跟着纸走）。
     *
     * 圆角 + 描边：窗口是无边框透明的，圆角要自己画；外面那圈描边和主窗口
     * 一个做法（见 Main.qml 末尾那条 border 矩形），否则深色纸贴在深色桌面上
     * 边界会糊掉。
     */
    Rectangle {
        id: paper
        anchors.fill: parent
        anchors.leftMargin: root.tabStripWidth
        color: root.noteData ? root.noteData.color : "transparent"
        radius: 8
        border.width: 1
        border.color: root.softInkColor
    }

    /*
     * ===========================================================================
     * 左边那条"文件夹标签"：组合里的便签互相切换
     * ===========================================================================
     *
     * 一块便签**组合**进某一摞之后，界面上只摆那一张纸（露头那块），同组其余
     * 几块收成左边这排色块；点一下就把那一块换上来（见 StickyNotes::
     * switchGroupTab）。没组合过的便签**没有这条标签条** —— 用户明确要求：
     * "这个左边的 tab 不是每个卡片都有，只有组合的才有"。
     *
     * 几件必须说清楚的事：
     *   * 色块的样子按用户给的参考图：矩形**右下角切掉一个斜角**（像一张文件夹
     *     标签），选中的那个描一圈重边；
     *   * 色块**露在卡片外面**、贴着桌面（纸从它右边开始，见上面 paper），和纸
     *     之间留着一条缝（那条缝在窗口里是透明的，桌面直接透上来）；
     *   * 颜色 / 编号是**每一块便签自己的**（groupTabs 里带来的）——这一块窗口
     *     拿不到别的便签的数据，那列表是总管算好给的（见 StickyNoteWindow::
     *     groupTabs）。
     *
     * 竖直排在左边、贴着顶：参考图就是从上往下排的，一眼能数出摞里有几张纸。
     * 摆不下（一摞里块数多、便签又被拉得很矮）就滚动。
     */
    Item {
        id: tabStrip
        /*
         * 色块只由**露头那块**画（见 chipStripDrawn 的说明）：同组几块要是都画
         * 一遍，后面几块的色块会落在前面那块透明的那一条上，斜着叠成一串。
         */
        visible: root.chipStripDrawn
        /* 锁定之后标签条上的色块也点不着（见 setLocked） */
        enabled: !root.locked
        /* 抬一层：悬停提示往右压在正文那一带上，别被后声明的 content 盖住（见下面 tabTip） */
        z: 5
        x: 0
        y: root.noteMargin
        width: root.tabStripWidth
        height: Math.max(0, root.height - 2 * root.noteMargin)

        /* 色块：宽 = 标签条那条宽，高 = 宽（参考图里就是这个比例） */
        readonly property real chipSize: root.tabStripWidth - 6
        /* 右下角那个斜角多大（参考图里约 45°、四分之一条边） */
        readonly property real chipBevel: 6

        /*
         * 上面那排色块：超出可视区就滚动（ListView 自己管内容高度）。
         *
         * 用 ListView 而不是 Flickable + Column：色块数量就是"这一摞里有几块
         * 便签"，多的时候列表滚动比手算便宜，也顺手带上滚轮。
         */
        ListView {
            id: tabColumn
            x: 3
            width: tabStrip.chipSize
            height: tabStrip.height
            spacing: 4
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: root.groupTabs
            interactive: contentHeight > height
            ScrollBar.vertical: NoteScrollBar { }

            delegate: Item {
                id: chip
                required property var modelData
                /* 选中的那个亮一圈（它就是右边露着的那张纸） */
                readonly property bool selected: modelData.selected === true
                width: tabColumn.width
                height: tabColumn.width

                Canvas {
                    id: chipShape
                    anchors.fill: parent
                    antialiasing: true

                    /*
                     * 形状：整块减掉右下角那个三角（参考图里"文件夹标签"的样子）。
                     * 用 Canvas 画而不是 Rectangle + radius：带圆角的矩形切一个
                     * 斜角，QML 里没有现成的图元（Rectangle 只有一个 radius）。
                     */
                    onPaint: {
                        const ctx = getContext("2d")
                        const w = width
                        const h = height
                        const b = tabStrip.chipBevel
                        /*
                         * 清干净用 clearRect + beginPath，**不要用 ctx.reset()**：
                         * 这个 Qt 里的 Canvas 没有实现 reset（调了什么都不发生，
                         * 路径是空的 -> fill() 画不出任何东西），色块就"根本没画"
                         * —— 用户看到的就是一张空白卡片、左边一格色块都没有
                         * （踩过：抓住图里第 17 列整列都是透明的，才定位到这里）。
                         * 本文件别的 Canvas 也都是 clearRect 起手。
                         */
                        ctx.clearRect(0, 0, w, h)
                        ctx.beginPath()
                        ctx.moveTo(0, 0)
                        ctx.lineTo(w, 0)
                        ctx.lineTo(w, h - b)
                        ctx.lineTo(w - b, h)
                        ctx.lineTo(0, h)
                        ctx.closePath()
                        ctx.fillStyle = chip.modelData.color
                        ctx.fill()
                        ctx.lineWidth = chip.selected ? 2 : 1
                        ctx.strokeStyle = chip.selected
                                          ? Qt.rgba(root.inkColor.r, root.inkColor.g,
                                                    root.inkColor.b, 0.75)
                                          : Qt.rgba(0, 0, 0, 0.28)
                        ctx.stroke()
                    }

                    /*
                     * 要重画的两个时机（Canvas 不会自己跟着绑定走）：
                     *   * 尺寸定下来的时候（Component.onCompleted 那一下宽度很可能
                     *     还是 0，画出来就是一片空白 —— 色块看着"根本没画"）；
                     *   * 选中状态 / 委托被复用到另一条色块上（modelData 换了）。
                     */
                    onWidthChanged: requestPaint()
                    onHeightChanged: requestPaint()
                    Component.onCompleted: requestPaint()
                }

                MouseArea {
                    id: chipHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        /* 点自己那一块什么都不用做（它已经露着了） */
                        if (!chip.selected)
                            noteWindow.selectGroupTab(chip.modelData.id)
                    }

                    /*
                     * 悬停：把"哪一块、它在哪一格"报给标签条那一层的提示（见下面
                     * tabTip）。**提示不能挂在色块身上** —— 色块在 clip: true 的
                     * ListView 里，委托是它的子树，画在色块外面的提示整块被裁掉
                     * （用户看到的就是"气泡只剩左边两个字"）。
                     */
                    onContainsMouseChanged: {
                        if (containsMouse) {
                            tabStrip.hoveredChipId = chip.modelData.id
                            tabStrip.hoveredChipNumber = chip.modelData.number
                            /* 上下沿都按标签条那一层算（色块滚动时跟着一起变） */
                            tabStrip.hoveredChipTop = chip.mapToItem(tabStrip, 0, 0).y
                            tabStrip.hoveredChipBottom = tabStrip.hoveredChipTop + chip.height
                        } else if (tabStrip.hoveredChipId === chip.modelData.id) {
                            /*
                             * 委托会被复用到别的色块上，鼠标从这一块挪到那一块时
                             * 两条（离开 / 进入）的先后不定：只在"离开的正是现在
                             * 报着的那一块"时才清，别把刚报上来的那次清掉。
                             */
                            tabStrip.hoveredChipId = ""
                        }
                    }
                }
            }
        }

        /*
         * 悬停提示：贴着被悬停那块色块的下沿（用户要的"气泡"）。
         *
         * **提示不能挂在色块那个委托里**：色块在 clip: true 的 ListView 里
         * （滚出可视区的色块不许画到便签外面去），委托是它的子树 —— 提示挂在
         * 色块身上，整块就被裁在那一条 28px 宽的色块列里；用户看到的是"气泡
         * 只剩左边两个字"（他们截了图："这个标签的气泡有些被截断了"）。
         *
         * 所以提示挂在标签条这一层（ListView 的**兄弟**），由色块委托把"哪一块
         * 被悬停、它在哪一格"报上来（见上面 chipHit.onContainsMouseChanged）。
         * 同一时刻只有一条提示，也就只有一个实例。
         *
         * 那个空 Item 只是个挂点：悬停时它挪到色块下沿，NoteTip 照它自己那条
         * 规矩（贴着锚点父项的下沿、横着不许探出便签）画出来 —— 这一带已经出了
         * ListView，裁不着了。x 跟着色块那一列，横着的位置和以前一样。
         *
         * 整条标签条抬一层（z）：提示往右会压在正文 / 链接卡片那一带上，不抬
         * 的话它会被**后声明的** content 盖住（色块自己不越过便签纸，抬了不影响
         * 别的）。
         */
        property string hoveredChipId: ""
        property int hoveredChipNumber: 0
        property real hoveredChipTop: 0
        property real hoveredChipBottom: 0

        Item {
            id: tabTipAnchor
            x: tabColumn.x
            /*
             * 挂点默认落在色块下沿（提示就画在它下面 4px）；下面真摆不下
             * （便签矮、色块又多）就翻到色块上面去 —— 提示画在窗口里，探出
             * 窗口的那部分和挂在色块身上时一样会被裁掉。
             *
             * 摆法写成绑定（不写在那条悬停报告里）：提示的高度跟着文字走，
             * 换一块色块 / 便签被拉高拉矮，这里都会自己重算一次。
             */
            y: tabStrip.hoveredChipBottom + 4 + tabTip.height > tabStrip.height
               ? Math.max(0, tabStrip.hoveredChipTop - 8 - tabTip.height)
               : tabStrip.hoveredChipBottom
            width: tabColumn.width

            NoteTip {
                id: tabTip
                hovered: tabStrip.hoveredChipId !== ""
                /* 只报编号：这一摞里有哪几张纸，用户点一下就知道了（不要多余的说明） */
                text: "便签 " + tabStrip.hoveredChipNumber
            }
        }
    }

    ColumnLayout {
        id: content
        anchors.fill: parent
        /*
         * 左边那条标签条占一条（见上面 tabStrip）：卡片从它右边开始，所以
         * 色块是露在卡片外面的。
         */
        anchors.leftMargin: root.tabStripWidth
        /*
         * 边距不给整块了：头部（标题那条）和底部链接栏各自留 noteMargin，
         * 正文那块要**铺满**便签左右 —— "整块都是输入区"（原来整块退 10px，
         * 正文那块再往里缩 6px，看着就是便签里又套了一个输入框）。
         */
        spacing: 8
        visible: !root.collapsed

        /* ------------------------------------------------------------------
         * 头部：编号 + 拖动条 + 一排按键
         * ---------------------------------------------------------------- */
        Item {
            id: header
            Layout.fillWidth: true
            Layout.preferredHeight: 22
            Layout.margins: root.noteMargin

            /* 整条都是拖动把手（按住就搬窗口）。锁定的那块拖不动（见 setLocked） */
            MouseArea {
                id: headerDrag
                anchors.fill: parent
                enabled: !root.locked
                cursorShape: root.locked ? Qt.ArrowCursor : Qt.OpenHandCursor
                acceptedButtons: Qt.LeftButton
                onPressed: noteWindow.beginDrag()
            }

            /*
             * 落点提示：有别的一块便签正被拖到这一块身上（拖头部叠过来），
             * 整条头部亮一层 —— 松手就是把两块**组合**到一摞里（见
             * StickyNotes::updateDropTarget）。不亮的话用户不知道松手会发生
             * 什么，只能靠猜。
             */
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                radius: 4
                z: -1
                visible: root.dropPreview
                color: Qt.rgba(root.inkColor.r, root.inkColor.g, root.inkColor.b, 0.22)
                border.width: 1
                border.color: Qt.rgba(root.inkColor.r, root.inkColor.g, root.inkColor.b, 0.55)
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
                 * 颜色按钮：一件 **T 恤**（用户点名的图标），衣身填的就是这块便签
                 * 现在的底色 —— 既是个图标，也一眼看得出当前是什么色。
                 *
                 * 用户明确要求："颜色放到外面来，做成弹窗的形式" + "颜色用 T 恤"
                 * —— 原来它藏在「⋯」菜单的子面板里，改一次色要点两下。
                 */
                Rectangle {
                    id: colorButton
                    /* 自检按名字找它 */
                    objectName: "noteColorButton"
                    readonly property bool hot: colorHit.containsMouse || palettePopup.opened

                    /* 和锁定 / ⋯ 一样大（用户要求"三个图标保持一样大小"） */
                    implicitWidth: 22
                    implicitHeight: 18
                    radius: 4
                    color: hot ? Qt.rgba(root.inkColor.r, root.inkColor.g,
                                         root.inkColor.b, 0.16) : "transparent"
                    Layout.alignment: Qt.AlignVCenter

                    Canvas {
                        id: shirtCanvas
                        /* 自检按名字找它（量"三个图标画出来一样大"，见 SelfTestNotes） */
                        objectName: "noteColorIcon"
                        /* 图标都画在 14×14 里（和锁定 / ⋯ 那两个一样大） */
                        anchors.centerIn: parent
                        width: 14
                        height: 14
                        antialiasing: true

                        Connections {
                            target: root
                            function onInkColorChanged() { shirtCanvas.requestPaint() }
                        }
                        Connections {
                            target: root.noteData
                            function onColorChanged() { shirtCanvas.requestPaint() }
                        }
                        Component.onCompleted: shirtCanvas.requestPaint()

                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.save()
                            /*
                             * 按 16×16 的坐标画（和主界面 IconProvider 那套同一个
                             * 尺度），再整体缩到画布大小。
                             *
                             * 三个图标（T 恤 / 挂锁 / ⋯）都画在同一个 16 的框里、
                             * 都占满大约 10 个单位高、四周留一圈边 —— 用户要的是
                             * "三个图标一样大"：光把画布定成一样大小不够，**画的
                             * 形状本身**也得占一样大（早先挂锁只画了 7 个单位宽，
                             * 摆在 T 恤旁边看着就是小一号）。
                             */
                            ctx.scale(width / 16, height / 16)
                            ctx.lineWidth = 1.5
                            ctx.lineJoin = "round"
                            ctx.lineCap = "round"
                            ctx.beginPath()
                            ctx.moveTo(6.2, 3.2)                           /* 左肩 */
                            ctx.quadraticCurveTo(8, 5.2, 9.8, 3.2)          /* 领口 */
                            ctx.lineTo(13.2, 4.9)                           /* 右肩 */
                            ctx.lineTo(14.6, 7.6)                           /* 右袖外下角 */
                            ctx.lineTo(11.6, 9.0)                           /* 右腋 */
                            ctx.lineTo(11.6, 13.4)                          /* 右腰 */
                            ctx.lineTo(4.4, 13.4)                           /* 左腰 */
                            ctx.lineTo(4.4, 9.0)                            /* 左腋 */
                            ctx.lineTo(1.4, 7.6)                            /* 左袖外下角 */
                            ctx.lineTo(2.8, 4.9)                            /* 左肩内侧 */
                            ctx.closePath()
                            /* 衣身 = 当前底色（深色纸上也看得见），轮廓 = 便签主色 */
                            ctx.fillStyle = root.noteData ? root.noteData.color : "transparent"
                            ctx.fill()
                            ctx.strokeStyle = root.inkColor
                            ctx.stroke()
                            /* 下摆那道横条（参考图里就有） */
                            ctx.beginPath()
                            ctx.moveTo(4.4, 10.9)
                            ctx.lineTo(11.6, 10.9)
                            ctx.stroke()
                            ctx.restore()
                        }
                    }

                    MouseArea {
                        id: colorHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.paletteButtonClicked()
                    }

                    NoteTip {
                        hovered: colorHit.containsMouse && !palettePopup.opened
                        text: "颜色"
                    }
                }

                /*
                 * 锁定按钮（鼠标穿透）：和菜单里那条「锁定（鼠标穿透）」同一条路
                 * （root.toggleLock -> handleMenuAct("lock")）。锁上时图标实心、
                 * 底下一块淡底，一眼看得出这块便签现在点不着。
                 */
                Rectangle {
                    id: lockButton
                    /* 自检按名字找它 */
                    objectName: "noteLockButton"
                    readonly property bool on: noteWindow ? noteWindow.locked : false
                    readonly property bool hot: lockHit.containsMouse || on

                    implicitWidth: 22
                    implicitHeight: 18
                    radius: 4
                    color: hot ? Qt.rgba(root.inkColor.r, root.inkColor.g,
                                         root.inkColor.b, on ? 0.22 : 0.16) : "transparent"
                    Layout.alignment: Qt.AlignVCenter

                    Canvas {
                        id: lockCanvas
                        /* 自检按名字找它（量图标画出来多大） */
                        objectName: "noteLockIcon"
                        anchors.centerIn: parent
                        width: 14
                        height: 14
                        antialiasing: true

                        Connections {
                            target: lockButton
                            function onOnChanged() { lockCanvas.requestPaint() }
                        }
                        Connections {
                            target: root
                            function onInkColorChanged() { lockCanvas.requestPaint() }
                        }

                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.clearRect(0, 0, width, height)
                            ctx.save()
                            /* 和 T 恤同一个 16 的框，**占满差不多一样大**（见
                               colorButton 里那段说明）：锁体 8.8 宽 × 6.8 高，
                               加上锁梁一共约 9.8 高 */
                            ctx.scale(width / 16, height / 16)
                            ctx.strokeStyle = root.inkColor
                            ctx.fillStyle = root.inkColor
                            ctx.lineWidth = 1.5
                            ctx.lineCap = "round"
                            ctx.lineJoin = "round"
                            /* 锁体：锁上时填实（"现在是锁着的"），没锁只描边 */
                            ctx.beginPath()
                            ctx.rect(3.6, 6.6, 8.8, 6.8)
                            if (lockButton.on)
                                ctx.fill()
                            else
                                ctx.stroke()
                            /* 锁梁：上半个圆 */
                            ctx.beginPath()
                            ctx.arc(8, 6.6, 3.0, Math.PI, 0)
                            ctx.stroke()
                            ctx.restore()
                        }

                        Component.onCompleted: lockCanvas.requestPaint()
                    }

                    MouseArea {
                        id: lockHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.toggleLock()
                    }

                    NoteTip {
                        hovered: lockHit.containsMouse
                        text: lockButton.on ? "解锁（现在点不到它）" : "锁定（鼠标穿透）"
                    }
                }
            }
        }

        /*
         * 便签菜单（**在便签上点右键**弹出来的那份，见上面那个 TapHandler）。
         *
         * 放在便签窗口的 QML 里（不是 C++）：条目都是"这一刻便签的状态"
         * （透明度、勾选、组合、便签条数），由菜单自己在 openAt 里现搭，
         * 见 NoteMenu.qml 的 rebuild()。
         */
        NoteMenu {
            id: noteMenu
            /* 自检要按名字找到它（见 StickyNotes::menuState） */
            objectName: "noteMenu"
            paper: root
            notes: Notes
        }

        /*
         * 颜色弹窗（便签头上那个色块按钮点开的调色板）。
         *
         * Popup.Window：只占自己那一小块**原生窗** —— 便签只有 330 宽，调色板
         * （6 列 20px 的小格子）塞在便签里会被窗口裁掉一半；做成独立小窗就能
         * 探到便签外面，和「⋯」菜单 / AskCard 那几块是同一类窗口。
         *
         * 长相照搬「⋯」菜单里原来那块颜色子面板（同一套配色和尺寸），只是现在
         * 由头上的按钮直接点开 —— 菜单里那一条已经撤掉了。
         */
        Popup {
            id: palettePopup
            /* 自检按名字找它 */
            objectName: "notePalette"

            popupType: Popup.Window
            modal: false
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

            /*
             * 这一下"关"是不是颜色按钮按出来的。
             *
             * 为什么要在**关的这一刻**判、而不是到按钮的 onClicked 里读 opened：
             * 弹窗带着 CloseOnPressOutside，按在按钮上那一下它先收到事件、先关，
             * 之后这次按下才被重放给便签窗口（实测顺序：onClosed -> onPressed ->
             * onClicked）——等到 onClicked，"开着没"永远是 false，"按钮再点一下就
             * 收"就判不出来了。
             *
             * 判据就是**关的这一刻光标还在不在颜色按钮里**：按在按钮上关的 = 这一下
             * 该算"收"（见 root.paletteButtonClicked）；点在别处 / 选色格关的，光标
             * 不在按钮里，标记留着 false，下一次点按钮照样是"开"。
             */
            property bool closedByButton: false
            onClosed: {
                var at = noteWindow.cursorPos()
                closedByButton = colorButton.contains(colorButton.mapFromGlobal(at.x, at.y))
            }
            padding: palettePopup.panePadding
            /* 贴着按钮下面 4px；越界由 openAt 里夹回来 */
            margins: 0

            readonly property color panelColor: "#f7f7f5"
            readonly property color textColor: "#24262a"
            readonly property color hoverColor: "#e8e8e4"
            /* 色板那一块：8 列小格子 */
            readonly property real swatchSize: 20
            readonly property real swatchGap: 4
            readonly property int swatchColumns: 8
            readonly property real panePadding: 6
            readonly property var swatches: Notes ? Notes.palette() : []

            readonly property int rows: Math.max(1, Math.ceil(swatches.length / swatchColumns))
            implicitWidth: panePadding * 2 + swatchColumns * swatchSize
                           + (swatchColumns - 1) * swatchGap
            /* 弹窗里只有色板这一块（「更多颜色…」那条已经删了，见下面那段说明） */
            implicitHeight: panePadding * 2 + rows * swatchSize + (rows - 1) * swatchGap

            /* 这一格就是便签现在的底色（画一圈重边） */
            function isCurrent(hex) {
                return root.noteData
                       && String(hex).toLowerCase() === String(root.noteData.color).toLowerCase()
            }
            function swatchAt(index) {
                return (index >= 0 && index < swatches.length) ? String(swatches[index]) : ""
            }
            /* 按第几格 = 换成那个底色（界面上点一格走的就是它） */
            function pickAt(index) {
                var hex = swatchAt(index)
                if (hex === "")
                    return false
                root.setBackground(hex)
                close()
                return true
            }

            /*
             * 摆在 anchor（头上那个色块按钮）下面，并夹进这块屏的可用区。
             *
             * 位置换算成**便签窗口内**的坐标（Popup 的 x/y 是相对 parent 的，
             * 窗口式 popup 由 Qt 再换算到屏幕）——所以先按屏幕坐标夹，再
             * mapFromGlobal 回来。
             */
            function openAt(anchor) {
                if (!anchor || !noteWindow)
                    return false
                var want = anchor.mapToGlobal(0, anchor.height + 4)
                var area = noteWindow.screenBounds()
                var px = Math.round(Math.min(Math.max(want.x, area.x + 4),
                                             area.x + area.width - implicitWidth - 4))
                var py = Math.round(Math.min(Math.max(want.y, area.y + 4),
                                             area.y + area.height - implicitHeight - 4))
                var local = root.mapFromGlobal(px, py)
                x = local.x
                y = local.y
                open()
                return opened
            }

            background: Rectangle {
                color: palettePopup.panelColor
                radius: 6
                border.width: 1
                border.color: Qt.rgba(0, 0, 0, 0.18)
            }

            contentItem: Column {
                spacing: palettePopup.swatchGap

                Grid {
                    columns: palettePopup.swatchColumns
                    spacing: palettePopup.swatchGap

                    Repeater {
                        model: palettePopup.swatches

                        delegate: Rectangle {
                            id: swatch
                            required property int index
                            required property var modelData
                            width: palettePopup.swatchSize
                            height: palettePopup.swatchSize
                            radius: 3
                            color: swatch.modelData
                            border.width: palettePopup.isCurrent(swatch.modelData) ? 2 : 1
                            border.color: palettePopup.isCurrent(swatch.modelData)
                                          ? palettePopup.textColor
                                          : Qt.rgba(0, 0, 0, 0.22)

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: palettePopup.pickAt(swatch.index)
                            }
                        }
                    }
                }

                /*
                 * 这里的弹窗**只有色板本身**：常用色点一下就换（8 列 48 格）。
                 *
                 * 「更多颜色」（要自己调色的那种、开 Qt 取色框）原来在便签的右键
                 * 菜单里，用户不要了 —— 那条连同它飞出的色板、以及 C++ 那侧那套
                 * 取色框（pickColor / colorDialogFor）一起删掉了。换底色就剩这
                 * 一个入口。
                 */
            }
        }
        /* ------------------------------------------------------------------
         * 正文
         * ---------------------------------------------------------------- */
        Item {
            id: body
            /* 自检按名字找它（量"锁定的便签正文点不动"） */
            objectName: "noteBody"
            Layout.fillWidth: true
            Layout.fillHeight: true
            /* 锁定的便签正文点不动（选字、链接、右键都不响应，见 setLocked） */
            enabled: !root.locked
            /* 正文只占"卡片栏之外"的那部分高度，见 linksStrip 的 preferredHeight */
            Layout.minimumHeight: 40
            /* 左右铺满便签（不再退纸边）：这一整块都是输入区 */
            Layout.leftMargin: 0
            Layout.rightMargin: 0

            Rectangle {
                anchors.fill: parent
                /*
                 * 正文底：便签纸本身稍微亮一点点，让它看着像"写在纸上的一块
                 * 区域"。色的分量都在根上算好了（见 washColor），这里只做赋值。
                 *
                 * 不描边、不圆角：这一块现在铺满便签左右，描边会压在便签自己的
                 * 边框上变成双线，圆角则在中间凭空多出两个角。
                 */
                color: root.washColor
            }

            Flickable {
                id: editorFlick
                /* 铺满整块：能点、能写、能拖选的范围就是这一整块 */
                anchors.fill: parent
                clip: true
                /* 让 TextEdit 自己长高，滚动交给这一层（便签正文通常不长） */
                contentWidth: width
                contentHeight: Math.max(height, editor.contentHeight
                                                + editor.topPadding + editor.bottomPadding)
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
                     * 高度至少铺满可视区。
                     *
                     * TextEdit 默认只有"内容那么高"（空便签就一行）—— 那样点便签
                     * 下半截落在 Flickable 上，不聚焦，输入光标出不来。这里让它
                     * 撑满整块，内容多了再跟着长，点击落在哪儿都能进编辑区。
                     */
                    height: Math.max(editorFlick.height, contentHeight)
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
                     * 留白挪到编辑区自己身上（原来是外面那层 Flickable 退 6px）：
                     * 这样"能点能写"的范围是整块，只是字不贴着边 ——
                     * 左边和头部的标题对齐（都是 noteMargin），右边多留一点
                     * 给滚动条（它出现时压在这条留白上）。
                     */
                    leftPadding: root.noteMargin
                    rightPadding: root.noteMargin + 6
                    topPadding: 6
                    bottomPadding: 8

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
                     * 这儿）。别的键一概不拦 —— 尤其是粘贴。
                     *
                     * 这里原来拦了一条"粘贴走纯文本"：`editor.insert(Clipboard.text)`。
                     * 那条路两个毛病叠在一起 —— Clipboard 那个 QML 单例在便签
                     * 这个引擎里解析不到（ReferenceError），而 TextEdit.insert()
                     * 要 (位置, 文本) 两个参数，少给一个就报
                     * "Insufficient arguments"。其实根本不用拦：编辑区的
                     * textFormat 是 PlainText，TextEdit 自己处理 Ctrl+V 时落进来的
                     * 就是纯文本（便签不吃外来样式）。
                     */
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Escape && noteMenu.opened) {
                            noteMenu.closeMenu()
                            event.accepted = true
                        }
                    }

                    Text {
                        /* 空的时候那句提示；有字了就没了 */
                        visible: root.emptyText
                        text: "随手写点什么…"
                        font.pixelSize: 12
                        color: root.softInkColor
                        wrapMode: Text.WordWrap
                        /*
                         * 跟着编辑区自己的留白走：x/y 是相对编辑区左上角算的
                         * （不含 padding），所以这里手动让开，不然提示会贴在
                         * 字该在的位置的左上角外面。
                         */
                        x: editor.leftPadding
                        y: editor.topPadding
                        width: editor.width - editor.leftPadding - editor.rightPadding
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
            /* 锁定的便签链接卡片也点不着（见 setLocked） */
            enabled: !root.locked
            /* 收起来时只留底下那条细杠（还能再展开） */
            Layout.preferredHeight: root.hasLinks
                                 ? (root.linksExpanded
                                    ? Math.min(linksFlow.height + 4, 150) : 1)
                                 : 0
            /* 链接栏还是原来的纸边（只有正文那块铺满，见上面 ColumnLayout 的说明） */
            Layout.margins: root.noteMargin
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
        /* 锁定的便签改不了大小（见 setLocked） */
        enabled: !root.locked
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
        /*
         * 提示不能比便签还宽：正文里那种长 URL（链接卡片上悬停也弹这个）会跟
         * 便签一样宽甚至更宽，那就没地方摆了。收窄之后文字自己 elide。
         */
        width: Math.min(implicitWidth, Math.max(48, root.width - 8))
        /*
         * 位置：贴着锚点那一格（「⋯」按钮 / 链接卡片）的**下沿**，横向默认从
         * 自己左边往右铺。
         *
         * 右边装不下就得往左让：提示是画在便签窗口里的，探出窗口的部分会被直接
         * 裁掉。便签贴屏幕右沿时「⋯」本来就贴着窗口右边，原来的写法固定 x = 2、
         * 一路往右铺，用户看到的就是"提示只剩左边几个字"。
         *
         * 用 mapToItem 换算到便签根上量：x 是相对锚点算的，而"有没有出便签"
         * 得跟便签自己的宽度比。
         */
        x: {
            const want = 2
            const anchorInRoot = noteTip.parent.mapToItem(root, 0, 0).x
            const over = anchorInRoot + want + noteTip.width - (root.width - 4)
            const shifted = over > 0 ? want - over : want
            /* 让过头了也别从左边探出去 */
            return Math.max(shifted, 4 - anchorInRoot)
        }
        y: parent.height + 4

        Label {
            id: tipLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, noteTip.width - 12)
            text: noteTip.text
            font.pixelSize: 10
            color: "#d6d7da"
            elide: Text.ElideRight
        }
    }

    /* 便签窗口里的滚动条（正文 / 链接栏共用）：细一点，别抢戏 */
    component NoteScrollBar: ScrollBar {
        id: noteBar
        policy: ScrollBar.AsNeeded
        /*
         * 只有内容真的比可视区高的时候才让它出现。
         *
         * 为什么不靠 policy 就完事：Fusion 这套样式下 AsNeeded 不生效 ——
         * 便签正文是空的时候 size 明明是 1（全在可视区里），滚动条照样画出来
         * （用 qml.exe 单独量过：size=1 / visible=true）。所以这里自己判。
         *
         * 藏的时候**只改 opacity/enabled、不动 width**：宽度一变，那个"挂靠"
         * 在 Flickable 上的定位就停在老值上（实测空便签先开、再贴进字之后，
         * 滚动条会飘到左边去）。宽度固定 6，位置一次摆对就一直是对的；
         * 不需要时不透明、也不接鼠标（不然右边会多一条 6px 的透明死区）。
         */
        readonly property bool needed: size < 1.0
        width: 6
        opacity: needed ? 1.0 : 0.0
        enabled: needed
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
