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
        { label: "刷新剪贴板", act: "refresh", shortcut: "F5", icon: "refresh" },
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
         * 这三条是**子菜单**：点它们不是执行命令，而是把菜单换一批条目
         * 重新摆出来（Main.dispatch 的 "menu:<名>" 分支 -> TopBar.openGroup）。
         * submenu: true 就是给 DropdownMenu 看的：这种条目点完**不能关菜单**，
         * 关了就再也弹不回来了（原因见 DropdownMenu.qml 的条目点击处理）。
         */
        { label: "语言", act: "menu:语言", submenu: true, disabled: !hasDoc },
        { label: "编码", act: "menu:编码", submenu: true, disabled: !hasDoc },
        { label: "换行符", act: "menu:换行", submenu: true }
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
    /* 图形化设置面板（快捷键 / 关于），见 qml/components/SettingsPanel.qml */
    items.push({ label: "打开设置面板…", act: "settings", icon: "gear" })
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
 * 左侧项目树标题栏的"更多"菜单（也挂在标题"项目 ∨"上，见 FolderTree.qml）。
 *
 * state 由 Main.qml 的 treeMenuState() 给：
 *   { folderCount, openCount, itemCount, newestFirst, locateEnabled }
 * 用它把"已经全展开 / 已经全折叠"的两条置灰 —— 和 PyCharm 一样，
 * 点下去没事发生的那两条就不该是可点的样子。
 *
 * 动作名同样由 Main.qml 的 dispatch 解析（treeNew / treeLocate / treeExpandAll /
 * treeCollapseAll / treeSortNewest / treeSortOldest / treeHide），
 * 刷新复用已有的 refresh。
 */
function treeMenu(state, ov) {
    var s = state || {}
    var folders = Number(s.folderCount || 0)
    var open = Number(s.openCount || 0)
    var allOpen = folders > 0 && open >= folders
    var allClosed = open <= 0
    var newest = s.newestFirst !== false
    return applyOverrides([
        { label: "新建条目", act: "treeNew", icon: "plus" },
        { label: "刷新列表", act: "refresh", shortcut: "F5", icon: "refresh" },
        /* 当前标签不在列表里（磁盘文件 / 未命名空白文档）时没什么可定位的 */
        { label: "定位当前文件", act: "treeLocate", icon: "locate",
          disabled: s.locateEnabled === false },
        { separator: true },
        { label: "全部展开", act: "treeExpandAll", icon: "expand-all", disabled: allOpen },
        { label: "全部折叠", act: "treeCollapseAll", icon: "collapse-all", disabled: allClosed },
        { separator: true },
        { label: "最新在前", act: "treeSortNewest", checked: newest },
        { label: "最早在前", act: "treeSortOldest", checked: !newest },
        { separator: true },
        { label: "收起面板", act: "treeHide", icon: "minus" }
    ], ov)
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
