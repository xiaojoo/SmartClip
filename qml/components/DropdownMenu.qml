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

    /*
     * 主菜单这一块**已经建出来的条目数**（自检拿它当"内容长齐了没有"的读数，
     * 见 src/SelfTest.cpp 的 surfaceStaysPut）。
     *
     * 为什么要有它：这块弹窗的宽高是算出来的（条目数 * 行高），所以"窗口尺寸
     * 没变"证明不了"画面没变" —— 委托项要哪一拍没建出来，屏幕上就是一块空面板，
     * 几何一个像素都不动。只看几何的自检对这种是瞎的。
     */
    readonly property int itemCount: entriesRepeater.count

    signal selected(string act)

    /*
     * 命令离开这块菜单的那一刻，菜单自己还可见吗（自检读，见 src/SelfTest.cpp
     * "命令开始跑之前菜单已经收掉了"）。
     *
     * 为什么记在菜单这一侧而不是 Main.qml：要判的就是"收菜单"和"发命令"谁在前，
     * 那两件事都发生在这里；绕到外面再回头看，量的就不是同一个时间点了。
     */
    property bool visibleWhenCommand: false

    /* 把命令交出去（唯一出口，见下面 delegate 的 onClicked） */
    function runCommand(act) {
        root.visibleWhenCommand = root.visible
        root.selected(act)
    }

    /*
     * 收菜单和发命令之间隔一拍。
     *
     * 用户报的"打开… / 保存 / 打开文件夹 这些要弹系统对话框的，菜单要等对话框
     * 出来才慢慢收"：onClicked 里 `close()` 紧跟 `selected(act)` 两句挨着写，
     * 顺序看着对，其实 close() 只是**开始**上面那段 32ms 的淡出 —— 那一帧一帧
     * 要事件循环来推。而 QFileDialog 是同步的，一进函数就把 GUI 线程占住
     * （建 shell 对话框那一两百 ms 里 Qt 一帧都轮不到），淡出就整个卡在那儿：
     * 屏幕上对话框已经出来了，菜单还挂着，等对话框关掉它才收。
     * （自检量到的原话：命令开始时菜单还可见=1。）
     *
     * 所以把命令挪到**下一个事件循环回合**：40ms = 淡出 8 + 按住 24（那段是为了
     * 不让 DWM 重放旧画面，见上面 exit 的说明）+ 一帧余量。点下去到对话框出现
     * 多等 40ms，换来的是"菜单先没了，对话框再来"。
     */
    Timer {
        id: commandTimer
        interval: 40
        property string act: ""
        onTriggered: root.runCommand(act)
    }

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
     * 菜单最高能有多高 = **宿主窗口可用高度**（内容多高就画多高，装不下才滚）。
     *
     * 这里原来还夹着一道固定的 460：语言菜单 27 项 764px 确实被治住了，可是
     * 「文件」622 / 「视图」595 / 「编辑」510 三条因此**常年带一条滚动条**，
     * 而 900 高的窗口下面明明放着（用户 2026-09-22 提的"文件下拉不要出现滚动条，
     * 超出了才出现滚动条"）。固定顶换成"按窗口给"：窗口够高就整块展开，
     * 窗口矮到装不下（比如压矮主窗口、小屏、上下分栏）才限高 + 出滚动条。
     *
     * 高度取宿主（Main.qml 的根 Rectangle，即整个窗口内容区）的高度：
     * Popup 本身不是 Item，拿不到 Window 附着属性，只能顺着 parent 往上问。
     */
    readonly property real hostHeight: root.parent ? root.parent.height : 800
    readonly property real maxMenuHeight: Math.max(168, hostHeight - 24)

    /*
     * 这一次打开，主栏最高画到哪儿 = min(全局上限, 锚点下面还剩的空间)。
     *
     * 全局上限（maxMenuHeight）只按宿主窗口算，可菜单是挂在**某一栏下面**的：
     * 「设置」内容 ~900px，比"顶栏下沿到窗口底"那 865px 还高，光按全局算就会
     * 判定"下面放不下"→ 翻到锚点上方 → 被夹回 y=2，整块压在导航栏上。
     * 按当场的空间再封一道顶，它就老老实实挂在栏下、自己出滚动条。
     *
     * 每次 openFor / openAtPoint 重算（窗口拉高拉矮、换锚点都跟着走）。
     */
    property real openCap: maxMenuHeight

    /* 主菜单那块面板实际画出来的高度 */
    readonly property real menuHeight: Math.min(entriesHeight, openCap)
    readonly property bool scrollable: entriesHeight > menuHeight + 1

    /*
     * 子菜单那一栏的上限。
     *
     * 除了全局上限，还要封顶到"这一行往下还剩多少"：子栏要是能从中间某一行一直
     * 伸出宿主下沿，展开时就得把整块弹窗往上挪 —— 而"露着的时候挪位置"在 Windows
     * 上就是 DWM 重放旧画面那一帧（闪），那份账本文件开头躲的就是它。
     * 宁可子栏自己滚，也不挪窗口。
     *
     * 写成现算的只读绑定（不是 openSubmenu 里赋一次值）：宿主高度、弹窗 y、
     * 那一行的位置任何一个变了，它都跟着走。
     */
    readonly property real submenuRoomBelow: root.parent
        ? root.parent.height - 4 - (root.y + submenuTop) : maxMenuHeight
    /*
     * 这里**不留下限**（不是"至少也给人家 168"）：留了下限，弹窗总高就会顶穿宿主
     * 下沿，展开那一下就得把窗口往上挪 —— 挪一下就是 DWM 重放旧画面那一帧。
     * 真挤到一点空间都不剩（宿主很矮、又是最后一行），就是这一栏暂时不出来，
     * 把主栏滚一滚，行位置一变空间就回来了。
     */
    readonly property real submenuCap: Math.max(0, Math.min(maxMenuHeight, submenuRoomBelow))

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
        var worst = Math.min(paneHeight(items), openCap)
        if (!items)
            return worst
        var rowTop = panePadding
        for (var i = 0; i < items.length; ++i) {
            var entry = items[i]
            var isSub = entry && entry.submenu === true && entry.items && entry.items.length > 0
            if (isSub) {
                var subH = Math.min(paneHeight(entry.items), openCap)
                worst = Math.max(worst, rowTop + subH)
            }
            rowTop += (entry && entry.separator) ? separatorHeight : itemHeight
        }
        return Math.min(worst, openCap)
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

    /*
     * 定位时到底按多高预留 = min(最坏展开高, 锚点下面还剩的空间)，但**至少装得下主栏**。
     *
     * 为什么要有这一层封顶（2026-09-22）：上限从固定的 460 换成"宿主可用高度"之后，
     * 带子菜单的「视图」「设置」算出来的 worstExpandedHeight 就是 876，
     * openFor 一看 `31 + 876 > 896` 就翻到锚点上面，再被 `Math.max(2, …)` 夹回
     * **y=2 —— 菜单整个压在导航栏上**（用户报的"设置/视图锚点不对，要在导航栏下"）。
     * 预留只是"防挪"的手段，不该反过来决定菜单挂到哪儿。
     *
     * 封顶之后还放不下子栏吗？放得下：子栏自己按"这一行往下还剩多少"收高
     * （见 submenuCap），所以弹窗的总高永远不超过宿主下沿，展开时一次都不用挪。
     * 主栏本身在下面真放不下时，照旧翻到锚点上方。
     */
    function reservedHeight(items, roomBelow) {
        return Math.max(Math.min(paneHeight(items), openCap),
                        Math.min(worstExpandedHeight(items), roomBelow))
    }

    /* 开之前用来夹位置的那两个数（见 worstExpandedHeight 的说明） */
    function worstExpandedWidth(items) {
        return hasSubmenuEntry(items) ? paneWidth * 2 + paneGap : paneWidth
    }

    /* ---- 子菜单那一栏 ---- */
    readonly property bool submenuOpened: subEntries !== undefined && subEntries !== null
                                          && subEntries.length > 0
    readonly property real submenuHeight: submenuOpened
                                          ? Math.min(subEntriesHeight, submenuCap) : 0
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
     * ======================================================================
     * 收菜单先淡一帧再藏：治"菜单在旧位置闪一下"
     * ======================================================================
     *
     * 现场量法（两样一起跑，时间点能对上）：
     *   powershell -File build/menu-geom-watch.ps1        # 每 ~1ms 采弹窗原生窗的矩形
     *   ffmpeg -f lavfi -i ddagrab=framerate=240 -vf "hwdownload,format=bgra,crop=…"
     *        -c:v libx264 -preset ultrafast -t 20 out.mp4 # 屏幕，实际能到 148fps
     * 逐帧量画面用：ffmpeg -i out.mp4 -vf "crop=…,signalstats,metadata=print:file=-"
     * 他连点 23 次，矩形日志里**每一次都只有一条，而且已经是最终位置**
     * （09:36:11.120 @2401,1041 → 09:36:11.869 @2059,1041 一步到位）
     * —— 说明 Qt 摆位没晚，"位置在开之前一次定死"那条规矩是守住了的。
     *
     * 但同一时刻的录屏（148fps 那段，帧 1180 / 1181）里：
     *
     *   帧 1180  菜单画在 (1855,1040)   ← 上一回开菜单的位置，YAVG 48.3066
     *   帧 1181  菜单画在 (2405, 985)   ← 这一次该在的地方，YAVG 47.1918
     *
     * 48.3066 和上一回稳定期的 48.3079 只差 0.0013 —— 那是**上一帧画面被原样重放**，
     * 不是新内容画错了地方。窗口重新露出来那一拍，DWM 先把它缓存的最后一帧
     * （旧内容、旧位置）合成出去，下一拍才轮到 Qt 画的新内容。一帧在 144Hz 上是
     * 6.9ms：24fps 的录屏看不见，眼睛看得见。
     *
     * DWM 那一拍管不了，能管的是**它缓存里存的是什么**：让这块窗口在被藏掉之前
     * 先画一帧全透的，于是下次露出来时被重放的那一帧是透明的 —— 屏幕上什么都没有，
     * 菜单直接出现在它该在的地方。
     *
     * 为什么用 exit 过渡而不是"藏掉再开一次透明的"：后者重新 show 的那一拍，
     * 被重放的正好是**旧菜单那一帧**，等于把闪烁从"开"挪到了"关"。过渡期窗口
     * 一直是露着的，只是画成透明，没有第二次 show。
     *
     * 淡出 8ms + 按住 24ms（一共 32ms）：够 DWM 至少合成一帧全透的，而"菜单正在
     * 消失"这件事本身没人会去盯。enter 那一段只负责把 opacity 拨回 1，时长 0 ——
     * 开菜单不许有任何淡入，那会真的慢一帧。
     *
     * 代价：这 32ms 里那块弹窗窗口还算"可见"（只是全透）。自检里数顶层窗口的
     * 那条会撞上它（"问句是一块小卡片"那条，实测三次一次红），所以那边数之前
     * 先泵一会儿事件等它落定（见 src/SelfTest.cpp 同一处）。
     */
    enter: Transition {
        PropertyAnimation { property: "opacity"; to: 1.0; duration: 0 }
    }
    exit: Transition {
        SequentialAnimation {
            PropertyAnimation { property: "opacity"; to: 0.0; duration: 8 }
            /*
             * 淡到 0 之后再按住 24ms。只淡不按住，实测"还是有点闪，但频率少了
             * 很多"——说明偶尔那一帧全透的还没被合成出去，窗口就已经藏了，
             * DWM 缓存里存的仍然是菜单。按住一段就是逼它至少出一帧空的。
             */
            PauseAnimation { duration: 24 }
        }
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
        var roomBelow = host.height - 4 - below.y
        openCap = Math.max(168, Math.min(maxMenuHeight, roomBelow))
        var worstH = reservedHeight(items, roomBelow)

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
        var roomBelow = host.height - 4 - p.y
        openCap = Math.max(168, Math.min(maxMenuHeight, roomBelow))
        var worstH = reservedHeight(items, roomBelow)

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
         *
         * 但这里**不能读 implicitHeight / implicitWidth**：这一句和上面
         * `subEntries = items` 在同一个 JS 回合里，那两个数还是"只有主栏"时的旧值。
         * 2026-09-22 就踩在这儿 —— 兜底按旧高度把 y 从 31 顶到 441，而子栏的当场
         * 上限（submenuCap）是按 y 算的，被这个错 y 一压直接缩成 0：屏幕上右边那块
         * 根本出不来。所以按"这一句之后会落到多少"现算一遍再夹。
         */
        var willSubH = Math.min(subEntriesHeight, Math.max(0, Math.min(maxMenuHeight,
                            host ? host.height - 4 - (root.y + submenuTop) : maxMenuHeight)))
        var willH = Math.max(menuHeight, submenuTop + willSubH)
        var willW = submenuInset + paneWidth
        if (host && root.y + willH > host.height - 4) {
            const ny = Math.max(2, host.height - willH - 4)
            if (root.opened && ny !== root.y)
                ++openShifts
            root.y = ny
        }
        if (host && root.x + willW > host.width - 4) {
            const nx = Math.max(2, host.width - willW - 4)
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
                        id: entriesRepeater
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
                    commandTimer.act = entry.modelData.act
                    commandTimer.start()
                }
            }
        }
    }
}
