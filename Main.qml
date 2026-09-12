import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import "qml/components"
import "qml/models"
import "qml/utils"
import "js/FolderManager.js" as Folders
import "js/TimeUtils.js" as Time
import "js/EditorMenus.js" as Menus
/*
 * 点击条目 / 菜单里的"复制"都要回填系统剪贴板。
 *
 * 数据源现在是 QML 单例 Store（SmartClip.Globals，见 main.cpp 的
 * qmlRegisterSingletonInstance）—— 任何 QML 文档只要 import 该模块就能直接用，
 * 不再需要 js/ClipboardManager.js 那层包装（那个文件已删除）。
 */
import SmartClip.Globals 1.0

Rectangle {
    id: window
    /*
     * 窗口尺寸/标题/边框/可见性现在由 C++ 的 QWidget 负责（见 src/main.cpp），
     * 这里只是 QQuickWidget 里的内容层。
     */
    width: 1460; height: 900

    /*
     * 窗口本身必须透明，整窗的圆角才画得出来。
     *
     * 无边框（FramelessWindowHint）之后四角就是方角，
     * 所以要自己圆：真正负责裁剪的是下面 interfaceMask 那一层
     * （白底圆角矩形只当 alpha 遮罩，不参与显示）。
     * 窗口这一层留透明，遮罩裁掉的四角才会露出桌面，
     * 而不是露出一块方形的底色。
     *
     * 注意 palette.window 一并删掉：Fusion 样式会照着它刷一层
     * 不透明窗口底，那样四角又会被这块底色填回方形。
     */
    color: "transparent"

    // 整窗圆角半径（和 FolderTree / EditorArea 卡片的 radius: 10 同一套观感）
    readonly property real cornerRadius: 12

    /*
     * 是否处于最大化（或正在进出的过程中）。
     *
     * 真值不在 QML 里，而是由 C++ 的 winHelper 维护（src/WindowHelper.h）：
     * 展开 / 收拢的动画由它驱动，所以只有它知道当前算不算最大化；
     * 用户把窗口拖到屏幕顶端吸附最大化时，它也会跟着窗口状态更新。
     *
     * 只有这个状态为 false 时才允许拖四边改窗口大小
     * （见文件末尾的 ResizeEdge），最大化时那四条热区整个关掉。
     */
    readonly property bool maximized: Win.maximized

    /*
     * 去掉系统原生标题栏（截图里顶上那条白底、带图标 / 标题 /
     * 最小化 / 最大化 / 关闭的栏）。
     *
     * 三个窗口按钮因此改由底部栏最右侧的 WindowControls 接管
     * （见 qml/components/WindowControls.qml）。
     * 没有边框了，四边和四角的拖动改变大小也要自己补
     * （见文件末尾的 resizeHandles），否则窗口只能靠按钮最大化。
     */

    property string searchText: ""
    property var selectedItem: null
    /*
     * 当前分组（点哪个分组就是哪个）。
     *
     * 注意它**不再**用来画高亮：一级菜单不亮蓝底，只有选中的条目亮
     * （见 FolderTree.qml 里 delegate 的 rowHighlight）。
     * 留着它是因为"当前是哪一组"这件事本身还有用 ——
     * dispatch("folder:<key>") 那条命令就落在它上面，以后按分组做操作也认它。
     */
    property string activeFolder: "today"
    property var treeRows: []
    /* 欢迎页 / 编辑器：现在由"有没有打开的标签"决定，见 EditorArea */

    /*
     * 编辑器本体（EditorArea 里的原生 EditorView）。
     *
     * 工具栏、状态栏、菜单里的勾选与禁用状态都绑在它身上；
     * 命令也全发给它（见下面的 dispatch）。
     */
    readonly property var view: editor.view

    // ---- 左侧列表宽度（可由中间间隙拖动调整） ----
    property real folderTreeWidth: 300
    /*
     * 左树面板是不是被"收起面板"（标题栏那条 −）收起来了。
     *
     * 收起来时布局里的槽位宽度给 0（见下面 FolderTree 的 Layout.preferredWidth），
     * 宽度值本身留着，所以再展开还是原样。注意面板一收，它自己那排按钮
     * 也跟着没了 —— 真正能把它叫回来的入口是左边图标条上的文件夹格子
     * （见 midRow 里那个 navCell / window.toggleFolderTree）。
     */
    property bool folderTreeHidden: false

    /*
     * 左树最窄 240：标题栏现在摆着"新建 / 刷新 / 定位 / 全部折叠 / 全部展开 /
     * 更多 / 收起"七个 22px 的按钮（见 FolderTree.qml），再窄标题就要被压掉了。
     */
    readonly property real folderTreeMinWidth: 240
    readonly property real folderTreeMaxWidth: 600

    /*
     * 树里条目的排序：true = 最新在前（默认，和库里 ORDER BY id DESC 一致），
     * false = 最早在前（在 QML 这侧把列表翻过来，见 orderedEntries()）。
     * 由"更多"菜单里的那两条切换。
     */
    property bool newestFirst: true

    /*
     * 全局强调色（#4c96d8）。
     *
     * 顶栏右侧的窗口按钮和左侧导航条的 hover 底色都用它，
     * 和编辑区 / 选中态本来就是同一个蓝，只在这里写一次值。
     */
    readonly property color accentColor: "#4c96d8"

    readonly property var folders: [
        { key: "today",     label: "今天" },
        { key: "yesterday", label: "昨天" },
        { key: "week",      label: "近 7 天" },
        { key: "older",     label: "更早" }
    ]
    property var expanded: ({ "today": true, "yesterday": false, "week": false, "older": false })

    /*
     * 可改键清单的镜像（见 src/EditorController.h 的 shortcutItems）。
     *
     * 菜单里那个快捷键文字（js/EditorMenus.js 里硬编码的那份）在用户改过键
     * 之后就对不上了，所以菜单构造时把这份清单传进 EditorMenus，让它用
     * 当前生效的组合键覆盖显示值。C++ 侧改键会发 shortcutsChanged()，
     * 下面那个 Connections 负责重新取一份。
     */
    property var shortcutItems: Cmd.shortcutItems

    function shortcutLabel(name, fallback) {
        for (var i = 0; i < shortcutItems.length; ++i)
            if (shortcutItems[i].name === name)
                return shortcutItems[i].shortcut !== "" ? shortcutItems[i].shortcut
                                                        : (fallback !== undefined ? fallback : "")
        return fallback !== undefined ? fallback : ""
    }

    ClipboardModel { id: cbm; onChanged: window.rebuild() }

    function rebuild() { treeRows = Folders.buildTree(orderedEntries(), folders, expanded, Time.periodFor) }
    function refresh() { cbm.reload(searchText) }

    /*
     * 树里条目按什么顺序排。
     *
     * 库里给的就是"最新在前"（items() 里 ORDER BY id DESC），所以默认直接用它；
     * "最早在前"只需要把同一份列表倒过来 —— 数据源那边不用再查一次。
     */
    function orderedEntries() {
        var list = cbm.entries
        if (newestFirst)
            return list
        var out = []
        for (var i = list.length - 1; i >= 0; --i)
            out.push(list[i])
        return out
    }

    function toggleFolder(key) {
        var e = ({})
        for (var k in expanded) e[k] = expanded[k]
        e[key] = !e[key]
        expanded = e; activeFolder = key; selectedItem = null
        rebuild()
    }
    function activateFolder(key) {
        var e = ({})
        for (var k in expanded) e[k] = expanded[k]
        e[key] = true
        expanded = e; activeFolder = key; selectedItem = null
        rebuild()
    }

    /* 全部展开 / 全部折叠（左树标题栏那两个按钮，也是"更多"菜单里的两条） */
    function setAllFolders(open) {
        var e = ({})
        for (var i = 0; i < folders.length; ++i)
            e[folders[i].key] = open
        expanded = e
        rebuild()
    }

    /*
     * 收起 / 展开左树面板。
     *
     * 只是把槽位宽度收成 0（宽度值留着），恢复入口在左边图标条上 ——
     * 面板收起来之后它自己那排按钮也跟着消失了。
     */
    function toggleFolderTree() {
        folderTreeHidden = !folderTreeHidden
        Cmd.remember("treeHidden", folderTreeHidden ? "1" : "0")
    }

    /* 拖动分隔线之后把宽度记下来（拖动过程中不写，见 splitterMouse.onReleased） */
    function rememberTreeWidth() {
        Cmd.remember("treeWidth", String(Math.round(folderTreeWidth)))
    }

    function setNewestFirst(on) {
        if (newestFirst === on)
            return
        newestFirst = on
        Cmd.remember("treeNewestFirst", on ? "1" : "0")
        rebuild()
    }

    /*
     * 左树标题栏的 "+"：手工新建一条文本条目。
     *
     * 和剪贴板采集（ClipboardManager -> ClipboardStore::addText）不是一条路：
     * 那条按内容去重，这里故意不去重（见 ClipboardStore::createTextEntry），
     * 否则"再建一条空的"会被 INSERT OR IGNORE 静默丢掉。
     *
     * 建完立刻：清掉搜索框里的过滤（不然新条目可能根本不显示）-> 展开它所在的
     * 日期分组 -> 选中 -> 在编辑器里打开。注意**不**写系统剪贴板
     * （selectItem 的第二个参数）：刚建出来是空的，往剪贴板里塞个空串没意义。
     * 编辑完按 Ctrl+S 就写回库里那一条（见 EditorViewItem::saveCurrent）。
     */
    function newEntry() {
        if (searchText !== "") {
            searchText = ""
            topBar.clearSearch()
        }

        var id = Store.createTextEntry("")
        if (id <= 0) {
            Cmd.alert("新建失败", "无法写入剪贴板库")
            return
        }

        refresh()

        var item = null
        for (var i = 0; i < cbm.entries.length; ++i) {
            if (cbm.entries[i].id === id) {
                item = cbm.entries[i]
                break
            }
        }
        if (!item) {
            Cmd.alert("新建失败", "新条目没能读回来，请刷新列表")
            return
        }

        activateFolder(Time.periodFor(item.createdAt))
        selectItem(item, false)
    }

    /*
     * 当前标签对应的条目 id；当前标签不是列表里的条目（磁盘文件 / 未命名
     * 空白文档）时返回 -1。
     *
     * 按 id 认，不按标题认：标题是跟着正文变的（见 ClipboardStore::updateTextEntry），
     * 认标题迟早对不上。
     */
    function currentClipId() {
        var docs = view.documents
        var i = view.currentIndex
        if (i < 0 || i >= docs.length || !docs[i].clipboard)
            return -1
        var id = docs[i].clipId
        return (id === undefined || id === null) ? -1 : id
    }

    /* 准星按钮能不能点（转给 FolderTree，见那边 locateEnabled） */
    function canLocateCurrent() { return currentClipId() >= 0 }

    /*
     * 在左树里定位当前标签（标题栏那个准星按钮 / "更多"菜单里的"定位当前文件"）。
     *
     * 只做三件事：展开它所在的那一组 -> 把它选上（蓝条）-> 滚到它。
     * **不**动编辑器里的正文，也**不**写系统剪贴板 —— 点列表里的条目会顺手
     * 复制到剪贴板（那是"点条目"的语义），这里只是"告诉我它在哪儿"。
     *
     * 搜索框里有过滤的话先清掉：定位是明确的"带我去看"动作，被过滤掉就白点了
     * （和 newEntry 清过滤是同一个理由）。
     * 返回有没有真的定位到（没定位到通常是当前标签根本不在列表里）。
     */
    function locateCurrentItem() {
        var id = currentClipId()
        if (id < 0)
            return false

        if (searchText !== "") {
            searchText = ""
            topBar.clearSearch()
            refresh()
        }

        var item = null
        for (var i = 0; i < cbm.entries.length; ++i) {
            if (cbm.entries[i].id === id) {
                item = cbm.entries[i]
                break
            }
        }
        if (!item)
            return false

        var key = Time.periodFor(item.createdAt)
        if (!expanded[key]) {
            var e = ({})
            for (var k in expanded) e[k] = expanded[k]
            e[key] = true
            expanded = e
            rebuild()
        }

        selectedItem = item
        /*
         * 滚动要等这一帧的列表更新完再做（树刚重建过，行下标这会儿还在算）——
         * Qt.callLater 就是"这一轮事件处理完再调"。
         */
        Qt.callLater(function () { folderTree.scrollToItem(id) })
        return true
    }

    /*
     * 点左侧条目。
     *
     *   图片 -> 走图片预览（没有正文可编辑）；
     *   文本 -> 载入编辑器标签（正文由 C++ 按 id 从库里取，
     *           全程不经过 QML 属性，见 src/EditorViewItem.h）。
     *
     * 两种都继续回填系统剪贴板 —— 这是这个应用本来的用途。
     * 只有"新建条目"那条路会传 copy = false：那时候条目还是空的，
     * 塞进剪贴板没有意义（见 newEntry）。
     */
    function selectItem(item, copy) {
        selectedItem = item

        if (!item) {
            editor.previewItem = null
            return
        }

        if (item.type === "image") {
            editor.previewItem = item
        } else {
            editor.previewItem = null
            view.openClipboardItem(item.id, item.title)
            view.requestEditorFocus()
        }

        if (copy !== false)
            Store.copyItem(item.id)
    }

    /* 当前标签是不是"来自剪贴板库"的那一类（在库里、没有磁盘文件） */
    function isClipboardDocument() {
        var docs = view.documents
        var i = view.currentIndex
        return i >= 0 && i < docs.length && docs[i].clipboard === true
    }

    /* ------------------------------------------------------------------
     * 命令分发
     *
     * 工具栏按钮、菜单项、快捷键（C++ 侧 QAction，见 EditorController）
     * 三条入口最后都落到这里，行为只有一份。
     * ---------------------------------------------------------------- */

    function commentPrefix() {
        var lang = view.language
        if (lang === "python" || lang === "bash" || lang === "yaml"
                || lang === "perl" || lang === "ruby" || lang === "makefile")
            return "#"
        if (lang === "sql" || lang === "lua")
            return "--"
        if (lang === "batch")
            return "REM "
        if (lang === "properties")
            return ";"
        if (lang === "fortran")
            return "!"
        if (lang === "tex")
            return "%"
        return "//"
    }

    function newFile() {
        view.newDocument()
        editor.previewItem = null
        view.requestEditorFocus()
    }

    function openFile() {
        var path = Cmd.openFileDialog()
        if (path === "")
            return
        if (view.openFile(path) < 0)
            Cmd.alert("打开失败", view.lastError)
        else {
            editor.previewItem = null
            view.requestEditorFocus()
        }
    }

    function saveFile() {
        if (!view.hasDocument)
            return false
        /*
         * 剪贴板条目在库里、没有磁盘文件：Ctrl+S 走的是"写回库里那一条"
         * （见 EditorViewItem::saveCurrent 的剪贴板分支），不该弹"另存为"。
         * 只有真正的未命名空白文档才需要问路径。
         */
        if (view.filePath === "" && !isClipboardDocument())
            return saveFileAs()
        if (!view.saveCurrent()) {
            Cmd.alert("保存失败", view.lastError)
            return false
        }
        return true
    }

    function saveFileAs() {
        if (!view.hasDocument)
            return false
        var suggested = view.filePath !== "" ? Cmd.fileNameOf(view.filePath)
                                             : view.displayName + ".txt"
        var path = Cmd.saveFileDialog(suggested)
        if (path === "")
            return false
        if (!view.saveCurrentAs(path)) {
            Cmd.alert("保存失败", view.lastError)
            return false
        }
        return true
    }

    /* 关闭一个标签（有未保存改动会先问）。返回是否真的关掉了。 */
    function closeTab(index) {
        var docs = view.documents
        if (index === undefined || index === null || index < 0)
            index = view.currentIndex
        if (index < 0 || index >= docs.length)
            return false

        if (docs[index].modified) {
            var answer = Cmd.confirmSave(docs[index].title)
            if (answer === 2)
                return false
            if (answer === 0) {
                view.activateDocument(index)
                if (!saveFile())
                    return false
            }
        }

        view.closeDocument(index)
        if (view.documents.length === 0)
            editor.previewItem = null
        return true
    }

    /* 从后往前关，前面的下标才不会跟着挪 */
    function closeTabs(indices) {
        indices.sort(function (a, b) { return b - a })
        for (var i = 0; i < indices.length; ++i) {
            if (!closeTab(indices[i]))
                return
        }
    }

    /*
     * 关掉除 index 之外的标签。
     *
     * index 不给就是"当前标签"（菜单栏 / 快捷键那条路的语义）；
     * 标签右键菜单会传**点中的那一个** —— 右键点的标签未必是激活的，
     * 不传的话"关闭其他"会把用户刚点的那一个也关掉。
     */
    function closeOtherTabs(index) {
        if (index === undefined || index === null)
            index = view.currentIndex
        var rest = []
        for (var i = 0; i < view.documents.length; ++i)
            if (i !== index) rest.push(i)
        closeTabs(rest)
    }

    function closeAllTabs() {
        var all = []
        for (var i = 0; i < view.documents.length; ++i)
            all.push(i)
        closeTabs(all)
    }

    function saveAll() {
        for (var i = 0; i < view.documents.length; ++i) {
            if (!view.documents[i].modified)
                continue
            view.activateDocument(i)
            if (!saveFile())
                return
        }
    }

    /*
     * 内容区 tab 上的右键菜单。
     *
     * 落点和菜单栏那套不一样：菜单栏是 openFor（挂在控件正下方），
     * 这里是 openAtPoint —— 菜单左上角紧贴鼠标右键的那一点。
     * 条目见 js/EditorMenus.js 的 tabMenu（关闭 / 关闭其他 / 关闭全部）。
     */
    function openTabMenu(index, anchor, x, y) {
        ddMenu.openAtPoint(anchor, x, y, Menus.tabMenu(view, index, shortcutOverrides()))
    }

    /* name -> 当前生效的快捷键。菜单里写的是出厂默认值，改过键的要以这份为准 */
    function shortcutOverrides() {
        var ov = ({})
        for (var i = 0; i < shortcutItems.length; ++i) {
            var item = shortcutItems[i]
            if (item && item.name)
                ov[item.name] = item.shortcut
        }
        return ov
    }

    /*
     * tab 右键菜单里那几条的动作名（自检核对用，见 src/SelfTest.cpp）。
     *
     * 走的是和 openTabMenu 完全同一份构造：断言里看到的条目就是菜单里弹出的条目。
     */
    function tabMenuActs(index) {
        var items = Menus.tabMenu(view, index === undefined ? 0 : index, shortcutOverrides())
        var out = []
        for (var i = 0; i < items.length; ++i)
            out.push(items[i] && items[i].act !== undefined ? String(items[i].act) : "separator")
        return out
    }

    /*
     * 设置菜单里各条的动作名（自检核对用，见 src/SelfTest.cpp）。
     *
     * 同样是"和弹出的那份同一个构造"：工具栏点"设置"走的就是
     * Menus.settingsMenu(view, ...)。行高的档位表只在 js/EditorMenus.js 里
     * 有一份，自检要认的就是它到底给了哪几档。
     */
    function settingsMenuActs() {
        var items = Menus.settingsMenu(view, shortcutOverrides())
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 视图菜单里各条的动作名（自检核对用，见 src/SelfTest.cpp）。
     *
     * 同样走"和弹出的那份同一个构造"：点"视图"弹出的就是 Menus.viewMenu。
     * 自检据此确认新的两条竖线开关确实进了菜单，而不是只在 C++ 里有属性。
     */
    function viewMenuActs() {
        var items = Menus.viewMenu(view, shortcutOverrides())
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 左树"更多"菜单要用到的当前状态（见 js/EditorMenus.js 的 treeMenu）。
     *
     * 给的是"已经全展开 / 已经全折叠"这类判断要用的量：菜单据此把点下去
     * 没事发生的那两条置灰。
     */
    function treeMenuState() {
        var open = 0
        for (var k in expanded)
            if (expanded[k]) ++open
        return { folderCount: folders.length,
                 openCount: open,
                 itemCount: cbm.entries.length,
                 newestFirst: newestFirst,
                 locateEnabled: canLocateCurrent() }
    }

    /*
     * 左树"更多"菜单里各条的动作名（自检核对用，见 src/SelfTest.cpp）。
     * 和弹出的那份同一个构造。
     */
    function treeMenuActs() {
        var items = Menus.treeMenu(treeMenuState(), shortcutOverrides())
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 左树当前状态（自检量"全部折叠 / 全部展开 / 收起面板"用，见 uiState）。
     *
     * panelWidth 是布局算出来的真实槽位宽度：面板收起来时它必须是 0，
     * 只把 folderTreeHidden 置上而宽度没跟着走，从界面上是能一眼看出来的。
     */
    function treeState() {
        var open = 0
        for (var k in expanded)
            if (expanded[k]) ++open
        return { folderCount: folders.length,
                 openFolders: open,
                 rows: treeRows.length,
                 items: cbm.entries.length,
                 hidden: folderTreeHidden,
                 width: folderTreeWidth,
                 panelWidth: folderTree.width,
                 newestFirst: newestFirst,
                 /* 定位用：当前标签对应的条目 id / 按钮是不是可点 / 选中的是哪条 */
                 currentClipId: currentClipId(),
                 locateEnabled: canLocateCurrent(),
                 selectedId: selectedItem ? selectedItem.id : -1,
                 /* 定位的最后一步（滚进可视区）有没有真的生效 */
                 locatedVisible: folderTree.rowVisible(selectedItem ? selectedItem.id : -1),
                 /* 亮着蓝底的行：分组必须恒为 0，条目最多 1（见 FolderTree.highlightCounts） */
                 highlighted: folderTree.highlightCounts() }
    }

    /*
     * 展开"视图"菜单里的某个子菜单（自检用，见 src/SelfTest.cpp）。
     *
     * 界面上这一步是"鼠标停到 语言 / 编码 / 换行符 上"（见 DropdownMenu.qml 里
     * MenuEntryItem 的 onEntered），C++ 侧悬停不出来 —— 但走的是同一条路：
     * 用同一份 Menus.menuItems("视图") 构造，找到那条带 items 的子菜单条目，
     * 把它交给 ddMenu.openSubmenu()。
     */
    function openSubmenuFor(act) {
        var items = Menus.menuItems("视图", view, shortcutOverrides())
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].submenu === true && items[i].act === act) {
                /*
                 * 找出界面上那一条的委托，把它交给 openSubmenu ——
                 * 和鼠标悬停走的是同一条路（连"子菜单顶边对齐哪一行"用的
                 * 都是同一个 y），不是在测试里另算一遍。
                 */
                var entryItem = ddMenu.entryItemFor(act)
                return entryItem ? ddMenu.openSubmenu(items[i].items, entryItem) : false
            }
        }
        return false
    }

    /* 关掉下拉菜单（自检收尾用，见 src/SelfTest.cpp） */
    function closeMenu() { ddMenu.close() }

    /*
     * 编辑区里的右键菜单。
     *
     * 用的就是菜单栏那份"编辑"菜单（Menus.editMenu，条目 / 图标 / 快捷键 /
     * 可用状态都是现成的），所以右键弹出来的观感和点"编辑"完全一致 ——
     * 原来那是 Scintilla 自带的 QtWidgets 菜单，英文、风格也不搭。
     *
     * (x, y) 是场景坐标（= QML 窗口内容区坐标），由 EditorView 的
     * contextMenuRequested 信号给（见 src/EditorViewItem.cpp 的 eventFilter）。
     * anchor 传 null：坐标直接按宿主坐标用（见 DropdownMenu.openAtPoint）。
     */
    function openEditorContextMenu(x, y) {
        ddMenu.openAtPoint(null, x, y, Menus.editMenu(view, shortcutOverrides()))
    }

    /*
     * 自检用：模拟"鼠标停到子菜单里第 index 条上"，返回停完之后子菜单还开着没。
     *
     * 这一步界面上就是鼠标往右挪进子菜单，C++ 侧悬停不出来；
     * 走的是 DropdownMenu.hoverEntry()，和委托的 onEntered 同一份判断。
     */
    function hoverSubmenuEntry(index) {
        return ddMenu.hoverSubmenuEntry(index === undefined ? 0 : index)
    }

    function showFind(replace) {
        if (!view.hasDocument)
            return
        if (replace) editor.findBar.openReplace()
        else editor.findBar.openFind()
    }

    function findStep(forward) {
        if (!editor.findBar.opened || editor.findBar.currentText() === "") {
            showFind(false)
            return
        }
        editor.findBar.findNext(forward)
    }

    function gotoLine() {
        if (!view.hasDocument)
            return
        var line = Cmd.askLineNumber(view.lineCount, view.cursorLine)
        if (line > 0) {
            view.gotoLine(line)
            view.requestEditorFocus()
        }
    }

    function toggleWrap() {
        view.wrapEnabled = !view.wrapEnabled
        Cmd.remember("wrap", view.wrapEnabled ? "1" : "0")
    }

    function toggleLineNumbers() {
        view.lineNumbersVisible = !view.lineNumbersVisible
        Cmd.remember("lineNumbers", view.lineNumbersVisible ? "1" : "0")
    }

    function toggleWhitespace() {
        view.whitespaceVisible = !view.whitespaceVisible
        Cmd.remember("whitespace", view.whitespaceVisible ? "1" : "0")
    }

    function showShortcuts() {
        settingsPanel.show("shortcuts")
    }

    function showAbout() {
        settingsPanel.show("about")
    }

    function dispatch(act) {
        if (act === undefined || act === null || act === "" || act === "none")
            return

        /* ---- 带参数的命令 ---- */
        if (act.indexOf("fontSize:") === 0) {
            var px = parseInt(act.substring(9))
            if (!isNaN(px) && px >= 6 && px <= 72)
                editor.editorFontSize = px      // 走绑定，见 EditorArea.editorFontSize
            return
        }
        if (act.indexOf("commentFontSize:") === 0) {
            var cpx = parseInt(act.substring(16))
            if (!isNaN(cpx) && cpx >= 0 && cpx <= 72)
                view.commentFontPixelSize = cpx
            return
        }
        if (act.indexOf("font:") === 0) { view.fontFamily = act.substring(5); return }
        if (act.indexOf("lineHeight:") === 0) {
            var lhf = parseFloat(act.substring(11))
            if (!isNaN(lhf))
                view.lineHeightFactor = lhf     // 越界由 C++ 侧夹住（1.0 ~ 3.0）
            return
        }
        /* 设置面板上的"行高 − / 行高 +"：按档位表走一格（表在 js/EditorMenus.js） */
        if (act === "lineHeightDown" || act === "lineHeightUp") {
            view.lineHeightFactor =
                Menus.stepLineHeight(view.lineHeightFactor, act === "lineHeightUp" ? 1 : -1)
            return
        }
        if (act.indexOf("lang:") === 0) { view.language = act.substring(5); return }
        if (act.indexOf("encoding:") === 0) { view.encoding = act.substring(9); return }
        if (act.indexOf("eol:") === 0) { view.eolMode = act.substring(4); return }
        if (act.indexOf("menu:") === 0) { topBar.openGroup(act.substring(5)); return }
        if (act.indexOf("folder:") === 0) { activateFolder(act.substring(7)); return }
        /* ---- 左侧项目树（标题栏那排按钮 / 标题上的"更多"菜单） ---- */
        if (act === "treeNew") { newEntry(); return }
        if (act === "treeLocate") { locateCurrentItem(); return }
        if (act === "treeExpandAll") { setAllFolders(true); return }
        if (act === "treeCollapseAll") { setAllFolders(false); return }
        if (act === "treeHide") { toggleFolderTree(); return }
        if (act === "treeSortNewest") { setNewestFirst(true); return }
        if (act === "treeSortOldest") { setNewestFirst(false); return }

        /*
         * 带下标的标签动作（tab 右键菜单用，见 openTabMenu）。
         * 作用在"被右键的那一个"标签上，而不是当前标签。
         */
        if (act.indexOf("closeTab:") === 0) { closeTab(parseInt(act.substring(9))); return }
        if (act.indexOf("closeOthers:") === 0) {
            closeOtherTabs(parseInt(act.substring(12)))
            return
        }

        /* ---- 文件 ---- */
        if (act === "new") { newFile(); return }
        if (act === "open") { openFile(); return }
        if (act === "save") { saveFile(); return }
        if (act === "saveAs") { saveFileAs(); return }
        if (act === "saveAll") { saveAll(); return }
        if (act === "closeTab") { closeTab(view.currentIndex); return }
        if (act === "closeOtherTabs") { closeOtherTabs(); return }
        if (act === "closeAllTabs") { closeAllTabs(); return }
        if (act === "print") { view.printDocument(); return }
        if (act === "refresh") { refresh(); return }
        if (act === "quit") { Win.closeWindow(); return }
        if (act === "clearsearch") { searchText = ""; topBar.clearSearch(); return }

        /* ---- 编辑 ---- */
        if (act === "undo") { view.undo(); return }
        if (act === "redo") { view.redo(); return }
        if (act === "cut") { view.cut(); return }
        if (act === "copy") { view.copy(); return }
        if (act === "paste") { view.paste(); return }
        if (act === "selectAll") { view.selectAll(); return }
        if (act === "copyLine") { view.copyCurrentLine(); return }
        if (act === "copyAll") { view.copyAll(); return }
        if (act === "deleteLine") { view.deleteLine(); return }
        if (act === "duplicateLine") { view.duplicateLine(); return }
        if (act === "toggleComment") { view.toggleComment(commentPrefix()); return }
        if (act === "toggleReadOnly") { view.readOnly = !view.readOnly; return }

        /* ---- 查找 ---- */
        if (act === "find") { showFind(false); return }
        if (act === "replace") { showFind(true); return }
        if (act === "findNext") { findStep(true); return }
        if (act === "findPrev") { findStep(false); return }
        if (act === "goto") { gotoLine(); return }

        /* ---- 视图 ---- */
        if (act === "zoomIn") { view.zoomIn(); return }
        if (act === "zoomOut") { view.zoomOut(); return }
        if (act === "zoomReset") { view.zoomReset(); return }
        if (act === "toggleWrap") { toggleWrap(); return }
        if (act === "toggleLineNumbers") { toggleLineNumbers(); return }
        if (act === "toggleWhitespace") { toggleWhitespace(); return }
        if (act === "toggleIndentGuides") {
            view.indentGuidesVisible = !view.indentGuidesVisible
            Cmd.remember("indentGuides", view.indentGuidesVisible ? "1" : "0")
            return
        }
        if (act === "toggleGutterLine") {
            view.gutterLineVisible = !view.gutterLineVisible
            Cmd.remember("gutterLine", view.gutterLineVisible ? "1" : "0")
            return
        }
        if (act === "toggleRuler") {
            view.rulerVisible = !view.rulerVisible
            Cmd.remember("rulerVisible", view.rulerVisible ? "1" : "0")
            return
        }
        /* 字数参考线列号：菜单里的固定档位（rulerColumn:80）走这条 */
        if (act.indexOf("rulerColumn:") === 0) {
            var rc = parseInt(act.substring(12))
            if (!isNaN(rc) && rc >= 1 && rc <= 2000)
                view.rulerColumn = rc
            return
        }
        /* 自定义列号：弹一个整数输入框（原生 QInputDialog，见 Cmd.askRulerColumn） */
        if (act === "rulerColumnAsk") {
            var picked = Cmd.askRulerColumn(view.rulerColumn)
            if (picked > 0)
                view.rulerColumn = picked
            return
        }
        if (act === "toggleFolding") { view.foldingEnabled = !view.foldingEnabled; return }
        if (act === "foldAll") { view.foldAll(); return }
        if (act === "unfoldAll") { view.unfoldAll(); return }

        /* ---- 其它 ---- */
        if (act === "shortcuts") { showShortcuts(); return }
        if (act === "settings") { showShortcuts(); return }
        if (act === "about") { showAbout(); return }
    }

    /*
     * 菜单真正画在宿主窗口里的左上角（给自检量，见 uiState 的 menuX / menuY）。
     *
     * 为什么不直接读 ddMenu.x：菜单是 popupType: Popup.Window，它有**自己的
     * 原生窗口**（原因见 DropdownMenu.qml 开头）—— 弹窗内容在自己那个窗口里
     * 就画在 (0,0)，水平位置全在窗口几何上，读 x/y 永远是 0（实测就是这样，
     * 自检一开始读出 (0,0)）。所以改成用屏幕坐标反推：
     *
     *     可见菜单左上角(全局) - 宿主窗口左上角(全局)
     *
     * 取的是 background 而不是 contentItem：菜单可见的那块矩形就是 background
     * （margins: 0，正好铺满弹窗），contentItem 按 padding 内缩了 4px。
     */
    function menuTopLeft() {
        if (!ddMenu.opened || !ddMenu.background)
            return Qt.point(0, 0)
        var a = ddMenu.background.mapToGlobal(0, 0)
        var b = window.mapToGlobal(0, 0)
        return Qt.point(a.x - b.x, a.y - b.y)
    }

    /* 菜单栏 / 旧接口名 */
    function handleCommand(act) { dispatch(act) }

    /*
     * 界面侧绑定状态，给 `--self-test` 用（见 src/SelfTest.h）。
     *
     * 工具栏按钮能不能点、状态栏有没有拿到编辑器，这些都是 QML 绑定，
     * C++ 侧看不到；自检时由这里把结果报出去。
     */
    function uiState() {
        return {
            hasView: view !== null && view !== undefined,
            topBarHasView: topBar.view !== null && topBar.view !== undefined,
            statusHasDoc: statusBar.hasDoc,
            findOpened: editor.findBar.opened,
            findReplaceVisible: editor.findBar.replaceVisible,
            /* 下拉菜单：长菜单（语言 27 项）必须限高 + 可滚动，
               否则会一路盖住左侧导航栏（见 DropdownMenu.maxMenuHeight） */
            menuOpened: ddMenu.opened,
            menuHeight: ddMenu.menuHeight,
            menuContentHeight: ddMenu.entriesHeight,
            menuScrollable: ddMenu.scrollable,
            /* 有图标的菜单：图标在左、快捷键在右（工具栏已取消） */
            menuHasIcons: ddMenu.hasIcons,

            /*
             * 子菜单（视图 -> 语言 / 编码 / 换行符）：右边那一块面板。
             *
             * submenuInset 是那一块相对弹窗内容区的位置，必须等于主栏宽度
             * ——自检据此确认子菜单在**右边**，不是又盖在主菜单上；
             * submenuTop 是它的顶边，必须等于父级那一条所在的行
             * （submenuRowY），也就是"从鼠标停的那一条旁边伸出来"；
             * menuTotalWidth / menuTotalHeight 是弹窗实际尺寸。
             */
            submenuOpened: ddMenu.submenuOpened,
            submenuHeight: ddMenu.submenuHeight,
            submenuContentHeight: ddMenu.subEntriesHeight,
            submenuScrollable: ddMenu.submenuScrollable,
            submenuInset: ddMenu.submenuInset,
            submenuTop: ddMenu.submenuTop,
            submenuRowY: ddMenu.submenuRowY,
            menuPaneWidth: ddMenu.paneWidth,
            menuPaneGap: ddMenu.paneGap,
            menuTotalWidth: ddMenu.width,
            menuTotalHeight: ddMenu.height,

            /*
             * 内容区 tab 的右键菜单落在哪。
             *
             * 取的是菜单**实际画在窗口里**的那个左上角（menuTopLeft：弹窗是独立
             * 原生窗口，读 ddMenu.x 只会得到 0），自检把同一个点传给 openTabMenu，
             * 再拿这里的值对齐 —— 要的就是"菜单左上角紧贴鼠标右键那一点"。
             */
            menuX: window.menuTopLeft().x,
            menuY: window.menuTopLeft().y,

            /*
             * tab 栏和顶上那条横向滚动条的几何（tab 撑满容器时才出现）。
             * 自检量的是"横条有没有压到容器圆角上、有没有盖住标签"。
             */
            tabBar: editor.tabBarState(),

            /*
             * 正文卡片和里面那个原生编辑器的几何。
             * 自检量的是"横条离卡片底边有多远"和"左右有没有留一点点余量"
             * （让开一点点才是卡片下方两角圆角的保命条件，见 EditorArea.editorCardState）。
             */
            editorCard: editor.editorCardState(),

            /*
             * 查找栏几何（自检量"两个输入框一样长、圆角、左右有间隙"）。
             * 从 FindBar 自己报上来 —— 布局是 QML 算的，只有它知道最终宽度。
             */
            findFieldWidth: editor.findBar.findFieldWidth,
            findReplaceFieldWidth: editor.findBar.replaceFieldWidth,
            findPanelLeftGap: editor.findBar.panelLeftGap,
            findPanelRightGap: editor.findBar.panelRightGap,
            findPanelRadius: editor.findBar.panelRadius,
            findBarHeight: editor.findBar.barHeight,

            /*
             * 工具提示的配色（自检用）。
             *
             * main.cpp 里给应用调色板设了 ToolTipBase / ToolTipText，Fusion 的
             * ToolTip 模板就是读这两个角色画的 —— 这里读的是 **QML 这侧**看到的
             * 值，用来确认那份调色板真的传到了界面上（不是只设了 C++ 那一份）。
             * 注意：QML 这侧的提示框已经不走 ToolTip 控件了（见 AppToolTip.qml），
             * 这两个值只是"调色板确实到了 QML"的证据。
             */
            toolTipBase: window.palette.toolTipBase,
            toolTipText: window.palette.toolTipText,

            /*
             * 深色提示框组件自己的配色（见 AppToolTip.qml）。
             *
             * 界面上真正的提示都用那个组件画 —— 样式（Fusion）那套取的是平台主题
             * 的浅色调色板，应用调色板 / QML 调色板都压不住（实测还是 #FFFFE1）。
             * 这里挂一个不显示的实例，只为把它的配色报给自检。
             */
            tipBackground: tipProbe.tipBackground,
            tipTextColor: tipProbe.tipTextColor,
            tipRadius: tipProbe.background.radius,
            tipDelay: tipProbe.appearDelay,

            /*
             * 分隔线热区的纵向范围（自检里量它有没有越界）。
             *
             * splitterTop / splitterBottom 必须和中间行（midRow）的上下边界
             * 对齐：高了会压住顶栏菜单，低了会压住底部状态栏，
             * 那两条上也就能拖动左树宽度（改之前就是这个毛病）。
             */
            splitterTop: splitterMouse.y,
            splitterBottom: splitterMouse.y + splitterMouse.height,
            midRowTop: window.mapFromItem(midRow, 0, 0).y,
            midRowBottom: window.mapFromItem(midRow, 0, 0).y + midRow.height,
            topBarHeight: topBar.height,
            statusBarHeight: statusBar.height,
            windowWidth: window.width,
            windowHeight: window.height,

            /*
             * 左树标题栏那排工具按钮 / 面板的折叠状态（自检用）。
             *
             * toolbarButtons 是标题栏里**实际摆出来的**按钮个数 ——
             * 不是写死的常量，少摆一个自检就会报出来（见 SelfTest.cpp
             * 的"标题栏那排按钮"那一节）。
             */
            treeToolbarButtons: folderTree.toolbarButtonCount,
            tree: window.treeState()
        }
    }

    /*
     * 最大化 / 还原。
     *
     * 不再直接 showMaximized() / showNormal()：那两步由 C++ 的
     * winHelper 接管（尺寸一次到位，界面做一次淡入，见
     * src/WindowHelper.cpp 顶部为什么不做几何动画）。
     */
    function toggleMaximize() { Win.toggleMaximize() }

    /*
     * 分段计时"点开一条长文本"这条路径。
     *
     * 之前只测到一个总数（2 秒），无法判断是"切换界面"、"写剪贴板"、
     * 还是"虚拟行重算"造成的，所以这里把每一步单独计时。
     */
    Component.onCompleted: {
        /*
         * 窗口已经由 C++ 侧交给 WindowHelper（main.cpp 里 attachWidget），
         * 这里只需要把数据刷出来，并把上次的编辑器视图设置恢复回来。
         */

        /*
         * 左树：宽度 / 是不是收起来 / 条目排序都按上次的样子回来。
         * 必须在 refresh() 之前 —— 列表就是按 newestFirst 排的。
         * 宽度照夹一遍：设置文件被手改成 3 这种值，面板会窄到连按钮都放不下。
         */
        var treeW = parseFloat(Cmd.recall("treeWidth", "300"))
        if (!isNaN(treeW))
            folderTreeWidth = Math.max(folderTreeMinWidth,
                                       Math.min(folderTreeMaxWidth, treeW))
        folderTreeHidden = Cmd.recall("treeHidden", "0") === "1"
        newestFirst = Cmd.recall("treeNewestFirst", "1") === "1"

        refresh()

        /*
         * 恢复上次的字号 / 换行 / 行号 / 空白字符设置。
         * 字号出厂默认 12（见 EditorArea.editorFontSize 与 EditorViewItem）。
         */
        var size = parseInt(Cmd.recall("fontSize", "12"))
        if (!isNaN(size) && size >= 6 && size <= 72)
            editor.editorFontSize = size

        /* 注释字号 / 字体家族也是上次怎么设的怎么回来 */
        var commentSize = parseInt(Cmd.recall("commentFontSize", "0"))
        if (!isNaN(commentSize) && commentSize >= 0 && commentSize <= 72)
            view.commentFontPixelSize = commentSize
        var family = Cmd.recall("fontFamily", "Consolas")
        if (family !== "")
            view.fontFamily = family
        /* 行高倍数：1.0 = 跟随字体（越界值由 C++ 侧夹住） */
        var lineHeight = parseFloat(Cmd.recall("lineHeight", "1"))
        if (!isNaN(lineHeight))
            view.lineHeightFactor = lineHeight

        view.wrapEnabled = Cmd.recall("wrap", "0") === "1"
        view.lineNumbersVisible = Cmd.recall("lineNumbers", "1") === "1"
        view.whitespaceVisible = Cmd.recall("whitespace", "0") === "1"
        view.indentGuidesVisible = Cmd.recall("indentGuides", "1") === "1"

        /*
         * 行号右侧的分隔竖线、字数参考线（默认开，"一行 120 字"）。
         * 列号越界/写坏就退回默认 120，别让设置文件里的垃圾值把线顶到画面外。
         */
        view.gutterLineVisible = Cmd.recall("gutterLine", "1") === "1"
        view.rulerVisible = Cmd.recall("rulerVisible", "1") === "1"
        var rulerCol = parseInt(Cmd.recall("rulerColumn", "120"))
        view.rulerColumn = (!isNaN(rulerCol) && rulerCol >= 1 && rulerCol <= 2000)
                           ? rulerCol : 120
    }

    Connections { target: Store; function onChanged() { window.refresh() } }

    /* 快捷键（C++ 侧注册的 QAction，见 src/EditorController.h）走到同一份分发 */
    Connections {
        target: Cmd
        function onCommandRequested(name) { window.dispatch(name) }
        /* 改键 / 恢复默认之后，菜单里的快捷键文字要跟着变 */
        function onShortcutsChanged() { window.shortcutItems = Cmd.shortcutItems }
    }

    /* 需要记住的编辑器视图设置 */
    Connections {
        target: editor.view

        function onFontChanged() {
            Cmd.remember("fontSize", String(editor.view.fontPixelSize))
            Cmd.remember("commentFontSize", String(editor.view.commentFontPixelSize))
            Cmd.remember("fontFamily", editor.view.fontFamily)
            Cmd.remember("lineHeight", String(editor.view.lineHeightFactor))
        }
        function onWrapChanged() {
            Cmd.remember("wrap", editor.view.wrapEnabled ? "1" : "0")
        }
        function onLineNumbersChanged() {
            Cmd.remember("lineNumbers", editor.view.lineNumbersVisible ? "1" : "0")
        }
        function onWhitespaceChanged() {
            Cmd.remember("whitespace", editor.view.whitespaceVisible ? "1" : "0")
        }
        function onIndentGuidesChanged() {
            Cmd.remember("indentGuides", editor.view.indentGuidesVisible ? "1" : "0")
        }
        function onGutterLineChanged() {
            Cmd.remember("gutterLine", editor.view.gutterLineVisible ? "1" : "0")
        }
        function onRulerChanged() {
            Cmd.remember("rulerVisible", editor.view.rulerVisible ? "1" : "0")
            Cmd.remember("rulerColumn", String(editor.view.rulerColumn))
        }
        function onErrorOccurred(message) {
            /*
             * 自检模式（--self-test）里不弹模态框。
             *
             * 自检有一条"故意往不存在的路径写文件"的检查项，它会触发这个信号；
             * 照平时那样弹 QMessageBox 就会卡在模态框上，桌面上留一个点不掉的
             * 窗口（真踩过：自检进程挂住，用户看到一个"出错了"对话框）。
             */
            if (!Cmd.selfTestMode)
                Cmd.alert("出错了", message)
        }

        /* 编辑区里按下右键：弹 QML 那套"编辑"菜单（见 openEditorContextMenu） */
        function onContextMenuRequested(x, y) {
            window.openEditorContextMenu(x, y)
        }
    }

    /*
     * 下拉菜单。
     *
     * 它是**原生弹窗**（见 qml/components/DropdownMenu.qml 开头）：
     * 编辑区是原生子窗口，场景内的浮层会被它盖住，所以菜单必须自己是一个
     * 同级原生窗口。这里不再有 anchors.fill —— Popup 不是 Item。
     */
    DropdownMenu {
        id: ddMenu
        parent: window
        onSelected: (act) => window.dispatch(act)
    }

    /*
     * 自检用：界面上真正的提示框都是 AppToolTip 这个组件（深色，自己画的
     * Popup），这里挂一个不显示的实例，只为把那套配色报给自检（见 uiState）。
     */
    AppToolTip {
        id: tipProbe
        visible: false
    }

    /*
     * 设置面板（快捷键 / 关于）。
     *
     * 和下拉菜单一样是**原生弹窗**（见 qml/components/SettingsPanel.qml 开头）：
     * 左边是操作步骤、右边是具体内容，编辑区那个原生子窗口盖不住它。
     */
    SettingsPanel {
        id: settingsPanel
        parent: window
        view: window.view
        entries: window.shortcutItems
        onCommandRequested: (act) => window.dispatch(act)
    }

    /*
     * 整个界面套一层圆角容器。
     *
     * 为什么不直接把 ApplicationWindow 自己的 color 设成圆角矩形：
     * 窗口的 color 只画在窗口最底层，上层任何铺满的矩形
     * （顶栏、状态栏、编辑区）都会把四角重新盖成方角出来。
     * 所以改成“先正常画完整个界面，再按圆角形状裁一刀”。
     *
     * 做法是 MultiEffect 的蒙版：它把 contentRoot 整棵子树
     * 渲染成一张纹理，再用 maskTexture 的 alpha 通道去裁。
     * 白 = 保留，透明 = 挖掉，于是四角被啃掉、露出透明窗口，
     * 圆角才真正成立（clip: true 做不到这件事 —— Qt Quick 的
     * clip 只认矩形，radius 不参与裁剪）。
     */
    Item {
        id: interfaceRoot

        anchors.fill: parent

        /*
         * 最大化 / 还原的淡入。
         *
         * 窗口尺寸是一次到位的（见 src/WindowHelper.cpp 里为什么不做几何
         * 动画），所以这里用一次短促的"压暗 -> 回全亮"把这次跳变盖过去。
         *
         * 从暗处淡入（而不是从亮处淡出）：界面本身是深色，压暗再回来
         * 看起来是"刷新了一下"，比发白自然 —— 之前试过淡到 0.72 再回来，
         * 屏幕上一片灰白，就是那个味道不对。
         *
         * 淡入只作用在这一层的 opacity 上，不参与布局，
         * 所以内容再多（三千行文本也一样）都不会因此变慢。
         */
        property bool transitioning: Win.transitioned
        opacity: 1.0

        NumberAnimation on opacity {
            running: interfaceRoot.transitioning
            from: 0.45
            to: 1.0
            duration: 150
            easing.type: Easing.OutCubic
        }

        Rectangle {
            id: contentRoot

            anchors.fill: parent

            // 卡片之外那圈底（编辑区右侧 5px 间隙、左树面板左侧的留白）
            color: "#313335"

            /*
             * 圆角不在这里做。
             *
             * 原来（QQuickWindow 时代）是把整棵子树渲染成纹理、再用蒙版裁圆角；
             * 换成 QWidget + QQuickWidget 之后这套失效了 —— 内容不再由顶层
             * QQuickWindow 直接合成，蒙版管不到窗口四角。
             * 现在圆角由窗口自己负责，见 WindowHelper::applyRoundedMask()。
             */

            ColumnLayout {
                anchors.fill: parent; spacing: 0

        /*
         * 顶部这一行（原生标题栏去掉后它就是窗口最顶上的一行）：
         * 应用图标 / 应用名 / 菜单 tab 在左，搜索框和
         * 缩小 / 放大 / 关闭 三个窗口按钮在最右边。
         *
         * 原来左边还有 ☰ 和「main」分支选择器，已经去掉，
         * 详见 qml/components/TopBar.qml 开头。
         */
        TopBar {
            id: topBar
            Layout.fillWidth: true
            host: window
            view: window.view
            shortcuts: window.shortcutItems
            onOpenMenu: (anchor, items) => ddMenu.openFor(anchor, items)
            onSearchChanged: (text) => { window.searchText = text; window.refresh() }
        }

        /*
         * 工具栏那一行已经取消：所有命令都收进上面菜单栏的下拉菜单
         * （文件 / 编辑 / 搜索 / 视图 / 语言 / 编码 / 换行 / 帮助），
         * 菜单条目左边显示图标、右边显示快捷键（见 js/EditorMenus.js）。
         */

        RowLayout {
            /*
             * 中间这一行（图标条 / 左树 / 5px 间隙 / 编辑区）。
             *
             * 它同时是分隔线热区的"容器"：热区高度只认这一行的上下边界
             * （见文件末尾 splitterMouse），顶栏和底部状态栏都占不到。
             */
            id: midRow

            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 0

            // ---- 左侧工具窗口图标条（已取消边框） ----
            Rectangle {
                Layout.fillHeight: true; Layout.preferredWidth: 34
                color: "#313335"
                // 已删除 border.color 和 border.width

                IconProvider { id: stripIcons }
                Column {
                    anchors.fill: parent; anchors.topMargin: 8; anchors.bottomMargin: 8; spacing: 6
                    Repeater {
                        model: [ { k: "folder", active: true }, { k: "file", active: false },
                                 { k: "search", active: false }, { k: "play", active: false },
                                 { k: "branch", active: false } ]
                        delegate: Rectangle {
                            id: navCell
                            required property var modelData
                            width: 26; height: 26; x: 4; radius: 5

                            /*
                             * 这一格算不算"当前打开的工具窗口"。
                             *
                             * 文件夹那格不再看 modelData.active：它现在是项目树
                             * 的开关，面板收起来时这一格就该是未选中的样子
                             * （和 PyCharm 左边那排工具窗口按钮一个道理）——
                             * 面板收起来之后，标题栏那排按钮跟着没了，
                             * 这里就是唯一能把树叫回来的地方。
                             */
                            readonly property bool selected: modelData.k === "folder"
                                                             ? !window.folderTreeHidden
                                                             : modelData.active

                            /*
                             * 悬停态：整格填强调蓝 + 图标转白。
                             *
                             * 选中那一格原本是 #3a4a5a 的浅蓝底，
                             * 鼠标压上去时也让位给同一片蓝色 ——
                             * 否则 hover 在选中的格子上完全没反馈。
                             *
                             * 这里用绑定而不是 onEntered/onExited 手动改色：
                             * 绑定是幂等的，鼠标快速划过多格也不会串色。
                             */
                            readonly property bool hot: navHit.containsMouse
                            color: hot ? window.accentColor
                                       : (selected ? "#3a4a5a" : "transparent")

                            AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: modelData.k
                                      tint: navCell.hot ? "#ffffff"
                                                        : (navCell.selected ? window.accentColor : "#9aa0a8")
                                      size: 16 }
                            MouseArea {
                                id: navHit
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: modelData.k === "folder" ? Qt.PointingHandCursor
                                                                      : Qt.ArrowCursor
                                onClicked: (mouse) => {
                                    if (modelData.k === "folder")
                                        window.toggleFolderTree()
                                }
                            }

                            /* 只有文件夹那格接上了动作，提示也只给它 */
                            AppToolTip {
                                hovered: navHit.containsMouse && modelData.k === "folder"
                                text: window.folderTreeHidden ? "显示项目树" : "收起项目树"
                                /* 贴着窗口左沿放：默认的"居中在格子上"会往左出界 */
                                x: 2
                                y: -implicitHeight - 3
                            }
                        }
                    }
                    Item { width: 1; height: Math.max(1, parent.height - 300) }

                    /*
                     * 底部齿轮：打开设置面板（快捷键 / 关于）。
                     *
                     * 原来它只是个装饰格子（不接点击），现在接上 ——
                     * 设置入口本来就该在这里，也省得再去菜单里找。
                     */
                    Rectangle {
                        id: gearCell
                        width: 26; height: 26; x: 4; radius: 5
                        readonly property bool hot: gearHit.containsMouse
                        color: hot ? window.accentColor : "transparent"
                        AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: "gear"
                                  tint: gearCell.hot ? "#ffffff" : "#9aa0a8"; size: 16 }
                        MouseArea {
                            id: gearHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.showShortcuts()
                        }
                    }
                }
            }

            FolderTree {
                id: folderTree
                Layout.fillHeight: true
                /*
                 * 收起面板 = 槽位宽度给 0（窗口里那 5px 的拖拽间隙还在，
                 * 从那条缝往右拖也能把面板拖回来，见 splitterMouse）。
                 */
                Layout.preferredWidth: window.folderTreeHidden ? 0 : window.folderTreeWidth

                /*
                 * 面板右边缘在窗口里的 x。
                 *
                 * 这里用实际几何反推，不手算常量：布局槽位的右边界
                 * 正好就是那条 5px 间隙的左边界（实测 raw=334，
                 * 间隙在 334..338），所以直接取它。
                 * FolderTree 自己的 anchors 边距不算进槽位里，
                 * 之前写 folderTreeWidth + 12 就是差了这一段。
                 */
                readonly property real panelRight: mapToItem(window.contentItem, width, 0).x

                rows: window.treeRows
                selected: window.selectedItem
                onFolderClicked: (key) => window.toggleFolder(key)
                onItemClicked: (item) => window.selectItem(item)

                /* 标题栏那排按钮：动作全在 Main 这边（数据都在这儿） */
                onNewEntryRequested: window.newEntry()
                onRefreshRequested: window.refresh()
                onLocateRequested: window.locateCurrentItem()
                onCollapseAllRequested: window.setAllFolders(false)
                onExpandAllRequested: window.setAllFolders(true)
                onHideRequested: window.toggleFolderTree()
                /* 当前标签不是列表里的条目时，准星按钮置灰（没什么可定位的） */
                locateEnabled: window.canLocateCurrent()
                /* 标题"项目 ∨"和右边那个 ⋯ 弹的是同一份菜单 */
                onMenuRequested: (anchor) =>
                    ddMenu.openFor(anchor, Menus.treeMenu(window.treeMenuState(),
                                                          window.shortcutOverrides()))
            }

            /*
             * FolderTree / EditorArea 中间的间隙。
             *
             * 拖动的热区不在这里：它挪到窗口级了（见文件末尾的
             * splitterMouse）——放在布局里会被四边 resize 热区压住，
             * 收不到 hover，光标也就一直是箭头。
             * 这里只负责占住那 5px 的间隙。
             */
            Item {
                id: splitterGap

                /*
                 * 面板收起来时这条缝也一起收掉。
                 *
                 * 留着的话收起之后编辑区左边会多出 5px 和图标条同色的暗带：
                 * 展开时这一列到面板卡片左边缘为止（34px），收起后却到 39px ——
                 * 看着就是"折叠和展开左边这一列宽度不一样"。收掉之后
                 * 编辑区卡片正好顶到图标条右边，和展开时面板卡片的起点对齐。
                 */
                Layout.preferredWidth: window.folderTreeHidden ? 0 : 5
                Layout.fillHeight: true
            }

            EditorArea {
                id: editor
                Layout.fillWidth: true;
                Layout.fillHeight: true
                // 原来的 Layout.leftMargin: 5 已由上面的透明热区占据

                /*
                 * 右侧留出和左侧热区等宽的 5px。
                 *
                 * 不留的话卡片右边缘会紧贴窗口边框：
                 * 右上 / 右下的圆角虽然画出来了，
                 * 但紧挨着窗口那圈浅色边框，看着就像被切掉的方角，
                 * 和左边（splitter 5px 背景间隙）不是一套。
                 */
                Layout.rightMargin: 5

                /*
                 * 图片条目走这里；文本条目直接进编辑器的标签。
                 * （正文由 EditorView 的原生 QScintilla 渲染：
                 *   同一条 66 万字符的条目，QML 侧 2.5~4.3 秒，
                 *   原生 27ms —— 见 src/EditorViewItem.h）
                 */
                previewItem: {
                    var it = window.selectedItem
                    return (it && it.type === "image") ? it : null
                }

                onTabCloseRequested: (index) => window.closeTab(index)
                onTabCloseAllRequested: window.closeAllTabs()
                onNewTabRequested: window.newFile()
                onClipboardRefreshRequested: window.refresh()
                /*
                 * tab 右键菜单：把"被右键的标签 + 鼠标在标签里的坐标"转给
                 * openTabMenu —— 菜单左上角要落在鼠标那一点上。
                 */
                onTabContextMenuRequested: (index, anchor, x, y) =>
                    window.openTabMenu(index, anchor, x, y)
            }
        }

        /*
         * 底部状态栏：行列号 / 选中 / 字符数 / 语言 / 编码 / 换行符 / 缩放。
         *
         * 窗口按钮不在这里 —— 它们和搜索框一起在顶部那一行的最右边
         * （见 TopBar.qml 末尾的 WindowControls）。
         */
        StatusBar {
            id: statusBar
            Layout.fillWidth: true
            view: window.view
            count: cbm.entries.length
        }
            }
        }
    }

    /*
     * 圆角外侧那一圈描边。
     *
     * 窗口底色已经是透明的，深灰界面直接贴到桌面上会显得“糊”，
     * 压一条比底色亮一点点的细线，边界才立得住。
     * 它和遮罩用同一个半径，所以描边是贴着裁剪边缘走的。
     */
    Rectangle {
        anchors.fill: parent
        z: 10
        color: "transparent"
        radius: window.cornerRadius
        border.width: 1
        border.color: "#4b4d4f"
    }

    /*
     * 无边框窗口的四边 / 四角拖动改变大小。
     *
     * 拆成四条贴着窗口外沿的长条热区（上下各 6px 高、左右各 6px 宽），
     * 用 Qt 系统级的 startSystemResize 交给窗口管理器处理，
     * 比自己算增量更跟手。
     *
     * 最大化时整条热区直接 enabled: false —— 铺满屏幕的窗口没有
     * "拖大"这回事，留着只会让人误拖；顺带 enabled 关掉之后
     * hover 也不再收，边缘不会再多显示一个调整大小的光标。
     *
     * 为什么不再是原来那个 anchors.fill 的整窗热区：
     * 它 hoverEnabled: true，会把整个窗口的 hover 事件全吃掉 ——
     * 鼠标不管落在哪里，事件都先到它这一层（z:1000）并被 accept，
     * 下层的顶栏菜单 tab、左侧图标条、右上角三个窗口按钮
     * 就永远收不到 hover，hover 底色也就永远不亮。
     * （同一个原因，中间那条分隔线的光标以前也被压成箭头，
     *   当时是把分隔线单独抬到 z:2000 绕过去的，见下面 splitterMouse。）
     *
     * 只贴四条边，窗口内部就还给下面的控件；
     * 四角的行为和以前一致：按下时把相邻的那条边一起带进 startSystemResize。
     */
    component ResizeEdge: MouseArea {
        // 这条热区代表哪条边（Qt.LeftEdge / Qt.TopEdge / …）
        required property int edge
        // 要操作的窗口
        property var host: null

        // 四角 10px 内同时带上相邻的边
        readonly property int corner: 10

        // 最大化时不许拖边改大小
        enabled: !window.maximized

        hoverEnabled: true
        acceptedButtons: Qt.LeftButton

        /*
         * 光标：边上给「左右 / 上下」双箭头，四角给「↖↘」斜双箭头。
         *
         * 斜箭头只出现在四个角那一小块（corner × corner），边上一律
         * 还是横 / 竖箭头。这么分是因为左右两条竖边正好从中间那条
         * 可拖动的分隔线（splitter）两头经过，如果整条边都给斜箭头，
         * 顶到角落时分隔线上看着也像在拖角。现在斜箭头被夹在角里，
         * 不会再串到分隔线那一段。
         *
         * 光标要跟着鼠标在角里 / 不在角里切换，所以不能用静态表达式，
         * 而是在 onPositionChanged 里算出当前落在哪个角。
         *
         * 最大化时不给缩放光标，换成鼠标默认的箭头 —— 这里必须
         * 直接把 cursorShape 换掉，不能只靠上面的 enabled: false。
         *
         * 实测（受控 A/B，同一个窗口里并排两个同构 MouseArea）：
         *   enabled:false + cursorShape:SizeHorCursor -> 依然是 <-> 
         *   enabled:true  + cursorShape:SizeHorCursor -> 依然是 <-> 
         * 也就是说 enabled 只挡事件、不挡光标，光标提示照样生效。
         * 所以最大化时窗口边缘一直有个拖不动的 <->。
         * 另外单独验过一个什么自绘热区都没有的无边框窗口，
         * 边缘全是 ARROW —— 排除了 Qt 平台层 / Windows 的可能，
         * 这个 <-> 就是这两条热区自己刷出来的。
         */
        cursorShape: window.maximized
                     ? Qt.ArrowCursor
                     : cursorFor(activeCorner, edge)

        // 鼠标当前压在哪个角上（不在角上就是 0）
        property int activeCorner: 0

        /*
         * 这个点落在哪个角里。
         *
         * 入参用窗口坐标（和 onPressed 里的判定同一套口径），
         * 兼容外面直接喂 (x, y) 进来单独验证。
         */
        function cornerAt(px, py) {
            var nearTop = py <= corner
            var nearBottom = py >= parent.height - corner
            var nearLeft = px <= corner
            var nearRight = px >= parent.width - corner

            if (nearTop && nearLeft)    return Qt.TopEdge | Qt.LeftEdge
            if (nearTop && nearRight)   return Qt.TopEdge | Qt.RightEdge
            if (nearBottom && nearLeft) return Qt.BottomEdge | Qt.LeftEdge
            if (nearBottom && nearRight) return Qt.BottomEdge | Qt.RightEdge
            return 0
        }

        // 角 → 斜箭头；不在角上就退回这条边原来的横 / 竖箭头
        function cursorFor(c, e) {
            if (c === (Qt.TopEdge | Qt.LeftEdge) ||
                c === (Qt.BottomEdge | Qt.RightEdge))
                return Qt.SizeFDiagCursor      // ↖↘
            if (c === (Qt.TopEdge | Qt.RightEdge) ||
                c === (Qt.BottomEdge | Qt.LeftEdge))
                return Qt.SizeBDiagCursor      // ↗↙

            return (e === Qt.LeftEdge || e === Qt.RightEdge)
                   ? Qt.SizeHorCursor : Qt.SizeVerCursor
        }

        onPositionChanged: (mouse) => {
            // 和 onPressed / cornerAt 统一用窗口坐标
            var p = mapToItem(parent, mouse.x, mouse.y)
            activeCorner = cornerAt(p.x, p.y)
        }

        onExited: activeCorner = 0

        onReleased: (mouse) => { activeCorner = 0 }

        onPressed: (mouse) => {
            // 热区自身坐标 -> 窗口坐标：四角的判定要按整窗尺寸来算
            var p = mapToItem(parent, mouse.x, mouse.y)

            var e = edge
            if (p.y <= corner) e |= Qt.TopEdge
            if (p.y >= parent.height - corner) e |= Qt.BottomEdge
            if (p.x <= corner) e |= Qt.LeftEdge
            if (p.x >= parent.width - corner) e |= Qt.RightEdge

            Win.startSystemResize(e)
            mouse.accepted = true
        }
    }

    ResizeEdge {
        edge: Qt.LeftEdge; host: window; z: 1000
        width: 6
        anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
    }
    ResizeEdge {
        edge: Qt.RightEdge; host: window; z: 1000
        width: 6
        anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
    }
    ResizeEdge {
        edge: Qt.TopEdge; host: window; z: 1000
        height: 6
        anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
    }
    ResizeEdge {
        edge: Qt.BottomEdge; host: window; z: 1000
        height: 6
        anchors.left: parent.left; anchors.right: parent.right; anchors.bottom: parent.bottom
    }

    /*
     * FolderTree / EditorArea 中间那条可拖动的分隔线。
     *
     * 原来是塞在布局里的一个 Item，问题是它会被当时那层
     * 整窗 resize 热区（anchors.fill + 更高的 z）压住：
     * 鼠标移上去 hover 事件全被 resize 层吃掉，
     * 分隔线自己的 cursorShape 根本不会生效，一直是箭头；
     * 靠上/靠下时还会显示 resize 层的斜箭头。
     *
     * 所以改成窗口级的独立热区，z 比 resize 层更高，
     * 位置跟着树面板的右边缘走。这样 hover 一定是它先收到，
     * 光标稳定显示 Qt.SplitHCursor（左右两个箭头），
     * 而且落点正好在那条 5px 的间隙上。
     *
     * 高度只认"中间那一行"（midRow：图标条 / 左树 / 间隙 / 编辑区）的
     * 上下边界，不再铺满整个窗口：
     *
     *   原来写的是 y: 0 / height: parent.height，热区从窗口最顶上一直
     *   拉到最底下，于是顶栏和底部状态栏那两条上也压着热区 ——
     *   鼠标停在顶栏菜单上、停在底栏状态文字上，光标都会变成 <->，
     *   那里也真的能拖动左树宽度，看着就是"分隔线溢出了上下两条栏"。
     *
     *   间隙本身只有 midRow 那么高，热区跟着它走就够了：
     *   y 取 midRow.y、height 取 midRow.height（为什么不用 mapFromItem
     *   换算，见下面 y 绑定那里的实测记录）。
     *
     * 最大化时：拖动功能关掉（enabled: false），而且光标也要回到
     * 鼠标默认的箭头 —— 注意 enabled 挡不住 cursorShape（见上面
     * ResizeEdge 里的实测记录），所以这里把 cursorShape 也一起做成
     * 条件绑定，否则全屏以后左侧列表右边那条缝上还会一直冒 <->。
     */
    MouseArea {
        id: splitterMouse

        // 最大化时不给拖
        enabled: !window.maximized

        /*
         * 位置直接取树面板的右边缘（= 间隙左边界），
         * 往左外扩 4px、往右盖住 5px 的间隙，落点就是那条缝。
         */
        x: Math.max(0, folderTree.panelRight - 4)
        width: 9

        // 纵向跟着中间那一行走（顶栏 / 底栏各占多少高度由布局决定）
        /*
         * 这里不要用 mapFromItem(..., 0, 0).y 去反推 midRow 的顶边。
         *
         * 实测（自检里量过）：同一个表达式写在 uiState() 里能算出 34，
         * 写在这个 MouseArea 的 y 绑定里却一直是 0 —— 高度那条绑定
         * （读的是 midRow.height）算得 840，两条绑定一个对一个不对，
         * 于是热区还是从窗口最顶上铺下来。改成直接读 midRow.y，
         * 不经过坐标换算，绑定就对得上。
         *
         * midRow 是顶栏下面那一行（ColumnLayout 里紧挨着 TopBar），
         * 所以 midRow.y 就是顶栏的下沿。
         */
        y: midRow.y
        height: midRow.height

        z: 2000

        hoverEnabled: true
        cursorShape: window.maximized ? Qt.ArrowCursor : Qt.SplitHCursor
        acceptedButtons: Qt.LeftButton

        property real pressSceneX: 0
        property real pressWidth: 0

        onPressed: (mouse) => {
            pressSceneX = mapToItem(null, mouse.x, 0).x
            /*
             * 面板收起来时先把它叫回来：收起来之后标题栏那排按钮也跟着没了，
             * 这条 5px 的缝就是最自然的抓手（往右拖 = 把树拉出来）。
             */
            if (window.folderTreeHidden)
                window.toggleFolderTree()
            pressWidth = window.folderTreeWidth
            mouse.accepted = true
        }

        onPositionChanged: (mouse) => {
            if (!(mouse.buttons & Qt.LeftButton))
                return

            // 用场景坐标算位移，热区自身随宽度移动也不受影响
            var delta = mapToItem(null, mouse.x, 0).x - pressSceneX

            window.folderTreeWidth =
                Math.max(window.folderTreeMinWidth,
                         Math.min(window.folderTreeMaxWidth,
                                  pressWidth + delta))
            mouse.accepted = true
        }

        onReleased: (mouse) => {
            mouse.accepted = true
            /* 拖完才记一次宽度：拖动过程中每动一像素写一次设置太浪费 */
            window.rememberTreeWidth()
        }
    }
}
