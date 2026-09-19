pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../utils"
import SmartClip.Globals 1.0

/*
 * IDE 风格下拉菜单。
 *
 * ===========================================================================
 * 为什么是原生弹窗（popupType: Popup.Window），不是场景里的浮层
 * ===========================================================================
 * 编辑区是**原生 QScintilla 子窗口**（QWidget::createWindowContainer，见
 * src/EditorViewItem.h）。原生子窗口永远画在 QQuickWidget 的内容之上，
 * 这不是 z 值能改的事：QML 场景内的任何浮层（Item 叠加层、Popup.Item）
 * 只要和编辑区重叠，就会被编辑区整个盖掉 —— 实测表现就是
 * "语言下拉点出来什么都看不见，被内容区盖住了"。
 *
 * 所以菜单必须自己是一个**同级原生窗口**，由窗口管理器保证它盖在编辑区之上。
 * Qt 6.8 起 Popup 支持 popupType 属性：
 *   Popup.Item   —— 场景内浮层（默认，会被原生子窗口盖住，不能用）
 *   Popup.Window —— 独立原生窗口，内容还是这份 QML（本文件用的）
 *   Popup.Native —— 平台原生菜单
 *
 * ===========================================================================
 * 子菜单：同一个窗口里的第二块面板
 * ===========================================================================
 * "视图 -> 语言 / 编码 / 换行符" 这三条点开要在右边再来一块面板
 * （条目数据见 js/EditorMenus.js 里带 submenu: true + items 的那几条）。
 *
 * 两条关键规矩：
 *
 *  1) **只开一个原生弹窗**。两个 Popup 会互相抢焦点：点右边那块对左边来说
 *     就是"点了外面"，CloseOnPressOutside 会先关掉父菜单，子菜单跟着没，
 *     点击就丢了。做在同一个窗口里，鼠标在两边走都是"菜单内部"。
 *
 *  2) 两栏**各画各的面板**（各自的底、边框、圆角），不是共用一块底 ——
 *     子菜单的顶边要对齐"鼠标停的那一条"所在的行（见 openSubmenu），
 *     两栏高度本来就不一样；共用一个底会在父菜单下面拖出一大块空白。
 *
 * ===========================================================================
 * 条目格式（见 js/EditorMenus.js）
 * ===========================================================================
 *   { label, act, shortcut, checked, disabled }             普通项
 *   { label, submenu: true, items: [ ...同上... ] }          子菜单项（右边带 >）
 *   { separator: true }                                     分隔线
 */
Popup {
    id: root

    property var entries: []
    /*
     * 子菜单那一栏的条目（非空时右边多出一块面板）。
     * 由 openSubmenu() 填；条目自己带的 items 就是它。
     */
    property var subEntries: []
    /*
     * 子菜单那块面板的顶边（相对弹窗内容区）—— openSubmenu 时按"父级那一条
     * 所在的行"算出来，所以它和 submenuRowY 应该正好相等（自检量这个）。
     */
    property real submenuTop: 0
    /* 父级那一条所在行的 y（对齐的目标值，只用于自检对照） */
    property real submenuRowY: 0

    /*
     * "弹窗已经露出来之后，位置又被改过"的次数（自检读，见 Main.qml 的 uiState）。
     *
     * 规矩：位置在 openFor / openAtPoint 里一次定死，开出来之后只许长、不许挪。
     * 挪一下在 Windows 上就是"旧画面按新位置合成一帧"（闪）。正常恒为 0；
     * 一旦不是 0，就是"按最坏展开尺寸预留位置"那份账算漏了（见 openSubmenu）。
     */
    property int openShifts: 0

    signal selected(string act)

    readonly property color bgColor:     "#3c3f41"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor:   "#bbbbbb"
    readonly property color textHot:     "#e8e8e8"
    readonly property color hoverColor:  "#46484a"
    readonly property color mutedColor:  "#6f737a"
    readonly property color accentColor: "#4c96d8"

    readonly property real itemHeight: 28
    readonly property real separatorHeight: 9
    /* 面板四周给条目留的内缩（原来挂在弹窗的 padding 上，现在各面板自己留） */
    readonly property real panePadding: 4

    /*
     * 一块面板的宽度。
     *
     * menuWidth 保留成"只有主菜单时弹窗有多宽"，openFor / openAtPoint 的
     * 贴边夹取和老的自检值都还用这个数（两者现在相等）。
     */
    readonly property real paneWidth: 244
    readonly property real menuWidth: paneWidth

    /*
     * 两块面板之间的间隙。
     *
     * 子菜单是**一块独立面板**（JetBrains 那种），不是贴死在父菜单上：
     * 留一点缝，两边的圆角才都看得见 —— 贴在一起时为了不留豁口只能把
     * 相邻的两个角切成方的（试过），看着就是"圆角没了"。
     */
    readonly property real paneGap: 6

    /*
     * 一栏条目撑开后的总高度（分隔线更矮，含上下各 4px 内缩）。
     * 注意不能叫 contentHeight —— Popup 自己已经有这个名字的 FINAL 属性。
     */
    function paneHeight(items) {
        var h = 2 * panePadding
        for (var i = 0; i < (items ? items.length : 0); ++i)
            h += items[i] && items[i].separator ? separatorHeight : itemHeight
        return h
    }

    /* 这一栏里有没有带图标的条目（决定要不要留出图标列） */
    function paneHasIcons(items) {
        if (!items)
            return false
        for (var i = 0; i < items.length; ++i)
            if (items[i] && items[i].icon)
                return true
        return false
    }

    readonly property real entriesHeight: paneHeight(entries)
    readonly property real subEntriesHeight: paneHeight(subEntries)

    /*
     * 菜单最高能有多高。
     *
     * 语言菜单有 27 项，不限高就是 764px —— 会盖住左侧导航栏和大半个窗口。
     * 主流编辑器的长菜单都是限高 + 内部滚动，这里照做：最多到宿主窗口高度的
     * 一半左右，超出部分用滚轮 / 右侧细滚动条看。
     *
     * 高度取宿主（Main.qml 的根 Rectangle，即整个窗口内容区）的高度：
     * Popup 本身不是 Item，拿不到 Window 附着属性，只能顺着 parent 往上问。
     */
    readonly property real hostHeight: root.parent ? root.parent.height : 800
    readonly property real maxMenuHeight: Math.max(168, Math.min(hostHeight - 24, 460))

    /* 主菜单那块面板实际画出来的高度 */
    readonly property real menuHeight: Math.min(entriesHeight, maxMenuHeight)
    readonly property bool scrollable: entriesHeight > menuHeight + 1

    /*
     * ======================================================================
     * "最坏能展开到多大" —— 开菜单之前就要按它把位置定死
     * ======================================================================
     *
     * 起因（自检"弹窗：展开子菜单时弹窗的左上角一动不动"钉的就是它）：
     * 位置原来是**展开子菜单那一刻**才夹的 —— 顶出宿主下沿就整体往上挪一下。
     * 而"露着的时候改位置"在 Windows 上必然闪：系统先拿窗口的旧画面按新位置
     * 合成一帧，Qt 下一帧才画新内容（便签菜单那边为同一条把主栏和子面板拆成了
     * 两块窗口，见 qml/notes/NoteMenu.qml 里那段实测记录）。
     *
     * 所以规矩改成：**位置在 openFor / openAtPoint 里一次定死，开出来之后只许
     * 长高长宽、不许挪**。既然不知道用户会点开哪一个子菜单，就按"这一份菜单里
     * 最坏的那个"预留：
     *
     *   高度 = max(主栏高, 带子菜单的每一行里 (那一行的位置 + 那一栏的高度) 的最大值)
     *   宽度 = 有子菜单条目时两栏并排那么宽，否则就是主栏宽
     *
     * 代价（写在明处）：宿主窗口比较矮时，菜单会**一开始**就摆到"最坏情况也装得下"
     * 的位置（可能被顶到宿主顶上），而不是先挂在它那一栏下面、等展开子菜单再跳上去。
     * 换来的是整场菜单里窗口一动不动 —— 不动就没有"旧画面按新位置合成"那一帧。
     *
     * 这里的算式要和 openSubmenu 里摆子栏那几行**对得上**（同一套 paneHeight /
     * itemHeight / separatorHeight / panePadding），对不上就会退回"展开时挪一下"
     * 那条路 —— 那时 openShifts 会记账，自检会红。
     */
    function worstExpandedHeight(items) {
        /*
         * 封顶：主栏真实画出来最多只有 maxMenuHeight 那么高（见 menuHeight），
         * 所以"最坏展开高度"也不能超过它。
         *
         * 不封顶会这样：长菜单（比如"设置"，内容 ~808px）算出的 worstH 是 808，
         * openFor 里 `py + worstH > host.height - 4`（31 + 808 > 896）成立，
         * 于是翻到锚点上方，py = 31-808-6 = -783，最后被 Math.max(2, …) 夹到
         * **y=2 —— 整条菜单压在导航栏上**（用户报的"设置这个下拉框贴在顶上了"）。
         * 而菜单真实高度只有 460，下面明明放得下。
         */
        var worst = Math.min(paneHeight(items), maxMenuHeight)
        if (!items)
            return worst
        var rowTop = panePadding
        for (var i = 0; i < items.length; ++i) {
            var entry = items[i]
            var isSub = entry && entry.submenu === true && entry.items && entry.items.length > 0
            if (isSub) {
                var subH = Math.min(paneHeight(entry.items), maxMenuHeight)
                worst = Math.max(worst, rowTop + subH)
            }
            rowTop += (entry && entry.separator) ? separatorHeight : itemHeight
        }
        return Math.min(worst, maxMenuHeight)
    }

    /* 这一份菜单里有没有"能展开"的条目（决定宽度要不要按两栏预留） */
    function hasSubmenuEntry(items) {
        if (!items)
            return false
        for (var i = 0; i < items.length; ++i) {
            var entry = items[i]
            if (entry && entry.submenu === true && entry.items && entry.items.length > 0)
                return true
        }
        return false
    }

    /* 开之前用来夹位置的那两个数（见 worstExpandedHeight 的说明） */
    function worstExpandedWidth(items) {
        return hasSubmenuEntry(items) ? paneWidth * 2 + paneGap : paneWidth
    }

    /* ---- 子菜单那一栏 ---- */
    readonly property bool submenuOpened: subEntries !== undefined && subEntries !== null
                                          && subEntries.length > 0
    readonly property real submenuHeight: submenuOpened
                                          ? Math.min(subEntriesHeight, maxMenuHeight) : 0
    readonly property bool submenuScrollable: subEntriesHeight > submenuHeight + 1
    readonly property bool submenuHasIcons: paneHasIcons(subEntries)

    /*
     * 子菜单那一块面板在主菜单右边多远（相对弹窗内容区）= 主栏宽 + 那条缝。
     * 自检据此确认"子菜单确实在右边、而且和主菜单之间留着缝"。
     */
    readonly property real submenuInset: paneWidth + paneGap

    /*
     * 这组菜单里有没有带图标的条目。
     *
     * 语言 / 编码这种纯值列表没有图标，那就别留出图标列的位置；
     * 有图标的菜单（文件 / 编辑 / …）统一留出，保证所有文字左对齐。
     */
    readonly property bool hasIcons: paneHasIcons(entries)

    /* 勾选列 + 图标列的宽度（条目标签的左缩进就是它们之和） */
    readonly property real checkColumn: 14
    readonly property real iconColumn: hasIcons ? 17 : 0
    readonly property real leadColumn: checkColumn + iconColumn

    /*
     * 弹窗尺寸 = 两块面板的**外接矩形**。
     *
     * 宽度：开着子菜单时两栏并排；高度：子栏顶边在 submenuTop，所以是
     * max(主栏高, submenuTop + 子栏高) —— 子栏从中间某一行往下伸，
     * 比主栏矮/高都正常，不再强行"一样高"。
     *
     * 写的是 implicit*，不是 width / height：Popup 打开时自己会去摆
     * width / height（QQuickPopup 内部要算位置和贴边），显式 width 那条绑定
     * 会被它赋一次值给打断 —— 之后再开子菜单，弹窗尺寸就不跟着走了
     * （实测：子菜单那一栏已经画出来，弹窗还是 244 宽）。
     */
    implicitWidth: submenuOpened ? paneWidth * 2 + paneGap : paneWidth
    implicitHeight: Math.max(menuHeight, submenuOpened ? submenuTop + submenuHeight : 0)

    /*
     * 弹窗自己**不画底**：两块面板各画各的（见 contentItem）。
     *
     * 这层还是留一个空 Item 而不是设成 null —— Main.menuTopLeft() 拿
     * background 去反推菜单左上角（自检量菜单落点用），margins: 0 时它正好
     * 铺满整个弹窗。
     */
    background: Item {}
    margins: 0
    padding: 0
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    /* 关键：独立原生窗口，否则会被原生编辑区盖住（见文件头注释） */
    popupType: Popup.Window

    IconProvider { id: menuIcons }

    /*
     * 菜单关掉时把子菜单那一栏也清掉。
     *
     * 不清的话下次 openFor() 之前它会一直挂在那儿（虽然看不见），
     * uiState 之类的读数也会跟着虚报。
     */
    onClosed: {
        subEntries = []
        submenuTop = 0
        submenuRowY = 0
    }

    /*
     * openFor 的 anchor 是菜单栏 tab / 工具栏按钮（都是 QML 里的 Item）。
     *
     * 坐标是**相对宿主窗口内容区**的，不是屏幕坐标：弹窗虽然是个独立原生
     * 窗口，Popup 的 x/y 语义没变，Qt 自己会换算成屏幕位置。实测传屏幕坐标
     * 会被再加一次窗口原点，菜单直接跑到窗口外面（出现在 (2683,1243)）。
     *
     * 下方放不下就翻到锚点上方（和主流菜单一致）。
     */
    function openFor(anchor, items) {
        entries = items
        subEntries = []
        submenuTop = 0
        submenuRowY = 0
        list.contentY = 0

        var host = root.parent
        var below = anchor.mapToItem(host, 0, anchor.height + 3)
        var above = anchor.mapToItem(host, 0, 0)

        /*
         * 夹位置用的是"最坏展开到多大"，不是当前主栏的尺寸 ——
         * 这样下面展开子菜单时窗口只管往下长，一次都不用挪（见
         * worstExpandedHeight 上面那一大段）。
         *
         * 算式的形状要和 openSubmenu 里那条兜底夹取**一模一样**
         * （max(2, min(想要的, 宿主 - 最坏尺寸 - 4))）：下限同样是 2。
         * 不一致就会露馅 —— 这里落在 8、兜底想要 2，展开子菜单时窗口照样挪 6px
         * （实测：自检"展开子菜单时弹窗的左上角一动不动"就是这么红的）。
         */
        var worstW = worstExpandedWidth(items)
        var worstH = worstExpandedHeight(items)

        var px = Math.max(2, Math.min(below.x, host.width - worstW - 4))
        var py = below.y
        if (py + worstH > host.height - 4)
            py = above.y - worstH - 6      /* 下边放不下：翻到锚点上面去 */
        py = Math.max(2, Math.min(py, host.height - worstH - 4))

        root.x = px
        root.y = py
        root.open()
    }

    /*
     * 在**鼠标那一点**打开：菜单左上角紧贴 (px, py)（内容区 tab 的右键菜单）。
     *
     * 和 openFor 是一件事的两种落点：
     *   openFor      锚一个控件，菜单挂在它的正下方（菜单栏 tab / 工具栏按钮）；
     *   openAtPoint  锚一个坐标，菜单左上角就压在右键按下的那一点上。
     *
     * (px, py) 是 anchor 的本地坐标（右键事件里的 mouse.x / mouse.y），
     * 先换算成宿主窗口内容区坐标 —— Popup 的 x/y 要的是宿主坐标，不是屏幕
     * 坐标（原因见 openFor 上面的说明）。anchor 传 null 时按宿主坐标直接用。
     *
     * 只有贴边放不下时才往回收：右边 / 下边分别夹进宿主窗口。
     * 弹窗是独立原生窗口，伸到窗口外面不会被裁掉，但那几条就点不到了。
     * 一般位置（tab 在窗口上半部分，右边还留着菜单宽度）夹取不生效，
     * 左上角就是鼠标那一点。
     *
     * 夹的是"最坏展开到多大"，不是主栏当前的尺寸 —— 见 worstExpandedHeight
     * 上面那一大段：位置开之前一次定死，之后展开子菜单只长不挪。
     *
     * 先算好坐标再 open()，和 openFor 一样：不在打开之后二次移动，
     * 否则会闪一下"初始位置的菜单"再跳到鼠标这里。
     */
    function openAtPoint(anchor, px, py, items) {
        entries = items
        subEntries = []
        submenuTop = 0
        submenuRowY = 0
        list.contentY = 0

        var host = root.parent
        var p = anchor ? anchor.mapToItem(host, px, py) : Qt.point(px, py)
        var worstW = worstExpandedWidth(items)
        var worstH = worstExpandedHeight(items)

        root.x = Math.round(Math.max(2, Math.min(p.x, host.width - worstW - 4)))
        root.y = Math.round(Math.max(2, Math.min(p.y, host.height - worstH - 4)))
        root.open()
    }

    /*
     * 在右边展开子菜单（条目 hover / 点上去时调，见下面 MenuEntryItem）。
     *
     * fromItem 是**父级那一条**（委托自己）：子菜单面板的顶边要和它所在的行
     * 对齐，所以这里量它的 y。行 y 要减去列表的滚动量 —— 列表滚过之后，
     * 条目在面板里的位置是 entry.y - contentY（委托的 y 是内容坐标）。
     * 再加上面板那条 4px 内缩，就是面板坐标里的行顶边。
     *
     * 展开之后弹窗要往下长（见 implicitHeight），可能顶出宿主窗口下沿 ——
     * 那就整体往上挪（和菜单自己贴边时的处理一致，两栏一起挪，相对关系不动）。
     *
     * 返回有没有展开（没条目 / 空列表就当没这回事）。
     */
    function openSubmenu(items, fromItem) {
        if (!items || items.length === 0)
            return false

        subEntries = items
        subList.contentY = 0

        var rowY = fromItem ? Math.max(0, fromItem.y - list.contentY) : 0
        submenuRowY = Math.round(rowY + panePadding)
        submenuTop = submenuRowY

        var host = root.parent
        /*
         * 顶出宿主下沿 / 右沿时把整块弹窗挪回来。
         *
         * **这一步是"露着的时候改位置"** —— 而位置在 Windows 上改一下，系统就会
         * 把窗口的**旧画面**按新位置先合成一帧，Qt 下一帧才画新内容：用户看到的
         * 就是"菜单闪一下再跳过去"（便签菜单那边为这一条专门拆成了两块窗口，
         * 见 qml/notes/NoteMenu.qml 里那段实测记录）。
         *
         * 所以规矩是：**位置在 openFor / openAtPoint 里一次定死，开出来之后只许
         * 长高长宽、不许挪**（见那两个函数里按"最坏展开到多大"夹位置）。
         * 这里保留夹取只当兜底 —— 真走到这儿就说明上面那份预留算漏了，
         * openShifts 记一笔，自检("弹窗：展开子菜单没有走到"露着的时候挪位置"
         * 那条兜底")会红。
         */
        if (host && root.y + implicitHeight > host.height - 4) {
            const ny = Math.max(2, host.height - implicitHeight - 4)
            if (root.opened && ny !== root.y)
                ++openShifts
            root.y = ny
        }
        if (host && root.x + implicitWidth > host.width - 4) {
            const nx = Math.max(2, host.width - implicitWidth - 4)
            if (root.opened && nx !== root.x)
                ++openShifts
            root.x = nx
        }
        return true
    }

    /* 收起子菜单那一栏（鼠标移到普通条目上时调） */
    function clearSubmenu() {
        subEntries = []
        submenuTop = 0
        submenuRowY = 0
    }

    /*
     * ======================================================================
     * 主窗口挪了 / 改了尺寸：**把菜单收起来**（不是跟着挪）
     * ======================================================================
     *
     * 用户报的问题：整条菜单没挂在「视图」那一栏下面，偏了 700 多像素。
     *
     * 原因是菜单这块**独立原生窗口**（popupType: Popup.Window）不跟着宿主窗口走：
     * 它的屏幕位置在开出来那一刻就定死了，主窗口后来一挪（拖窗口、最大化 / 还原、
     * 系统贴边吸附、换显示器、DPI 变化），弹窗还钉在原来的屏幕位置上
     * （实测：窗口 (400,200) -> (100,116)，菜单留在 (657,231)；最大化那一下同理，
     * 见 build\probe-submenu*.ps1 那几套探针）。
     *
     * 为什么不"跟着挪"而选择**收起来**：
     *
     *  1) Qt 自己会**反着来**：父窗口一移动，它就把弹窗的 x / y 改掉，好让弹窗
     *     留在原来的屏幕位置上（实测：窗口 (400,200)->(100,116)，弹窗的 x 被从
     *     257 改成 -43）。要"跟着走"就得跟它这套记账对着干，而且改一次位置在
     *     Windows 上就是"旧画面按新位置合成一帧"（闪）—— 而这正是本文件开头那
     *     一整段在躲的事（位置上开出来之后一次都不动）。实测硬掰只能对第一次
     *     移动有效，后面几次会被 Qt 的记账盖回去。
     *  2) 系统原生菜单就是这么做的：窗口一动，菜单收起来。用户看到的行为是
     *     "菜单不会跑到别处去"，而不是"菜单飘在半空中"。
     *
     * 位置还是按老规矩：**在 openFor / openAtPoint 里一次定死**，之后只有"开 / 关"，
     * 没有"挪"。openShifts 那条不变。
     *
     * 信号来自 C++ 的 WindowHelper（见那里的 hostGeometry）：宿主 QWidget 的真实
     * 几何只有它知道 —— QML 这侧的 Window.x / mapToGlobal 和它不是一套坐标系
     * （main.cpp 给识别卡片量位置时踩过同一条）。
     */
    Connections {
        target: Win
        function onHostGeometryChanged() { root.close() }
    }

    /*
     * 鼠标停到某一条上（委托的 onEntered 调它）。
     *
     * 子菜单条目 -> 展开右边那块；普通条目 -> 把右边那块收掉，免得鼠标移开
     * 之后它还挂在那儿、和当前高亮的那条对不上。
     *
     * **子菜单自己那栏里的条目不算"普通条目"**：鼠标从父级那一条往右挪进
     * 子菜单时，先进入的就是子菜单里的第一条，那时候要是把子菜单收掉，
     * 面板会在鼠标底下消失 —— 手感就是"还没移上去它就没了"（用户报的就是这个）。
     *
     * 抽成组件上的函数而不是写在委托里，是为了让自检也能走同一条判断：
     * 悬停 C++ 侧点不出来，见 hoverSubmenuEntry()。
     */
    function hoverEntry(entry) {
        if (!entry || entry.isDisabled || entry.isSeparator)
            return
        if (entry.hasSubItems)
            openSubmenu(entry.modelData.items, entry)
        else if (!entry.inSubmenu && submenuOpened)
            clearSubmenu()
    }

    /* 子菜单里第 index 条委托（自检用；跳过 Column 里那个 Repeater） */
    function submenuEntry(index) {
        var n = 0
        for (var i = 0; i < subCol.children.length; ++i) {
            var item = subCol.children[i]
            if (!item || item.modelData === undefined)
                continue
            if (n === index)
                return item
            ++n
        }
        return null
    }

    /*
     * 自检用：模拟"鼠标停到子菜单里第 index 条上"。
     * 返回停完之后子菜单是不是还开着（这正是当初出 bug 的地方）。
     */
    function hoverSubmenuEntry(index) {
        var item = submenuEntry(index === undefined ? 0 : index)
        if (!item)
            return false
        hoverEntry(item)
        return submenuOpened
    }

    /*
     * 按动作名找主菜单里那一条的**委托**（自检用，见 Main.openSubmenuFor）。
     *
     * 界面上展开子菜单是"鼠标停到某一条上"，委托会把自己（连同自己的 y）
     * 交给 openSubmenu；C++ 侧悬停不出来，自检就靠这个把同一个委托找出来，
     * 走的是同一条路、同一个 y —— 而不是在测试里另算一遍行的位置。
     */
    function entryItemFor(act) {
        for (var i = 0; i < col.children.length; ++i) {
            var item = col.children[i]
            if (item && item.modelData && item.modelData.act === act)
                return item
        }
        return null
    }

    contentItem: Item {
        id: contentArea

        /* ---------------- 主菜单面板 ---------------- */
        Item {
            id: mainPane

            x: 0
            y: 0
            width: root.paneWidth
            height: root.menuHeight

            /*
             * 面板自己的底和边框。四个角都是圆的 —— 子菜单那块隔着 paneGap
             * 站在右边，不再和这一块贴死，所以这边不用再切方角。
             */
            Rectangle {
                anchors.fill: parent
                color: root.bgColor
                border.color: root.borderColor
                radius: 5
            }

            Flickable {
                id: list

                anchors.fill: parent
                anchors.margins: root.panePadding
                contentWidth: width
                contentHeight: col.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: root.scrollable

                /* 滚轮也能滚：Flickable 自己处理 wheel 事件 */
                ScrollBar.vertical: ThinScrollBar {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                }

                Column {
                    id: col

                    /* 有滚动条时给右边让出一点位置，免得文字压在滚动条上 */
                    width: list.width - (root.scrollable ? 6 : 0)
                    spacing: 0

                    Repeater {
                        model: root.entries
                        delegate: MenuEntryItem {}
                    }
                }
            }

            /* 上面 / 下面还有内容时给个小箭头提示（长菜单才出现） */
            AppIcon {
                anchors.horizontalCenter: list.horizontalCenter
                anchors.top: list.top
                anchors.topMargin: 1
                provider: menuIcons
                kind: "chevron-up"
                size: 10
                tint: root.mutedColor
                visible: root.scrollable && list.contentY > 1
            }

            AppIcon {
                anchors.horizontalCenter: list.horizontalCenter
                anchors.bottom: list.bottom
                anchors.bottomMargin: 1
                provider: menuIcons
                kind: "chevron-down"
                size: 10
                tint: root.mutedColor
                visible: root.scrollable
                         && list.contentY < list.contentHeight - list.height - 1
            }
        }

        /* ---------------- 子菜单面板（右边那一块） ---------------- */
        Item {
            id: subPane

            visible: root.submenuOpened
            /* 右边那块面板：和主菜单之间留着 paneGap 那条缝 */
            x: root.submenuInset
            /* 顶边 = 父级那一条所在的行（用户要的效果：从那条旁边伸出来） */
            y: root.submenuTop
            width: root.paneWidth
            height: root.submenuHeight

            /* 独立一块面板：四个角都是圆的（和主菜单之间隔着那条缝） */
            Rectangle {
                anchors.fill: parent
                color: root.bgColor
                border.color: root.borderColor
                radius: 5
            }

            Flickable {
                id: subList

                anchors.fill: parent
                anchors.margins: root.panePadding
                contentWidth: width
                contentHeight: subCol.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                interactive: root.submenuScrollable

                ScrollBar.vertical: ThinScrollBar {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                }

                Column {
                    id: subCol

                    width: subList.width - (root.submenuScrollable ? 6 : 0)
                    spacing: 0

                    Repeater {
                        model: root.subEntries
                        delegate: MenuEntryItem { inSubmenu: true }
                    }
                }
            }

            AppIcon {
                anchors.horizontalCenter: subList.horizontalCenter
                anchors.top: subList.top
                anchors.topMargin: 1
                provider: menuIcons
                kind: "chevron-up"
                size: 10
                tint: root.mutedColor
                visible: root.submenuScrollable && subList.contentY > 1
            }

            AppIcon {
                anchors.horizontalCenter: subList.horizontalCenter
                anchors.bottom: subList.bottom
                anchors.bottomMargin: 1
                provider: menuIcons
                kind: "chevron-down"
                size: 10
                tint: root.mutedColor
                visible: root.submenuScrollable
                         && subList.contentY < subList.contentHeight - subList.height - 1
            }
        }
    }

    /*
     * 一条菜单条目 —— 主菜单和子菜单**共用这一份绘制**。
     *
     * 用 inline component 而不是复制两份：两栏的条目长得一模一样，
     * 只有两点不同，靠 inSubmenu 区分：
     *   * 图标列留不留位置（语言 / 编码这种纯值列表没有图标）；
     *   * 右边显示的是快捷键，还是"会展开子菜单"的那个 >。
     */
    component MenuEntryItem: Item {
        id: entry

        required property var modelData
        /* 这条属于右边那块面板（子菜单）吗 */
        property bool inSubmenu: false

        readonly property bool isSeparator: modelData && modelData.separator === true
        readonly property bool isDisabled: modelData && modelData.disabled === true
        /* 点开会往右边再展开一块面板的条目（EditorMenus.js 里标了 submenu: true） */
        readonly property bool isSubmenu: !!(modelData && modelData.submenu === true)
        readonly property bool hasSubItems: isSubmenu && modelData.items !== undefined
                                            && modelData.items !== null
                                            && modelData.items.length > 0
        /* 勾选列 + 图标列：本栏没有图标条目时就不留图标列的位置 */
        readonly property real leadWidth: root.checkColumn
                                          + ((inSubmenu ? root.submenuHasIcons
                                                        : root.hasIcons) ? root.iconColumn : 0)

        width: parent ? parent.width : 0
        height: entry.isSeparator ? root.separatorHeight : root.itemHeight

        /* ---- 分隔线 ---- */
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            height: 1
            color: root.borderColor
            opacity: 0.8
            visible: entry.isSeparator
        }

        /* ---- 普通条目 ---- */
        Rectangle {
            id: itemRect

            anchors.fill: parent
            visible: !entry.isSeparator
            radius: 4
            color: itemHit.containsMouse && !entry.isDisabled
                   ? root.hoverColor : "transparent"

            Row {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 10
                spacing: 6

                /* 勾选列（当前语言 / 当前编码这类"选中项"打勾） */
                Item {
                    width: root.checkColumn
                    height: parent.height

                    AppIcon {
                        anchors.centerIn: parent
                        provider: menuIcons
                        kind: "check"
                        size: 13
                        visible: entry.modelData
                                 && entry.modelData.checked === true
                        tint: root.accentColor
                    }
                }

                /*
                 * 图标列：图标在**左边**，快捷键在右边。
                 *
                 * 工具栏那一整行已经去掉，命令全部收进这些下拉菜单，
                 * 图标就是它们的"脸"。
                 */
                Item {
                    width: entry.leadWidth - root.checkColumn
                    height: parent.height

                    AppIcon {
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        provider: menuIcons
                        kind: entry.modelData && entry.modelData.icon
                              ? entry.modelData.icon : ""
                        size: 14
                        /* 和工具栏原来那套图标一个色：平时灰、悬停转亮 */
                        tint: entry.isDisabled ? "#4d5157"
                                               : (itemHit.containsMouse
                                                  ? root.textHot
                                                  : "#9aa0a8")
                    }
                }

                Text {
                    id: itemLabel
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width - entry.leadWidth - 6
                            - (shortcutLabel.visible
                               ? shortcutLabel.implicitWidth + 12 : 0)
                            - (submenuArrow.visible ? submenuArrow.width + 8 : 0)
                    /*
                     * 分隔线条目里没有 label / shortcut，取值前必须
                     * 判存在：直接取会得到 undefined，赋给 QString
                     * 属性会刷 "Unable to assign [undefined] to QString"。
                     */
                    text: entry.modelData
                          && entry.modelData.label !== undefined
                          ? entry.modelData.label : ""
                    color: entry.isDisabled ? root.mutedColor
                                            : (itemHit.containsMouse
                                               ? root.textHot : root.textColor)
                    font.pixelSize: 13
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }

                Text {
                    id: shortcutLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: entry.modelData && entry.modelData.shortcut
                          ? entry.modelData.shortcut : ""
                    visible: text !== ""
                    color: entry.isDisabled ? "#55585d" : root.mutedColor
                    font.pixelSize: 11
                    verticalAlignment: Text.AlignVCenter
                }

                /*
                 * 子菜单标记：这一条点开会在右边再来一块面板（参考图里那个 >）。
                 *
                 * 位置和快捷键是同一个位置 —— 两条同时在的条目不存在
                 * （子菜单条目本来就不挂快捷键）。
                 */
                AppIcon {
                    id: submenuArrow
                    anchors.verticalCenter: parent.verticalCenter
                    visible: entry.isSubmenu
                    provider: menuIcons
                    kind: "chevron-right"
                    size: 11
                    tint: entry.isDisabled ? "#55585d"
                                           : (itemHit.containsMouse ? root.textHot
                                                                    : root.mutedColor)
                }
            }

            MouseArea {
                id: itemHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: entry.isDisabled ? Qt.ArrowCursor
                                              : Qt.PointingHandCursor

                /*
                 * 鼠标停到条目上：交给 root.hoverEntry() 判断（子菜单条目展开
                 * 右边那块、普通条目把右边那块收掉，子菜单**里面**的条目
                 * 两个都不做 —— 原因见 hoverEntry 的说明）。
                 */
                onEntered: root.hoverEntry(entry)

                onClicked: {
                    if (entry.isDisabled || entry.isSeparator)
                        return

                    /* 子菜单条目：点 = 展开（悬停已经展开过一次，这里兜底） */
                    if (entry.hasSubItems) {
                        root.openSubmenu(entry.modelData.items, entry)
                        return
                    }

                    /*
                     * 其余命令：**先收菜单，再发命令**。
                     *
                     * 反过来（先发命令再 close）碰上"打开…" "保存"
                     * "另存为…" "打印…" 这种会弹**模态**原生对话框的命令
                     * 就露馅了：QFileDialog 是同步的，它在自己的嵌套事件
                     * 循环里把整条 JS 调用栈堵住，后面那句 close() 要等
                     * 用户关掉对话框才轮得到 —— 于是菜单一直挂在对话框
                     * 上面（用户截图报的就是这个）。
                     */
                    root.close()
                    root.selected(entry.modelData.act)
                }
            }
        }
    }
}
