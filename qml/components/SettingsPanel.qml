pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../utils"
import SmartClip.Globals 1.0

/*
 * 设置 / 帮助面板（左侧操作步骤，右侧具体内容）。
 *
 * ===========================================================================
 * 为什么它是一块自己的原生窗口，而不是场景里的浮层
 * ===========================================================================
 * 和 DropdownMenu 同一个原因：编辑区是**原生 QScintilla 子窗口**
 * （QWidget::createWindowContainer，见 src/EditorViewItem.h），
 * 原生子窗口永远画在 QQuickWidget 内容之上，场景内的浮层压不住它。
 *
 * 但它以前是 `Popup { popupType: Popup.Window }`，那个壳有个副作用：Qt 给
 * Popup.Window 建的是 **Qt::Popup** 窗（实测 flags 0x40000809），Windows 上
 * 这种窗一点到外面 —— 包括点到桌面、点别的应用 —— 系统就把它关掉，
 * 用户那边就是"设置面板碰一下就没了"。而 closePolicy 去掉 CloseOnPressOutside
 * 改不了窗口类型，建好之后再 setFlags 更糟（销毁重建 HWND，实测会把面板压到
 * 最大化主窗口后面去，那轮已撤）。
 *
 * 现在它是一块**从一开始就是** Qt::Tool 的顶层 Window：不进任务栏、不置顶
 * （系统文件对话框照样盖得住它）、点外面不会再关掉它。transientParent 由
 * WindowHelper::attachAsToolWindow 接（宿主是 QWidget，QML 拿不到那块 HWND），
 * 所以它照样浮在主窗口上面。
 *
 * 面板跟着宿主窗口移动/缩放：openSection() 里按宿主几何摆位置，
 * 之后每次打开都重新摆一次（用户拖走的位置不持久化 —— 下次还是居中）。
 *
 * ===========================================================================
 * 快捷键为什么在这里改，而不是改 js/EditorMenus.js
 * ===========================================================================
 * 菜单里的快捷键只是**显示**，真正按键生效的是 C++ 侧挂在宿主 QWidget 上的
 * QAction（见 src/EditorController.h 开头的说明：焦点在原生编辑区里，
 * QML 的 Shortcut 收不到）。所以改键必须改 QAction，改完落 QSettings，
 * 顺手 emit shortcutsChanged() —— 菜单标签再通过 ShortcutOverrides 重算。
 *
 * 采样键盘用 TextInput 的 Keys.onPressed，不是全局 KeyboardHandler：
 * 这个面板是活动原生窗口时宿主窗口不是活动的，那一串 QAction 不会触发，
 * 所以能放心把 Ctrl+N 这类组合原样抓下来。
 */

Window {
    id: root

    /* 外边（Main.qml）传进来的"可改键清单"，每项见 EditorController.h */
    property var entries: []

    /* 编辑器状态（Main.qml 的 window.view），关于栏要读当前字号 */
    property var view: null

    /* 面板里的"字号 + / 自动换行"这类按钮，转回 Main.qml 的命令分发 */
    signal commandRequested(string act)

    /* 每次 Cmd.shortcutsChanged 自增，用来强制 delegate 重读条目数据 */
    property int revision: 0

    /* 当前正在采键的命令名（空串 = 没在采） */
    property string capturing: ""

    /* 改键失败的原因（撞键 / 不认识的键），显示在右栏底部 */
    property string hint: ""

    /* 显示的栏目：shortcuts / storage / translate / about */
    property string section: "shortcuts"

    /*
     * 「另存为…」要一个带输入框的弹框。那张卡片（AskCard）挂在 Main.qml 上，
     * 组件自己叫不出来 —— 所以由 Main 把函数递进来：askText(标题, 提示, 初值, 回调)。
     * 没传就不显示这个按钮能做的事（点了没反应是死控件，见下面 onClicked 里那句 return）。
     */
    property var askText: null
    /* 「另存为…」失败的原因（名字撞了内置档、名字里有非法字符等） */
    property string schemeSaveErr: ""

    /*
     * 自检用：配色方案这一栏到底建出来没有。
     *
     * Repeater 长出来的 delegate 从外面 findChild 找不到（这个工程踩过两次，
     * 见 TerminalPanel 的 firstView / StickyNotes 那几处），所以只能由组件自己
     * 报数 —— 否则"整栏没渲染"和"渲染了但只有一条"在自检里长得一模一样。
     */
    readonly property int schemeRowCount: schemeListRepeater.count
    readonly property bool schemeNavShown: navItems.some(function (n) { return n.key === "scheme" })

    /*
     * 配色方案的 font 段钉住了哪几项（键名清单）。
     *
     * 这里读的是 Theme.fontOverride **属性**，所以换方案时这条会重算；下面那排按钮
     * 绑的是 lockedByScheme()，它读的就是这个清单 —— 链条接得上。直接绑
     * Theme.fontOverridden("size") 不行：函数调用里的读取 QML 追不到依赖，
     * 换了方案按钮不会醒（同一个坑这工程踩过几回了）。
     */
    readonly property var schemeFontKeys: Object.keys(Theme.fontOverride)
    function lockedByScheme(key) { return schemeFontKeys.indexOf(key) >= 0 }
    /* 下拉的条目（DropdownMenu 要的那份清单）。prefix 是命令前缀：font: / termFont: */
    function familyEntries(current, prefix) {
        var out = []
        for (var i = 0; i < fontFamilyChoices.length; ++i) {
            var f = fontFamilyChoices[i]
            out.push({ label: f.label, act: prefix + f.family, checked: f.family === current })
        }
        return out
    }

    /*
     * 字体家族的候选。和 js/EditorMenus.js 的 kFontFamilies 是同一份东西，
     * 那边是顶上「设置」菜单用的 —— 名字必须用**英文家族名**（Scintilla 走 toLatin1，
     * 中文名会压成问号，字体就静默失效）。改这一列记得改那一列。
     */
    readonly property var fontFamilyChoices: [
        { label: "Consolas", family: "Consolas" },
        { label: "Cascadia Mono", family: "Cascadia Mono" },
        { label: "新宋体", family: "NSimSun" },
        { label: "更纱黑体", family: "Sarasa Mono SC" },
        { label: "Courier New", family: "Courier New" }
    ]

    /* 终端字体的基线（由 Main.qml 从终端面板那两个属性转过来） */
    property string termFamilyNow: "Cascadia Mono"
    property int termSizeNow: 13
    /*
     * 终端那两行的**生效值**：方案钉住就是方案那个值，否则是用户在设置里选的基线。
     * 编辑区那五行不用这么算 —— Main.qml 已经把方案的值推到 view 上了。
     */
    readonly property string termFamilyEff: lockedByScheme("terminalFamily")
                                            ? String(Theme.fontOverride.terminalFamily)
                                            : termFamilyNow
    readonly property int termSizeEff: lockedByScheme("terminalSize")
                                       ? Number(Theme.fontOverride.terminalSize) : termSizeNow

    /* 自检探针：侧栏里到底有没有「字体」这一栏 */
    readonly property bool fontNavShown: navItems.some(function (n) { return n.key === "font" })
    /* 勾选框里那个勾、下拉右边那个箭头，都从图标那套来（不引新依赖） */
    IconProvider { id: fontIcons }
    /*
     * 「字体」那一栏的下拉只开这一个（工程里那条"一次只开一个原生弹窗"的规矩）。
     * parent 挂内容层：它是这个顶层 Window 自己的场景，不是主窗口的 —— 主窗口那个
     * ddMenu 锚点算的是主窗口的坐标，拿过来会跑到隔壁去。
     */
    DropdownMenu {
        id: fontMenu
        parent: content
        onSelected: (act) => root.commandRequested(act)
    }
    /*
     * 字体那一栏建出来没有、七行齐不齐、哪几行正被方案钉着。
     * 探针必须挂在**面板根**上（和 schemeRowCount 同一本账）：挂在里面那个 Column 上，
     * 自检从外面 property 读不到，量出来是"0 行"（第一次就红在这儿）。
     * lockedFlags 的七位顺序 = 那七行的顺序：family / size / commentSize / lineHeight /
     * wrap / terminalFamily / terminalSize。
     */
    readonly property int fontRowCount: fontRows.children.length + termRows.children.length
    /*
     * 每一行"右边控件的起始 x"（自检卡对齐用）。没有控件的行（自动换行）跳过，
     * 剩下六行必须一模一样。
     */
    function fontRowBodyLefts() {
        var out = []
        var all = fontRows.children.concat(termRows.children)
        for (var i = 0; i < all.length; ++i)
            /* rowBody 是 flRow.data 的别名，里面**永远**有勾选框槽位和标题那两项：
               所以"这一行有没有控件"要看是不是多于那两项（自动换行那行没控件） */
            if (all[i] && all[i].bodyLeft !== undefined && all[i].rowBody.length > 2)
                out.push(all[i].bodyLeft)
        return out
    }
    readonly property string fontLockedFlags:
        (lockedByScheme("family") ? "1" : "0")
        + (lockedByScheme("size") ? "1" : "0")
        + (lockedByScheme("commentSize") ? "1" : "0")
        + (lockedByScheme("lineHeight") ? "1" : "0")
        + (lockedByScheme("wrap") ? "1" : "0")
        + (lockedByScheme("terminalFamily") ? "1" : "0")
        + (lockedByScheme("terminalSize") ? "1" : "0")

    /*
     * 换栏目 = 回到顶部。
     *
     * 右栏从"直接铺满"改成了 Flickable（见下面 content 的说明）：上一栏滚到一半
     * 的位置会留在 contentY 上，点进下一栏第一眼就是半截内容。
     */
    onSectionChanged: content.contentY = 0

    /* ---- 汇总 / 归档那两栏的状态（见下面的 summarizeColumn / archiveColumn） ---- */

    /*
     * 待审草稿和归档清单。
     *
     * 为什么不直接在 model 里写 Store.drafts()：那两个是**函数**不是属性，
     * QML 没有任何东西可以绑上去 —— 采纳 / 丢弃之后界面不会自己动。
     * 所以自己存两份，Store.changed 一来就重取（Store 在每次落盘之后都会发它）。
     */
    property var draftRows: []
    property var archiveRows: []
    /* 汇总区间（yyyy-MM-dd，和剪贴板日期目录同一套写法） */
    property string sumFrom: ""
    property string sumTo: ""
    /* 采纳 / 归档那几下点完的一句话结果（"收进归档 3 份"），换栏目就清掉 */
    property string sumHint: ""

    /*
     * 两份清单只在面板开着的时候才重取。
     *
     * Store.changed 是**每次采集剪贴板**都会发的，面板九成时间收着 —— 收着的
     * 时候去列目录、查库，纯给主线程添活（而且这面板是 Popup.Window，主线程
     * 一卡就连"打开的那一帧"都受影响）。
     */
    function reloadSummarize() {
        if (!root.opened)
            return
        root.draftRows = Store.drafts()
        root.archiveRows = Store.archivedFiles()
    }

    /* 相对今天偏移 n 天的日期串（-6 = 最近 7 天里最早那天） */
    function dayText(offsetDays) {
        var d = new Date()
        d.setDate(d.getDate() + offsetDays)
        return Qt.formatDate(d, "yyyy-MM-dd")
    }

    /*
     * 密钥的打码样子：头 4 位 + 尾 2 位 + 总长。
     *
     * 为什么留头几位：`sk-` 开头那截本来就不是秘密，留着才认得出"这是 DeepSeek
     * 那把还是另一把"；为什么不留中段：肩后瞟一眼要抄的就是中段。
     * 短于 8 位的（本机 Ollama 那种随手填的）干脆全遮 —— 那么短，露两头等于露全部。
     */
    function maskedKey(value) {
        var s = (value === undefined || value === null) ? "" : String(value).trim()
        if (s === "")
            return ""
        if (s.length <= 8)
            return "•".repeat(s.length) + "（" + s.length + " 位）"
        return s.substring(0, 4) + "•".repeat(Math.min(12, s.length - 6))
               + "…（" + s.length + " 位，尾 " + s.substring(s.length - 2) + "）"
    }

    function setRange(days) {
        root.sumTo = root.dayText(0)
        root.sumFrom = root.dayText(-(days - 1))
        root.sumHint = ""
    }

    /* 进面板时先备好"今天"这个区间，不然第一次点开始是空串（见上面那处 onCompleted） */
    Connections {
        target: Store
        function onChanged() { root.reloadSummarize() }
    }

    Connections {
        target: Sum
        /* 一轮跑完（中途每写出一份草稿也会经由 Store.changed 刷新，这条是兜底） */
        function onRunFinished(count) { root.reloadSummarize() }
    }

    readonly property color bgColor:      Theme.c("#2b2d30", Theme.rev)
    readonly property color sidebarColor: Theme.c("#26282b", Theme.rev)
    readonly property color headerColor:  Theme.c("#33363a", Theme.rev)
    readonly property color borderColor:  Theme.c("#4b4d4f", Theme.rev)
    /*
     * 面板最外圈那一道边框。
     *
     * borderColor（#4b4d4f）是内部那几条分隔线的颜色，直接拿来当外框太暗：
     * 面板是浮在深色桌面上的独立窗口，1px 的分隔线色在截图里几乎看不见，
     * 整个框像是没有边界。所以外框单独提一档亮、加粗到 2px，
     * 宽度统一收在 frameWidth 上（背景圆角和标题栏圆角都跟着它走）。
     */
    readonly property color frameColor:   Theme.c("#5c6066", Theme.rev)
    readonly property int frameWidth:     2
    readonly property color rowHover:     Theme.c("#34373b", Theme.rev)
    readonly property color rowSel:       Theme.c("#2f3a44", Theme.rev)
    readonly property color textColor:    Theme.c("#c8ccd1", Theme.rev)
    readonly property color textBright:   Theme.c("#e8e8e8", Theme.rev)
    readonly property color mutedColor:   Theme.c("#8a9098", Theme.rev)
    readonly property color accentColor:  "#4c96d8"
    readonly property color warnColor:    Theme.c("#c8503c", Theme.rev)

    readonly property int rowHeight: 26

    /*
     * 右栏快捷键表里"快捷键"这一列的位置。
     *
     * keyColumnX 是这一列的左边界：表头文字和行里那个组合键文字都从这条线
     * 开始，所以两边只读这一个值，不会再错开（用户报过中间这列比表头右移
     * 6px —— 那 6px 是格子给"采键高亮框"留的左内边距，格子本身得往左让出来，
     * 文字才落在线上）。
     */
    readonly property real keyColumnX: 250
    readonly property real keyCellInset: 6

    /*
     * 格式化工具表（"格式化"那一栏里一行一个语言）。
     *
     * ===========================================================================
     * 为什么是 ListModel，而不是一个 var 属性装 Fmt.toolList() 那份数组
     * ===========================================================================
     * 原来存成 `property var fmtTools`，每次重取就是**换一整个数组**：Repeater
     * 会把 15 行全部销毁重建。而"重取"以前发生在输入框失焦那一刻 —— 重建正好
     * 落在"点进另一个输入框"的那一瞬，点中的那个框跟着被销毁、焦点落空，看起来
     * 就是点一下卡一下、有时还得再点一次。
     *
     * ListModel 能**按行改**（setProperty），改一行只重算那一行的绑定，不销毁
     * 任何 delegate。所以现在：真改了某个命令 → 原地更新（见 refreshFormatTools），
     * 谁也不重建；只有行数对不上（理论上不会发生）才整表重来。
     *
     * 角色名就是 C++ 那边 QVariantMap 的键（id / label / tool / defaultCommand /
     * command / available，见 Formatter::toolList），delegate 里用 required
     * property 接住 —— 拼错一个字母是编译期错误，不是运行时静默 undefined。
     */
    ListModel { id: fmtToolModel }

    Component.onCompleted: {
        refreshFormatTools()
        /*
         * 汇总区间先给"今天"：不然第一次点「开始汇总」传的是空串，
         * 只能回一句"区间不对"。（清单不在这里取 —— openSection() 打开时取。）
         */
        if (root.sumFrom === "" || root.sumTo === "") {
            root.sumFrom = root.dayText(0)
            root.sumTo = root.dayText(0)
        }
    }

    /*
     * Esc 关面板。
     *
     * Popup 时代这条是 closePolicy: CloseOnEscape 白送的，换成顶层 Window 就得
     * 自己给 —— 放在面板这一侧（不是 Main.qml）是因为它现在是独立的一块活动窗，
     * 按键先到它这儿。
     *
     * 用 sequences 而不是 sequence：Qt 6 里 Shortcut 的 sequence 是"多绑定"的
     * 那个属性（QML 会警告 "Only binding to one of multiple key bindings"，
     * 而且官方说以后会去掉），sequences 才是现在那一条。
     */
    Shortcut {
        sequences: ["Escape"]
        enabled: root.visible
        onActivated: root.hide()
    }

    Connections {
        target: Fmt
        function onToolsChanged() { root.refreshFormatTools() }
    }

    /* 栏目表：左边"操作步骤"那一列 */
    readonly property var navItems: [        { key: "shortcuts", label: "快捷键", icon: "gear" },
        { key: "storage",   label: "存储",   icon: "folder" },
        /*
         * 配色方案：选一套、另存为一份、直接改文件。
         * 单开一栏是因为这套东西的**主用法在文件里**（改 json 立刻生效），
         * 藏在别的栏目里没人会去找；卡片顶上那句路径就是入口。
         */
        { key: "scheme",    label: "配色方案", icon: "palette" },
        /*
         * 字体单开一栏，不塞进「配色方案」：这几项的日常改法是"我今天想大一号"，
         * 和换配色不是同一个动作。哪一项被方案钉住了，在这一栏里看得见（灰 + 角标），
         * 改法在那一栏的说明里写着。
         */
        { key: "font",      label: "字体",     icon: "font" },
        /*
         * 这一栏不叫「翻译」而叫「模型」：它配的是**一个** LLM，翻译卡片和
         * 编辑区校验都用它（见这一栏开头那段、还有「校验」里那句"见左边…"）。
         * 叫「翻译」的话，用户在校验那一栏看到"去配模型"就得猜是哪儿。
         * key 还是 translate（C++ / 自检那边按 key 找栏目，改 key 是另一件事）。
         */
        { key: "translate", label: "模型",   icon: "translate" },
        /* 校验（中文用词 / 代码语法）—— 它有独立开关，所以单开一栏 */
        { key: "check",     label: "校验",   icon: "spellcheck" },
        /* 格式化：认本机装了哪些格式化工具、按语言指定命令 */
        { key: "format",    label: "格式化", icon: "format" },
        { key: "document",  label: "识别",   icon: "ocr" },
        /* 汇总：一段时间的复制内容 -> 分类文档（人工审核后才落定） */
        { key: "summarize", label: "汇总",   icon: "markdown" },
        /* 归档：已经汇总过、从树上收起来的原文（只有这里找得到） */
        { key: "archive",   label: "归档",   icon: "archive" },
        { key: "about",     label: "关于",   icon: "info" }
    ]

    /*
     * 文档识别用哪个引擎 —— 从**那条命令**上认，不另存一个"引擎名"。
     *
     * 为什么不单独存一个字段：命令本身就是唯一的真相（用户可以直接编辑那一行
     * 改成任何东西）。存两份的话，改了命令而没改字段，界面上的高亮就骗人了。
     */
    function docEngineKey() {
        var cmd = Doc.runner
        if (cmd.indexOf("doc_runner_paddleocr_vl") >= 0)
            return "paddle"
        if (cmd.indexOf("doc_runner_granite") >= 0)
            return "granite"
        if (cmd.indexOf("doc_runner_rapid") >= 0)
            return "rapid"
        return ""
    }

    /* 三个按钮各自对应的那条命令（脚本路径和默认那条同一个目录） */
    function runnerForEngine(key) {
        var script = key === "paddle" ? "doc_runner_paddleocr_vl.py"
                   : (key === "granite" ? "doc_runner_granite.py"
                                        : "doc_runner_rapid.py")
        return Doc.commandForScript(script)
    }

    /*
     * 把界面那个命令框刷成 C++ 那边当前的值。
     *
     * 换引擎 / 选完解释器之后要调一次：那个框里的 text 是用户点进来时抄的一份，
     * C++ 改了它不会自己跟着变（TextField 的 text 不是绑定 —— 绑定会被用户输入
     * 打断，也会把正在敲的字弹回去）。
     */
    function refreshDocRunner() {
        docRunnerField.text = Doc.runner
    }

    /*
     * 格式化那一栏：把工具表跟 C++ 侧对齐（每行的命令 + "装没装"）。
     *
     * 行数一样就**按行改**（setProperty）：只重算被改那一行的绑定，不销毁任何
     * delegate。这一步会走到"用户刚改完某个命令"（C++ 的 toolsChanged，值没变
     * 那边不发信号）—— 那一刻用户很可能正把鼠标点向下一个输入框，整表重建会
     * 把点中的那个框一起换掉，焦点落空（见上面 ListModel 那段说明）。
     *
     * 行数变了才整表重来（clear + append）：这一条只是兜底，kTools 是常量表，
     * 正常跑不到；真跑到的时候多半是有人在改语言清单，那时候重建才是对的。
     *
     * tool 那一列不用刷：工具名来自 kTools，不会变；变的只有用户填的命令和
     * "本机装没装"这个判断结果。
     */
    function refreshFormatTools() {
        var list = Fmt.toolList()

        if (fmtToolModel.count === list.length) {
            for (var i = 0; i < list.length; ++i) {
                fmtToolModel.setProperty(i, "command", list[i].command)
                fmtToolModel.setProperty(i, "available", list[i].available)
            }
            return
        }

        fmtToolModel.clear()
        for (var j = 0; j < list.length; ++j)
            fmtToolModel.append(list[j])
    }

    /*
     * 右栏当前这一栏**内容自然高度**（不含上下各 14px 的留白）。
     *
     * 六栏的容器 Column 都只锚上 / 左 / 右、不定高，所以 implicitHeight 就是
     * 内容真实需要多少像素 —— Flickable 的 contentHeight 拿它加两边的留白。
     * 快捷键那一栏返回 0：它自己是个 ListView（高度绑在父级上，见 keyList），
     * 再套一层滚动会变成两个滚动条抢同一段滚动距离。
     */
    function sectionContentHeight() {
        switch (section) {
        case "storage":   return storageColumn.implicitHeight
        case "translate": return translateColumn.implicitHeight
        case "document":  return documentColumn.implicitHeight
        case "check":     return checkColumn.implicitHeight
        case "format":    return formatColumn.implicitHeight
        case "summarize": return summarizeColumn.implicitHeight
        case "archive":   return archiveColumn.implicitHeight
        /*
         * 配色方案和字体这两栏是后加的，当时漏了这里：switch 落空就 return 0，
         * 于是 Flickable 的 contentHeight 只剩两边留白 —— **内容超出视口的那一截
         * 根本滚不到**（自检量的是属性，读得到，所以界面缺一段它不知道）。
         */
        case "scheme":    return schemeColumn.implicitHeight
        case "font":      return fontColumn.implicitHeight
        case "about":     return aboutSectionColumn.implicitHeight
        }
        return 0
    }

    /*
     * 语言下拉的条目（[ { label, act, checked } ]，见 DropdownMenu 的说明）。
     * 语言清单在 C++ 侧那一份（Llm.languages / Llm.targetLanguages），界面不另抄。
     */
    function languageEntries(list, current) {
        var out = []
        for (var i = 0; i < (list ? list.length : 0); ++i)
            out.push({ label: list[i], act: list[i], checked: list[i] === current })
        return out
    }

    /*
     * 把"选中的本地推理程序 / 模型文件"填进对应那个框。
     *
     * 对话框由 Main.qml 开，不由这个面板自己调 —— 面板是置顶的原生窗口，
     * Windows 的文件选择框会被它压住。那边先让面板让开再开框，选完从这里
     * 回来填（同一条路见 Main.qml 的 withSettingsPanelAway / chooseStorageRoot）。
     */
    function setLocalPath(kind, path) {
        if (kind === "exe")
            exeField.text = path
        else if (kind === "mmproj")
            mmprojField.text = path
        else
            modelField.text = path
    }

    function titleFor(key) {
        for (var i = 0; i < navItems.length; ++i)
            if (navItems[i].key === key)
                return navItems[i].label
        return key
    }

    /*
     * 打开面板并定位到某一栏。
     *
     * 名字从 show() 改成 openSection()：顶层 Window 自己就有一个不带参数的
     * show()，同名函数会跟它打架（到底调的是哪个，读代码的人看不出来）。
     */
    function openSection(sectionKey) {
        if (sectionKey)
            section = sectionKey
        capturing = ""
        hint = ""
        /* 上一次的"收进归档 3 份"不该跟着面板一直开着还在 */
        sumHint = ""
        /*
         * 挂成宿主的工具窗，**得赶在第一次 visible 之前**，而且要每次打开都调：
         *
         * 原来只在这块组件 Component.onCompleted 里调一次，那一次必定打不上 ——
         * main.cpp 是先 setSource(QML) 再 host.show()，QML 跑完时宿主 QWidget
         * 还没有原生窗口（windowHandle() 为空），transientParent 根本没接上。
         * 后果就是用户报的"点自己程序的界面，设置面板照样收起来"。
         * 这条是幂等的（挂上了直接返回 true），所以放在打开这一步里最稳。
         */
        Win.attachAsToolWindow(root)
        placeOverHost()
        visible = true
        raise()
        requestActivate()
        /* 开完了再取清单：reloadSummarize 只在面板开着的时候干活（见它的说明） */
        reloadSummarize()
    }

    /*
     * 摆到宿主窗口的正中（高度也按宿主钳一次）。
     *
     * 这块窗的 x / y 现在是**屏幕坐标**（Popup 时代是宿主内容区坐标，两者差一个
     * 宿主窗自己的位置）—— 不加那一段，面板会摆到屏幕左上角那一块。
     *
     * 几何**现读**、不存成属性：`Win.hostScreenGeometry()` 是个函数，QML 没有
     * 任何东西可绑，写成 `readonly property` 就只会在创建时算一次（那时宿主还没
     * 建窗口，读到的是空矩形），之后主窗口怎么挪、怎么最大化它都是旧值。
     *
     * 原来那两处 `Math.max(8, …)` 加在**偏移量**上：宿主比面板窄（实测宿主 705、
     * 面板 820）时居中偏移是 -57，被它夹成 +8 → 面板整个贴到宿主左边线上，
     * 中心差 65 像素 —— 就是"没有在屏幕中央"。现在夹的是**绝对坐标**：
     * 能居中就居中，只在会掉出屏幕左 / 上沿时才顶到 8 像素那一条。
     *
     * 别用 `Screen.virtualGeometry` 来夹屏幕边：这块窗是 QQuickWidget 里造出来的
     * 顶层 Window，`Screen` 那个 attached 属性在 show 之前属性全是 undefined
     * （最小样实测：`Screen.virtualGeometry.x` 直接 TypeError，openSection 从那一行
     * 就断了，面板连 visible 都没轮到设 —— 而 QML 的报错**不会**进自检日志）。
     */
    function placeOverHost() {
        var hw = Win.hostScreenGeometry()
        if (hw.width <= 0 || hw.height <= 0)
            return
        root.height = Math.min(560, Math.max(360, hw.height - 60))
        root.x = Math.max(8, hw.x + Math.round((hw.width - root.width) / 2))
        root.y = Math.max(8, hw.y + Math.round((hw.height - root.height) / 2))
    }

    /*
     * 采键结果落库。Cmd.setShortcut 返回空串 = 成功（并在 C++ 侧 emit 了
     * shortcutsChanged()，条目数据下一帧就是新的），非空 = 撞键之类的原因。
     */
    function commitKey(name, keyText) {
        var error = Cmd.setShortcut(name, keyText)
        hint = error ? error : ""
        capturing = ""
    }

    /* Qt.PortableText 这套写法：命令里的修饰位对应字符串里的 Ctrl/Alt/Shift/Meta */
    function keyTextFromEvent(event) {
        var text = ""
        if (event.modifiers & Qt.ControlModifier)
            text += "Ctrl+"
        if (event.modifiers & Qt.AltModifier)
            text += "Alt+"
        if (event.modifiers & Qt.ShiftModifier)
            text += "Shift+"
        if (event.modifiers & Qt.MetaModifier)
            text += "Meta+"

        var key = event.key
        /* 只按了修饰键：继续等真正的那个键 */
        if (key === Qt.Key_Control || key === Qt.Key_Shift
                || key === Qt.Key_Alt || key === Qt.Key_Meta
                || key === Qt.Key_AltGr)
            return ""

        /*
         * 字符键用 event.text 的原样字符：'/'、'='、'-' 这类
         * 在 PortableText 里就是字面量，拿 Key_ slash 之类的枚举名反而对不上。
         * Ctrl+Shift+= 出来的 '+' 也正好是我们要的（和默认键位一致）。
         */
        if (event.text !== "" && event.text.charCodeAt(0) >= 0x20
                && key !== Qt.Key_Escape && key !== Qt.Key_Tab
                && key !== Qt.Key_Backtab && key !== Qt.Key_Backspace
                && key !== Qt.Key_Return && key !== Qt.Key_Enter
                && key !== Qt.Key_Delete && key !== Qt.Key_Insert
                && key < Qt.Key_F1) {
            text += event.text.toUpperCase()
        } else {
            text += eventNativeName(key)
        }
        return text
    }

    /* 非字符键：用 QKeySequence 认得的枚举名（F1…F35 / Del / Ins / Home…） */
    function eventNativeName(key) {
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35)
            return "F" + (key - Qt.Key_F1 + 1)

        switch (key) {
        case Qt.Key_Delete:    return "Del"
        case Qt.Key_Insert:    return "Ins"
        case Qt.Key_Home:      return "Home"
        case Qt.Key_End:       return "End"
        case Qt.Key_PageUp:    return "PgUp"
        case Qt.Key_PageDown:  return "PgDown"
        case Qt.Key_Left:      return "Left"
        case Qt.Key_Right:     return "Right"
        case Qt.Key_Up:        return "Up"
        case Qt.Key_Down:      return "Down"
        case Qt.Key_Tab:       return "Tab"
        case Qt.Key_Backtab:   return "Backtab"
        case Qt.Key_Backspace: return "Backspace"
        case Qt.Key_Return:
        case Qt.Key_Enter:     return "Return"
        case Qt.Key_Space:     return "Space"
        default:               return ""
        }
    }

    width: 820
    /* 真实高度在 openSection() 里按宿主钳（见 placeOverHost）；这里只是没打开时的默认值 */
    height: 560
    /* 圆角靠下面那块背景 Rectangle 画，窗口本身要透明 */
    color: "transparent"
    /*
     * 工具窗：不进任务栏、不置顶（系统文件对话框照样盖得住它）。
     *
     * 以前这块窗是 Popup（popupType: Popup.Window），Qt 给它建的是 **Qt::Popup**
     * —— Windows 上这类窗一点外面（含点到别的应用）系统就自己关掉，就是用户报的
     * "点一下桌面设置就没了"。而 closePolicy 去掉 CloseOnPressOutside 改不了
     * 窗口类型；建好之后再 setFlags 更不行（销毁重建 HWND，实测会把面板压到
     * 最大化主窗口后面 —— 那轮已撤，见 git 历史 / 下面的 attach 注释）。
     * 所以让它**从一开始**就是普通工具窗。
     *
     * transientParent 得由 C++ 接（宿主是 QWidget，QML 这边拿不到那块 HWND）：
     * 见 WindowHelper::attachAsToolWindow，在 openSection() 里每次打开都调一次
     * （组件完成那一刻宿主还没 show，调了也接不上 —— 那边的注释写着为什么）。
     */
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint

    /* Popup 时代的接口，Main.qml 和自检都在读：留着，别再改调用方 */
    readonly property bool opened: visible
    /* 现在根本没有"点外面关"这条路径（不是关掉了策略，是这块窗不是 Popup 了） */
    readonly property bool closesOnOutsidePress: false

    /*
     * 密钥那一栏这会儿是不是"显示全文"。
     *
     * 放在面板根上而不是那一行的 delegate 里：一栏只有一份，而且收起面板必须
     * 自动退回打码 —— 不然他哪天开着全文切去别的应用，回来还摊在那儿。
     */
    property bool showKey: false

    /*
     * 收起来要清的那几样（Popup 时代是 onClosed，这块窗现在只有可见性）。
     */
    onVisibleChanged: {
        if (visible)
            return
        capturing = ""
        hint = ""
        showKey = false
    }

    IconProvider { id: icons }

    /*
     * 面板里所有输入框都用这个，而不是直接用 TextField。
     *
     * 为的是关掉 Qt 自带的那个**浅色**文本右键菜单（Undo / Redo / Cut / Copy /
     * Paste / Delete / Select All）：Fusion 风格的 TextField 自己挂了一个 ——
     * `ContextMenu.menu: TextEditingContextMenu { editor: control }`，白底英文，
     * 和这套深色界面完全不搭；这一屏也没有非右键不可的操作（复制 / 粘贴 / 撤销
     * 用键盘 Ctrl+C / Ctrl+V / Ctrl+Z，输入框里左键拖选照旧）。
     *
     * 为什么不是"盖一层只收右键的 MouseArea"（那样写过一版，**没用**）：
     * 这个菜单是 **QContextMenuEvent** 弹出来的（见 Qt 文档 ContextMenu 那一页：
     * "show a context menu upon a platform-specific event, such as a right click
     * or the context menu key"），它不走"鼠标按下 -> 谁 acceptedButtons 认领"
     * 那条路，盖在最上层也拦不住。官方给的口子就是把这个 Menu 置空。
     *
     * 注意：ContextMenu 是 Qt 6.9 才有的公共附加类型（这个浅色菜单本身也是 6.9
     * 才加的），和上面 acceptedButtons 一样 —— 这套代码现在实际按 Qt >= 6.9 走。
     */
    component PanelField: TextField {
        ContextMenu.menu: null
        /*
         * 再挂一个空的 requested：Qt 文档里那句是"If no menu is set, but this
         * signal is connected, the context menu event will be accepted and will
         * not propagate" —— 连"事件继续往上冒"这条后路一起堵掉。
         */
        ContextMenu.onRequested: (position) => { }
    }

    /*
     * 右栏里那种小按钮（配色方案那一节的"另存为… / 打开文件夹 / 重新加载"）。
     * 界面里没有第二个地方用得上这种"一行里摆两三个动作"的形状，
     * 所以就近声明，不去污染 qml/components 那一层。
     */
    component PanelButton: Rectangle {
        id: btn
        property string label: ""
        property bool accent: false
        /* 点不动但**留着**：配色方案钉住这一项时，整套开关都是这个样子（见 FontStep） */
        property bool greyed: false
        signal clicked
        implicitWidth: btnLabel.implicitWidth + 22
        implicitHeight: 26
        radius: 5
        opacity: greyed ? 0.45 : 1.0
        color: accent ? root.accentColor
                      : (btnHit.containsMouse && !greyed ? root.rowHover : "transparent")
        border.width: 1
        border.color: accent ? root.accentColor : root.borderColor
        Text {
            id: btnLabel
            anchors.centerIn: parent
            text: btn.label
            font.pixelSize: 12
            color: btn.accent ? "#ffffff" : root.textBright
        }
        MouseArea {
            id: btnHit
            anchors.fill: parent
            enabled: !btn.greyed
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: btn.clicked()
        }
    }

    /* ==================================================================
     * 设置 → 字体 那一栏的零件（照 IDEA 那个设置页的排法）
     * ==================================================================
     * 分组标题是一行小字 + 一条通栏细线；每一行是「(勾选框) 标签 · 控件内联」；
     * 没有"关"这个状态的项（字体、字号）就不放勾选框 —— IDEA 自己也是混着排的。
     * 被配色方案的 font 段钉住的行：整行淡一档、控件点不动、行尾挂一个「方案钉住」，
     * 下面再跟一句去哪儿改（同一句话由 Theme.fontOverrideNote 出）。
     */

    /* 一行：标题 + 一条通栏的细线 */
    component FontGroupTitle: Row {
        id: fgt
        property string title: ""
        spacing: 10
        Text {
            id: fgtText
            anchors.verticalCenter: parent.verticalCenter
            text: fgt.title
            color: root.textBright
            font.pixelSize: 12
            font.bold: true
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(0, fontColumn.width - fgtText.implicitWidth - fgt.spacing - 28)
            height: 1
            color: root.borderColor
        }
    }

    /* 自绘勾选框（原生 CheckBox 一律不用）；勾本身用图标那套里的 "check" */
    component FontCheck: Rectangle {
        id: fc
        property bool on: false
        property bool greyed: false
        signal toggled
        implicitWidth: 16
        implicitHeight: 16
        radius: 3
        opacity: greyed ? 0.45 : 1.0
        color: on ? root.accentColor : "transparent"
        border.width: 1
        border.color: on ? root.accentColor : root.borderColor
        Image {
            anchors.centerIn: parent
            sourceSize.width: 13
            sourceSize.height: 13
            visible: fc.on
            source: fontIcons.svg("check", "#ffffff")
        }
        MouseArea {
            anchors.fill: parent
            enabled: !fc.greyed
            cursorShape: Qt.PointingHandCursor
            onClicked: fc.toggled()
        }
    }

    /* 一行的骨架：勾选框（可选）+ 标签 + 这一行的控件（default 子项） */
    component FontLine: Column {
        id: fl
        property string title: ""
        property bool locked: false
        property string note: ""
        property bool checkable: false
        property bool checked: false
        property alias rowOpacity: flRow.opacity
        signal toggle
        default property alias rowBody: flRow.data
        spacing: 3

        /*
         * 这一行"右边控件从哪个 x 开始"。勾选框那一格恒定占宽，所以七行这个值
         * 必须一模一样 —— 自检拿它卡对齐（他圈的就是"字号那两行的框靠左一截"）。
         */
        readonly property real bodyLeft: flSlot.x + flSlot.width + flRow.spacing
                                         + flTitle.width + flRow.spacing

        Row {
            id: flRow
            spacing: 8
            opacity: fl.locked ? 0.45 : 1.0

            /*
             * 勾选框那一格**恒定占宽**：有勾的行和没勾的行，右边的控件才落在同一条
             * 竖线上（他圈的就是这个：字号那两行的框比下面三行靠左一截）。
             */
            Item {
                id: flSlot
                width: 16
                height: 16
                anchors.verticalCenter: parent.verticalCenter
                FontCheck {
                    anchors.fill: parent
                    visible: fl.checkable
                    on: fl.checked
                    greyed: fl.locked
                    onToggled: fl.toggle()
                }
            }
            Text {
                id: flTitle
                width: 124
                anchors.verticalCenter: parent.verticalCenter
                text: fl.title
                color: root.textBright
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }
        Text {
            visible: fl.locked && fl.note.length > 0
            width: parent.width
            wrapMode: Text.WordWrap
            text: fl.note
            color: root.mutedColor
            font.pixelSize: 11
        }
    }

    /*
     * 数字框：输入框 + 后缀单位，回车/失焦才提交（不像 −/+ 那样一点就发命令）。
     *
     * 框的底色/描边用面板里其它输入框**同一份**（#26282b + borderColor + radius 4）：
     * 原来这里自己另画了一层 headerColor 的底，同一栏里就出现两种框色。
     * 灰的那一档靠整行 opacity + readOnly，**不用 enabled:false** —— 后者会让
     * Controls 自己套一层"禁用色"，又变成第三种颜色。
     */
    component FontField: Row {
        id: ff
        property string suffix: ""
        property bool greyed: false
        /* 行高是小数（1.15 倍），其余三项是整数 px —— 一个框两种校验 */
        property bool decimal: false
        /* 模型里那个值（字符串）。只在**没在敲这个框**的时候跟上去，敲的时候归用户 */
        property string shown: ""
        property alias text: ffEdit.text
        signal commit(string v)

        spacing: 5
        opacity: greyed ? 0.45 : 1.0
        /* 别给 Row/Column 写 implicitHeight：那是只读的（位置器自己算），
           写了 qmlcachegen 不拦，**运行期整份 QML 加载失败**，程序直接退（exit 1） */

        /* 两种校验得写成两个对象：`cond ? IntValidator{…} : …` 这种写法 QML 的
           解析器直接把 { 当成代码块，报 "Expected token \`:\`"（编译期就过不去） */
        IntValidator { id: ffIntVal; bottom: 1; top: 999 }
        DoubleValidator { id: ffDblVal; bottom: 1.0; top: 3.0; decimals: 2 }

        PanelField {
            id: ffEdit
            width: 62
            height: 24
            horizontalAlignment: Text.AlignRight
            verticalAlignment: Text.AlignVCenter
            leftPadding: 7
            rightPadding: 7
            color: root.textColor
            selectionColor: root.accentColor
            selectedTextColor: "#ffffff"
            font.pixelSize: 12
            readOnly: ff.greyed
            validator: ff.decimal ? ffDblVal : ffIntVal
            onEditingFinished: ff.commit(text)
            background: Rectangle {
                color: Theme.c("#26282b", Theme.rev)
                border.color: root.borderColor
                border.width: 1
                radius: 4
            }
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: ff.suffix.length > 0
            text: ff.suffix
            color: root.mutedColor
            font.pixelSize: 11
        }
        Binding {
            target: ffEdit
            property: "text"
            value: ff.shown
            when: !ffEdit.activeFocus
        }
    }

    /* 下拉：自绘的框 + 一个 chevron，点开的是面板自己那一个 DropdownMenu */
    component FontSelect: Rectangle {
        id: fsl
        property string shown: ""
        property var entries: []
        property bool greyed: false
        signal openRequested
        implicitWidth: 168
        implicitHeight: 24
        radius: 4
        opacity: greyed ? 0.45 : 1.0
        color: Theme.c("#26282b", Theme.rev)
        border.width: 1
        border.color: root.borderColor

        Row {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 7
            anchors.rightMargin: 6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            Text {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - fslArrow.width - 6
                text: fsl.shown
                color: root.textBright
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            Image {
                id: fslArrow
                anchors.verticalCenter: parent.verticalCenter
                sourceSize.width: 12
                sourceSize.height: 12
                source: fontIcons.svg("chevron-down", root.mutedColor)
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: !fsl.greyed
            cursorShape: Qt.PointingHandCursor
            onClicked: fsl.openRequested()
        }
    }

    /*
     * 背景：窗口本身透明（color: "transparent"），圆角和边框由这块画。
     * Popup 时代它是 background 属性，换成顶层 Window 之后就是第一个子项。
     */
    Rectangle {
        anchors.fill: parent
        color: root.bgColor
        radius: 6
        border.color: root.frameColor
        border.width: root.frameWidth
    }

    /*
     * 内容层（原来是 Popup 的 contentItem）。
     *
     * Popup 会按 contentItem 的 implicitWidth/Height 反推自己的尺寸，所以那时候
     * 要显式给 implicit*；现在宽高由 Window 自己定死，直接 anchors.fill 铺满，
     * 里面那几层照旧读 parent.width/height。
     */
    Item {
        id: panelRoot
        anchors.fill: parent

        Column {
            /*
             * 四边各让出 frameWidth：内容层是"整个窗口那么大"（标题栏默认铺满
             * 整宽、左栏默认铺满整高），不留边的话它会把背景上的边框盖住 ——
             * 顶边和左边就是这样被标题栏和左栏吃掉的，只有右边、下边能看见框线。
             */
            x: root.frameWidth
            y: root.frameWidth
            width: parent.width - root.frameWidth * 2
            height: parent.height - root.frameWidth * 2
            spacing: 0

            /* ---------------- 标题栏（可拖动） ---------------- */
            Rectangle {
                id: header
                width: parent.width
                height: 36
                color: root.headerColor
                topLeftRadius: 4
                topRightRadius: 4
    
                AppIcon {
                    id: headerIcon
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    provider: icons
                    kind: "gear"
                    size: 15
                    tint: Theme.c("#9aa0a8", Theme.rev)
                }
    
                Text {
                    anchors.left: headerIcon.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "SmartClip — 设置"
                    color: root.textBright
                    font.pixelSize: 13
                }
    
                /* 右上角关闭按钮：hover 走同一套红（和窗口那个关闭按钮一致） */
                Rectangle {
                    id: closeCell
                    width: 30; height: 30
                    anchors.right: parent.right
                    anchors.rightMargin: 5
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 5
                    color: closeHit.containsMouse ? Theme.c("#c8503c", Theme.rev) : "transparent"
    
                    AppIcon {
                        anchors.centerIn: parent
                        provider: icons
                        kind: "close"
                        size: 13
                        tint: closeHit.containsMouse ? "#ffffff" : Theme.c("#9aa0a8", Theme.rev)
                    }
    
                    MouseArea {
                        id: closeHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.hide()
                    }
                }
    
                /*
                 * 拖标题栏移动面板：交给窗口管理器，不在 QML 里算增量。
                 *
                 * 原来那版是"按下时记一下 mapToItem(null, ...) 的场景坐标，
                 * 移动时 root.x += 现在 - 按下"。问题是这块窗**就在自己那个场景
                 * 里被搬走**：窗一动，光标的场景坐标就反向变一份，于是每次增量里
                 * 都含着上一次的位移 —— 他报的"点标题栏拖动就乱跑"就是那个。
                 * （主窗口那块早就用 Win.startSystemMove() 了，见 TopBar.qml。）
                 */
                MouseArea {
                    anchors.left: parent.left
                    anchors.right: closeCell.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    cursorShape: Qt.SizeAllCursor
                    property bool started: false

                    onPressed: (mouse) => {
                        /*
                         * 先 accepted 再发起：startSystemMoveFor() 是阻塞的，
                         * 进去之后要等这一次拖动结束才回来。
                         */
                        mouse.accepted = true
                        started = true
                        try {
                            if (!Win.startSystemMoveFor(panelRoot))
                                started = false
                        } catch (e) {
                            console.warn("SettingsPanel: startSystemMoveFor 不可用：", e)
                            started = false
                        }
                    }
                    onReleased: (mouse) => { started = false }
                }
            }
    
            /* ---------------- 主体：左栏 + 右栏 ---------------- */
            Row {
                width: parent.width
                height: parent.height - header.height
                spacing: 0
    
                /* ---- 左栏：操作步骤 ---- */
                Rectangle {
                    id: sidebar
                    width: 210
                    height: parent.height
                    color: root.sidebarColor
                    bottomLeftRadius: 4
    
                    Column {
                        anchors.top: parent.top
                        anchors.topMargin: 10
                        width: parent.width
                        spacing: 1
    
                        Repeater {
                            model: root.navItems
    
                            delegate: Rectangle {
                                id: navRow
                                required property var modelData
    
                                width: sidebar.width - 12
                                x: 6
                                height: 30
                                radius: 5
                                readonly property bool active: root.section === navRow.modelData.key
                                readonly property bool hot: navHit.containsMouse
                                color: navRow.active ? root.rowSel
                                                     : (navRow.hot ? root.rowHover : "transparent")
    
                                AppIcon {
                                    id: navIcon
                                    anchors.left: parent.left
                                    anchors.leftMargin: 9
                                    anchors.verticalCenter: parent.verticalCenter
                                    provider: icons
                                    kind: navRow.modelData.icon
                                    size: 14
                                    tint: navRow.active ? root.accentColor
                                                        : (navRow.hot ? root.textBright : Theme.c("#9aa0a8", Theme.rev))
                                }
    
                                Text {
                                    anchors.left: navIcon.right
                                    anchors.leftMargin: 8
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: navRow.modelData.label
                                    color: navRow.active ? root.textBright
                                                         : (navRow.hot ? root.textBright
                                                                       : root.textColor)
                                    font.pixelSize: 13
                                    font.bold: navRow.active
                                    elide: Text.ElideRight
                                }
    
                                MouseArea {
                                    id: navHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.section = navRow.modelData.key
                                        root.capturing = ""
                                        root.hint = ""
                                    }
                                }
                            }
                        }
                    }
                }
    
                /* 一条竖分隔线，和主界面的卡片缝一个色 */
                Rectangle {
                    width: 1
                    height: parent.height
                    color: root.borderColor
                }
    
                /* ---- 右栏：具体操作 ---- */
                /*
                 * 右栏是个 Flickable。
                 *
                 * 面板高度跟着宿主窗口算（见上面 width / height 那两行），宿主一矮，
                 * **任何一栏**的内容都会比可视区高。原来那几栏是直接铺在 Item 上
                 * （Column { anchors.fill: parent }），高出来的部分被窗口边缘切掉，
                 * 想滚也没得滚 —— 用户截图报的"格式化那一栏底下的语言看不见"就是这个。
                 *
                 * contentHeight 取**当前这一栏**的自然高度（sectionContentHeight），
                 * 不是所有栏里最高的那一栏：否则在短栏目里也能往下滚出一片空白。
                 * 内容比可视区矮时 Math.max 把它压回 height，滚动条自己也就不显示了
                 * （ThinScrollBar 的 visible 是 size < 1.0）。
                 *
                 * clip 必须开着：Flickable 默认不裁剪，滚上去的部分会画到标题栏上。
                 *
                 * acceptedButtons 置空：这一栏只认**滚轮 / 滚动条**，不认"按住左键
                 * 拖动翻页"。翻译 / 识别那几栏里全是 TextField（API Key、模型路径…），
                 * Flickable 默认会把子项的鼠标拖动抢走 —— 想拖选一段文字，结果整页
                 * 往上跑（Qt 自己在 6.9 的说明里也说鼠标拖动翻页"不合预期"，
                 * acceptedButtons 就是为这个加的；置 Qt.NoButton = 关掉拖动）。
                 *
                 * interactive 那条路走不通：把它设 false 会把**滚轮**也一起关掉
                 * （实测过了，滚轮纹丝不动）—— 那样等于回到"不能滚"的老毛病。
                 *
                 * 六栏的容器 Column 因此都只锚上 / 左 / 右、不定高（快捷键那栏除外）：
                 * 定了高就等于又按可视区裁一次，内容照样长不出来。
                 */
                Flickable {
                    id: content
                    width: parent.width - sidebar.width - 1
                    height: parent.height
                    clip: true
                    acceptedButtons: Qt.NoButton
                    contentWidth: width
                    contentHeight: Math.max(height, root.sectionContentHeight() + 28)
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.VerticalFlick
                    ScrollBar.vertical: ThinScrollBar { }
    
                    /* ============ 快捷键 ============ */
                    Column {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8
                        visible: root.section === "shortcuts"
    
                        /* 标题 + 一句用法（用 x 定位，不用 Row —— 见下面"操作列"的注释） */
                        Item {
                            width: parent.width
                            height: 22
    
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: "快捷键"
                                color: root.textBright
                                font.pixelSize: 14
                                font.bold: true
                            }
                            Text {
                                x: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "点右边“修改”后按下新的组合键；Esc 取消"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                        }
    
                        /*
                         * 表头。
                         *
                         * 底下**不画那条横线**（原来这里有一条 borderColor 的
                         * 1px 线）：表头和第一条是同一张表，那条线把标题和内容
                         * 割成两半，看着像多出来的一行。行与行之间的细线照旧留着
                         * （见下面 delegate 里的分隔线），第一条自己那条也关掉了，
                         * 不然表头底下还是会冒一条出来。
                         */
                        Rectangle {
                            width: parent.width
                            height: 24
                            color: "transparent"
    
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: "命令"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: root.keyColumnX
                                anchors.verticalCenter: parent.verticalCenter
                                text: "快捷键"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 430
                                anchors.verticalCenter: parent.verticalCenter
                                text: "操作"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                        }
    
                        /*
                         * 条目表。
                         *
                         * 不用 ListView 的 delegate 缓存来存"正在采键"，
                         * 状态一律放 root.capturing（命令名）—— 滚动时 delegate
                         * 会被复用/销毁，状态放里边会丢。
                         */
                        ListView {
                            id: keyList
                            width: parent.width
                            height: parent.height - 22 - 24 - 8 - 24 - 8
                            clip: true
                            model: Cmd.shortcutItems
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ThinScrollBar { }
    
                            delegate: Item {
                                id: keyRow
                                required property var modelData
                                /* 表头底下不画线，所以第一条自己那条也关掉（见上面表头） */
                                required property int index
                                width: keyList.width
                                height: root.rowHeight
    
                                readonly property string cmdName: modelData.name
                                readonly property bool capturing: root.capturing === keyRow.cmdName
                                readonly property bool hot: keyHit.containsMouse
    
                                Rectangle {
                                    anchors.fill: parent
                                    color: keyRow.hot ? root.rowHover : "transparent"
    
                                    /* 前一条的分隔线 */
                                    Rectangle {
                                        anchors.top: parent.top
                                        width: parent.width
                                        height: 1
                                        color: root.borderColor
                                        opacity: 0.45
                                        visible: keyRow.index > 0
                                    }
    
                                    /* 命令名 */
                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 230
                                        text: keyRow.modelData.label
                                        color: root.textColor
                                        font.pixelSize: 13
                                        elide: Text.ElideRight
                                    }
    
                                    /* 当前组合键（采键时变成提示文字） */
                                    Rectangle {
                                        id: keyCell
                                        anchors.left: parent.left
                                        /*
                                         * 格子往左让出 keyCellInset：文字（下面
                                         * Text 的 leftMargin 就是它）才会正好落在
                                         * keyColumnX 那条线上、和表头对齐。
                                         */
                                        anchors.leftMargin: root.keyColumnX - root.keyCellInset
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 168
                                        height: 20
                                        radius: 4
                                        color: keyRow.capturing ? Theme.c("#1e2023", Theme.rev) : "transparent"
                                        border.width: keyRow.capturing ? 1 : 0
                                        border.color: root.accentColor
    
                                        Text {
                                            anchors.fill: parent
                                            anchors.leftMargin: root.keyCellInset
                                            verticalAlignment: Text.AlignVCenter
                                            text: keyRow.capturing
                                                  ? "按下新的组合键…"
                                                  : (keyRow.modelData.shortcut === ""
                                                     ? "（未设置）"
                                                     : keyRow.modelData.shortcut)
                                            color: keyRow.capturing ? root.accentColor
                                                  : (keyRow.modelData.shortcut === "" ? root.mutedColor
                                                                                      : (keyRow.modelData.conflict
                                                                                         ? root.warnColor
                                                                                         : root.textBright))
                                            font.pixelSize: 12
                                            font.family: "Consolas"
                                            elide: Text.ElideRight
                                        }
    
                                        /* 采键输入：只在正在采的这一行可见/可用 */
                                        TextInput {
                                            id: keyInput
                                            anchors.fill: parent
                                            visible: keyRow.capturing
                                            focus: keyRow.capturing
                                            color: "transparent"
                                            cursorVisible: false
                                            selectByMouse: false
                                            activeFocusOnTab: false
    
                                            Keys.onPressed: (event) => {
                                                event.accepted = true
                                                if (event.key === Qt.Key_Escape) {
                                                    root.capturing = ""
                                                    root.hint = ""
                                                    return
                                                }
                                                if (event.key === Qt.Key_Backspace
                                                        || event.key === Qt.Key_Delete) {
                                                    root.commitKey(keyRow.cmdName, "")
                                                    return
                                                }
                                                var text = root.keyTextFromEvent(event)
                                                if (text === "")
                                                    return
                                                root.commitKey(keyRow.cmdName, text)
                                            }
                                        }
                                    }
    
                                    /*
                                     * 操作列。
                                     *
                                     * 这里不能用 Row：Row 会给子项重算 x，
                                     * 而子项上的 anchors 又是非法组合
                                     * （运行时会刷 "Cannot specify left … inside Row"）。
                                     * 三个动作的落点本来就是固定的，直接给 x。
                                     */
                                    Text {
                                        id: modifyLabel
                                        x: 430
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: keyRow.capturing ? "取消" : "修改"
                                        color: keyHit.containsMouse ? root.accentColor
                                                                    : root.mutedColor
                                        font.pixelSize: 12
    
                                        MouseArea {
                                            id: keyHit
                                            anchors.fill: parent
                                            anchors.margins: -6
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                root.hint = ""
                                                root.capturing = keyRow.capturing
                                                                 ? ""
                                                                 : keyRow.cmdName
                                            }
                                        }
                                    }
    
                                    Text {
                                        x: modifyLabel.x + modifyLabel.implicitWidth + 22
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: keyRow.modelData.custom
                                        text: "恢复默认"
                                        color: resetHit.containsMouse ? root.accentColor
                                                                      : root.mutedColor
                                        font.pixelSize: 12
    
                                        MouseArea {
                                            id: resetHit
                                            anchors.fill: parent
                                            anchors.margins: -6
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                root.capturing = ""
                                                Cmd.resetShortcut(keyRow.cmdName)
                                                root.hint = ""
                                            }
                                        }
                                    }
    
                                    /* 撞键告警：红点 + 悬浮说明（深色提示框，见 AppToolTip） */
                                    AppIcon {
                                        x: modifyLabel.x + modifyLabel.implicitWidth + 106
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: keyRow.modelData.conflict === true
                                        provider: icons
                                        kind: "info"
                                        size: 13
                                        tint: root.warnColor
                                        AppToolTip {
                                            hovered: conflictHit.containsMouse
                                            text: "这个组合键和另一个命令重复，实际不会生效"
                                        }
                                        MouseArea {
                                            id: conflictHit
                                            anchors.fill: parent
                                            anchors.margins: -5
                                            hoverEnabled: true
                                        }
                                    }
                                }
                            }
                        }
    
                        /* 底部：提示 / 全部恢复默认 / 关闭 */
                        Item {
                            width: parent.width
                            height: 24
    
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 260
                                text: root.hint
                                color: root.warnColor
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
    
                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8
    
                                /* 小按钮：恢复全部默认 */
                                Rectangle {
                                    width: resetAllLabel.implicitWidth + 20
                                    height: 22
                                    radius: 4
                                    color: resetAllHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
    
                                    Text {
                                        id: resetAllLabel
                                        anchors.centerIn: parent
                                        text: "全部恢复默认"
                                        color: resetAllHit.containsMouse ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }
    
                                    MouseArea {
                                        id: resetAllHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            root.capturing = ""
                                            Cmd.resetAllShortcuts()
                                            root.hint = ""
                                        }
                                    }
                                }
    
                                Rectangle {
                                    width: 76
                                    height: 22
                                    radius: 4
                                    color: okHit.containsMouse ? Theme.c("#3a3e42", Theme.rev) : Theme.c("#33363a", Theme.rev)
                                    border.width: 1
                                    border.color: okHit.containsMouse ? root.accentColor : root.borderColor
    
                                    Text {
                                        anchors.centerIn: parent
                                        text: "完成"
                                        color: root.textBright
                                        font.pixelSize: 12
                                    }
    
                                    MouseArea {
                                        id: okHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.hide()
                                    }
                                }
                            }
                        }
                    }
    
                    /* ============ 存储 ============ */
                    /*
                     * 剪贴板内容现在是**磁盘上的 md 文件**（日期目录 / 时分秒.md），
                     * 数据库里只剩元数据。这一栏就是把"东西到底存哪儿了"讲清楚，
                     * 顺带给两个入口：换保存位置、把别的文件夹挂上来看。
                     */
                    Column {
                        id: storageColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 10
                        visible: root.section === "storage"

                        Text {
                            text: "剪贴板存储"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: storageInfo.height + 28
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor

                            Column {
                                id: storageInfo
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.right: parent.right
                                anchors.rightMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 7

                                Repeater {
                                    model: [
                                        { k: "保存位置", v: Store.rootPath },
                                        /*
                                         * 内容实际躺在「保存位置/剪贴板」下面 —— 保存位置
                                         * 是用户可以换的那个根（默认「文档/SmartClip」），
                                         * 剪贴板内容是它下面的一个子目录（见
                                         * ClipboardStore::contentRoot）。单独列一行，
                                         * 用户才找得到文件在哪。
                                         */
                                        { k: "内容目录", v: Store.contentRoot },
                                        { k: "文件",     v: Store.fileCount + " 份 md" },
                                        { k: "内容",     v: Store.entryCount + " 条" },
                                        { k: "切分",     v: "每份 md 写满 20K 就另起一份，名字取那一条的时间" },
                                        { k: "图片",     v: "PNG 存在当天的 assets/ 里，md 里用相对路径引用" }
                                    ]

                                    delegate: Row {
                                        required property var modelData
                                        width: parent.width
                                        spacing: 12

                                        Text {
                                            width: 74
                                            text: modelData.k
                                            color: root.mutedColor
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            width: parent.width - 86
                                            text: modelData.v
                                            color: root.textColor
                                            font.pixelSize: 12
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }
                            }
                        }

                        Row {
                            spacing: 10

                            Repeater {
                                model: [
                                    { label: "选择保存位置…", act: "treeChooseRoot" },
                                    { label: "打开保存位置",   act: "treeOpenRoot" },
                                    { label: "刷新列表",       act: "refresh" }
                                ]

                                delegate: Rectangle {
                                    required property var modelData
                                    width: 118
                                    height: 24
                                    radius: 4
                                    color: storageBtnHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: storageBtnHit.containsMouse ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }

                                    MouseArea {
                                        id: storageBtnHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.commandRequested(modelData.act)
                                    }
                                }
                            }
                        }

                        Text {
                            text: "导入的文件夹"
                            color: root.textBright
                            font.pixelSize: 13
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "挂上来看的：里面的 md / txt 会显示在左树上、也能搜到，"
                                  + "但新复制的内容永远只写进上面的保存位置，不会动这些文件夹。"
                        }

                        Rectangle {
                            width: parent.width
                            height: Math.max(30, importedColumn.height + 16)
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor

                            Column {
                                id: importedColumn
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.right: parent.right
                                anchors.rightMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Text {
                                    visible: Store.importedFolders.length === 0
                                    text: "（还没导入任何文件夹）"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }

                                Repeater {
                                    model: Store.importedFolders

                                    delegate: Row {
                                        required property string modelData
                                        width: parent.width
                                        spacing: 10

                                        Text {
                                            width: parent.width - 90
                                            text: modelData
                                            color: root.textColor
                                            font.pixelSize: 12
                                            elide: Text.ElideMiddle
                                        }
                                        Rectangle {
                                            width: 70
                                            height: 22
                                            radius: 4
                                            color: removeHit.containsMouse ? root.rowHover : "transparent"
                                            border.width: 1
                                            border.color: root.borderColor

                                            Text {
                                                anchors.centerIn: parent
                                                text: "移除"
                                                color: removeHit.containsMouse ? root.textBright : root.textColor
                                                font.pixelSize: 11
                                            }
                                            MouseArea {
                                                id: removeHit
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.commandRequested("removeImport:" + modelData)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            width: 118
                            height: 24
                            radius: 4
                            color: importHit.containsMouse ? root.rowHover : "transparent"
                            border.width: 1
                            border.color: root.borderColor

                            Text {
                                anchors.centerIn: parent
                                text: "导入文件夹…"
                                color: importHit.containsMouse ? root.textBright : root.textColor
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: importHit
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.commandRequested("treeImportFolder")
                            }
                        }
                    }

                    /* ============ 模型（LLM 怎么配：翻译卡片和编辑区校验共用） ============ */
                    Column {
                        id: translateColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 9
                        visible: root.section === "translate"

                        Text {
                            text: "模型（翻译 / 校验都用它）"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "翻译卡片（左侧图标条上那个地球图标，或 Ctrl+Alt+T）和编辑区校验"
                                  + "（左边「校验」那一栏）用的是同一个模型 —— 在这里配一次，"
                                  + "两边都跟着变。接口按 OpenAI 兼容格式填："
                                  + "OpenAI / DeepSeek / 通义 / Kimi / Ollama / LM Studio 都是这个格式；"
                                  + "密钥只存在本机设置里。"
                        }

                        /* ---- 两种模式：现成的 API 服务 / 本机自己启动一个 ---- */
                        Row {
                            spacing: 8

                            Repeater {
                                model: [ { k: "api", label: "API 模型" },
                                         { k: "local", label: "本地模型（本程序启动）" } ]

                                delegate: Rectangle {
                                    id: modeCell
                                    required property var modelData

                                    readonly property bool active: Llm.mode === modeCell.modelData.k

                                    width: modeLabel.implicitWidth + 22
                                    height: 26
                                    radius: 4
                                    color: modeHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: modeCell.active ? root.accentColor : root.borderColor

                                    Text {
                                        id: modeLabel
                                        anchors.centerIn: parent
                                        text: modeCell.modelData.label
                                        color: modeCell.active ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }

                                    MouseArea {
                                        id: modeHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: Llm.mode = modeCell.modelData.k
                                    }
                                }
                            }
                        }

                        /* ---- API 模式：地址 / 密钥 / 模型名 ---- */
                        Column {
                            width: parent.width
                            spacing: 6
                            visible: Llm.mode === "api"

                            Repeater {
                                model: [ { k: "apiBase", label: "接口地址",
                                           hint: "https://api.deepseek.com/v1" },
                                         { k: "apiKey",  label: "密钥",
                                           hint: "sk-…（Ollama 这类本地服务可以留空）" },
                                         { k: "model",   label: "模型名",
                                           hint: "deepseek-chat / gpt-4o-mini / qwen-plus …" },
                                         /*
                                          * 截图识别要**能看图**的模型（多模态）。
                                          * 和上面那个翻译模型分开填：翻译用便宜的
                                          * 文本模型、识别用视觉模型，是常见配法；
                                          * 只配了一个视觉模型的话这项留空就行
                                          * （留空 = 用上面那个模型名）。
                                          */
                                         { k: "ocrModel", label: "识别模型",
                                           hint: "留空 = 用上面那个；要能看图的，如 qwen-vl-max / glm-4v / gpt-4o" } ]

                                delegate: Column {
                                    id: apiRow
                                    required property var modelData
                                    readonly property bool isKey: modelData.k === "apiKey"
                                    spacing: 3

                                    Row {
                                        spacing: 8

                                        Text {
                                            width: 62
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: apiRow.modelData.label
                                            color: root.mutedColor
                                            font.pixelSize: 12
                                        }

                                        PanelField {
                                            id: apiField
                                            width: 420
                                            height: 26
                                            text: Llm[apiRow.modelData.k]
                                            placeholderText: apiRow.modelData.hint
                                            /*
                                             * 密钥默认打码。这面板经常一开就是半天，
                                             * 明文摊在那儿等于让身后的人 / 截图 /
                                             * 投屏替他把 key 读走一遍。
                                             */
                                            echoMode: apiRow.isKey && !root.showKey
                                                      ? TextField.Password : TextField.Normal
                                            color: root.textColor
                                            placeholderTextColor: root.mutedColor
                                            font.pixelSize: 12
                                            selectByMouse: true
                                            leftPadding: 7
                                            rightPadding: 7
                                            /* 改完（或按回车）就落盘：Llm 的属性 setter 自己写 QSettings */
                                            onEditingFinished: Llm[apiRow.modelData.k] = text
                                            /*
                                             * 路径 / 地址是从存档填进来的，光标默认落在末尾 ——
                                             * 不聚焦时会显示成"…尾巴那一截"。这里把它拨回开头，
                                             * 看着才是完整的一条（自己敲字时不动它）。
                                             */
                                            onTextChanged: if (!activeFocus) cursorPosition = 0
                                            background: Rectangle {
                                                color: Theme.c("#26282b", Theme.rev)
                                                border.color: root.borderColor
                                                border.width: 1
                                                radius: 4
                                            }
                                        }

                                        /*
                                         * "显示"是给**他自己核对**用的（到底存进去的是哪一把），
                                         * 不是给常态阅读用的：收起面板就自动回到打码
                                         * （见根上那个 showKey 的 onClosed）。
                                         */
                                        Rectangle {
                                            visible: apiRow.isKey
                                            width: keyToggleText.width + 18
                                            height: 26
                                            radius: 4
                                            anchors.verticalCenter: parent.verticalCenter
                                            color: keyToggleHit.containsMouse ? root.rowHover : "transparent"
                                            border.width: 1
                                            border.color: root.borderColor

                                            Text {
                                                id: keyToggleText
                                                anchors.centerIn: parent
                                                text: root.showKey ? "隐藏" : "显示"
                                                color: keyToggleHit.containsMouse ? root.textBright : root.textColor
                                                font.pixelSize: 12
                                            }
                                            MouseArea {
                                                id: keyToggleHit
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.showKey = !root.showKey
                                            }
                                        }
                                    }

                                    /*
                                     * 打码那一行才是"只显示一部分"：头 4 位 + 尾 2 位 +
                                     * 总长。够他认出这是哪一把 key，又不足以让人瞟一眼抄走。
                                     * 点「显示」时这行让位（全文已经在框里了，不重复）。
                                     */
                                    Text {
                                        visible: apiRow.isKey && !root.showKey
                                                 && Llm.apiKey.trim().length > 0
                                        text: "当前：" + root.maskedKey(Llm.apiKey)
                                        color: root.mutedColor
                                        font.pixelSize: 11
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "已经在跑 Ollama / LM Studio 的话，选这种模式、地址填 "
                                      + "http://127.0.0.1:11434/v1（Ollama）或它给的地址就行，不用下面那套。"
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "截图的时候还能认字：框好区域点工具条上的「识别」，"
                                      + "图会交给这里的视觉模型（识别模型，留空就用上面那个模型名），"
                                      + "结果摆在选区旁边那张卡片上，可以复制、也可以直接贴到截图上。"
                                      + "要翻译的话选一下目标语言 —— 认和翻在同一次请求里做完。"
                            }
                        }

                        /* ---- 本地模式：自己启动一个 OpenAI 兼容的推理服务 ---- */
                        Column {
                            width: parent.width
                            spacing: 6
                            visible: Llm.mode === "local"

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "服务程序"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                PanelField {
                                    id: exeField
                                    width: 320
                                    height: 26
                                    text: Llm.localExe
                                    placeholderText: "llama-server.exe（llama.cpp 那个）"
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.localExe = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: Theme.c("#26282b", Theme.rev)
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                                Rectangle {
                                    width: 58; height: 26; radius: 4
                                    anchors.verticalCenter: exeField.verticalCenter
                                    color: exePickHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: "选择…"
                                        color: root.textColor
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: exePickHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        /* 交给 Main.qml 开框（面板要先让开，见 setLocalPath） */
                                        onClicked: root.commandRequested("translateChooseExe")
                                    }
                                }
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "模型文件"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                PanelField {
                                    id: modelField
                                    width: 320
                                    height: 26
                                    text: Llm.localModel
                                    placeholderText: "*.gguf 模型文件"
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.localModel = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: Theme.c("#26282b", Theme.rev)
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                                Rectangle {
                                    width: 58; height: 26; radius: 4
                                    anchors.verticalCenter: modelField.verticalCenter
                                    color: modelPickHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: "选择…"
                                        color: root.textColor
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: modelPickHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        /* 同上：走 Main.qml，面板先让开 */
                                        onClicked: root.commandRequested("translateChooseModel")
                                    }
                                }
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "多模态投影"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                PanelField {
                                    id: mmprojField
                                    width: 320
                                    height: 26
                                    text: Llm.localMmproj
                                    placeholderText: "mmproj-model-f16.gguf（纯文本模型留空）"
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.localMmproj = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: Theme.c("#26282b", Theme.rev)
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                                Rectangle {
                                    width: 58; height: 26; radius: 4
                                    anchors.verticalCenter: mmprojField.verticalCenter
                                    color: mmprojPickHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: "选择…"
                                        color: root.textColor
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: mmprojPickHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        /* 同上：走 Main.qml，面板先让开 */
                                        onClicked: root.commandRequested("translateChooseMmproj")
                                    }
                                }
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "端口"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                PanelField {
                                    id: portField
                                    width: 90
                                    height: 26
                                    text: String(Llm.localPort)
                                    color: root.textColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    validator: IntValidator { bottom: 1; top: 65535 }
                                    onEditingFinished: Llm.localPort = parseInt(text)
                                    background: Rectangle {
                                        color: Theme.c("#26282b", Theme.rev)
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }

                                Rectangle {
                                    width: 118; height: 26; radius: 4
                                    anchors.verticalCenter: portField.verticalCenter
                                    color: startHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: Llm.localRunning ? root.warnColor : root.accentColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: Llm.localRunning ? "停止本地模型" : "启动本地模型"
                                        color: Llm.localRunning ? root.warnColor : root.textBright
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: startHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (Llm.localRunning)
                                                Llm.stopLocal()
                                            else
                                                Llm.startLocal()
                                        }
                                    }
                                }

                                /*
                                 * 提示：本地服务第一次要加载模型，几秒到几十秒都有可能 ——
                                 * 文案跟着 Llm.status 走（"模型加载中…" / "已就绪" / 失败原因）。
                                 */
                                Text {
                                    anchors.verticalCenter: portField.verticalCenter
                                    width: Math.max(60, parent.width - 62 - 90 - 118 - 32)
                                    text: Llm.status
                                    color: root.mutedColor
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }

                            /* 服务程序的输出（启动失败时唯一能看的地方，最多三行） */
                            Text {
                                width: parent.width
                                visible: Llm.mode === "local" && Llm.localLog !== ""
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 3
                                elide: Text.ElideRight
                                color: root.mutedColor
                                font.pixelSize: 10
                                font.family: "Consolas"
                                text: Llm.localLog
                            }

                            /*
                             * 真正会执行的那条命令行（和 C++ 拼出来的是同一份）。
                             * 模型加载不起来时，先把这条复制出去在终端里跑一遍，
                             * 报错信息比这里的日志全。
                             */
                            Text {
                                width: parent.width
                                visible: Llm.localCommand !== ""
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                color: root.mutedColor
                                font.pixelSize: 10
                                font.family: "Consolas"
                                text: Llm.localCommand
                            }

                            /*
                             * 这句是给用户吃定心丸的：本地模型不用手动点启动
                             * （点翻译时会自己拉起来，见 LlmClient::post）。
                             */
                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "配好之后不用每次手动启动：翻译卡片上一点「翻译」，"
                                      + "本地模型会自己起来（加载要几秒到几十秒，卡片上会显示进展）。"
                            }
                        }

                        /* ---- 两种模式共用：测试一下 ---- */
                        Row {
                            spacing: 10

                            Rectangle {
                                width: 88; height: 24; radius: 4
                                color: testHit.containsMouse ? root.rowHover : "transparent"
                                border.width: 1
                                border.color: root.borderColor
                                Text {
                                    anchors.centerIn: parent
                                    text: "测试连接"
                                    color: testHit.containsMouse ? root.textBright : root.textColor
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: testHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Llm.probe()
                                }
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.max(60, parent.width - 100)
                                text: Llm.busy ? "正在测试…" : Llm.status
                                color: (Llm.status.indexOf("正常") >= 0) ? root.accentColor
                                                                        : root.mutedColor
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        /* ---- 图上选字：贴图上用鼠标选字时，用哪个引擎 ---- */
                        Column {
                            width: parent.width
                            spacing: 6

                            Text {
                                text: "图上选字（贴图上按住拖，把图里的字选出来）"
                                color: root.textColor
                                font.pixelSize: 12
                            }

                            Row {
                                spacing: 6

                                Repeater {
                                    model: [ { k: "windows", label: "Windows 自带" },
                                             { k: "ppocr",   label: "PP-OCRv6" } ]

                                    delegate: Rectangle {
                                        id: engCell
                                        required property var modelData
                                        readonly property bool active: Llm.pinOcrEngine === engCell.modelData.k

                                        width: 96
                                        height: 24
                                        radius: 4
                                        color: engCell.active ? Theme.c("#2f3a44", Theme.rev)
                                                              : (engHit.containsMouse ? root.rowHover
                                                                                      : "transparent")
                                        border.width: 1
                                        border.color: engCell.active ? root.accentColor : root.borderColor

                                        Text {
                                            anchors.centerIn: parent
                                            text: engCell.modelData.label
                                            color: engCell.active ? root.accentColor : root.textColor
                                            font.pixelSize: 11
                                        }
                                        MouseArea {
                                            id: engHit
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: Llm.pinOcrEngine = engCell.modelData.k
                                        }
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "Windows 自带：离线、不用配（默认）。PP-OCRv6：跑下面这条本机命令，"
                                      + "框更准，但要先装好。"
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "OCR 程序"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                PanelField {
                                    id: ocrRunnerField
                                    width: 420
                                    height: 26
                                    text: Llm.pinOcrRunner
                                    placeholderText: "python \"…\\ppocr_runner.py\""
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.pinOcrRunner = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: Theme.c("#26282b", Theme.rev)
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "PP-OCR 那条路要装：pip install rapidocr onnxruntime（RapidOCR 3.9 "
                                      + "起内置 PP-OCRv6，模型第一次跑会自动下）。随包那个脚本第一次用到时会"
                                      + "落到 %APPDATA%/SmartClip/SmartClip/ppocr_runner.py，可以自己改。"
                                      + "选档：命令后面再接一个词 —— tiny / small / medium（默认 small，"
                                      + "medium 最准也最慢）；想指到自己下的 ONNX 文件，就把一份 params JSON "
                                      + "的路径接在后面当第三个参数。"
                            }
                        }
                    }

                    /* ============ 识别（文档 / 图片 -> Markdown） ============ */
                    Column {
                        id: documentColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 10
                        visible: root.section === "document"

                        Text {
                            text: "文档识别（PDF / 图片 / Office → Markdown 笔记）"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "把文件拖进主窗口（或者用「文件 → 识别文档…」）就会认一遍，"
                                  + "认出来的 Markdown 存成一份新笔记，文档里的插图一起落到笔记目录的 "
                                  + "assets/ 下。识别跑在后台，右下角那张小卡片显示进展。"
                        }

                        /* ---- 引擎：三个可选项，换的就是下面那条命令 ---- */
                        Text {
                            text: "引擎"
                            color: root.textColor
                            font.pixelSize: 12
                        }

                        Row {
                            spacing: 6

                            Repeater {
                                model: [ { k: "rapid",    label: "RapidDoc（默认）" },
                                         { k: "paddle",   label: "PaddleOCR-VL" },
                                         { k: "granite",  label: "Granite-Docling" } ]

                                delegate: Rectangle {
                                    id: docEngCell
                                    required property var modelData
                                    readonly property bool active: docEngineKey() === docEngCell.modelData.k

                                    width: 138
                                    height: 24
                                    radius: 4
                                    color: docEngCell.active ? Theme.c("#2f3a44", Theme.rev)
                                                             : (docEngHit.containsMouse ? root.rowHover
                                                                                        : "transparent")
                                    border.width: 1
                                    border.color: docEngCell.active ? root.accentColor : root.borderColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: docEngCell.modelData.label
                                        color: docEngCell.active ? root.accentColor : root.textColor
                                        font.pixelSize: 11
                                    }
                                    MouseArea {
                                        id: docEngHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            Doc.runner = runnerForEngine(docEngCell.modelData.k)
                                            refreshDocRunner()
                                        }
                                    }
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "RapidDoc：纯 ONNX，模型随包（约 800MB），CPU 就够，装 "
                                  + "pip install rapid-doc 即可 —— 默认这个。\n"
                                  + "PaddleOCR-VL 1.6：精度最高（版面 / 表格 / 公式 / 109 种语言），"
                                  + "要装 paddlepaddle + paddleocr[doc-parser]，权重约 1.8GB，建议有独显。\n"
                                  + "Granite-Docling 258M：最省资源（权重约 515MB），表格公式不错，"
                                  + "但语言以英文为主 —— 中文材料别选它。"
                        }

                        /* ---- 档位 ---- */
                        Row {
                            spacing: 8

                            Text {
                                width: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "档位"
                                color: root.mutedColor
                                font.pixelSize: 12
                            }

                            Repeater {
                                model: [ { k: "fast",     label: "快" },
                                         { k: "balanced", label: "均衡" },
                                         { k: "best",     label: "最准" } ]

                                delegate: Rectangle {
                                    id: tierCell
                                    required property var modelData
                                    readonly property bool active: Doc.tier === tierCell.modelData.k

                                    width: 60
                                    height: 24
                                    radius: 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: tierCell.active ? Theme.c("#2f3a44", Theme.rev)
                                                           : (tierHit.containsMouse ? root.rowHover
                                                                                    : "transparent")
                                    border.width: 1
                                    border.color: tierCell.active ? root.accentColor : root.borderColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: tierCell.modelData.label
                                        color: tierCell.active ? root.accentColor : root.textColor
                                        font.pixelSize: 11
                                    }
                                    MouseArea {
                                        id: tierHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: Doc.tier = tierCell.modelData.k
                                    }
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "「快」关掉表格和公式识别（一份 PDF 往往快好几倍）；「均衡」是默认；"
                                  + "「最准」一次处理更多页、更激进的排版还原。"
                        }

                        /* ---- 用哪个 Python ---- */
                        Row {
                            spacing: 8

                            Text {
                                width: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Python"
                                color: root.mutedColor
                                font.pixelSize: 12
                            }
                            PanelField {
                                id: docPythonField
                                width: 420
                                height: 26
                                text: Doc.pythonPath
                                placeholderText: "留空 = 用 PATH 里的 python"
                                color: root.textColor
                                placeholderTextColor: root.mutedColor
                                font.pixelSize: 12
                                selectByMouse: true
                                leftPadding: 7
                                rightPadding: 7
                                onEditingFinished: Doc.pythonPath = text
                                onTextChanged: if (!activeFocus) cursorPosition = 0
                                background: Rectangle {
                                    color: Theme.c("#26282b", Theme.rev)
                                    border.color: root.borderColor
                                    border.width: 1
                                    radius: 4
                                }
                            }
                            Rectangle {
                                width: 62
                                height: 26
                                radius: 4
                                anchors.verticalCenter: parent.verticalCenter
                                color: pyHit.containsMouse ? root.rowHover : "transparent"
                                border.width: 1
                                border.color: root.borderColor

                                Text {
                                    anchors.centerIn: parent
                                    text: "选择…"
                                    color: pyHit.containsMouse ? root.textBright : root.textColor
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    id: pyHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    /*
                                     * 走 commandRequested 转 Main.qml 的 dispatch ——
                                     * 文件框必须由主窗口开（这个面板是置顶窗，会盖住它），
                                     * 而 chooseDocPython() 就长在 Main.qml 的窗口根上。
                                     * 原来这里写的是 Cmd.chooseDocPython()：EditorController
                                     * 没这个函数，点一次抛一次 TypeError，按钮什么都不做。
                                     */
                                    onClicked: root.commandRequested("docChoosePython")
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "识别包装在虚拟环境里的话，这里要指到那个 venv 的 python.exe —— "
                                  + "PATH 里的 python 往往是系统那个，会报「No module named 'rapid_doc'」。"
                        }

                        /* ---- 那条命令 ---- */
                        Row {
                            spacing: 8

                            Text {
                                width: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "识别程序"
                                color: root.mutedColor
                                font.pixelSize: 12
                            }
                            PanelField {
                                id: docRunnerField
                                width: 420
                                height: 26
                                text: Doc.runner
                                placeholderText: "python \"…\\doc_runner_rapid.py\""
                                color: root.textColor
                                placeholderTextColor: root.mutedColor
                                font.pixelSize: 12
                                selectByMouse: true
                                leftPadding: 7
                                rightPadding: 7
                                onEditingFinished: Doc.runner = text
                                onTextChanged: if (!activeFocus) cursorPosition = 0
                                background: Rectangle {
                                    color: Theme.c("#26282b", Theme.rev)
                                    border.color: root.borderColor
                                    border.width: 1
                                    radius: 4
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "这条命令就是「用哪个引擎」唯一的接口：前面是程序，后面接到脚本的档位词"
                                  + "由上面那个「档位」自动接上。随包那三个脚本第一次用到时会落到 "
                                  + "%APPDATA%/SmartClip/SmartClip/ 下，可以自己看、自己改。"
                        }

                        /* ---- 识别程序的健康状况 ---- */
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: Doc.runnerProblem() === "" ? root.accentColor : root.mutedColor
                            font.pixelSize: 11
                            text: Doc.runnerProblem() === "" ? "识别程序就绪。"
                                                             : ("用不了：" + Doc.runnerProblem())
                        }
                    }

                    /* ============ 校验（中文用词 / 代码语法） ============ */
                    Column {
                        id: checkColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 9
                        visible: root.section === "check"

                        Text {
                            text: "编辑区校验"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        /*
                         * **没有开关**：这个功能不做自动校验。
                         *
                         * 原来这里有个"开启校验"的勾（默认关，勾上才发给模型）。
                         * 现在入口收成了右键菜单那一条 —— 用户点它就是要校验，
                         * 那一下本身就是"可以发请求"的许可，再挂一道开关只会
                         * 让人点了没反应（以为坏了）。要停就在模型那一栏把接口
                         * 清掉，或者干脆别点。
                         */
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.textColor
                            font.pixelSize: 12
                            text: "用法：编辑区里按右键 → 「校验当前文件」（或者按 "
                                  + Cmd.shortcutFor("checkFile")
                                  + "）。出问题的地方会在正文里画一条波浪线，"
                                  + "鼠标停上去看那一条的详情。"
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "不自动校验：要联网、要花 token 的事不替你做主，点了才发请求。"
                                  + "先跑本地规则（括号配对、中英文标点混用、重复字、的地得、"
                                  + "行尾空白 —— 毫秒出结果、不花钱），再把正文发给下面的模型，"
                                  + "让它查用词和语法（模型报的每一条都会先在正文里核对位置，"
                                  + "对不上的直接丢掉）。正文一改，波浪线自动清掉。"
                        }

                        /*
                         * 「现在用的是哪个模型」—— 这行由 C++ 侧拼（Checker::modelSummary）。
                         *
                         * 原来这里是 QML 里拼的 "模型就绪：" + Llm.model：那是**接口
                         * 模式**的模型名，出厂默认 "deepseek-chat"，用户切到本地模型
                         * 之后照样报它，本地那个起没起来也不说。判据（llmReady）在
                         * Checker 里，所以那句话也放那儿，免得两处各写一套。
                         */
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: Check.llmReady ? root.accentColor : Theme.c("#d7a85b", Theme.rev)
                            font.pixelSize: 11
                            text: Check.modelSummary
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "正文字数超过 1.2 万时只跑本地规则：那种长度下模型数的行号会不准，"
                                  + "而我们宁可少报几条，也不给你一堆点不到的假问题。"
                        }
                    }

                    /* ============ 格式化 ============ */
                    Column {
                        id: formatColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 9
                        visible: root.section === "format"

                        Text {
                            text: "代码格式化"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "编辑器里按右键 → 「格式化代码」（或 "
                                  + Cmd.shortcutFor("formatCode")
                                  + "）。Qt / QScintilla 本身不带格式化器，所以这里认本机装了"
                                  + "哪些工具；没装的可以下面填完整路径，或者用内置那几样"
                                  + "（JSON 重排 / XML 缩进 / 去行尾空白，不需要装任何东西）。"
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "命令里可以用 {file} 占位（会被换成临时文件路径）；"
                                  + "不写占位就自动追加在末尾。留空 = 用默认那一行。"
                        }

                        /* 每个语言一行：状态 + 命令输入框（角色名见上面 ListModel 那段） */
                        Repeater {
                            model: fmtToolModel

                            delegate: Row {
                                id: fmtRow
                                required property int index
                                required property string id
                                required property string label
                                required property string tool
                                required property string defaultCommand
                                required property string command
                                required property bool available

                                spacing: 8

                                Text {
                                    width: 92
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: fmtRow.label
                                    color: root.textColor
                                    font.pixelSize: 12
                                }

                                /* 装没装那个工具：一个小圆点 + 工具名 */
                                Rectangle {
                                    width: 8
                                    height: 8
                                    radius: 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: fmtRow.available ? Theme.c("#7bc47f", Theme.rev) : root.mutedColor
                                }

                                Text {
                                    width: 110
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: fmtRow.tool
                                          + (fmtRow.available ? "" : "（没找到）")
                                    color: fmtRow.available ? root.mutedColor
                                                            : Theme.c("#d7a85b", Theme.rev)
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }

                                PanelField {
                                    width: 300
                                    height: 24
                                    text: fmtRow.command
                                    placeholderText: fmtRow.defaultCommand
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 11
                                    selectByMouse: true
                                    leftPadding: 6
                                    rightPadding: 6
                                    onEditingFinished: {
                                        Fmt.setToolFor(fmtRow.id, text)
                                        /*
                                         * 把这一行刷成 C++ 侧规范化过的值（用户填了
                                         * "  clang-format  "，落盘的是 trim 过的，界面得跟着）。
                                         *
                                         * 只写这一行的 model，谁也不重建 —— 这是原来那个
                                         * Qt.callLater(root.refreshFormatTools) 的主要罪状：
                                         * 换整个数组 = Repeater 把 15 行全销毁重建，而这一步
                                         * 恰好发生在"点进另一个输入框"的那一瞬（点中的框一起
                                         * 被换掉、焦点落空，于是要么顿一下、要么得再点一次）。
                                         *
                                         * setProperty 那两下是给"真改了命令"准备的：C++ 会
                                         * 发 toolsChanged，refreshFormatTools 也会原地刷一遍，
                                         * 这里再写一次是**失焦但没改**那条路的兜底 —— 那条路
                                         * 不发信号，不写就看不到 trim 之后的样子。
                                         */
                                        fmtToolModel.setProperty(fmtRow.index, "command",
                                                                 Fmt.toolFor(fmtRow.id))
                                        fmtToolModel.setProperty(fmtRow.index, "available",
                                                                 Fmt.toolAvailable(fmtRow.id))
                                        /* text 的绑定被用户输入打断过（见上面 refreshDocRunner
                                           那段说明），所以这里得手动回填一次 */
                                        text = Fmt.toolFor(fmtRow.id)
                                    }
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: Theme.c("#26282b", Theme.rev)
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                            }
                        }
                    }

                    /* ============ 汇总（一段时间的复制内容 -> 分类文档） ============ */
                    Column {
                        id: summarizeColumn
                        /* objectName 是给自检用的：它要能按名字找到这一栏量尺寸 */
                        objectName: "summarizeColumn"
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 9
                        visible: root.section === "summarize"

                        Text {
                            text: "内容汇总"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.textColor
                            font.pixelSize: 12
                            text: "用法：选好区间 → 「开始汇总」→ 整理出来的东西先进「待审」，"
                                  + "在编辑器里看过、改过，再点「采纳」并进 <保存位置>/文档/<分类>.md；"
                                  + "最后那一步「收进归档」把这一段的原文从左侧树上收起来。"
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "分类是模型起的，但每轮都会把已有分类报给它，优先往里归 —— "
                                  + "真归错了不用在界面里改：草稿的文件名就是分类名，"
                                  + "改个名再采纳，内容并进的就是改完那份。"
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "用的是「模型」那一栏里配的同一个模型（和翻译、校验共用一套配置）。"
                                  + "内容按每 6000 字分批发，一批一次请求，一轮最多 30 批 —— "
                                  + "区间拉太长会直接让你缩短，不会闷着烧 token。"
                                  + "这一版只汇总文字，剪贴板里的截图不进汇总。"
                        }

                        /* ---- 区间 ---- */
                        Row {
                            spacing: 8

                            Repeater {
                                model: [        { label: "今天", from: 0, to: 0 },
                                    { label: "昨天", from: -1, to: -1 },
                                    { label: "最近 7 天", from: -6, to: 0 },
                                    { label: "最近 30 天", from: -29, to: 0 }
                                ]

                                delegate: Rectangle {
                                    id: presetHit
                                    required property var modelData
                                    width: presetText.width + 20
                                    height: 24
                                    radius: 4
                                    /* 选中的那个区间给一层底，不然四个按钮看不出点的是哪个 */
                                    color: root.sumFrom === root.dayText(modelData.from)
                                           && root.sumTo === root.dayText(modelData.to)
                                           ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor

                                    Text {
                                        id: presetText
                                        anchors.centerIn: parent
                                        text: presetHit.modelData.label
                                        color: root.textColor
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            root.sumFrom = root.dayText(presetHit.modelData.from)
                                            root.sumTo = root.dayText(presetHit.modelData.to)
                                            root.sumHint = ""
                                        }
                                    }
                                }
                            }
                        }

                        Row {
                            spacing: 8

                            PanelField {
                                id: fromField
                                width: 116
                                height: 26
                                text: root.sumFrom
                                placeholderText: "yyyy-MM-dd"
                                color: root.textColor
                                placeholderTextColor: root.mutedColor
                                font.pixelSize: 12
                                selectByMouse: true
                                leftPadding: 7
                                rightPadding: 7
                                onEditingFinished: root.sumFrom = text
                                background: Rectangle {
                                    color: Theme.c("#26282b", Theme.rev)
                                    border.color: root.borderColor
                                    border.width: 1
                                    radius: 4
                                }
                            }
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: "~"
                                color: root.mutedColor
                                font.pixelSize: 12
                            }
                            PanelField {
                                id: toField
                                width: 116
                                height: 26
                                text: root.sumTo
                                placeholderText: "yyyy-MM-dd"
                                color: root.textColor
                                placeholderTextColor: root.mutedColor
                                font.pixelSize: 12
                                selectByMouse: true
                                leftPadding: 7
                                rightPadding: 7
                                onEditingFinished: root.sumTo = text
                                background: Rectangle {
                                    color: Theme.c("#26282b", Theme.rev)
                                    border.color: root.borderColor
                                    border.width: 1
                                    radius: 4
                                }
                            }
                        }

                        /* ---- 开始 / 停 + 进度 ---- */
                        Row {
                            spacing: 8

                            Rectangle {
                                width: runText.width + 24
                                height: 26
                                radius: 4
                                /* 在跑的时候这颗按钮唯一的用处是"停"，所以它自己变灰 */
                                color: Sum.busy ? "transparent"
                                                : (runHit.containsMouse ? root.rowHover : "transparent")
                                border.width: 1
                                border.color: Sum.busy ? root.borderColor : root.accentColor

                                Text {
                                    id: runText
                                    anchors.centerIn: parent
                                    text: Sum.busy ? "正在汇总…" : "开始汇总"
                                    color: Sum.busy ? root.mutedColor : root.textBright
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: runHit
                                    anchors.fill: parent
                                    enabled: !Sum.busy
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.sumHint = ""
                                        Sum.start(root.sumFrom, root.sumTo)
                                    }
                                }
                            }

                            Rectangle {
                                visible: Sum.busy
                                width: stopText.width + 20
                                height: 26
                                radius: 4
                                color: stopHit.containsMouse ? root.rowHover : "transparent"
                                border.width: 1
                                border.color: root.borderColor

                                Text {
                                    id: stopText
                                    anchors.centerIn: parent
                                    text: "停"
                                    color: root.textColor
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: stopHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Sum.cancel()
                                }
                            }
                        }

                        /*
                         * 进度条按 Sum.totalSteps 走。那个分母在跑到一半时会长：
                         * 同一个分类被切到好几批时，后面还要各补一次"合并"请求，
                         * 所以这里读的是实时值，不是一开始算出来的批数。
                         */
                        Rectangle {
                            width: parent.width
                            height: 3
                            radius: 2
                            color: root.borderColor
                            visible: Sum.busy || Sum.totalSteps > 0

                            Rectangle {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                width: Sum.totalSteps > 0
                                       ? parent.width * Math.min(1, Sum.doneSteps / Sum.totalSteps) : 0
                                radius: 2
                                color: root.accentColor
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: Sum.busy ? root.accentColor : root.mutedColor
                            font.pixelSize: 11
                            text: Sum.status === ""
                                 ? "还没跑过：选好区间点「开始汇总」。"
                                 : Sum.status
                        }

                        /* ---- 待审草稿 ---- */
                        Text {
                            text: "待审草稿（" + root.draftRows.length + "）"
                            color: root.textBright
                            font.pixelSize: 13
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: Math.max(30, draftColumn.height + 16)
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor

                            Column {
                                id: draftColumn
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.right: parent.right
                                anchors.rightMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Text {
                                    visible: root.draftRows.length === 0
                                    text: "（没有待审的草稿。「文档/待审」里的文件也会列在这儿，"
                                          + "所以重启还在的）"
                                    wrapMode: Text.WordWrap
                                    width: parent.width
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }

                                Repeater {
                                    model: root.draftRows

                                    delegate: Row {
                                        id: draftRow
                                        required property var modelData
                                        width: parent.width
                                        spacing: 8

                                        Text {
                                            width: parent.width - 190
                                            text: draftRow.modelData.category
                                                  + "　" + draftRow.modelData.label
                                            color: root.textColor
                                            font.pixelSize: 12
                                            elide: Text.ElideMiddle
                                        }
                                        Repeater {
                                            model: [            { label: "打开", act: "open" },
                                                { label: "采纳", act: "adopt" },
                                                { label: "丢弃", act: "discard" }
                                            ]
                                            delegate: Rectangle {
                                                id: draftAct
                                                required property var modelData
                                                width: draftActText.width + 18
                                                height: 22
                                                radius: 4
                                                color: draftActHit.containsMouse ? root.rowHover : "transparent"
                                                border.width: 1
                                                border.color: root.borderColor

                                                Text {
                                                    id: draftActText
                                                    anchors.centerIn: parent
                                                    text: draftAct.modelData.label
                                                    color: draftActHit.containsMouse
                                                           ? root.textBright : root.textColor
                                                    font.pixelSize: 11
                                                }
                                                MouseArea {
                                                    id: draftActHit
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        var path = draftRow.modelData.path
                                                        var act = draftAct.modelData.act
                                                        if (act === "open") {
                                                            /* 草稿就是普通 md 文件：直接在编辑区里改 */
                                                            root.commandRequested("fileOpen:" + path)
                                                        } else if (act === "adopt") {
                                                            var doc = Store.adoptDraft(path)
                                                            root.sumHint = doc === ""
                                                                           ? "没能并进文档（草稿是空的？）"
                                                                           : ("已并入 " + doc)
                                                        } else {
                                                            Store.discardDraft(path)
                                                            root.sumHint = "丢掉了那份草稿"
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        /* ---- 收进归档 ---- */
                        Row {
                            spacing: 8

                            Rectangle {
                                width: archiveRunText.width + 24
                                height: 26
                                radius: 4
                                color: archiveRunHit.containsMouse ? root.rowHover : "transparent"
                                border.width: 1
                                border.color: root.borderColor

                                Text {
                                    id: archiveRunText
                                    anchors.centerIn: parent
                                    text: "把这区间的原文收进归档"
                                    color: archiveRunHit.containsMouse ? root.textBright : root.textColor
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: archiveRunHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        var n = Store.archiveRange(root.sumFrom, root.sumTo)
                                        root.sumHint = n > 0
                                                       ? ("收进归档 " + n
                                                          + " 份原文，去左边「归档」那一栏能翻出来")
                                                       : (root.sumFrom + " ~ " + root.sumTo
                                                          + " 没有可收的原文（或者已经收过了）")
                                    }
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            visible: root.sumHint !== ""
                            color: root.accentColor
                            font.pixelSize: 11
                            text: root.sumHint
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "「收进归档」只是让左侧树不再显示那些文件，磁盘上一个字节都没动 —— "
                                  + "内容还在原来的日期目录里，用记事本也打得开。"
                        }
                    }

                    /* ============ 归档（汇总过的原文，树上不再显示） ============ */
                    Column {
                        id: archiveColumn
                        objectName: "archiveColumn"
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 9
                        visible: root.section === "archive"

                        Text {
                            text: "归档内容"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.textColor
                            font.pixelSize: 12
                            text: "汇总过、已经从左侧树上收起来的原文。要哪份点「还原」，"
                                  + "它就重新回到自己的日期目录里去；点「打开」直接看内容。"
                        }

                        Rectangle {
                            width: parent.width
                            height: Math.max(30, archivedColumn.height + 16)
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor

                            Column {
                                id: archivedColumn
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.right: parent.right
                                anchors.rightMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Text {
                                    visible: root.archiveRows.length === 0
                                    text: "（还没归档过任何内容）"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }

                                Repeater {
                                    model: root.archiveRows

                                    delegate: Row {
                                        id: archivedRow
                                        required property var modelData
                                        width: parent.width
                                        spacing: 8

                                        Text {
                                            /*
                                             * 归档只是"树上不显示"，文件本来还在原地 —— 原地却又不在了，
                                             * 那就是他自己从文件管理器里删了。这一句必须说出来，
                                             * 不然看着像这个程序把内容弄丢了。
                                             */
                                            width: parent.width - 150
                                            text: archivedRow.modelData.dateKey + " / "
                                                  + archivedRow.modelData.label
                                                  + (archivedRow.modelData.exists
                                                     ? "" : "（文件已不在）")
                                            color: archivedRow.modelData.exists
                                                   ? root.textColor : root.mutedColor
                                            font.pixelSize: 12
                                            elide: Text.ElideMiddle
                                        }
                                        Repeater {
                                            model: [            { label: "打开", act: "open" },
                                                { label: "还原", act: "back" }
                                            ]
                                            delegate: Rectangle {
                                                id: archivedAct
                                                required property var modelData
                                                width: archivedActText.width + 18
                                                height: 22
                                                radius: 4
                                                color: archivedActHit.containsMouse ? root.rowHover : "transparent"
                                                border.width: 1
                                                border.color: root.borderColor

                                                Text {
                                                    id: archivedActText
                                                    anchors.centerIn: parent
                                                    text: archivedAct.modelData.label
                                                    color: archivedActHit.containsMouse
                                                           ? root.textBright : root.textColor
                                                    font.pixelSize: 11
                                                }
                                                MouseArea {
                                                    id: archivedActHit
                                                    anchors.fill: parent
                                                    hoverEnabled: true
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        var path = archivedRow.modelData.path
                                                        if (archivedAct.modelData.act === "open")
                                                            root.commandRequested("fileOpen:" + path)
                                                        else
                                                            Store.unarchiveFile(path)
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    /* ============ 配色方案 ============ */
                    Column {
                        id: schemeColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 10
                        visible: root.section === "scheme"

                        Text {
                            text: "配色方案"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 12
                            text: "点一行换一套。方案就是一个 json 文件，"
                                  + "改完存盘界面立刻跟着变，不用重启；"
                                  + "内置的 Dark / Light 不可改，想改先「另存为…」一份自己的。\n"
                                  + "除了颜色，文件里还能写 font 那一段（字体、字号、注释字号、行高、"
                                  + "自动换行、终端字体）：写了哪一项就以方案为准，那一排的开关会置灰，"
                                  + "没写的项照用你在菜单里设的值。"
                        }

                        Rectangle {
                            width: parent.width
                            height: schemeListCol.height + 28
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor

                            Column {
                                id: schemeListCol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.margins: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Repeater {
                                    id: schemeListRepeater
                                    model: Theme.schemeNames
                                    delegate: Rectangle {
                                        id: schemeRow
                                        required property var modelData
                                        readonly property bool on: Theme.scheme === modelData
                                        readonly property bool builtin: modelData === "Dark"
                                                                      || modelData === "Light"
                                        width: parent.width
                                        height: 34
                                        radius: 5
                                        color: on ? root.rowSel
                                                  : (schemeRowHit.containsMouse ? root.frameColor
                                                                                : "transparent")
                                        border.width: 1
                                        border.color: on ? root.accentColor : root.borderColor

                                        Row {
                                            anchors.left: parent.left
                                            anchors.leftMargin: 10
                                            anchors.verticalCenter: parent.verticalCenter
                                            spacing: 8
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: schemeRow.modelData
                                                color: root.textBright
                                                font.pixelSize: 13
                                            }
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                text: schemeRow.builtin ? "内置 · 只读" : "用户文件"
                                                color: root.mutedColor
                                                font.pixelSize: 11
                                            }
                                        }
                                        MouseArea {
                                            id: schemeRowHit
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: Theme.setScheme(schemeRow.modelData)
                                        }
                                    }
                                }
                            }
                        }

                        /*
                         * 方案文件里有什么毛病就摊在这里说：手改 json 的人第一眼要看见的
                         * 就是"哪一行、哪个键、为什么没生效"。少了这一块，改错了只会
                         * 表现为"我改了没变"，永远查不到。
                         */
                        Rectangle {
                            width: parent.width
                            height: schemeErrCol.height + 24
                            radius: 6
                            visible: Theme.schemeError.length > 0
                            color: "#2a1416"
                            border.width: 1
                            border.color: root.warnColor

                            Column {
                                id: schemeErrCol
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.margins: 12
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 4

                                Text {
                                    width: parent.width
                                    text: "方案文件有地方没生效"
                                    color: root.warnColor
                                    font.pixelSize: 12
                                    font.bold: true
                                }
                                Repeater {
                                    model: Theme.schemeError.split("\n")
                                    Text {
                                        /*
                                         * 必须自己声明 modelData：Qt6 的委托里那个隐式的
                                         * modelData 上下文属性已经不给了，不声明就是
                                         * "ReferenceError: modelData is not defined"，
                                         * 结果是**方案报的错在界面上一个字都不显示**
                                         * （后端 schemeError 明明有内容）。
                                         */
                                        required property var modelData
                                        width: parent.width
                                        wrapMode: Text.WordWrap
                                        text: String(modelData)
                                        color: root.textColor
                                        font.pixelSize: 11
                                        visible: String(modelData).trim().length > 0
                                    }
                                }
                            }
                        }

                        Row {
                            spacing: 8
                            PanelButton {
                                label: "另存为…"
                                accent: true
                                onClicked: {
                                    if (!root.askText)
                                        return
                                    var suggest = Theme.scheme
                                    if (suggest === "Dark" || suggest === "Light")
                                        suggest = suggest + " Copy"
                                    root.askText("另存为配色方案",
                                                 "方案名（会写成 名字.json；不能用 Dark / Light）",
                                                 suggest,
                                                 function (name) {
                                        root.schemeSaveErr = Theme.saveSchemeAs(name)
                                    })
                                }
                            }
                            PanelButton {
                                label: "打开方案文件夹"
                                onClicked: Cmd.revealInExplorer(Theme.schemesDir())
                            }
                            PanelButton {
                                label: "重新加载"
                                onClicked: {
                                    root.schemeSaveErr = ""
                                    Theme.reloadSchemes()
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            visible: root.schemeSaveErr.length > 0
                            text: root.schemeSaveErr
                            color: root.warnColor
                            font.pixelSize: 11
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            text: Theme.schemesDir()
                            color: root.mutedColor
                            font.pixelSize: 11
                            textFormat: Text.PlainText
                        }
                    }


                    /* ============ 字体 ============ */
                    Column {
                        id: fontColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 12
                        visible: root.section === "font"

                        Text {
                            text: "字体"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 12
                            text: "这里改的是你自己的默认档。配色方案的 font 段写了哪一项，"
                                  + "那一行就整行淡一档、控件点不动，下面跟一句是哪个方案定的；"
                                  + "没写的项照用这里设的值。"
                        }

                        FontGroupTitle { title: "编辑区" }

                        Column {
                            id: fontRows
                            width: parent.width
                            spacing: 8

                            FontLine {
                                id: famLine
                                title: "字体"
                                locked: root.lockedByScheme("family")
                                note: Theme.fontOverrideNote("family")
                                FontSelect {
                                    id: famSel
                                    anchors.verticalCenter: parent.verticalCenter
                                    shown: root.view ? root.view.fontFamily : ""
                                    greyed: famLine.locked
                                    entries: root.familyEntries(root.view ? root.view.fontFamily : "",
                                                                "font:")
                                    onOpenRequested: {
                                        fontMenu.paneWidth = famSel.width
                                        fontMenu.openFor(famSel, famSel.entries)
                                    }
                                }
                            }

                            FontLine {
                                id: sizeLine
                                title: "字号"
                                locked: root.lockedByScheme("size")
                                note: Theme.fontOverrideNote("size")
                                FontField {
                                    anchors.verticalCenter: parent.verticalCenter
                                    suffix: "px"
                                    shown: root.view ? String(Math.round(root.view.fontPixelSize)) : ""
                                    greyed: sizeLine.locked
                                    onCommit: (v) => {
                                        var n = parseInt(v)
                                        if (!isNaN(n) && n >= 6 && n <= 72)
                                            root.commandRequested("fontSize:" + n)
                                    }
                                }
                            }

                            /* 有"关"这一档的两行才放行首勾选框：不勾 = 跟随正文 / 跟随字体 */
                            FontLine {
                                id: cmtLine
                                title: "单独设注释字号"
                                checkable: true
                                checked: root.view ? root.view.commentFontPixelSize > 0 : false
                                locked: root.lockedByScheme("commentSize")
                                note: Theme.fontOverrideNote("commentSize")
                                onToggle: root.commandRequested(checked
                                                               ? "commentFontSize:0"
                                                               : "commentFontSize:"
                                                                 + Math.round(root.view.fontPixelSize))
                                FontField {
                                    anchors.verticalCenter: parent.verticalCenter
                                    suffix: "px"
                                    shown: root.view && root.view.commentFontPixelSize > 0
                                           ? String(root.view.commentFontPixelSize) : ""
                                    greyed: cmtLine.locked || !cmtLine.checked
                                    onCommit: (v) => {
                                        var n = parseInt(v)
                                        if (!isNaN(n) && n >= 6 && n <= 72)
                                            root.commandRequested("commentFontSize:" + n)
                                    }
                                }
                            }

                            FontLine {
                                id: lhLine
                                title: "自定义行高"
                                checkable: true
                                checked: root.view ? root.view.lineHeightFactor > 1.001 : false
                                locked: root.lockedByScheme("lineHeight")
                                note: Theme.fontOverrideNote("lineHeight")
                                onToggle: root.commandRequested(checked ? "lineHeight:1.0"
                                                                        : "lineHeight:1.15")
                                FontField {
                                    anchors.verticalCenter: parent.verticalCenter
                                    decimal: true
                                    suffix: root.view
                                            ? "倍 · 约 " + Math.round(root.view.naturalLineHeight
                                                                      * root.view.lineHeightFactor) + " px"
                                            : "倍"
                                    shown: root.view ? root.view.lineHeightFactor.toFixed(2) : ""
                                    greyed: lhLine.locked || !lhLine.checked
                                    onCommit: (v) => {
                                        var f = parseFloat(v)
                                        if (!isNaN(f) && f >= 1.0 && f <= 3.0)
                                            root.commandRequested("lineHeight:" + f.toFixed(2))
                                    }
                                }
                            }

                            FontLine {
                                id: wrapLine
                                title: "自动换行"
                                checkable: true
                                checked: root.view ? root.view.wrapEnabled : false
                                locked: root.lockedByScheme("wrap")
                                note: Theme.fontOverrideNote("wrap")
                                onToggle: root.commandRequested("toggleWrap")
                            }
                        }

                        FontGroupTitle { title: "终端" }

                        Column {
                            id: termRows
                            width: parent.width
                            spacing: 8

                            FontLine {
                                id: termFamLine
                                title: "字体"
                                locked: root.lockedByScheme("terminalFamily")
                                note: Theme.fontOverrideNote("terminalFamily")
                                FontSelect {
                                    id: termFamSel
                                    anchors.verticalCenter: parent.verticalCenter
                                    shown: root.termFamilyEff
                                    greyed: termFamLine.locked
                                    entries: root.familyEntries(root.termFamilyEff, "termFont:")
                                    onOpenRequested: {
                                        fontMenu.paneWidth = termFamSel.width
                                        fontMenu.openFor(termFamSel, termFamSel.entries)
                                    }
                                }
                            }

                            FontLine {
                                id: termSizeLine
                                title: "字号"
                                locked: root.lockedByScheme("terminalSize")
                                note: Theme.fontOverrideNote("terminalSize")
                                FontField {
                                    anchors.verticalCenter: parent.verticalCenter
                                    suffix: "px"
                                    shown: String(root.termSizeEff)
                                    greyed: termSizeLine.locked
                                    onCommit: (v) => {
                                        var n = parseInt(v)
                                        if (!isNaN(n) && n >= 8 && n <= 40)
                                            root.commandRequested("termFontSize:" + n)
                                    }
                                }
                            }
                        }
                    }

                    /* ============ 关于 ============ */
                    Column {
                        id: aboutSectionColumn
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.margins: 14
                        spacing: 10
                        visible: root.section === "about"
    
                        Text {
                            text: "关于 SmartClip"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }
    
                        Rectangle {
                            width: parent.width
                            height: aboutColumn.height + 28
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor
    
                            Column {
                                id: aboutColumn
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 7
    
                                Repeater {
                                    model: [
                                        { k: "名称",     v: "SmartClip" },
                                        { k: "版本",     v: "0.1.0" },
                                        { k: "作用",     v: "剪贴板历史 + 内置编辑器" },
                                        { k: "编辑器内核", v: "QScintilla 2.14（GPLv3）" },
                                        { k: "界面内核",  v: "Qt " + Qt.version + " / QML" },
                                        { k: "当前字号",  v: (view ? view.fontPixelSize : 12) + " px（设置里可改）" },
                                        /*
                                         * 行高：显示**实际像素**，倍数是设置里那一档。
                                         * 两个数都从编辑器读（view.lineHeight / lineHeightFactor），
                                         * 换字体 / 换字号之后这里跟着变。
                                         */
                                        { k: "当前行高",  v: (view ? view.lineHeight : 15) + " px"
                                                              + (view && view.lineHeightFactor > 1.0
                                                                 ? "（" + view.lineHeightFactor + " 倍）"
                                                                 : "（跟随字体）") }
                                    ]
    
                                    delegate: Row {
                                        required property var modelData
                                        spacing: 12
    
                                        Text {
                                            width: 74
                                            text: modelData.k
                                            color: root.mutedColor
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            text: modelData.v
                                            color: root.textColor
                                            font.pixelSize: 12
                                        }
                                    }
                                }
                            }
                        }
    
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "撤销 / 重做 / 剪切 / 复制 / 粘贴 / 全选（Ctrl+Z / Ctrl+Y / Ctrl+X / "
                                  + "Ctrl+C / Ctrl+V / Ctrl+A）由编辑器内核自己处理，不在上面的可改清单里。"
                        }
    
                        /* 编辑器字号这些既有开关，顺手给个入口，省得翻菜单 */
                        Text {
                            text: "编辑器"
                            color: root.textBright
                            font.pixelSize: 13
                            font.bold: true
                        }
    
                        Row {
                            spacing: 10
    
                            Repeater {
                                model: [
                                    { label: "字号 −", act: "fontSize:11",
                                      locked: root.lockedByScheme("size") },
                                    { label: "字号 +", act: "fontSize:14",
                                      locked: root.lockedByScheme("size") },
                                    { label: "行高 −", act: "lineHeightDown",
                                      locked: root.lockedByScheme("lineHeight") },
                                    { label: "行高 +", act: "lineHeightUp",
                                      locked: root.lockedByScheme("lineHeight") },
                                    /* 缩放是临时视图态（不落盘、也不在方案里），永远点得动 */
                                    { label: "重置缩放", act: "zoomReset", locked: false },
                                    { label: "自动换行", act: "toggleWrap",
                                      locked: root.lockedByScheme("wrap") }
                                ]
    
                                delegate: Rectangle {
                                    id: aboutBtn
                                    required property var modelData
                                    /* 方案钉住的那一格：灰着、不响应点击，但**不消失** */
                                    readonly property bool off: modelData.locked === true
                                    width: 86
                                    height: 24
                                    radius: 4
                                    opacity: off ? 0.45 : 1.0
                                    color: !off && aboutBtnHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
    
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: !off && aboutBtnHit.containsMouse ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }
    
                                    MouseArea {
                                        id: aboutBtnHit
                                        anchors.fill: parent
                                        enabled: !aboutBtn.off
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.commandRequested(modelData.act)
                                    }
                                }
                            }
                        }
    
                        /*
                         * 置灰不写清楚为什么，就是"按钮坏了"。这一行只在方案真的钉了
                         * 某一项字体时出现，说的是去哪儿改。
                         */
                        Text {
                            visible: root.schemeFontKeys.length > 0
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "上面置灰的那几项由方案「" + Theme.scheme
                                  + "」的 font 段定着，要改就去改那个文件（设置 → 配色方案 上面有路径）。"
                        }
                    }
                }
            }
        }

    }
}
