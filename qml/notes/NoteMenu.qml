pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects

/*
 * 便签头部的「⋯」菜单（点开就是 Windows 便签那一套）。
 *
 * ===========================================================================
 * 为什么又是自己写一个，不复用 components/DropdownMenu.qml
 * ===========================================================================
 * 那个是主界面（深色 IDE 风格）的菜单：底色写死 #3c3f41、文字 #bbbbbb，
 * 高度和贴边夹取都按 Main.qml 那个宿主窗口算。便签纸有深有浅（柠檬黄到
 * 石墨黑），套过去就是"深色菜单浮在浅黄纸上"。所以这份菜单的颜色全部从
 * 便签纸推（见 inkColor / softInkColor）。
 *
 * ===========================================================================
 * 这是一个 Window，不是 Popup
 * ===========================================================================
 * 本来用的是 `Popup { popupType: Popup.Window }`（Qt 6.8 起那种"独立原生
 * 窗口"的 Popup），踩了两层坑，最后换成 Window：
 *
 *   1) Popup 的 x/y 是**相对宿主窗口**的，而它那套"按 parent 定位"的逻辑
 *      会在 open() 那一刻把 x/y 重算一遍 —— 按屏幕坐标摆好的位置每次都被
 *      打回去（实测：算好的 1500,900 读回来是 0,0）；
 *   2) 写在 open() 之前、之后、onOpened 里、推到下一拍、把 parent 置空，
 *      全都拦不住它。
 *
 * Window 的 x/y 就是**屏幕坐标**，摆一次就定住。代价是 `Popup.opened` /
 * `closePolicy` 这些得自己来（见 opened / closeMenu / outerMouse）。
 *
 * ===========================================================================
 * 摆位：贴着鼠标，不是贴着按钮
 * ===========================================================================
 * 菜单左上角就落在"点「⋯」时鼠标那一点"（Windows 便签也是这样）。
 *
 * 子菜单（颜色 / 透明度 / 正文里的链接）是一块**飞出面板**，画在同一个窗口
 * 里，位置由 layout() 一次算死：
 *
 *   * 右边装得下就放右边，装不下就翻到**左边**；
 *   * 面板顶边对齐"鼠标停的那一条"，并夹进屏幕；
 *   * 位置只在**展开那一刻**算一次，鼠标在主栏里上下挪（不换条目）不重算
 *     —— 之前每动一下都重算，看着就是"飘动、每次位置都不固定"。
 *
 * 为什么子菜单画在同一个窗口里、不做成第二个窗口：鼠标从主栏往面板上挪的
 * 时候会离开主栏那块矩形，分成两个窗口的话中间那一下就是"点到了外面"，
 * 菜单会先关掉、子面板跟着没，鼠标根本移不上去。同一个窗口里，鼠标在两边
 * 走都算"窗口内部"。
 */
Window {
    id: root

    /* 便签窗口（菜单里那些命令最后都发给它） */
    property var win: null
    /* 便签纸那个根项：颜色全从它上面取 */
    property var paper: null
    /* 便签总管（新建便签 / 排列这些走它） */
    property var notes: null

    /* 点击那一下鼠标在屏幕上的位置（菜单左上角就摆在这儿） */
    property real anchorX: 0
    property real anchorY: 0

    /* 菜单开着没（原来用 Popup.opened，换成 Window 之后自己维护） */
    property bool opened: false

    signal customColorRequested()

    /* ---- 配色：跟着便签纸走 ---- */
    readonly property color inkColor: paper && paper.inkColor ? paper.inkColor : "#1e2024"
    readonly property color softInkColor: paper && paper.softInkColor
                                          ? paper.softInkColor : Qt.rgba(0, 0, 0, 0.55)
    readonly property color panelColor: "#f7f7f5"
    readonly property color textColor: "#24262a"
    readonly property color mutedColor: "#8a8f96"
    readonly property color hoverColor: "#e8e8e4"
    readonly property color dangerColor: "#c0392b"

    readonly property real itemHeight: 30
    readonly property real separatorHeight: 9
    readonly property real panePadding: 4
    readonly property real paneWidth: 208
    /* 主栏和子面板之间的缝：留一点，两块面板的圆角才都看得见 */
    readonly property real paneGap: 6
    /* 整块菜单离屏幕边缘至少留这么宽 */
    readonly property real screenMargin: 4
    /* 判"光标还在菜单上吗"时往外放宽这么几像素（见 cursorInsideMenu） */
    readonly property real hoverSlack: 6
    /* 色板那一块：6 列小格子（和 Windows 便签那个一样） */
    readonly property real swatchSize: 20
    readonly property real swatchGap: 4
    readonly property int swatchColumns: 6

    /* 主栏条目 */
    property var entries: []
    /* 子面板里是什么："" 没有 / "color" / "opacity" / "links" */
    property string flyoutKind: ""
    /* 面板挂在左边还是右边（layout() 按屏幕空间决定） */
    property string flyoutSide: "right"
    /*
     * 面板顶边对齐的那一条，在主栏内容区里的 y。
     * 只在鼠标停到某一条上时更新一次（见 openFlyout），展开之后不再变 ——
     * 这就是"位置固定"的来源。
     */
    property real flyoutItemY: 0

    /* 色板（调色板那一份，见 rebuild） */
    property var swatches: []
    /* 透明度档位 */
    property var opacities: []
    /* 正文里的链接 */
    property var links: []

    property string currentColor: ""
    property int currentOpacity: 100

    readonly property bool flyoutOpen: flyoutKind !== ""

    /* 主栏 / 子面板各自的尺寸 */
    readonly property real entriesWidth: paneWidth
    readonly property real entriesHeight: paneHeight(entries)
    readonly property real flyoutWidth: flyoutKind === "color"
                                        ? 2 * panePadding + swatchColumns * swatchSize
                                          + (swatchColumns - 1) * swatchGap
                                        : paneWidth
    readonly property real flyoutHeight: heightForKind(flyoutKind)
    /* 色板占几行（按列数折算） */
    readonly property int colorRows: Math.ceil((swatches ? swatches.length : 0) / swatchColumns)
    readonly property real swatchGridHeight: colorRows * (swatchSize + swatchGap)

    /* 某一块子面板多高（flyoutHeight 用它；摆放判断也要用） */
    function heightForKind(kind) {
        if (kind === "color")
            return 2 * panePadding + colorRows * (swatchSize + swatchGap) + itemHeight
        if (kind === "opacity")
            return 2 * panePadding + (opacities ? opacities.length : 0) * itemHeight
        if (kind === "links")
            return 2 * panePadding + (links ? links.length : 0) * itemHeight
        return 0
    }

    /*
     * 子面板那块窗口，左上角在**屏幕**上的位置。
     *
     * 放右边：主栏之后，隔一条缝。
     * 放左边：主栏之前，隔一条缝。
     *
     * 它是一块独立的窗口（见下面 flyoutWindow 那段说明），所以这里直接给屏幕
     * 坐标，不用再拿"窗口内局部坐标"跟主栏窗口的挪动互相补偿。
     */
    readonly property real flyoutScreenX: flyoutSide === "left"
        ? x - paneGap - flyoutWidth
        : x + entriesWidth + paneGap
    readonly property real flyoutScreenY: {
        if (!flyoutOpen)
            return y
        const area = screenArea()
        /* 顶边对齐"挂着它的那一条"，再夹进屏幕（只挪面板自己） */
        const wanted = y + panePadding + flyoutItemY
        const minY = area.y + screenMargin
        const maxY = area.y + area.height - screenMargin - flyoutHeight
        return Math.round(Math.max(minY, Math.min(wanted, maxY)))
    }

    function paneHeight(items) {
        var h = 2 * panePadding
        for (var i = 0; i < (items ? items.length : 0); ++i)
            h += items[i] && items[i].separator ? separatorHeight : itemHeight
        return h
    }

    /*
     * ===================================================================
     * 子面板是**自己一块窗口**，不跟主栏挤在一个窗口里
     * ===================================================================
     *
     * 这一段是踩出来的，别改回去。
     *
     * 面板挂在右边时，窗口往右长一下就够了；可便签贴屏幕右沿时右边塞不下，
     * 面板只能翻到**左边** —— 那就得让窗口整体往左挪一块、同时把主栏在窗口里
     * 的局部位置反向补偿回来（不然主栏会跟着窗口一起跑掉）。这一"挪 + 补偿"
     * 有两层坑，实测都躲不掉：
     *
     *   1) 一步挪到位：Windows 先把窗口的**旧画面**按新位置合成出来，Qt 下一帧
     *      才画新内容 —— 中间那一帧看着就是"菜单闪一下"；
     *   2) 分帧挪（给这几个量配 Behavior 动画）：窗口的**原生位置**是赋值当帧
     *      就生效的，可窗口里的**内容**要等下一次渲染 —— 两个错开二三十像素，
     *      一帧一帧看过去就是"菜单一伸一缩地抖"（放大截图量过：窗口右缘和主栏
     *      左缘在中间几帧一起往左偏 38px 再回来）。
     *
     * 所以干脆分成两块窗口：主栏窗口就是主栏、面板窗口就是面板，各自摆到位。
     * 子面板展开/收起时**任何窗口都不挪不变**（面板窗口是 show/hide + 摆位置，
     * 里面那块面板永远贴在窗口的 (0,0)），"闪"和"伸缩"就都没有了。这也是
     * Windows 自己那套菜单的做法（子菜单本来就是一块独立的 popup）。
     *
     * 代价是鼠标从主栏走到面板上时中间会离开主栏窗口 —— 但"鼠标还在不在菜单上"
     * 早就改成按**光标位置**判断了（见 watchHover/cursorKeepsFlyout），
     * 两个窗口各算各的矩形，跨窗口走一点事都没有。
     */

    /*
     * 面板窗口这一帧要不要把面板画出来。
     *
     * 收起时先把面板清掉、等两帧再藏窗口：不然下次 show 出来的一瞬间，Windows
     * 会把这块窗口上**上次**那张画面先合成一下（旧面板在新位置闪一下）。
     */
    property bool flyoutPainted: false
    readonly property int flyoutHideDelayMs: 32

    /*
     * 摆好面板窗口并按需 show / 收起时走这里（openFlyout / closeFlyout 调）。
     */
    function syncFlyoutWindow() {
        if (flyoutOpen) {
            flyoutHideTimer.stop()
            flyoutWindow.x = flyoutScreenX
            flyoutWindow.y = flyoutScreenY
            flyoutWindow.width = flyoutWidth
            flyoutWindow.height = flyoutHeight
            flyoutWindow.visibility = Window.Windowed
            /* 和主栏窗口一个道理：便签也是置顶的，得顶到置顶带最上面去 */
            flyoutWindow.raise()
            flyoutPainted = true
        } else {
            flyoutPainted = false
            flyoutHideTimer.restart()
        }
    }

    Timer {
        id: flyoutHideTimer
        interval: root.flyoutHideDelayMs
        repeat: false
        onTriggered: {
            if (!root.flyoutOpen)
                flyoutWindow.visibility = Window.Hidden
        }
    }

    /*
     * 打开菜单：左上角摆在鼠标那一点（夹进屏幕）。
     *
     * (screenX, screenY) 是**屏幕坐标**（鼠标在屏幕上的位置）—— Window 的
     * x/y 就是屏幕坐标，直接写。
     */
    function openAt(screenX, screenY, node) {
        if (!node)
            return
        win = node
        anchorX = screenX
        anchorY = screenY
        flyoutKind = ""
        flyoutSide = "right"
        rebuild(node)
        layout()
        /* 上一次可能留了块面板窗口在那儿没来得及藏（收起是延迟两帧藏的），
           这里按"没展开"再收一次，顺手把面板内容清掉 */
        syncFlyoutWindow()

        /*
         * "这次弹出之后，光标进来过没有"。
         *
         * 界面上点「⋯」走的就是这条路，点开那一下光标就压在菜单角上，所以这里
         * 量一次就够。自检里用 hook 打开菜单（见 SelfTestNotes）时，真实光标还
         * 在屏幕别处 —— 那种情况下保持 false，否则 watchHover 一上来就按"光标
         * 在外面"把菜单收掉，菜单那一节的用例会全红。
         */
        menuHoverSeen = cursorInsideMenu()

        opened = true
        /* 用 visibility 而不是 visible：声明了 visibility: Hidden，两个一起写
           会报 "Conflicting properties 'visible' and 'visibility'" */
        visibility = Window.Windowed

        /*
         * 把它顶到**置顶带最上面**。
         *
         * 便签自己也是"始终置顶"（QWidget 那边挂着 Qt::WindowStaysOnTopHint），
         * 而菜单是个**不接激活**的窗口（WindowDoesNotAcceptFocus，见 flags 那段
         * 为什么）—— 点「⋯」那一下被激活、被系统抬到最上面的是**便签**。于是
         * 两块都在置顶带里、便签压在菜单上面：便签贴着屏幕右边时主栏不得不往左
         * 让位，让出来的那一半（图标 + 条目文字）正好被便签盖住，用户看到的就是
         * "菜单只剩右边几个 > 和快捷键，标题栏也糊着"。
         *
         * raise() 在 Windows 上就是 SetWindowPos(HWND_TOP, SWP_NOACTIVATE)：
         * 只在置顶带里往上挪一格，不抢激活 —— 便签该是激活的还是激活的。
         */
        raise()
        /* 再补一拍：窗口刚 show 出来的一瞬间排序偶尔会被系统重排回去，
           下一轮事件循环再顶一次就稳了 */
        raiseLater.restart()
    }

    /* 下一拍再顶一次（见 openAt 的说明） */
    Timer {
        id: raiseLater
        interval: 0
        repeat: false
        onTriggered: root.raise()
    }

    /*
     * 关掉菜单。
     *
     * Window 没有 Popup 那套 closePolicy，所以"点别处"和 Esc 都要自己接：
     * 点别处由便签窗口那边的点击转发过来（见 StickyNoteWindow 的 closeNoteMenu
     * 调用点），Esc 走 handleEscape。
     */
    function closeMenu() {
        closeFlyout()          /* 面板窗口跟着藏（见 syncFlyoutWindow） */
        opened = false
        visibility = Window.Hidden
    }

    /* 便签窗口所在那块屏的可用区域（由 C++ 给，见 screenBounds 的说明） */
    function screenArea() {
        if (win && win.screenBounds)
            return win.screenBounds()
        return Qt.rect(0, 0, 1920, 1080)
    }

    /* 自检用：把 screenArea() 的结果报出去（它错了菜单就会乱摆） */
    function screenAreaForTest() { return screenArea() }

    /*
     * 摆主栏：左上角落在鼠标那一点（贴边时夹进屏幕），窗口就是主栏那么大。
     *
     * 比原来简单多了 —— 子面板既然是自己一块窗口（见上面那段说明），主栏窗口
     * 就不用再为它长、为它挪，整场菜单里这块窗口一动不动。这也是"不闪"的根本：
     * 只要窗口不动，就没有"旧画面按新位置合成"那一说。
     *
     * 顺带把子面板挂哪边也定了：只看**屏幕**够不够，和鼠标停在哪一条无关 ——
     * 所以整场菜单里它不会变（边翻来翻去也是抖动的一个来源）。
     * 便签不用再躲：菜单已经顶到置顶带最上面了（见 openAt 里的 raise）。
     */
    function layout() {
        const area = screenArea()

        const mainLeft = Math.round(Math.max(area.x + screenMargin,
                                             Math.min(anchorX,
                                                      area.x + area.width - entriesWidth
                                                          - screenMargin)))
        const mainTop = Math.round(Math.max(area.y + screenMargin,
                                            Math.min(anchorY,
                                                     area.y + area.height - entriesHeight
                                                         - screenMargin)))

        const rightFits = mainLeft + entriesWidth + paneGap + paneWidth
                          <= area.x + area.width - screenMargin
        const leftFits = mainLeft - paneGap - paneWidth >= area.x + screenMargin
        flyoutSide = rightFits || !leftFits ? "right" : "left"

        width = entriesWidth
        height = entriesHeight
        x = mainLeft
        y = mainTop
    }

    /* 收起子面板：面板清掉、面板窗口藏起来（主栏窗口一动不动） */
    function closeFlyout() {
        if (!flyoutOpen)
            return
        flyoutKind = ""
        syncFlyoutWindow()
    }

    /*
     * 展开子面板，并把它**钉在**对应那一条上。
     *
     * itemY 是那一条在主栏内容区里的 y（相对主栏顶边，含 panePadding）。
     * 位置只在这里算一次；鼠标之后在主栏里上下挪不会重算，所以不飘。
     */
    function openFlyout(kind, itemY) {
        if (kind === "")
            return
        if (kind === flyoutKind && Math.abs(itemY - flyoutItemY) < 0.5)
            return                      /* 已经开着、还是那一条：什么都不做 */
        flyoutKind = kind
        flyoutItemY = itemY
        syncFlyoutWindow()
    }

    /*
     * ===================================================================
     * 「鼠标挪开就收」：一律看光标在哪儿，不看 hover 事件
     * ===================================================================
     *
     * 两条规矩：
     *
     *   1) 鼠标离开子面板 —— 挪到别的条目上、或者干脆出了菜单 —— 子面板收掉。
     *      只停在"挂着这块面板的那一条"上时留着：不然鼠标从面板上往回流，
     *      面板已经没了，而鼠标还压在那一条上、onEntered 不会再响，就再也叫不
     *      出来了（Windows 便签也是停在父条目上不收）。
     *   2) 鼠标离开整块菜单 —— 连菜单一起收掉。
     *
     * 为什么不靠每一行的 onEntered/onExited：
     *
     *   * 主栏的留白（panePadding 4px）和主栏/子面板之间那条缝（paneGap 6px）
     *     上没有 MouseArea —— 鼠标"从留白上走出去"一个事件都没有，菜单（或者
     *     子面板）就挂在那儿不走了；
     *   * 菜单是独立原生窗口，展开子面板时窗口正在改尺寸，那一瞬间的 hover
     *     事件并不可靠（原来那条"子菜单闪一下就消失"就是这么来的）。
     *
     * 所以这里 160ms 问一次光标在哪儿。光标位置从 C++ 拿：QML 里没有取光标
     * 位置的原语（QCursor 是 C++ 类，写在这儿会报 "QCursor is not defined"，
     * 然后整个判断静默失效）。
     */

    /*
     * "这次弹出之后，光标进来过没有"。
     *
     * 没进来过就什么都不做 —— 菜单也有不是鼠标点开的时候（见 openAt 里的说明）。
     */
    property bool menuHoverSeen: false

    /* 光标在屏幕上的位置；拿不到就 null */
    function cursorOnScreen() {
        if (!win || !win.cursorPos)
            return null
        return win.cursorPos()
    }

    /* 一个点落没落在一块矩形里（QML 里没有 QRectF 的 contains） */
    function pointInRect(r, p) {
        return p.x >= r.x && p.x <= r.x + r.width
               && p.y >= r.y && p.y <= r.y + r.height
    }

    /*
     * 光标是不是还压在**整块菜单**上。
     *
     * 菜单现在由两块窗口拼起来（主栏一块、子面板一块，见 flyoutWindow 那段），
     * 所以"还在不在菜单上"得两块都认：
     *
     *   * 主栏窗口的矩形（Window 的 x/y/width/height 就是屏幕坐标）；
     *   * 子面板那一块 —— 展开时它是一块独立窗口，鼠标从主栏挪过去要跨过中间
     *     那条缝（缝上没有窗口），跨过去的那一下不能算"出去了"（见
     *     cursorKeepsFlyout：它按"父条目 + 面板"的并集算，缝在里面）。
     *
     * 往外放宽 hoverSlack：菜单是贴着鼠标那一点弹出来的，点完那一下手抖一两
     * 像素，不能就把菜单晃没了。
     */
    function cursorInsideMenu() {
        const p = cursorOnScreen()
        if (!p)
            return false
        const r = Qt.rect(x - hoverSlack, y - hoverSlack,
                          width + 2 * hoverSlack, height + 2 * hoverSlack)
        if (pointInRect(r, p))
            return true
        return flyoutOpen && cursorKeepsFlyout(p)
    }

    /*
     * 光标是不是还该留着子面板：落在"父条目 → 子面板"这一片范围里。
     *
     * 不老实比两个矩形 —— 面板贴屏幕边时会上下夹取（见 flyoutScreenY），夹完
     * 有可能和它那一条不再对齐，中间空出一截。所以取两者的并集，再往外放宽
     * paneGap：鼠标从条目挪到面板上，不管中间空多少都算数。
     */
    function cursorKeepsFlyout(p) {
        if (!flyoutOpen || !p)
            return false
        const rowTop = y + panePadding + flyoutItemY
        const fr = flyoutScreenRect()
        const left = Math.min(x, fr.x) - paneGap
        const right = Math.max(x + entriesWidth, fr.x + fr.width) + paneGap
        const top = Math.min(rowTop, fr.y) - paneGap
        const bottom = Math.max(rowTop + itemHeight, fr.y + fr.height) + paneGap
        return p.x >= left && p.x <= right && p.y >= top && p.y <= bottom
    }

    /*
     * 一拍：光标出了菜单就把菜单收了；还在菜单里、但不在"父条目或子面板"那一片
     * 上（比如挪到了别的条目）就把子面板收了。
     */
    function watchHover() {
        const p = cursorOnScreen()
        if (!p)
            return
        if (!cursorInsideMenu()) {
            /* 光标在菜单外面：进来过才收（见 menuHoverSeen） */
            if (menuHoverSeen)
                closeMenu()
            return
        }
        menuHoverSeen = true
        if (flyoutOpen && !cursorKeepsFlyout(p))
            closeFlyout()
    }

    /* 菜单开着的时候就盯着（160ms 一拍，看着就是"鼠标一移开就收"） */
    Timer {
        id: hoverWatch
        interval: 160
        repeat: true
        running: root.opened
        onTriggered: root.watchHover()
    }

    /* 鼠标停在某一条上（每一行的 MouseArea onEntered 调） */
    function hoverEntry(entry, itemY) {
        menuHoverSeen = true
        if (!entry || entry.flyout === "") {
            /* 挪到不带子面板的条目上：子面板立刻收掉（"鼠标移开子菜单就没了"） */
            closeFlyout()
            return
        }
        openFlyout(entry.flyout, itemY)
    }

    /*
     * 按便签现在的状态现搭条目。
     *
     * 每次打开都重搭：这些条目的文字 / 勾选状态都是"这一刻"的
     * （"排列便签（3 块摆着）"、"始终置顶 ✓"、透明度 100%），便签可以在两次
     * 打开之间被别处改（托盘里收起来、主界面排列过）。菜单很小，开销可忽略。
     */
    function rebuild(node) {
        var pinned = node ? node.staysOnTop : true
        var locked = node ? node.locked : false
        var note = node && node.noteData ? node.noteData : null
        var linkCount = note ? note.linkCount : 0
        var total = notes ? notes.count : 0
        var shown = notes ? notes.visibleCount : 0
        var lockedTotal = notes ? notes.lockedCount() : 0

        currentColor = note ? String(note.color).toLowerCase() : ""
        currentOpacity = note ? note.opacityPercent : 100
        /*
         * 色板 = 便签纸的调色板（C++ 侧那一份，见 StickyNote::palette）。
         * 注意要**调用**它：palette() 是 Q_INVOKABLE，写成 notes.palette
         * 拿到的是函数对象本身，绑到 Repeater.model 上会报
         * "Unable to assign a function to a property of any type other than var"。
         */
        swatches = notes ? notes.palette() : []
        opacities = [100, 85, 70, 55, 40]

        links = []
        if (note && note.links) {
            for (var i = 0; i < note.links.count; ++i) {
                var row = note.links.index(i, 0)
                links.push({
                    label: String(note.links.data(row, note.links.titleRole) || ""),
                    url: String(note.links.data(row, note.links.urlRole) || "")
                })
            }
        }

        var out = []
        out.push({ label: "新建便签", act: "new", icon: "plus", shortcut: "Ctrl+Alt+N" })
        out.push({ label: shown > 0 ? "排列所有便签（" + shown + " 块）" : "排列所有便签",
                   act: "arrange", icon: "grid", disabled: shown === 0 })
        out.push({ separator: true })
        /* 这三条右边带 >，鼠标停上去在右边（装不下就在左边）飞出面板 */
        out.push({ label: "颜色", act: "color", flyout: "color", icon: "color" })
        out.push({ label: "透明度  " + currentOpacity + "%", act: "opacity",
                   flyout: "opacity", icon: "opacity" })
        out.push({ label: linkCount > 0 ? "正文里的链接（" + linkCount + "）" : "正文里的链接",
                   act: "links", flyout: "links", icon: "link", disabled: linkCount === 0 })
        out.push({ separator: true })
        out.push({ label: "复制正文", act: "copy", icon: "copy" })
        out.push({ label: "始终置顶", act: "pin", icon: "pin", checked: pinned })
        out.push({ label: "锁定（鼠标穿透）", act: "lock", icon: "lock", checked: locked })
        out.push({ label: lockedTotal > 0 ? "解锁所有便签（" + lockedTotal + "）" : "解锁所有便签",
                   act: "unlock", icon: "unlock", disabled: lockedTotal === 0 })
        out.push({ separator: true })
        out.push({ label: "收起这块便签", act: "hide", icon: "hide" })
        out.push({ label: "收起其他便签", act: "hideothers", icon: "hide", disabled: shown <= 1 })
        out.push({ label: "关闭全部便签", act: "hideall", icon: "hide", disabled: shown === 0 })
        out.push({ separator: true })
        out.push({ label: "删除这块便签", act: "delete", icon: "trash", danger: true,
                   disabled: total === 0 })

        for (var n = 0; n < out.length; ++n)
            out[n] = normalize(out[n])
        entries = out
    }

    /*
     * 把一条条目补成"键齐全"的形状。
     *
     * QML 里读一个不存在的键拿到 undefined，绑到 bool / string 属性上会刷
     * "Unable to assign [undefined] to bool"。条目是现搭的，各条带的键本来
     * 就不一样 —— 统一过一道，委托里就能直接读。
     */
    function normalize(e) {
        var has = function (key) { return e[key] !== undefined && e[key] !== null }
        return {
            label: has("label") ? String(e.label) : "",
            act: has("act") ? String(e.act) : "",
            icon: has("icon") ? String(e.icon) : "",
            shortcut: has("shortcut") ? String(e.shortcut) : "",
            flyout: has("flyout") ? String(e.flyout) : "",
            separator: e.separator === true,
            disabled: e.disabled === true,
            danger: e.danger === true,
            checked: e.checked === true
        }
    }

    /* 触发一条命令 */
    function fire(act) {
        var target = win
        closeMenu()
        if (act === "new")
            notes.createNote()
        else if (act === "arrange")
            notes.arrangeAll()
        else if (act === "unlock")
            notes.unlockAll()
        else if (act === "hide")
            target.closeNote()
        else if (act === "hideothers")
            notes.hideOthers(target)
        else if (act === "hideall")
            notes.hideAll()
        else if (act === "delete")
            target.deleteNote()
        else if (act === "copy")
            target.copyText()
        else if (act === "pin")
            target.toggleStaysOnTop()
        else if (act === "lock")
            target.setLocked(!target.locked)
        else if (act === "pickColor")
            customColorRequested()
    }

    /* ---- 自检口子（见 src/SelfTest.cpp）：外面点不出 hover，只能从进程内走 ---- */

    /* 整份菜单落在屏幕上的矩形（Window 的 x/y 就是屏幕坐标，直接用） */
    function screenRect() { return Qt.rect(x, y, width, height) }

    /* 子面板这会儿在屏幕上的矩形（没展开时是空矩形） */
    function flyoutScreenRect() {
        if (!flyoutOpen)
            return Qt.rect(0, 0, 0, 0)
        return Qt.rect(flyoutScreenX, flyoutScreenY, flyoutWidth, flyoutHeight)
    }

    /* 让某一条的子面板展开（界面上是鼠标停上去） */
    function openFlyoutForTest(kind) {
        var y = panePadding
        for (var i = 0; i < entries.length; ++i) {
            var e = entries[i]
            if (e.flyout === kind) {
                openFlyout(kind, y)
                return true
            }
            y += e.separator ? separatorHeight : itemHeight
        }
        return false
    }
    /* 当前展开的在哪一边（自检用：验证"右边不够就翻到左边"） */
    function flyoutSideNow() { return flyoutSide }
    function swatchAt(index) {
        if (!swatches || index < 0 || index >= swatches.length)
            return ""
        return String(swatches[index])
    }

    /* ---- 窗口本身 ---- */
    /*
     * Qt.Tool：不占任务栏一格。
     * WindowDoesNotAcceptFocus：**不抢激活**。
     *
     * 为什么不抢：这个菜单是"点一下、选一条、收起来"的临时窗口，抢了激活之后
     * 要还回去，而"还给谁"在 Qt 里是不确定的 —— 实测主界面那边紧跟着要弹的
     * 东西（下拉菜单、AskCard 那几块卡片）会时有时无，自检里表现为随机红的
     * 几条（"dispatch(menu:视图) 打开视图菜单""问句是一块小卡片"）。
     * 不抢激活就没这回事：鼠标点击照样能收到（按位置派发，不看焦点），
     * Esc 由便签窗口那边接（见 StickyNoteWindow 的 Esc 处理）。
     */
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
           | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    visibility: Window.Hidden

    /*
     * Esc 收起。
     *
     * 菜单不接焦点（见上面的 flags），键盘事件都落在便签窗口那边 ——
     * StickyNoteWindow.qml 的编辑区按键处理器会在 Esc 时调这里。
     */
    function handleEscape() {
        if (!opened)
            return false
        closeMenu()
        return true
    }

    /* ---------------- 主栏 ---------------- */
    Rectangle {
        id: entriesPane
        x: 0
        y: 0
        width: root.entriesWidth
        height: root.entriesHeight
        radius: 6
        color: root.panelColor
        border.width: 1
        border.color: Qt.rgba(0, 0, 0, 0.18)

        /*
         * 注意这上面**不能**再盖一块满尺寸的 MouseArea 去跟踪"鼠标还在不在
         * 菜单上"：它会盖住下面每一行的 MouseArea，hover 全被它吃掉，
         * 条目就再也没有 onEntered 了。所以"鼠标还在不在"由定时器按光标位置
         * 问（见 watchHover），这里只负责"停在某一条上"这一件事。
         */

        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.35)
            shadowBlur: 0.5
            shadowVerticalOffset: 2
        }

        Column {
            x: root.panePadding
            y: root.panePadding
            width: parent.width - 2 * root.panePadding

            Repeater {
                model: root.entries

                delegate: Item {
                    id: entryRow
                    required property var modelData

                    width: parent.width
                    height: entryRow.modelData.separator ? root.separatorHeight : root.itemHeight
                    /* 这一条在主栏内容区里的 y（子面板顶边对齐它） */
                    readonly property real itemY: y

                    /* 分隔线 */
                    Rectangle {
                        visible: entryRow.modelData.separator
                        anchors.verticalCenter: parent.verticalCenter
                        x: 4
                        width: parent.width - 8
                        height: 1
                        color: Qt.rgba(0, 0, 0, 0.12)
                    }

                    /* 普通条目 */
                    Rectangle {
                        id: entryBg
                        visible: !entryRow.modelData.separator
                        anchors.fill: parent
                        radius: 4

                        readonly property var entry: entryRow.modelData
                        readonly property bool disabled: entry.disabled
                        readonly property bool danger: entry.danger
                        /* 正展开着子面板的那一条：底色留着，一眼看得出是它 */
                        readonly property bool active: root.flyoutOpen
                                                       && entry.flyout === root.flyoutKind

                        color: active ? root.hoverColor
                                      : (entryHit.containsMouse && !disabled
                                         ? root.hoverColor : "transparent")

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 6
                            spacing: 8

                            /* 勾选 / 图标列（固定 14px，条目才对得齐） */
                            Item {
                                width: 14
                                height: parent.height
                                NoteMenuIcon {
                                    anchors.centerIn: parent
                                    kind: entryBg.entry.checked ? "check" : entryBg.entry.icon
                                    tint: entryBg.danger ? root.dangerColor
                                          : (entryBg.disabled ? root.mutedColor : root.textColor)
                                }
                            }

                            Label {
                                width: parent.width - 14 - 8 - trailing.width
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                text: entryBg.entry.label
                                font.pixelSize: 12
                                color: entryBg.danger ? root.dangerColor
                                       : (entryBg.disabled ? root.mutedColor : root.textColor)
                                elide: Text.ElideRight
                            }

                            /*
                             * 右边那个尾标：带子面板的条目给「>」（Windows 便签
                             * 就是这个），有快捷键的给快捷键文字。
                             */
                            Item {
                                id: trailing
                                width: Math.max(chevron.implicitWidth, shortcutLabel.implicitWidth)
                                height: parent.height

                                Label {
                                    id: shortcutLabel
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entryBg.entry.flyout === "" && !entryBg.entry.checked
                                          ? entryBg.entry.shortcut : ""
                                    font.pixelSize: 10
                                    color: root.mutedColor
                                }

                                NoteMenuIcon {
                                    id: chevron
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    kind: entryBg.entry.flyout !== "" ? "chevron-right" : ""
                                    tint: entryBg.disabled ? root.mutedColor : root.textColor
                                }
                            }
                        }

                        MouseArea {
                            id: entryHit
                            anchors.fill: parent
                            hoverEnabled: true
                            enabled: !entryBg.disabled
                            cursorShape: entryBg.disabled ? Qt.ArrowCursor
                                                          : Qt.PointingHandCursor
                            /*
                             * 用 MouseArea 的 onEntered，不用 HoverHandler：
                             * 菜单是独立原生窗口，实测 HoverHandler 在它里面
                             * 收不到 hover。
                             *
                             * 不用接 onExited 去"过一会儿收子面板"：留白和缝上
                             * 没有 MouseArea，鼠标从那些地方走出去根本收不到事件
                             * （见 watchHover 的说明）。
                             */
                            onEntered: root.hoverEntry(entryBg.entry, entryRow.itemY)
                            onClicked: root.fire(entryBg.entry.act)
                        }
                    }
                }
            }
        }
    }

    /*
     * ---------------- 子面板（自己一块窗口） ----------------
     *
     * 为什么是一块独立窗口、而不是画在主栏窗口里：见本文件开头"子面板是自己
     * 一块窗口"那段。要点是主栏窗口整场菜单里不挪不变，面板窗口只在自己的
     * 位置上 show/hide、里面那块面板永远贴在 (0,0) —— 两边的几何都不用在
     * 运行时互相补偿，也就没有"闪"和"伸缩"。
     *
     * flags 和主栏一样（工具窗、置顶、不抢激活）；raise() 在 syncFlyoutWindow
     * 里做，因为便签也是置顶的。
     */
    Window {
        id: flyoutWindow
        flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
               | Qt.WindowDoesNotAcceptFocus
        color: "transparent"
        visibility: Window.Hidden
        /* 下面这三个只在 show 之前用得到（syncFlyoutWindow 里直接赋值），
           绑定只是给"还没展开"时一个合理值 */
        x: root.flyoutScreenX
        y: root.flyoutScreenY
        width: root.flyoutWidth
        height: root.flyoutHeight

        Rectangle {
            id: flyoutPane
            anchors.fill: parent
            /* 收起时先清内容、隔两帧再藏窗口（见 syncFlyoutWindow） */
            visible: root.flyoutPainted
            radius: 6
            color: root.panelColor
            border.width: 1
            border.color: Qt.rgba(0, 0, 0, 0.18)

            layer.enabled: true
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.35)
                shadowBlur: 0.5
                shadowVerticalOffset: 2
            }

            /*
             * 鼠标在这块面板上（面板的留白 / 子项没盖到的地方）。
             *
             * 这里只记"光标进来过"：收不收由 watchHover 按光标实际位置定，不靠
             * 进出的先后 —— 从主栏往面板上挪的那一路（缝里、留白上）收不到事件，
             * 而且现在这还是**另一块窗口**。
             */
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.NoButton
                onEntered: root.menuHoverSeen = true
            }

            /* ---- 色板 ---- */
            Item {
                visible: root.flyoutKind === "color"
                anchors.fill: parent
                anchors.margins: root.panePadding

                Grid {
                    id: swatchGrid
                    columns: root.swatchColumns
                    spacing: root.swatchGap

                    Repeater {
                        model: root.swatches

                        delegate: Rectangle {
                            id: swatch
                            required property var modelData
                            width: root.swatchSize
                            height: root.swatchSize
                            radius: 3
                            color: swatch.modelData
                            border.width: root.isCurrent(swatch.modelData) ? 2 : 1
                            border.color: root.isCurrent(swatch.modelData)
                                      ? root.textColor : Qt.rgba(0, 0, 0, 0.22)

                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (root.win)
                                        root.win.setNoteColor(swatch.modelData)
                                    root.closeMenu()
                                }
                            }
                        }
                    }
                }

                /* 色板下面那条"更多颜色…"（开系统取色框） */
                Rectangle {
                    id: moreColor
                    anchors.top: swatchGrid.bottom
                    anchors.topMargin: root.swatchGap
                    width: parent.width
                    height: root.itemHeight
                    radius: 4
                    color: moreHit.containsMouse ? root.hoverColor : "transparent"

                    Row {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        spacing: 6
                        NoteMenuIcon {
                            anchors.verticalCenter: parent.verticalCenter
                            kind: "color"
                            tint: root.textColor
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "更多颜色…"
                            font.pixelSize: 12
                            color: root.textColor
                        }
                    }

                    MouseArea {
                        id: moreHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.closeMenu()
                            root.customColorRequested()
                        }
                    }
                }
            }

            /* ---- 透明度 ---- */
            Column {
                visible: root.flyoutKind === "opacity"
                anchors.fill: parent
                anchors.margins: root.panePadding

                Repeater {
                    model: root.opacities

                    delegate: Rectangle {
                        id: opacityRow
                        required property var modelData
                        width: parent.width
                        height: root.itemHeight
                        radius: 4
                        color: opacityHit.containsMouse ? root.hoverColor : "transparent"

                        Row {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            spacing: 6
                            Item {
                                width: 14
                                height: parent.height
                                NoteMenuIcon {
                                    anchors.centerIn: parent
                                    kind: root.currentOpacity === opacityRow.modelData ? "check" : ""
                                    tint: root.textColor
                                }
                            }
                            Label {
                                height: parent.height
                                verticalAlignment: Text.AlignVCenter
                                text: opacityRow.modelData + "%"
                                font.pixelSize: 12
                                color: root.textColor
                            }
                        }

                        MouseArea {
                            id: opacityHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (root.win)
                                    root.win.setOpacityPercent(opacityRow.modelData)
                                root.closeMenu()
                            }
                        }
                    }
                }
            }

            /* ---- 正文里的链接（点一条把光标挪到正文那一行） ---- */
            Column {
                visible: root.flyoutKind === "links"
                anchors.fill: parent
                anchors.margins: root.panePadding
                clip: true

                Repeater {
                    model: root.links

                    delegate: Rectangle {
                        id: linkRow
                        required property int index
                        required property var modelData
                        width: parent.width
                        height: root.itemHeight
                        radius: 4
                        color: linkHit.containsMouse ? root.hoverColor : "transparent"

                        Label {
                            anchors.fill: parent
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            verticalAlignment: Text.AlignVCenter
                            text: linkRow.modelData.label !== "" ? linkRow.modelData.label
                                                                : linkRow.modelData.url
                            font.pixelSize: 11
                            color: root.textColor
                            elide: Text.ElideMiddle
                        }

                        MouseArea {
                            id: linkHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                var note = root.win ? root.win.noteData : null
                                var index = linkRow.index
                                root.closeMenu()
                                if (note)
                                    root.notes.revealLink(note, index)
                            }
                        }
                    }
                }
            }
        }
    }

    /* 这一格色块是不是当前底色 */
    function isCurrent(hex) {
        return String(hex).toLowerCase() === root.currentColor
    }

    /*
     * 菜单里那些小图标。
     *
     * 就地画（不引 AppIcon 那套 data URL）：这里的图标色要跟着菜单面板走，
     * 也不依赖主界面的 IconProvider（那是 QML 单例，便签窗口这边 import 不到）。
     */
    component NoteMenuIcon: Canvas {
        id: menuIcon

        required property string kind
        property color tint: "#24262a"

        implicitWidth: 14
        implicitHeight: 14
        width: 14
        height: 14
        antialiasing: true

        onKindChanged: requestPaint()
        onTintChanged: requestPaint()
        Component.onCompleted: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            ctx.strokeStyle = menuIcon.tint
            ctx.fillStyle = menuIcon.tint
            ctx.lineWidth = 1.5
            ctx.lineCap = "round"
            ctx.lineJoin = "round"

            var k = menuIcon.kind
            if (k === "") {
                /* 空：什么都不画 */
            } else if (k === "check") {
                ctx.beginPath()
                ctx.moveTo(2.5, 7.5); ctx.lineTo(6, 11); ctx.lineTo(12, 3.5)
                ctx.stroke()
            } else if (k === "chevron-right") {
                ctx.beginPath()
                ctx.moveTo(5.5, 3.5); ctx.lineTo(9.5, 7); ctx.lineTo(5.5, 10.5)
                ctx.stroke()
            } else if (k === "plus") {
                ctx.beginPath()
                ctx.moveTo(7, 2.5); ctx.lineTo(7, 11.5)
                ctx.moveTo(2.5, 7); ctx.lineTo(11.5, 7)
                ctx.stroke()
            } else if (k === "grid") {
                ctx.strokeRect(1.5, 1.5, 4.5, 4.5)
                ctx.strokeRect(8, 1.5, 4.5, 4.5)
                ctx.strokeRect(1.5, 8, 4.5, 4.5)
                ctx.strokeRect(8, 8, 4.5, 4.5)
            } else if (k === "color") {
                ctx.fillRect(2.5, 3.5, 9, 7)
                ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.35)
                ctx.strokeRect(2.5, 3.5, 9, 7)
            } else if (k === "opacity") {
                ctx.beginPath(); ctx.arc(7, 7, 5, 0, Math.PI * 2); ctx.stroke()
                ctx.beginPath(); ctx.arc(7, 7, 5, -Math.PI / 2, Math.PI / 2)
                ctx.closePath(); ctx.fill()
            } else if (k === "link") {
                ctx.beginPath()
                ctx.moveTo(5.5, 8.5); ctx.lineTo(8.5, 5.5)
                ctx.moveTo(4, 6.5); ctx.lineTo(2.5, 8)
                ctx.moveTo(10, 7.5); ctx.lineTo(11.5, 6)
                ctx.stroke()
            } else if (k === "copy") {
                ctx.strokeRect(2.5, 1.5, 7, 8.5)
                ctx.strokeRect(5, 4, 7, 8.5)
            } else if (k === "pin") {
                ctx.beginPath()
                ctx.moveTo(4.5, 1.5); ctx.lineTo(9.5, 1.5)
                ctx.lineTo(8.2, 5); ctx.lineTo(10, 7.5)
                ctx.lineTo(4, 7.5); ctx.lineTo(5.8, 5)
                ctx.closePath(); ctx.fill()
                ctx.beginPath(); ctx.moveTo(7, 7.5); ctx.lineTo(7, 12.5); ctx.stroke()
            } else if (k === "lock") {
                ctx.strokeRect(3.5, 6.5, 7, 6)
                ctx.beginPath(); ctx.arc(7, 6.5, 2.2, Math.PI, 0); ctx.stroke()
            } else if (k === "unlock") {
                ctx.strokeRect(3.5, 6.5, 7, 6)
                ctx.beginPath(); ctx.arc(8.4, 6.5, 2.2, Math.PI, Math.PI * 1.75); ctx.stroke()
            } else if (k === "hide") {
                ctx.beginPath()
                ctx.moveTo(2.5, 4); ctx.lineTo(11.5, 4)
                ctx.moveTo(7, 6.5); ctx.lineTo(7, 12)
                ctx.moveTo(4.5, 9.5); ctx.lineTo(7, 12); ctx.lineTo(9.5, 9.5)
                ctx.stroke()
            } else if (k === "trash") {
                ctx.beginPath()
                ctx.moveTo(3.5, 4); ctx.lineTo(10.5, 4)
                ctx.moveTo(5.5, 4); ctx.lineTo(5.5, 2.5)
                ctx.lineTo(8.5, 2.5); ctx.lineTo(8.5, 4)
                ctx.moveTo(4.5, 4); ctx.lineTo(5, 12)
                ctx.lineTo(9, 12); ctx.lineTo(9.5, 4)
                ctx.stroke()
            }
        }
    }
}
