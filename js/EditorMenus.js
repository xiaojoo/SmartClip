.pragma library

/*
 * 菜单数据。
 *
 * 为什么单独放一个 .pragma library 文件：菜单栏（TopBar）和工具栏
 * （ToolBar）都要"语言 / 编码 / 换行符"这三张表，写两份迟早会不一致。
 * 这里只算数据、不碰界面，两处调用同一份。
 *
 * 每个条目：
 *   { label, act, shortcut, icon, checked, disabled }   普通项
 *   { separator: true }                                 分隔线
 *
 * icon 是 IconProvider 里的图形名，显示在条目**左边**（快捷键在右边）——
 * 工具栏那一行已经去掉，命令全部收进这些下拉菜单，图标就是它们的"脸"。
 *
 * act 前缀（由 Main.qml 的 dispatch 解析）：
 *   lang:<id> / encoding:<名> / eol:<LF|CRLF|CR> / 其余是纯命令名
 *
 * 关于快捷键显示：这里写的 shortcut 是**出厂默认**，只当缺省值。
 * 用户在设置面板里改过键之后，真正生效的组合键在 C++ 侧（QAction），
 * Main.qml 通过 Cmd.shortcutItems 读回来，以 ov 的形式传进这些函数 ——
 * 有登记的动作一律以 ov 为准，没登记的（撤销 / 复制 / 粘贴…）才用这里的默认值。
 */

/* 把用户改过的快捷键 / 菜单文字覆盖到一批条目上（返回新的数组） */
function applyOverrides(items, ov, labels) {
    if (!ov && !labels)
        return items
    var out = []
    for (var i = 0; i < items.length; ++i) {
        var it = items[i]
        if (!it || it.separator || !it.act) {
            out.push(it)
            continue
        }
        var copy = {}
        for (var k in it)
            copy[k] = it[k]
        if (ov && typeof ov[it.act] === "string")
            copy.shortcut = ov[it.act]
        if (labels && typeof labels[it.act] === "string")
            copy.label = labels[it.act]
        out.push(copy)
    }
    return out
}

function languageItems(view) {
    var out = []
    if (!view || !view.hasDocument)
        return out
    var list = view.languages()
    for (var i = 0; i < list.length; ++i) {
        out.push({
            label: list[i].label,
            act: "lang:" + list[i].id,
            checked: view.language === list[i].id
        })
    }
    return out
}

function encodingItems(view) {
    var all = ["UTF-8", "UTF-8 BOM", "ANSI", "UTF-16LE", "UTF-16BE", "Latin-1"]
    var out = []
    for (var i = 0; i < all.length; ++i) {
        out.push({
            label: all[i],
            act: "encoding:" + all[i],
            checked: !!view && view.hasDocument && view.encoding === all[i],
            disabled: !view || !view.hasDocument
        })
    }
    return out
}

function eolItems(view) {
    var all = [ { id: "LF", label: "LF（Unix）" },
                { id: "CRLF", label: "CRLF（Windows）" },
                { id: "CR", label: "CR（Mac 经典）" } ]
    var out = []
    for (var i = 0; i < all.length; ++i) {
        out.push({
            label: all[i].label,
            act: "eol:" + all[i].id,
            checked: !!view && view.hasDocument && view.eolMode === all[i].id,
            disabled: !view || !view.hasDocument
        })
    }
    return out
}

function fileMenu(ov) {
    return applyOverrides([
        { label: "新建", act: "new", shortcut: "Ctrl+N", icon: "new" },
        { label: "打开…", act: "open", shortcut: "Ctrl+O", icon: "open" },
        /*
         * 打开文件夹 = 把某个目录挂到左边那棵树上（这就是本程序里"打开一个
         * 项目"的意思）。走的是树那边"导入文件夹…"同一条路，act 都不换 ——
         * 以前"文件"里只有"打开文件"，想开一个目录只能绕到左边树的"更多"里找。
         */
        { label: "打开文件夹…", act: "treeImportFolder", icon: "folder" },
        /*
         * 截图（选区 / 加文字 / 固定到桌面，见 src/Screenshot.h）。
         * 放在"文件"里是因为产物也是一份文件（png）；默认键位取
         * Ctrl+Alt+A —— 国内截图工具的通用键位，改键在设置面板里。
         */
        { label: "截图…", act: "shot", shortcut: "Ctrl+Alt+A", icon: "image" },
        /*
         * 便签（桌面上的小块文本，见 src/StickyNotes.h）。
         *
         * 放在截图旁边：两者是一类东西 —— "把一小块东西从程序里拿出来钉在
         * 桌面上"，而且都能在收进托盘之后用（截图 Ctrl+Alt+A、新建便签
         * Ctrl+Alt+N 都是系统级热键）。排列那一条是把摆着的便签一键排成网格，
         * 没有全局键位（它只在主窗口里用得上）。
         */
        { label: "新建便签", act: "note", shortcut: "Ctrl+Alt+N", icon: "note" },
        { label: "排列便签", act: "notesArrange", icon: "grid" },
        { label: "显示全部便签", act: "notesShowAll", icon: "note" },
        { label: "收起全部便签", act: "notesHideAll", icon: "note" },
        /*
         * 翻译卡片（见 src/Translate.h）。和便签同一类东西：桌面上的一块小卡片，
         * 收进托盘也能用（Ctrl+Alt+T 也是系统级热键）。只有一张 —— 点它就是
         * 叫出来（没有就建）。
         */
        { label: "翻译卡片", act: "translate", shortcut: "Ctrl+Alt+T", icon: "translate" },
        { separator: true },
        { label: "保存", act: "save", shortcut: "Ctrl+S", icon: "save" },
        { label: "另存为…", act: "saveAs", shortcut: "Ctrl+Shift+S", icon: "save-as" },
        { label: "全部保存", act: "saveAll", icon: "save" },
        { separator: true },
        { label: "关闭标签", act: "closeTab", shortcut: "Ctrl+W", icon: "close" },
        { label: "关闭其他标签", act: "closeOtherTabs", icon: "close" },
        { separator: true },
        { label: "打印…", act: "print", shortcut: "Ctrl+P", icon: "print" },
        { separator: true },
        { label: "刷新剪贴板列表", act: "refresh", shortcut: "F5", icon: "refresh" },
        { separator: true },
        { label: "退出", act: "quit" }
    ], ov)
}

function editMenu(view, ov) {
    var hasDoc = !!view && view.hasDocument
    var canEdit = hasDoc && !view.readOnly
    return applyOverrides([
        { label: "撤销", act: "undo", shortcut: "Ctrl+Z", icon: "undo",
          disabled: !canEdit || !view.canUndo },
        { label: "重做", act: "redo", shortcut: "Ctrl+Y", icon: "redo",
          disabled: !canEdit || !view.canRedo },
        { separator: true },
        { label: "剪切", act: "cut", shortcut: "Ctrl+X", icon: "cut",
          disabled: !canEdit || !view.hasSelection },
        { label: "复制", act: "copy", shortcut: "Ctrl+C", icon: "copy",
          disabled: !hasDoc || !view.hasSelection },
        { label: "粘贴", act: "paste", shortcut: "Ctrl+V", icon: "paste",
          disabled: !canEdit },
        { separator: true },
        { label: "全选", act: "selectAll", shortcut: "Ctrl+A", icon: "select-all",
          disabled: !hasDoc },
        { label: "复制当前行", act: "copyLine", icon: "copy", disabled: !hasDoc },
        { label: "复制全文", act: "copyAll", icon: "copy", disabled: !hasDoc },
        { separator: true },
        { label: "切换注释", act: "toggleComment", shortcut: "Ctrl+/", icon: "comment",
          disabled: !canEdit },
        { label: "删除当前行", act: "deleteLine", icon: "trash", disabled: !canEdit },
        { label: "复制当前行到下一行", act: "duplicateLine", icon: "plus",
          disabled: !canEdit },
        { separator: true },
        { label: "只读模式", act: "toggleReadOnly", icon: "lock",
          checked: hasDoc && view.readOnly, disabled: !hasDoc }
    ], ov)
}

function searchMenu(view, ov) {
    var hasDoc = !!view && view.hasDocument
    return applyOverrides([
        { label: "查找…", act: "find", shortcut: "Ctrl+F", icon: "search",
          disabled: !hasDoc },
        { label: "替换…", act: "replace", shortcut: "Ctrl+H", icon: "replace",
          disabled: !hasDoc },
        { separator: true },
        { label: "查找下一个", act: "findNext", shortcut: "F3", icon: "chevron-down",
          disabled: !hasDoc },
        { label: "查找上一个", act: "findPrev", shortcut: "Shift+F3", icon: "chevron-up",
          disabled: !hasDoc },
        { separator: true },
        { label: "转到行…", act: "goto", shortcut: "Ctrl+G", icon: "goto",
          disabled: !hasDoc }
    ], ov)
}

function viewMenu(view, ov) {
    var hasDoc = !!view && view.hasDocument
    return applyOverrides([
        { label: "放大", act: "zoomIn", shortcut: "Ctrl+=", icon: "zoom-in",
          disabled: !hasDoc },
        { label: "缩小", act: "zoomOut", shortcut: "Ctrl+-", icon: "zoom-out",
          disabled: !hasDoc },
        { label: "重置缩放", act: "zoomReset", shortcut: "Ctrl+0", icon: "zoom-reset",
          disabled: !hasDoc },
        { separator: true },
        { label: "自动换行", act: "toggleWrap", shortcut: "Alt+Z", icon: "wrap",
          checked: !!view && view.wrapEnabled },
        { label: "显示行号", act: "toggleLineNumbers", icon: "numbers",
          checked: !!view && view.lineNumbersVisible },
        { label: "显示空白字符", act: "toggleWhitespace", icon: "whitespace",
          checked: !!view && view.whitespaceVisible },
        { separator: true },
        { label: "缩进参考线", act: "toggleIndentGuides", icon: "indent",
          checked: !!view && view.indentGuidesVisible },
        /* 行号栏右侧那条分隔竖线（见 EditorViewItem 的 gutterLineVisible） */
        { label: "行号分隔线", act: "toggleGutterLine", icon: "numbers",
          checked: !!view && view.gutterLineVisible },
        /* "一行 N 字"那条竖线；N 在"设置"菜单里改 */
        { label: "字数参考线", act: "toggleRuler", icon: "indent",
          checked: !!view && view.rulerVisible },
        { label: "代码折叠", act: "toggleFolding", icon: "branch",
          checked: !!view && view.foldingEnabled },
        { label: "折叠全部", act: "foldAll", icon: "chevron-right",
          disabled: !hasDoc || !(!!view && view.foldingEnabled) },
        { label: "展开全部", act: "unfoldAll", icon: "chevron-down",
          disabled: !hasDoc || !(!!view && view.foldingEnabled) },
        { separator: true },
        /*
         * 这三条是**子菜单**：右边带一个 >，鼠标停上去（或点一下）在菜单右边
         * 再展开一栏，条目就是这里的 items —— 渲染和交互全在 DropdownMenu.qml
         * 的 MenuEntryItem 里，不再绕 dispatch 一圈。
         *
         * items 在**菜单弹出时**现算（本函数就是那时候调的），所以
         * "当前语言/编码打勾"、以及没打开文档时整条置灰，都是最新的状态。
         */
        { label: "语言", act: "menu:语言", submenu: true, items: languageItems(view),
          disabled: !hasDoc },
        { label: "编码", act: "menu:编码", submenu: true, items: encodingItems(view),
          disabled: !hasDoc },
        { label: "换行符", act: "menu:换行", submenu: true, items: eolItems(view),
          disabled: !hasDoc }
    ], ov)
}

/* 编辑器字号可选值（12 是出厂默认）。单位是**像素**，和 QML 的 font.pixelSize 一致 */
var kFontSizes = [10, 11, 12, 13, 14, 16, 18, 20, 24]
var kDefaultFontSize = 12

/* 注释字号：比正文小一档看着舒服，也可以"跟随正文" */
var kCommentFontSizes = [10, 11, 12, 13, 14, 16]

/*
 * 正文字体家族。
 *
 * 名字必须用**英文家族名**：QScintilla 转发给 Scintilla 时走的是
 * QFont::family().toLatin1()，中文名（"新宋体"）会被压成问号，字体就失效了。
 * 新宋体 / 更纱黑体这类是"中英都覆盖的等宽字体"，中英混排时比 Consolas 整齐 ——
 * Scintilla 没法按"汉字/拉丁字母"分别设字体，只能整篇换。
 */
var kFontFamilies = [
    { label: "Consolas（默认）", family: "Consolas" },
    { label: "Cascadia Mono", family: "Cascadia Mono" },
    { label: "新宋体 NSimSun（中英等宽）", family: "NSimSun" },
    { label: "更纱黑体 Sarasa Mono SC（装了才有）", family: "Sarasa Mono SC" },
    { label: "Courier New", family: "Courier New" }
]

function fontSizeItems(view) {
    var current = view ? view.fontPixelSize : kDefaultFontSize
    var out = []
    for (var i = 0; i < kFontSizes.length; ++i) {
        out.push({
            label: "字号 " + kFontSizes[i] + " px",
            act: "fontSize:" + kFontSizes[i],
            checked: current === kFontSizes[i] || (i === kFontSizes.length - 1
                                                    && current > kFontSizes[i])
        })
    }
    out.push({ separator: true })
    out.push({
        label: "恢复默认字号（" + kDefaultFontSize + " px）",
        act: "fontSize:" + kDefaultFontSize,
        icon: "refresh",
        enabled: current !== kDefaultFontSize,
        disabled: current === kDefaultFontSize
    })
    return out
}

/* 注释字号：0 = 跟随正文 */
function commentFontSizeItems(view) {
    var current = view ? view.commentFontPixelSize : 0
    var out = [{
        label: "跟随正文",
        act: "commentFontSize:0",
        checked: !current
    }]
    for (var i = 0; i < kCommentFontSizes.length; ++i) {
        out.push({
            label: kCommentFontSizes[i] + " px",
            act: "commentFontSize:" + kCommentFontSizes[i],
            checked: current === kCommentFontSizes[i]
        })
    }
    return out
}

function fontFamilyItems(view) {
    var current = view ? view.fontFamily : "Consolas"
    var out = []
    for (var i = 0; i < kFontFamilies.length; ++i) {
        out.push({
            label: kFontFamilies[i].label,
            act: "font:" + kFontFamilies[i].family,
            checked: current === kFontFamilies[i].family
        })
    }
    return out
}

/*
 * 行高倍数（单位是"倍"，不是像素）。
 *
 * 1.0 = 跟随字体：就是字体自带的那个行高（Consolas 12px 约 15px），
 * 不再额外加空。往上是把行拉开 —— Scintilla 侧是按倍数把差额平均加到
 * 每行的上下两侧（见 EditorViewItem::applyLineSpacing）。
 * 只给 ≥ 1.0 的值：比字体自带的还紧会压字。
 */
var kLineHeightFactors = [1.0, 1.15, 1.3, 1.5, 1.75, 2.0, 2.5]

function lineHeightItems(view) {
    var current = view ? view.lineHeightFactor : 1.0
    /* 自然行高：菜单上顺手把"这一档大概多高"写出来，改字号后它自己跟着变 */
    var base = view ? view.naturalLineHeight : 0
    var out = []
    for (var i = 0; i < kLineHeightFactors.length; ++i) {
        var f = kLineHeightFactors[i]
        var px = base > 0 ? "（" + Math.round(base * f) + " px）" : ""
        out.push({
            label: (i === 0 ? "跟随字体" : f + " 倍") + px,
            act: "lineHeight:" + f,
            checked: Math.abs(current - f) < 0.001
        })
    }
    return out
}

/*
 * 设置面板上"行高 − / 行高 +"用：在 kLineHeightFactors 里往前 / 往后走一格。
 *
 * 当前值不在表里（手改过设置文件之类）就取最近的一格当起点 —— 否则
 * "减一档"会直接跳到最后或最前一格。
 */
function stepLineHeight(current, dir) {
    var idx = 0
    var best = 1e9
    for (var i = 0; i < kLineHeightFactors.length; ++i) {
        var d = Math.abs(kLineHeightFactors[i] - current)
        if (d < best) {
            best = d
            idx = i
        }
    }
    idx += dir > 0 ? 1 : -1
    if (idx < 0)
        idx = 0
    if (idx > kLineHeightFactors.length - 1)
        idx = kLineHeightFactors.length - 1
    return kLineHeightFactors[idx]
}

/*
 * 字数参考线（"一行 N 字"那条竖线）的可选列号。
 *
 * 120 是出厂默认，和 EditorViewItem 里的 m_rulerColumn 一致 —— 两处都改才算换了
 * 默认值。不在表里的值（自定义输入的）就只显示在"自定义…"那一行上。
 */
var kRulerColumns = [60, 72, 80, 100, 120]

function rulerColumnItems(view) {
    var current = view ? view.rulerColumn : 120
    var out = []
    for (var i = 0; i < kRulerColumns.length; ++i) {
        out.push({
            label: kRulerColumns[i] + " 字" + (kRulerColumns[i] === 120 ? "（默认）" : ""),
            act: "rulerColumn:" + kRulerColumns[i],
            checked: current === kRulerColumns[i]
        })
    }
    out.push({ separator: true })
    out.push({
        label: "自定义…（当前 " + current + " 字）",
        act: "rulerColumnAsk",
        icon: "gear"
    })
    return out
}

function settingsMenu(view, ov) {
    var hasDoc = !!view && view.hasDocument
    var items = []
    /* 图形化设置面板（快捷键 / 存储 / 关于），见 qml/components/SettingsPanel.qml */
    items.push({ label: "打开设置面板…", act: "settings", icon: "gear" })
    /* 剪贴板内容存哪儿、导入了哪些外部文件夹 —— 直接翻到"存储"那一栏 */
    items.push({ label: "存储与保存位置…", act: "storage", icon: "folder" })
    items.push({ separator: true })
    items.push({ label: "字号", icon: "zoom-reset", disabled: true })
    var sizes = fontSizeItems(view)
    for (var i = 0; i < sizes.length; ++i)
        items.push(sizes[i])

    items.push({ separator: true })
    items.push({ label: "注释字号", icon: "comment", disabled: true })
    var commentSizes = commentFontSizeItems(view)
    for (var c = 0; c < commentSizes.length; ++c)
        items.push(commentSizes[c])

    items.push({ separator: true })
    items.push({ label: "字体", icon: "gear", disabled: true })
    var families = fontFamilyItems(view)
    for (var f = 0; f < families.length; ++f)
        items.push(families[f])

    items.push({ separator: true })
    items.push({ label: "行高", icon: "line-height", disabled: true })
    var heights = lineHeightItems(view)
    for (var h = 0; h < heights.length; ++h)
        items.push(heights[h])

    items.push({ separator: true })
    items.push({ label: "字数参考线列", icon: "indent", disabled: true })
    var cols = rulerColumnItems(view)
    for (var r = 0; r < cols.length; ++r)
        items.push(cols[r])

    items.push({ separator: true })
    items.push({ label: "自动换行", act: "toggleWrap", icon: "wrap",
                 checked: !!view && view.wrapEnabled })
    items.push({ label: "显示行号", act: "toggleLineNumbers", icon: "numbers",
                 checked: !!view && view.lineNumbersVisible })
    items.push({ label: "显示空白字符", act: "toggleWhitespace", icon: "whitespace",
                 checked: !!view && view.whitespaceVisible })
    items.push({ separator: true })
    items.push({ label: "只读模式", act: "toggleReadOnly", icon: "lock",
                 checked: hasDoc && view.readOnly, disabled: !hasDoc })
    return applyOverrides(items, ov)
}

function helpMenu(ov) {
    return applyOverrides([
        { label: "快捷键一览", act: "shortcuts", icon: "info" },
        { separator: true },
        { label: "关于 SmartClip", act: "about", icon: "info" }
    ], ov)
}

/*
 * 内容区标签栏的右键菜单（点 tab 弹出，见 qml/components/EditorArea.qml）。
 *
 * index 是**右键点中的那个**标签，不是当前激活的那个 —— 右键一个没激活的
 * 标签时两者不是同一个，"关闭其他"必须以点中的为基准，否则会把用户刚点的
 * 那一个一起关掉。所以这里用带下标的动作 closeTab:<i> / closeOthers:<i>，
 * 由 Main.qml 的 dispatch 解析（纯命令名的 closeTab / closeOtherTabs
 * 仍然是"当前标签"，菜单栏和快捷键走那条）。
 *
 * 只有关闭类命令：保存 / 另存为这些作用在"当前文档"上，而右键点的标签
 * 未必是当前那个，放进来会动错文件。
 */
function tabMenu(view, index, ov) {
    var count = (view && view.documents) ? view.documents.length : 0
    var has = index >= 0 && index < count
    /* 快捷键显示值跟用户改过的走（closeTab 在可改键清单里） */
    var closeKey = (ov && typeof ov["closeTab"] === "string" && ov["closeTab"] !== "")
                   ? ov["closeTab"] : "Ctrl+W"
    return [
        { label: "关闭", act: "closeTab:" + index, shortcut: closeKey, icon: "close",
          disabled: !has },
        { label: "关闭其他", act: "closeOthers:" + index, icon: "close",
          disabled: !has || count < 2 },
        { label: "关闭全部", act: "closeAllTabs", icon: "trash",
          disabled: count < 1 }
    ]
}

/*
 * 左侧那排工具格里**便签那一格**的右键菜单（点一下是新建，右键弹这份）。
 *
 * 以前这一格用的是 Qt Quick Controls 那个 `Menu`：白底、没图标，和界面里其它
 * 菜单（深色 + 图标列 + 右边快捷键）不是一个长相 —— 用户要求改成和"帮助"那
 * 一份一样的样子，所以现在走共用那份 DropdownMenu，条目就是下面这几条。
 *
 * act 用的是"文件"菜单里同一批（note / notesArrange / notesShowAll /
 * notesHideAll），由 Main.qml 的 dispatch 落到 Notes 那四条命令上 ——
 * 和托盘菜单点的是同一份实现，三处行为必须一模一样。
 *
 * state 由 Main.qml 的 notesMenuState() 给：
 *   { count, visible }（便签总条数 / 正摆在桌面上的条数）
 * "排列 / 收起"按 visible 置灰，"显示全部"按 count 置灰：一条都没有时点下去
 * 什么也不会发生的条目，不该是可点的样子。
 */
function notesMenu(state, ov) {
    var s = state || {}
    var total = Number(s.count || 0)
    var shown = Number(s.visible || 0)
    return applyOverrides([
        { label: "新建便签", act: "note", shortcut: "Ctrl+Alt+N", icon: "note" },
        { separator: true },
        { label: "排列便签（" + shown + " 块摆着）", act: "notesArrange", icon: "grid",
          disabled: shown === 0 },
        { label: "显示全部便签", act: "notesShowAll", icon: "note", disabled: total === 0 },
        { label: "收起全部便签", act: "notesHideAll", icon: "note", disabled: shown === 0 }
    ], ov)
}

/*
 * 左侧项目树标题栏的"更多"菜单（也挂在标题"项目 ∨"上，见 FolderTree.qml）。
 *
 * state 由 Main.qml 的 treeMenuState() 给：
 *   { folderCount, openCount, itemCount, entryCount, rootPath, importedCount,
 *     newestFirst, locateEnabled }
 * 用它把"已经全展开 / 已经全折叠"的两条置灰 —— 和 PyCharm 一样，
 * 点下去没事发生的那两条就不该是可点的样子；保存位置也顺便显示在这儿，
 * 让用户一眼看到"东西到底存哪儿了"。
 *
 * 动作名同样由 Main.qml 的 dispatch 解析（treeNew / treeLocate / treeExpandAll /
 * treeCollapseAll / treeSortNewest / treeSortOldest / treeHide /
 * treeImportFolder / treeOpenRoot / treeChooseRoot）。
 */
function treeMenu(state, ov) {
    var s = state || {}
    var folders = Number(s.folderCount || 0)
    var open = Number(s.openCount || 0)
    var allOpen = folders > 0 && open >= folders
    var allClosed = open <= 0
    var newest = s.newestFirst !== false
    var root = s.rootPath ? String(s.rootPath) : ""
    return applyOverrides([
        { label: "新建文件", act: "treeNew", icon: "plus" },
        { label: "刷新列表（重扫磁盘）", act: "refresh", shortcut: "F5", icon: "refresh" },
        /* 当前标签不在左树里（未命名空白文档 / 别处打开的文件）时没什么可定位的 */
        { label: "定位当前文件", act: "treeLocate", icon: "locate",
          disabled: s.locateEnabled === false },
        { separator: true },
        { label: "全部展开", act: "treeExpandAll", icon: "expand-all", disabled: allOpen },
        { label: "全部折叠", act: "treeCollapseAll", icon: "collapse-all", disabled: allClosed },
        { separator: true },
        { label: "最新在前", act: "treeSortNewest", checked: newest },
        { label: "最早在前", act: "treeSortOldest", checked: !newest },
        { separator: true },
        /* 导入的文件夹只是"挂上来看"，新内容永远写进保存目录 */
        { label: "导入文件夹…", act: "treeImportFolder", icon: "open" },
        { label: "打开保存位置", act: "treeOpenRoot", icon: "folder", disabled: root === "" },
        { label: "设置保存位置…", act: "treeChooseRoot", icon: "gear" },
        { separator: true },
        { label: "收起面板", act: "treeHide", icon: "minus" }
    ], ov)
}

/*
 * 左树上一个**文件**行的右键菜单（Main.qml 的 openAtPoint 弹它）。
 *
 * row 就是 js/FolderManager.js 拍出来的那一行（带 path / entries / size…）。
 * 动作带路径：fileOpen:/fileRename:/fileDelete:/fileReveal:，
 * 由 Main.qml 的 dispatch 按前缀切开再执行。
 */
function fileContextMenu(row) {
    var path = (row && row.path) ? row.path : ""
    return [
        { label: "打开", act: "fileOpen:" + path, icon: "open" },
        { separator: true },
        { label: "重命名…", act: "fileRename:" + path, icon: "save-as" },
        { label: "删除…", act: "fileDelete:" + path, icon: "trash" },
        { separator: true },
        { label: "在文件夹中显示", act: "fileReveal:" + path, icon: "folder" }
    ]
}

/*
 * 左树上一个**文件夹**行的右键菜单。
 *
 * 导入的目录（folderKind === "imported"）多一条"移除"：它只是挂上来看的，
 * 移开不动磁盘上的文件。
 */
function folderContextMenu(row) {
    var path = (row && row.path) ? row.path : ""
    var items = [
        { label: "新建文件", act: "treeNew", icon: "plus" },
        { label: "刷新列表（重扫磁盘）", act: "refresh", icon: "refresh" },
        { separator: true },
        { label: "在文件夹中显示", act: "folderReveal:" + path, icon: "folder" }
    ]
    if (row && row.folderKind === "imported")
        items.push({ label: "移除此导入目录", act: "removeImport:" + path, icon: "trash" })
    return items
}

/*
 * 编辑器**滚动条**的右键菜单。
 *
 * 原来这里是 Qt 自带的那个（浅色底 + 英文条目 "Scroll here / Left edge /
 * Page left / …"），跟界面里其它菜单完全不是一个样子 —— 现在换成和编辑区
 * 右键同一套 QML 菜单（见 EditorViewItem::scrollBarContextMenuRequested）。
 *
 * horizontal 决定这一条是横向还是纵向：条目名不一样（左/右 vs 上/下），
 * 动作名也不一样（scroll:h:* / scroll:v:*），由 Main.qml 的 dispatch 拆开
 * 交给 EditorViewItem::scrollBarAction()。
 *
 * 语义和 Qt 原来那七条一一对应，不是自己重定义的：
 *   滚动到这里 / 左(顶)边缘 / 右(底)边缘 / 翻一页 / 滚一行。
 */
function scrollBarMenu(horizontal) {
    var axis = horizontal ? "h" : "v"
    function act(what) { return "scroll:" + axis + ":" + what }
    if (horizontal) {
        return [
            { label: "滚动到这里", act: act("here") },
            { separator: true },
            { label: "左边缘", act: act("edgeStart") },
            { label: "右边缘", act: act("edgeEnd") },
            { separator: true },
            { label: "向左一页", act: act("pageBack"), icon: "chevron-left" },
            { label: "向右一页", act: act("pageForward"), icon: "chevron-right" },
            { separator: true },
            { label: "向左滚动", act: act("lineBack"), icon: "chevron-left" },
            { label: "向右滚动", act: act("lineForward"), icon: "chevron-right" }
        ]
    }
    return [
        { label: "滚动到这里", act: act("here") },
        { separator: true },
        { label: "顶边", act: act("edgeStart") },
        { label: "底边", act: act("edgeEnd") },
        { separator: true },
        { label: "向上一页", act: act("pageBack"), icon: "chevron-up" },
        { label: "向下一页", act: act("pageForward"), icon: "chevron-down" },
        { separator: true },
        { label: "向上滚动", act: act("lineBack"), icon: "chevron-up" },
        { label: "向下滚动", act: act("lineForward"), icon: "chevron-down" }
    ]
}

/* 菜单栏 tab */
function tabLabels() {
    return ["文件", "编辑", "搜索", "视图", "语言", "编码", "换行", "设置", "帮助"]
}

function hasMenu(label) {
    return tabLabels().indexOf(label) >= 0
}

function menuItems(label, view, ov) {
    if (label === "文件") return fileMenu(ov)
    if (label === "编辑") return editMenu(view, ov)
    if (label === "搜索") return searchMenu(view, ov)
    if (label === "视图") return viewMenu(view, ov)
    if (label === "语言") return languageItems(view)
    if (label === "编码") return encodingItems(view)
    if (label === "换行") return eolItems(view)
    if (label === "设置") return settingsMenu(view, ov)
    if (label === "帮助") return helpMenu(ov)
    return [{ label: "（暂无）", act: "none", disabled: true }]
}
