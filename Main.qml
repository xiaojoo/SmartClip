import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import "qml/components"
import "qml/models"
import "qml/utils"
import "js/FolderManager.js" as Folders
import "js/EditorMenus.js" as Menus
/*
 * 左树点一个文件 / 菜单里的"复制"，都会回到编辑器里那份真实的 md 上。
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
     * 这一层（QQuickWidget 里的内容根）留透明。
     *
     * 无边框（FramelessWindowHint）之后四角就是方角，所以要自己圆：
     * 真正负责裁剪的是 **C++ 那边的圆角遮罩**（WindowHelper::applyRoundedMask，
     * 落到 Windows 上是 SetWindowRgn，见 src/WindowHelper.h 的说明）——
     * 不是这里，也不是透明本身。裸的深色底由宿主 QWidget 的调色板给
     * （见 src/main.cpp 里 host 那一段：那个窗口是**不透明**的）。
     *
     * 别把这行改回成"靠窗口透明来露四角"：透明窗口在**最大化**时会有
     * 一帧是空的，那一帧屏幕上看到的是窗口底下的东西（底下是个白底网页时
     * 就是"白色的背影一闪"）。这条踩过，自检里钉着
     * （"最大化/还原：来回切四次，窗口一次都没露底"）。
     *
     * 注意 palette.window 一并删掉：Fusion 样式会照着它刷一层不透明窗口底，
     * 那样四角又会被这块底色填回方形。
     */
    color: "transparent"

    /*
     * 整窗圆角半径：**只有一个来源**，就是 C++ 那个遮罩半径（WindowHelper.cornerRadius）。
     *
     * 这里以前抄死成 12，而遮罩用的是 10 —— 于是描边那段本来就带抗锯齿的弧
     * 被更小的遮罩削掉了，露出来的恰好是最硬的那条边（实测 (9,0) 该有约 62% 的
     * 描边，量到的是纯桌面白 255）。
     *
     * 走 DWM 那条路时（Win.dwmRound）半径由系统定（100% 缩放下实测约 8px），
     * 这个值只喂下面那条描边，而那条描边也跟着让位了。
     */
    readonly property real cornerRadius: Win.cornerRadius

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
    /*
     * 当前选中的是哪一个文件 —— 按**路径**认（见 FolderTree.selectedPath）。
     *
     * 左树现在列的是磁盘上的真实 md 文件，选中的是文件本身，"哪个文件开着"
     * 就是它的绝对路径；重命名之后把新路径填回来就行。
     */
    property string selectedPath: ""
    property var treeRows: []

    /*
     * 上一次弹出下拉菜单时锚在哪个控件上（见 TopBar 的 onOpenMenu）。
     * 只给自检用：验"菜单挂在被点的那一栏下方"，见 uiState 里的 menuAnchorWidth。
     */
    property var lastMenuAnchor: null

    /* 欢迎页 / 编辑器：现在由"有没有打开的标签"决定，见 EditorArea */

    /*
     * 编辑器本体（EditorArea 里的原生 EditorView）。
     *
     * 工具栏、状态栏、菜单里的勾选与禁用状态都绑在它身上；
     * 命令也全发给它（见下面的 dispatch）。
     */
    readonly property var view: editor.view

    /* ------------------------------------------------------------------
     * 字体这几项的两个来源：注册表（用户自己在菜单里设的）+ 方案文件的 font 段
     * ---------------------------------------------------------------- */

    /*
     * 注册表里那份**原样**的备份。方案可以覆盖显示出来的值，但永远不写回注册表
     * —— 换掉或删掉方案那一项，界面就得回到用户自己设的那个，而不是回到 12。
     * 键名和 Theme.h 那个 SCHEMEFONT_CASE 清单一一对应；终端那两项不在这里，
     * 它由 TerminalView 自己在 C++ 侧应用（见那边的 applySchemeFont）。
     */
    property var storedFont: ({ family: "Consolas", size: 12, commentSize: 0,
                                lineHeight: 1.0, wrap: false })
    /* 这一项现在生效的是哪个值：方案写了就以方案为准，没写就用注册表那份 */
    function fontEffective(key) {
        const o = Theme.fontOverride
        return o[key] !== undefined ? o[key] : storedFont[key]
    }

    /* 方案有没有钉住这一项（菜单和设置面板那一排按钮据此置灰） */
    function fontLocked(key) {
        return Theme.fontOverridden(key)
    }

    /*
     * 给菜单构造器（js/EditorMenus.js 的 settingsMenu）那一包：
     * 每一项带"钉没钉" + 置灰那格上写的一句话。
     *
     * 表达式开头读一下 Theme.scheme 是**这条绑定的扳机**：QML 不追函数调用内部的
     * 依赖（lockOne 里读的东西它看不见），换方案时整条不重算就会一直摆着旧状态。
     * 文案走 Theme.fontOverrideNote()，和设置面板那排按钮同一份，写两遍迟早对不上。
     */
    /*
     * 构造菜单时的那个状态包：快捷键覆盖 + 方案钉住的字体项。
     * 自检读的就是这份（settingsMenuActs），弹出来的那份走 TopBar —— 两边都得带上
     * fontLock，否则自检量到的是"没置灰"的那一份，界面上置灰了它也不知道。
     */
    function menuOv() {
        var b = shortcutOverrides()
        b.fontLock = fontLock
        return b
    }

    function lockOne(k) { return { on: Theme.fontOverridden(k), note: Theme.fontOverrideNote(k) } }
    property var fontLock: ({ scheme: Theme.scheme,
                               size: lockOne("size"),
                               commentSize: lockOne("commentSize"),
                               family: lockOne("family"),
                               lineHeight: lockOne("lineHeight"),
                               wrap: lockOne("wrap") })

    /*
     * 把生效值推到视图上。启动时推一次、换方案 / 改完方案文件热加载时再推一次
     * （Theme 的 rev 每换一次方案必 +1，绑在它身上就够）。
     *
     * 字号走 editor.editorFontSize：view.fontPixelSize 一直绑着它，直接给
     * fontPixelSize 赋值会把那条绑定打断（见 EditorArea.qml 里那段的说明）。
     */
    function applyFontScheme() {
        /*
         * 每个写入都**先比一下**：字号那一改是整篇重排（Scintilla 要重算每一行的
         * 行高），而 revChanged 在方案自检里会被打好几回 —— 值没变就别白重排一次。
         */
        const size = fontEffective("size")
        if (editor.editorFontSize !== size)
            editor.editorFontSize = size      // 走绑定，别直接写 view.fontPixelSize
        const fam = fontEffective("family")
        const csize = fontEffective("commentSize")
        const lh = fontEffective("lineHeight")
        const wrap = fontEffective("wrap")
        applyToPanes(function (v) {
            if (v.fontFamily !== fam)
                v.fontFamily = fam
            if (v.commentFontPixelSize !== csize)
                v.commentFontPixelSize = csize
            if (v.lineHeightFactor !== lh)
                v.lineHeightFactor = lh
            if (v.wrapEnabled !== wrap)
                v.wrapEnabled = wrap
        })
    }

    Connections {
        target: Theme
        function onRevChanged() { window.applyFontScheme() }
    }

    /* ------------------------------------------------------------------
     * Markdown 预览（见 qml/components/MarkdownView.qml）
     * ---------------------------------------------------------------- */

    /*
     * 预览看的是**当前那一栏**（activeView）的正文。
     *
     * 分栏之后两栏可能看着两份不同的文件，所以"预览哪一份"这件事必须跟着
     * 焦点走 —— 用户点了右边那一栏再点预览，看到的就是右边那一份。
     */
    property bool markdownPreview: false
    /* 渲染好的 HTML（给 MarkdownView 的 html 属性） */
    property string markdownHtml: ""
    /* 渲染结果为空时那句提示 */
    property string markdownHint: "没有可预览的内容"
    /*
     * 上次退出时是不是在看预览。
     *
     * 打开下一份 md 时按这个偏好开局 —— 用户既然在看文章，切一份多半还是
     * 想接着看（见 applyMarkdownPreference）。
     */
    property bool markdownPreviewPreferred: false

    /* 当前这一栏（哪一栏，不是哪一份文档）能不能预览 Markdown */
    readonly property bool canPreviewMarkdown: {
        var v = activeView()
        if (!v || !v.hasDocument)
            return false
        var path = String(v.filePath || "")
        if (path === "")
            return false
        return /\.(md|markdown|mdown|mkd|mdtext)$/i.test(path)
    }

    /*
     * 按当前那一栏重算预览该不该开着，并重新渲染一遍。
     *
     * 每次切换都重渲染（不做缓存）：一份 md 几毫秒，而"改了源码预览不跟着变"
     * 是更难查也更烦人的 bug。触发点：换文档、正文变了、存盘了、用户点开关。
     */
    function applyMarkdownPreference() {
        if (!canPreviewMarkdown) {
            markdownPreview = false
            markdownHtml = ""
            return
        }
        markdownPreview = markdownPreviewPreferred
        refreshMarkdown()
    }

    /* 重新渲染一遍（预览没开就清空，省得留着上一份的内容） */
    /*
     * 预览的配色不在 QML 里，在 Cmd.markdownHtml() 生成的那段 CSS 字符串里
     * （见 src/EditorController.cpp 的 themedCss）—— 字符串不吃绑定，
     * 所以切主题必须手动重生成一次，否则预览还是上一档的颜色。
     */
    Connections {
        target: Theme
        function onLightChanged() { window.refreshMarkdown() }
    }

    function refreshMarkdown() {
        if (!canPreviewMarkdown || !markdownPreview) {
            markdownHtml = ""
            return
        }
        var v = activeView()
        var path = v ? String(v.filePath || "") : ""
        var dir = path !== "" ? path.substring(0, path.lastIndexOf("/")) : ""
        markdownHtml = Cmd.markdownHtml(v.currentText(), dir)
        markdownHint = markdownHtml === "" ? "这份文档是空的" : ""
    }

    /*
     * 开 / 关预览（标签栏那个开关、菜单里那一条、Ctrl+Shift+V 都走这里）。
     * 偏好也跟着记：下次打开 md 按这次的选择开局。
     */
    function toggleMarkdownPreview() {
        if (!canPreviewMarkdown)
            return
        markdownPreview = !markdownPreview
        markdownPreviewPreferred = markdownPreview
        Cmd.remember("markdownPreview", markdownPreview ? "1" : "0")
        if (markdownPreview)
            refreshMarkdown()
        else
            activeView().requestEditorFocus()
    }

    /* ------------------------------------------------------------------
     * "当前编辑器"：分栏之后命令要发给用户正在用的那一栏
     * ---------------------------------------------------------------- */

    /*
     * 分栏时有两栏，两栏是**对等**的编辑组（各自一组标签，共用一份文档池，
     * 见 qml/components/EditorArea.qml）。编辑器那边记着"最后被点的是哪一栏"
     * （EditorViewItem 的 paneFocus），这里读它 —— 读的是**属性**（不是
     * 函数调用），所以绑在它上面的东西会跟着焦点自动重算。
     */
    function activeView() {
        if (editor.splitting && editor.mirrorView && editor.mirrorView.activePane)
            return editor.mirrorView
        return view
    }

    /* 前面那一栏现在打开着哪一份（没有文档就是 null；菜单 / 标签用） */
    function activeDoc() {
        var v = activeView()
        if (!v || !v.hasDocument)
            return null
        var docs = v.documents
        var i = v.currentIndex
        return (i >= 0 && i < docs.length) ? docs[i] : null
    }

    /*
     * 视图级设置（字号 / 字体 / 行高 / 换行 / 行号 / 各种竖线 / 缩放 …）
     * 要**两栏一起**改。
     *
     * 这些设置每栏各存一份（Scintilla 的视图选项是视图级的），只改当前那一栏
     * 的话分栏之后两栏看着就不一样了 —— 用户改的是"编辑器怎么显示"，
     * 不是"左边那一栏怎么显示"。文档级的东西（语言 / 编码 / 换行符）不走这里，
     * 那些是按文档设的。
     */
    function applyToPanes(fn) {
        if (editor.mainView)
            fn(editor.mainView)
        if (editor.splitting && editor.mirrorView)
            fn(editor.mirrorView)
    }

    /*
     * 某一栏被点了：标记只能有一个（两栏都标着的话命令会落到先问到的那个）。
     *
     * paneFocus 在 C++ 侧是可写属性（见 EditorViewItem.h），赋值的同时它会把
     * "当前编辑器"（instance()）也指过来 —— 快捷键那几条最终就发给它。
     */
    function notePaneFocus(pane) {
        if (editor.mainView)
            editor.mainView.paneFocus = false
        if (editor.mirrorView)
            editor.mirrorView.paneFocus = false
        if (pane)
            pane.paneFocus = true
        /*
         * "当前栏"变了，预览要按新那一栏重算（两栏可能看着不同的文件：
         * 点一下右边，md 开关和预览内容都得跟着右边走）。
         */
        applyMarkdownPreference()
    }

    /* ------------------------------------------------------------------
     * 分栏（tab 右键菜单那三条，见 js/EditorMenus.js 的 tabMenu）
     * ---------------------------------------------------------------- */

    /*
     * tab 右键菜单里那三条的可用状态。
     *
     * canSplit 看的是**右键点中的那一栏**里有没有文档（不是"主栏有没有"）——
     * 分栏之后主栏完全可能是空的，而右边那一栏开着东西。
     */
    function splitState(pane) {
        var v = pane ? pane : activeView()
        return { canSplit: !!(v && v.hasDocument),
                 mode: editor.splitMode,
                 count: v ? v.documents.length : 0 }
    }

    /*
     * 开 / 关 / 换分栏方向。
     *
     * 两件事必须一起做：布局（editor.splitMode）和第二栏有没有内容
     * （editor.openMirrorWithCurrent —— 把**当前那一份**放进新分出来的那一栏）。
     * 只做一半的话就是"分了两栏，另一边一直空着"。
     */
    function setSplit(mode) {
        var next = (editor.splitMode === mode) ? "" : mode
        if (next !== "" && (!activeView() || !activeView().hasDocument))
            return
        editor.splitMode = next
        if (next === "") {
            editor.unbindMirror()
        } else {
            /*
             * 分栏时把"当前那一份"放进第二栏（VS Code 里 split editor 也是
             * 把当前这个标签挪到新组里）。第二栏之后想换别的文件，点它自己
             * 那条标签栏就行 —— 两栏是各自独立的标签。
             */
            editor.openMirrorWithCurrent()
        }
        /* 切完把焦点还给当前那一栏：用户刚看完菜单，接着多半要打字 */
        Qt.callLater(function () {
            var v = activeView()
            if (v && v.hasDocument)
                v.requestEditorFocus()
        })
    }

    /*
     * 第二栏被关空了（最后一条标签关掉）：这一栏就别留着了，整条收起。
     *
     * 用户报的"右边那条小叉点了没反应"就是这里：小叉本身是好的
     * （closeDocument 之后 documents 确实空了），但紧接着那一下
     * "第二栏空了就补一份主栏当前文档"又把标签长了回来 —— 屏幕上看着
     * 就是点了没用。所以补那一份的逻辑去掉了，改成"空了就退出分栏"。
     */
    function leaveSplitForEmptyMirror() {
        if (editor.splitMode === "")
            return
        editor.splitMode = ""
        editor.unbindMirror()
    }

    /*
     * 分栏**不跨启动记着**：程序退在"两栏"的样子上，下次打开还是单栏
     * （用户要的就是这个 —— 分栏是一次性的看文档动作，不该变成启动布局）。
     *
     * 所以这里没有 restoreSplit：splitMode 开局恒为 ""，想分栏点右键菜单 /
     * Alt+Shift+2。原来那个"记下来 + 下次照原样开"的写法连设置项
     * （editor/splitMode）一起去掉了，免得留一个谁也不读的键。
     */

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
     * 底部终端面板（见 qml/components/TerminalPanel.qml）。
     *
     * 和上面左树那两条一个路子：高度是"用户拖出来的那个值"，收起时槽位给 0，
     * 值本身留着，再展开还是原样。
     */
    property bool terminalHidden: true
    property real terminalHeight: 260
    /* 最大化 = 把中间那一行整个让给面板（和 VS Code 面板那个上箭头一个意思） */
    property bool terminalMaximized: false
    readonly property real terminalMinHeight: 120

    /*
     * 树里文件的排序：true = 最新在前。
     *
     * 文件名就是时分秒（073100.md），所以"按名字倒序"天然就是时间倒序；
     * 真正的排序在 C++ 那边做（见 ClipboardStore::tree 的 newestFirst）。
     * 导入的文件夹里那些文件按名字字母序，不受这个开关影响。
     */
    property bool newestFirst: true

    /*
     * 全局强调色（#4c96d8）。
     *
     * 顶栏右侧的窗口按钮和左侧导航条的 hover 底色都用它，
     * 和编辑区 / 选中态本来就是同一个蓝，只在这里写一次值。
     */
    readonly property color accentColor: "#4c96d8"

    /*
     * 左树里哪些文件夹是展开的（key -> true）。
     *
     * key 就是 C++ 那边给的节点 key，形如 "dir:C:/Users/…/2026-09-13"
     * （见 ClipboardStore::tree）。日期文件夹和导入的文件夹共用这一份状态：
     * 树有几层、有多少个文件夹都是运行时才知道的，所以不能再像以前那样
     * 写死"今天 / 昨天 / 近 7 天 / 更早"四个键。
     *
     * 展开状态存 QSettings（treeExpanded，一行一个 key），下次启动回到原样。
     */
    property var expanded: ({})

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

    /*
     * 左树在看哪一份（标题「项目 ∨」那个菜单选的，见 Menus.treeScopeMenu）：
     *   "project"       整棵树（日期目录 + 里面的文件）—— 默认
     *   "projectFiles"  所有文件平铺（不带目录那层）
     *   "openFiles"     只列现在打开着的标签
     * 只活在内存里：重开程序还是回到整棵树（默认那个最不容易让人找不着东西）。
     */
    property string treeScope: "project"

    /* 现在打开着的文件路径（两栏都算）："打开的文件"那个视图用它筛 */
    function openPaths() {
        var out = []
        var panes = [editor.mainView, editor.mirrorView]
        for (var p = 0; p < panes.length; ++p) {
            var v = panes[p]
            if (!v)
                continue
            var docs = v.documents
            for (var i = 0; i < docs.length; ++i) {
                var path = docs[i] ? String(docs[i].path) : ""
                if (path !== "" && out.indexOf(path) < 0)
                    out.push(path)
            }
        }
        return out
    }

    function rebuild() {
        if (treeScope === "projectFiles")
            treeRows = Folders.flatFiles(cbm.nodes)
        else if (treeScope === "openFiles")
            treeRows = Folders.openFiles(cbm.nodes, openPaths())
        else
            treeRows = Folders.buildTree(cbm.nodes, expanded)
    }

    /* 换"看哪一份"（菜单里那三条走这儿；换完重建一遍行） */
    function switchTreeScope(scope) {
        if (treeScope === scope)
            return
        treeScope = scope
        rebuild()
    }

    /*
     * 重新读一遍数据。
     *
     * 数据源是"磁盘 + 元数据缓存"（C++ 的 Store）：query 非空时只留命中的文件。
     * 真正去扫盘的是 Store.rescan()，这里是拿现成的缓存重建左树。
     */
    function refresh() { cbm.reload(searchText, newestFirst) }

    /* 展开状态的深浅拷贝（改完整个赋回去，绑定才知道变了） */
    function copyExpanded() {
        var e = ({})
        for (var k in expanded)
            e[k] = expanded[k]
        return e
    }

    function rememberExpanded() {
        var keys = []
        for (var k in expanded)
            if (expanded[k])
                keys.push(k)
        Cmd.remember("treeExpanded", keys.join("\n"))
    }

    /*
     * 一个文件路径落在哪个文件夹节点上。
     *
     * C++ 那边统一用 '/' 拼路径（QDir::cleanPath），所以这里直接找最后一个斜杠。
     */
    function folderKeyOf(filePath) {
        var at = filePath.lastIndexOf("/")
        return at > 0 ? "dir:" + filePath.substring(0, at) : ""
    }

    function toggleFolder(key) {
        var e = copyExpanded()
        e[key] = !e[key]
        expanded = e
        rememberExpanded()
        rebuild()
    }

    function activateFolder(key) {
        if (!key)
            return
        var e = copyExpanded()
        e[key] = true
        expanded = e
        rememberExpanded()
        rebuild()
    }

    /* 全部展开 / 全部折叠（左树标题栏那两个按钮，也是"更多"菜单里的两条） */
    function setAllFolders(open) {
        var keys = Folders.allFolderKeys(cbm.nodes)
        var e = ({})
        for (var i = 0; i < keys.length; ++i)
            e[keys[i]] = open
        expanded = e
        rememberExpanded()
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

    /*
     * 收起 / 展开底部终端面板（Ctrl+` 和图标条那一格都走这里）。
     *
     * 展开之后把焦点交给终端 —— 不抢的话敲键盘还是打在编辑器上，
     * 面板开出来却什么都输不进去，看着就像没生效。
     * forceActiveFocus 要等这一槽布局算完才有意义，所以推到下一个事件循环。
     */
    function toggleTerminalPanel() {
        terminalHidden = !terminalHidden
        /*
         * 收起时把"最大化"一起退掉：中间那一行的 visible 只看 terminalMaximized，
         * 留着它的话面板一收就两头落空 —— 中间行藏着、面板高度给 0，
         * 顶栏和状态栏之间整片空白。图标条现在常驻，这一条更容易被点出来。
         */
        if (terminalHidden)
            terminalMaximized = false
        Cmd.remember("termHidden", terminalHidden ? "1" : "0")
        if (!terminalHidden) {
            /*
             * 先把会话补上再抢焦点：最后一条标签被关掉时面板是收起来的
             * （见 TerminalPanel.closeSession），那时模型是空的，
             * 不补就开出来一张空壳，而且 focusTerminal() 也拿不到视图。
             */
            Qt.callLater(function () {
                terminal.ensureSession()
                terminal.focusTerminal()
            })
        }
    }

    function rememberTerminalHeight() {
        Cmd.remember("termHeight", String(Math.round(terminalHeight)))
    }

    function setNewestFirst(on) {
        if (newestFirst === on)
            return
        newestFirst = on
        Cmd.remember("treeNewestFirst", on ? "1" : "0")
        rebuild()
    }

    /*
     * 左树标题栏的 "+"：在今天那个日期目录里新建一份 md。
     *
     * 已经不是"往库里插一条空条目"了 —— 建出来的是一份真实文件
     * （<root>/2026-09-13/073100.md，名字取当前时分秒），建完直接在编辑器里
     * 打开，写东西按 Ctrl+S 就是普通的保存文件。
     *
     * 建完立刻：清掉搜索框里的过滤（不然新文件可能根本不显示）-> 展开它所在的
     * 日期目录 -> 选中 -> 打开。
     */
    function newEntry() {
        if (searchText !== "") {
            searchText = ""
            topBar.clearSearch()
        }

        var path = Store.createFile("")
        if (path === "") {
            Cmd.alert("新建失败", "无法在保存目录里建文件，请检查设置里的保存位置")
            return
        }

        refresh()
        activateFolder(folderKeyOf(path))
        openTreeFile(path)
    }

    /*
     * 当前标签对应的左树文件路径；当前标签不是左树管着的文件（未命名空白文档、
     * 从菜单"打开"进来的别的文件）时返回空串。
     *
     * 按**路径**认，不按标题认：重命名之后标题跟着变，路径才是身份
     * （renameTreeFile 会把新路径同步给编辑器标签）。
     */
    function currentTreePath() {
        var v = activeView()
        var path = v ? String(v.filePath || "") : ""
        return isManagedPath(path) ? path : ""
    }

    /*
     * 这个路径在不在保存目录 / 导入的某个目录里面（左树管得着的范围）。
     *
     * 特意**不查 treeRows**：折叠起来的日期目录里那一份也是左树的一员，
     * 而"定位当前文件"本来就是要把它展开再选中 —— 拿 treeRows 判的话，
     * 面板一折叠（或者搜索框里有过滤），准星按钮就会莫名其妙地灰掉。
     */
    function isManagedPath(path) {
        if (!path)
            return false

        var root = Store.rootPath
        if (root !== "" && path.indexOf(root + "/") === 0)
            return true

        var dirs = Store.importedFolders
        for (var i = 0; i < dirs.length; ++i) {
            if (dirs[i] !== "" && path.indexOf(dirs[i] + "/") === 0)
                return true
        }
        return false
    }

    /* 准星按钮能不能点（转给 FolderTree，见那边 locateEnabled） */
    function canLocateCurrent() { return currentTreePath() !== "" }

    /*
     * 在左树里定位当前标签（标题栏那个准星按钮 / "更多"菜单里的"定位当前文件"）。
     *
     * 只做三件事：展开它自己（以及导入目录那些父目录）-> 把它选上（蓝条）-> 滚到它。
     * **不**动编辑器里的正文。
     *
     * 搜索框里有过滤的话先清掉：定位是明确的"带我去看"动作，被过滤掉就白点了
     * （和 newEntry 清过滤是同一个理由）。
     * 返回有没有真的定位到（没定位到通常是当前标签根本不在左树里）。
     */
    function locateCurrentItem() {
        var path = currentTreePath()
        if (path === "")
            return false

        if (searchText !== "") {
            searchText = ""
            topBar.clearSearch()
            refresh()
        }

        var e = copyExpanded()
        e[folderKeyOf(path)] = true
        /* 导入的目录可能套了好几层，一路展开上去（顶级目录不在树里，多了也无妨） */
        var dir = path.substring(0, path.lastIndexOf("/"))
        while (dir.length > 2) {
            e["dir:" + dir] = true
            var up = dir.substring(0, dir.lastIndexOf("/"))
            if (up === dir)
                break
            dir = up
        }
        expanded = e
        rebuild()

        selectedPath = path
        /*
         * 滚动要等这一帧的列表更新完再做（树刚重建过，行下标这会儿还在算）——
         * Qt.callLater 就是"这一轮事件处理完再调"。
         */
        Qt.callLater(function () { folderTree.scrollToPath(path) })
        return true
    }

    /*
     * 点左树里的一个文件：在编辑器里打开它。
     *
     * 树上没有"条目"了，点开的是一份真实的 md，所以走 openFile() 那条路 ——
     * 已经开着就切回那条标签（按路径认），Ctrl+S 直接写回这份文件。
     *
     * 打开在**当前那一栏**上（和 VS Code 一样：文件开在刚点过的那一组里），
     * 另一栏不动。
     */
    function openTreeFile(rowOrPath) {
        var path = (typeof rowOrPath === "string") ? rowOrPath : rowOrPath.path
        if (!path)
            return

        selectedPath = path
        editor.previewItem = null

        var v = activeView()
        if (!v)
            return

        var index = v.indexOfPath(path)
        if (index >= 0) {
            v.activateDocument(index)
            v.requestEditorFocus()
            return
        }

        if (v.openFile(path) < 0)
            Cmd.alert("打开失败", v.lastError)
        else
            v.requestEditorFocus()
    }

    /* ------------------------------------------------------------------
     * 文件与文件夹操作（左树右键菜单 / "更多"菜单）
     * ---------------------------------------------------------------- */

    /*
     * 重命名一份 md：改的是真实文件，编辑器里开着的那个标签跟着换路径。
     *
     * 输入走自绘卡片（以前是原生 QInputDialog），所以是回调不是返回值 ——
     * 和 deleteTreeFile 那一路 askConfirm 的写法一致。
     */
    function renameTreeFile(path) {
        var oldName = Cmd.fileNameOf(path)
        askInput("重命名", "新文件名（.md 可以省略）", oldName, {}, function (name) {
            if (name === "" || name === oldName)
                return

            var dir = path.substring(0, path.lastIndexOf("/"))
            var newName = /\.md$/i.test(name) ? name : name + ".md"
            var newPath = dir + "/" + newName

            if (!Store.renameFile(path, name)) {
                Cmd.alert("重命名失败", "同名文件可能已经存在：" + newName)
                return
            }

            /*
             * 打开着的那个标签也要跟着换，否则下一次 Ctrl+S 会写回旧名字。
             * 文档池是两栏共用的，所以哪一栏开着它、开在哪个标签上都不用管 ——
             * 两边都会看到新名字。
             */
            applyToPanes(function (v) { v.updateDocumentPath(path, newPath) })
            if (selectedPath === path)
                selectedPath = newPath
        })
    }

    /*
     * 删一份 md：开着的标签先关（有未保存改动会先问），再删文件。
     *
     * 问句和关标签都是异步的（卡片答完才走信号回来，见 AskCard.qml），
     * 所以"删"这一步得当成回调传进去 —— 用户按取消就整条链停下，
     * 磁盘上那份一动不动。
     *
     * 分栏时两栏可能都开着它（同一份文档），所以两栏都要关 —— 关掉一条
     * 标签不会动文档池里那一份，另一栏那条还挂在那儿。
     */
    function deleteTreeFile(path) {
        askConfirm("删除文件", "确定删除这一份吗？\n\n" + path, function () {
            var pairs = []
            if (editor.mainView && editor.mainView.indexOfPath(path) >= 0)
                pairs.push({ pane: editor.mainView, path: path })
            if (editor.splitting && editor.mirrorView
                    && editor.mirrorView.indexOfPath(path) >= 0)
                pairs.push({ pane: editor.mirrorView, path: path })
            if (pairs.length === 0) {
                removeTreeFile(path)        /* 没开着，直接删 */
                return
            }
            closeTheseTabs(pairs, function () { removeTreeFile(path) })
        })
    }

    /* 真删磁盘上那一份（"标签先关"那一步在 deleteTreeFile 里） */
    function removeTreeFile(path) {
        if (!Store.deleteFile(path)) {
            Cmd.alert("删除失败", "文件可能已经不在了：\n" + path)
            return
        }
        if (selectedPath === path)
            selectedPath = ""
    }

    function revealPath(path) {
        if (path)
            Cmd.revealInExplorer(path)
    }

    /*
     * 弹系统对话框之前，先把设置面板收起来，弹完原地放回来。
     *
     * 面板是 Popup.Window —— Qt 把这块窗口建成**置顶**的（实测 flags 里带着
     * WindowStaysOnTopHint），而 Windows 的规矩是非置顶窗口永远盖不住置顶窗口。
     * 所以面板里点"导入文件夹…"/"选择保存位置…"时，系统的文件夹选择框只会
     * 出现在面板下面（用户截图报的就是这个，试过在 C++ 里给对话框换 transient
     * parent、临时摘掉面板的置顶，都压不过 Qt 建这块窗口的规矩）。
     *
     * 与其跟窗口管理器较劲，不如让面板先让开：对话框一关立刻按原栏目放回来，
     * 位置、状态都不变 —— 用户看到的是"面板换成了文件选择框，选完又回来了"。
     */
    function withSettingsPanelAway(fn) {
        var wasOpen = settingsPanel.opened
        var section = settingsPanel.section
        if (wasOpen)
            settingsPanel.hide()
        var result = fn()
        if (wasOpen)
            settingsPanel.openSection(section)
        return result
    }

    /* 导入一个外部文件夹：只读地把它里面的 md / txt 挂到左树上 */
    function importFolder() {
        var dir = withSettingsPanelAway(function () {
            return Cmd.chooseFolderDialog("导入文件夹", Store.rootPath)
        })
        if (dir === "")
            return
        if (!Store.addImportedFolder(dir)) {
            Cmd.alert("导入失败", "这个文件夹不存在，或者已经在列表里了：\n" + dir)
            return
        }
        activateFolder("dir:" + dir)
    }

    function removeImportedFolder(path) {
        askConfirm("移除导入目录", "把它从左树上移开？（磁盘上的文件不动）\n\n" + path,
                   function () { Store.removeImportedFolder(path) })
    }

    /* 换剪贴板文件的保存位置（设置面板和"更多"菜单都有入口） */
    function chooseStorageRoot() {
        var dir = withSettingsPanelAway(function () {
            return Cmd.chooseFolderDialog("选择剪贴板保存位置", Store.rootPath)
        })
        if (dir === "")
            return
        if (!Store.setRootPath(dir))
            Cmd.alert("设置失败", "这个目录用不了：\n" + dir)
    }

    /*
     * 自检用：走一遍那层"让开再回来"的壳，回来时告诉自检"让开期间面板是不是
     * 真的收起来了"（见 withSettingsPanelAway）。
     */
    function probeSettingsAway() {
        return withSettingsPanelAway(function () {
            return settingsPanel.opened ? "still-open" : "away"
        })
    }

    /*
     * 设置 → 模型：选本地推理程序 / 模型文件（kind 是 "exe" / "model"）。
     *
     * 走和"选择保存位置…"同一条路：**先让设置面板让开**再开文件框。面板是
     * 置顶的原生窗口，不让开的话 Windows 的文件选择框会被它整块盖住（用户
     * 截图报的就是这个，详见 withSettingsPanelAway 上面那段说明）。选完面板
     * 按原栏目放回来，再把选中的路径填进对应那个框。
     */
    function chooseTranslateLocalFile(kind) {
        var path = withSettingsPanelAway(function () {
            return Cmd.chooseFileDialog(
                        kind === "exe" ? "选择推理服务程序"
                                       : (kind === "mmproj" ? "选择多模态投影文件（mmproj）"
                                                            : "选择模型文件"),
                        kind === "exe" ? "可执行文件 (*.exe);;所有文件 (*.*)"
                                       : "GGUF 模型 (*.gguf);;所有文件 (*.*)")
        })
        if (path === "")
            return
        if (kind === "exe")
            Llm.localExe = path
        else if (kind === "mmproj")
            Llm.localMmproj = path
        else
            Llm.localModel = path
        settingsPanel.setLocalPath(kind, path)
    }

    /*
     * 设置 → 识别：选识别脚本用的 Python 解释器。
     *
     * 为什么值得单独一个入口：那三个识别包一般装在某个虚拟环境里，而 PATH 里
     * 的 python 多半是系统那个 —— 装了却报 "No module named 'rapid_doc'"，
     * 用户完全看不出是解释器选错了。选完之后 Doc 会把默认那条命令里的解释器
     * 换掉（见 DocImport::setPythonPath）。
     */
    function chooseDocPython() {
        var path = withSettingsPanelAway(function () {
            return Cmd.chooseFileDialog("选择 Python 解释器（venv 里的 python.exe）",
                                        "Python (python.exe);;可执行文件 (*.exe);;所有文件 (*.*)")
        })
        if (path === "")
            return
        Doc.pythonPath = path
        settingsPanel.refreshDocRunner()
    }

    /* ------------------------------------------------------------------
     * 命令分发
     *
     * 工具栏按钮、菜单项、快捷键（C++ 侧 QAction，见 EditorController）
     * 三条入口最后都落到这里，行为只有一份。
     * ---------------------------------------------------------------- */

    function commentPrefix() {
        var v = activeView()
        var lang = v ? v.language : "plain"
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
        var v = activeView()
        if (!v)
            return
        v.newDocument()
        editor.previewItem = null
        v.requestEditorFocus()
    }

    function openFile() {
        var path = Cmd.openFileDialog()
        if (path === "")
            return
        var v = activeView()
        if (!v)
            return
        if (v.openFile(path) < 0)
            Cmd.alert("打开失败", v.lastError)
        else {
            editor.previewItem = null
            v.requestEditorFocus()
        }
    }

    function saveFile() {
        var v = activeView()
        if (!v || !v.hasDocument)
            return false
        /*
         * 剪贴板内容现在是真实文件（日期目录里的 md），打开它就带着路径，
         * Ctrl+S 走的是普通保存。只有真正的未命名空白文档才需要问路径。
         */
        if (v.filePath === "")
            return saveFileAs()
        if (!v.saveCurrent()) {
            Cmd.alert("保存失败", v.lastError)
            return false
        }
        return true
    }

    function saveFileAs() {
        var v = activeView()
        if (!v || !v.hasDocument)
            return false
        var suggested = v.filePath !== "" ? Cmd.fileNameOf(v.filePath)
                                          : v.displayName + ".txt"
        var path = Cmd.saveFileDialog(suggested)
        if (path === "")
            return false
        if (!v.saveCurrentAs(path)) {
            Cmd.alert("保存失败", v.lastError)
            return false
        }
        return true
    }

    /*
     * 关一个标签。
     *
     * pane 不给就是"当前那一栏"；标签栏点小叉 / 右键菜单会显式传是**哪一栏**
     * 的哪一个标签（分栏之后两栏各有一组标签，不能想当然）。
     *
     * 没改动的当场就关（和以前一样同步返回真假）；有未保存改动时先弹
     * 「保存 / 不保存 / 取消」那块卡片（见 qml/components/AskCard.qml）——
     * 卡片是原生小窗、答完走信号回来，所以这条路上**返回值只能当"这一刻
     * 关了没"看**（返回 false 不代表用户取消了）。要"关掉之后接着做点什么"
     * （比如删文件），走 requestCloseTabs(下标, 回调)。
     */
    function closeTab(index, pane) {
        var v = pane ? pane : activeView()
        if (!v)
            return false
        var docs = v.documents
        if (index === undefined || index === null || index < 0)
            index = v.currentIndex
        if (index < 0 || index >= docs.length)
            return false

        if (docs[index].modified) {
            requestCloseTabs([index], null, v)
            return false
        }

        finishCloseTab(index, v)
        return true
    }

    /*
     * 关一串标签：一个一个来，有未保存改动的那个停下来等用户回答，
     * 答完再接着关下一个；中途按"取消"整串停下 —— 这就是以前那个
     * for 循环 + return 的语义，只是现在要跨帧等回答。
     *
     * 队列里存的是**当时那一份的身份**（哪一栏 + 下标 + 标题 + 路径），
     * 不是光存下标：卡片是非模态的，等回答这段时间用户还能去关别的标签，
     * 下标会跟着挪（见 locateQueued）。
     */
    property var closeQueue: []         /* 等着关的那些标签，从后往前 */
    property var closeQueueThen: null   /* 整串关完之后接着做的事 */
    property var pendingClose: null     /* 正在等回答的那一条；null = 没在等 */

    /*
     * 关掉指定的几个标签（每一条自己带"哪一栏 + 哪一份文件"）。
     *
     * 给"删文件之前先把它开着的标签关掉"用：分栏时两栏可能同时开着它，
     * 而两栏的标签下标是各自算的，所以只能一条一条点名。
     */
    function closeTheseTabs(pairs, then) {
        if (!pairs || pairs.length === 0) {
            if (then)
                then()
            return
        }
        for (var i = 0; i < pairs.length; ++i)
            closeQueue.push({ pane: pairs[i].pane, index: -1, title: "",
                              path: pairs[i].path })
        if (then)
            closeQueueThen = then
        pumpCloseQueue()
    }

    /*
     * 关掉某一栏里的一串标签（快捷键 / 菜单那一类，下标是那一栏自己的）。
     */
    function requestCloseTabs(indices, then, pane) {
        var v = pane ? pane : activeView()
        if (!v)
            return
        if (then)
            closeQueueThen = then
        var docs = v.documents
        for (var i = 0; i < indices.length; ++i) {
            var d = docs[indices[i]]
            if (d)
                closeQueue.push({ pane: v, index: indices[i], title: d.title,
                                  path: d.filePath })
        }
        pumpCloseQueue()
    }

    function pumpCloseQueue() {
        if (pendingClose)
            return                      /* 正等着用户回答，答完自己会接着走 */

        while (closeQueue.length > 0) {
            var entry = closeQueue.shift()
            var index = locateQueued(entry)
            if (index < 0 || !entry.pane)
                continue                /* 等回答期间被别处关掉了，跳过 */

            if (entry.pane.documents[index].modified) {
                pendingClose = entry
                saveAsk.ask("“" + entry.pane.documents[index].title + "”有未保存的修改。",
                            "要保存这些修改吗？",
                            [ { label: "保存", primary: true },
                              { label: "不保存" },
                              { label: "取消" } ])
                return
            }
            finishCloseTab(index, entry.pane)
        }

        var then = closeQueueThen
        closeQueueThen = null
        if (then)
            then()
    }

    /*
     * 把队列里那一条重新对到现在的下标上。
     *
     * 对不上（同名的文件被换过、或者未命名标签挪了位）就返回 -1 ——
     * 宁可漏关一个，也不能关错文件。
     */
    function locateQueued(entry) {
        var v = entry.pane
        if (!v)
            return -1
        /* 按路径认最稳（两栏的文档池是同一份，关哪一栏都是同一份文件） */
        if (entry.path !== "") {
            var byPath = v.indexOfPath(entry.path)
            if (byPath >= 0)
                return byPath
        }
        var docs = v.documents
        var i = entry.index
        if (i >= 0 && i < docs.length && docs[i].title === entry.title)
            return i
        return -1
    }

    /*
     * 「保存 / 不保存 / 取消」答完了。
     *   0 = 保存（存不下就整串停下）  1 = 不保存  2 或 -1 = 取消（整串停下）
     */
    function answerSaveAsk(choice) {
        var entry = pendingClose
        pendingClose = null

        if (!entry || choice === 2 || choice === -1) {
            /* 用户取消：整串都别关了（和以前那个 return false 一样） */
            closeQueue = []
            closeQueueThen = null
            return
        }
        var index = locateQueued(entry)
        if (index < 0 || !entry.pane) {
            /* 问的那一份已经不在了：宁可什么都不关，也不能关错文件 */
            closeQueue = []
            closeQueueThen = null
            return
        }
        if (choice === 0) {
            /*
             * 存的是**那一栏那一份**：先切过去再走普通保存那条路
             * （saveFile 作用在"当前栏"上，所以这里要先把它切到前台）。
             */
            notePaneFocus(entry.pane)
            entry.pane.activateDocument(index)
            if (!saveFile()) {          /* 存不下：和以前一样，什么都不做 */
                closeQueue = []
                closeQueueThen = null
                return
            }
        }
        finishCloseTab(index, entry.pane)
        pumpCloseQueue()
    }

    /* 真把标签关掉（问句已经答完，或者本来就不需要问） */
    function finishCloseTab(index, pane) {
        var v = pane ? pane : activeView()
        if (!v)
            return
        var docs = v.documents
        var closedPath = (index >= 0 && index < docs.length) ? String(docs[index].filePath)
                                                             : ""
        v.closeDocument(index)

        /*
         * 第二栏最后一条标签关掉了 = 这一栏不用了：顺手把分栏收起。
         * （用户点右边那个小叉，要的就是"这一栏没了"；不收的话右边会留一条
         * 空标签栏，看着像没关干净。见 leaveSplitForEmptyMirror。）
         */
        if (editor.mirrorView && v === editor.mirrorView && v.documents.length === 0)
            leaveSplitForEmptyMirror()

        /*
         * 左树那一行的蓝底跟着撤掉。
         *
         * 用户报的：右边标签都关光了（编辑区回到欢迎页），左边还蓝着一行，
         * 看着像那份还开着。蓝底本来就是"你点开的是哪一份文件"
         * （selectedPath，见 openTreeFile / locateCurrentItem），关掉的正是它
         * 就该清掉 —— 留着的唯一结果就是"界面上没有这个文件了，树上却还选着"。
         *
         * 分栏时另一栏可能还开着同一份，那就不该撤（文件还在界面上）。
         */
        if (closedPath !== "" && closedPath === selectedPath
                && !pathStillOpen(closedPath))
            selectedPath = ""

        if (editor.mainView && editor.mainView.documents.length === 0
                && (!editor.splitting || (editor.mirrorView
                                          && editor.mirrorView.documents.length === 0)))
            editor.previewItem = null
    }

    /* 这个路径还开在某一栏里吗（分栏时两栏都要看） */
    function pathStillOpen(path) {
        if (path === "")
            return false
        if (editor.mainView && editor.mainView.indexOfPath(path) >= 0)
            return true
        if (editor.splitting && editor.mirrorView
                && editor.mirrorView.indexOfPath(path) >= 0)
            return true
        return false
    }

    /* 从后往前关，前面的下标才不会跟着挪 */
    function closeTabs(indices, pane) {
        indices.sort(function (a, b) { return b - a })
        requestCloseTabs(indices, null, pane)
    }

    /*
     * 关掉除 index 之外的标签。
     *
     * index / pane 不给就是"当前那一栏的当前标签"（菜单栏 / 快捷键那条路的
     * 语义）；标签右键菜单会传**点中的那一个**（以及它是哪一栏）—— 右键点的
     * 标签未必是激活的，不传的话"关闭其他"会把用户刚点的那一个也关掉。
     */
    function closeOtherTabs(index, pane) {
        var v = pane ? pane : activeView()
        if (!v)
            return
        if (index === undefined || index === null)
            index = v.currentIndex
        var rest = []
        for (var i = 0; i < v.documents.length; ++i)
            if (i !== index) rest.push(i)
        closeTabs(rest, v)
    }

    function closeAllTabs(pane) {
        var v = pane ? pane : activeView()
        if (!v)
            return
        var all = []
        for (var i = 0; i < v.documents.length; ++i)
            all.push(i)
        closeTabs(all, v)
    }

    /*
     * 全部保存（"文件"菜单那一条）。
     *
     * 分栏时两栏各自的标签都要存到 —— 文档池是共用的，同一份文档被两栏同时
     * 开着时只存一遍（按"存过哪些路径"去重，免得弹两次同一个对话框）。
     */
    function saveAll() {
        var done = ({})
        var panes = [ editor.mainView ]
        if (editor.splitting && editor.mirrorView)
            panes.push(editor.mirrorView)
        for (var p = 0; p < panes.length; ++p) {
            var v = panes[p]
            if (!v)
                continue
            for (var i = 0; i < v.documents.length; ++i) {
                var d = v.documents[i]
                if (!d.modified)
                    continue
                var key = d.filePath !== "" ? d.filePath : ("untitled:" + i)
                if (done[key])
                    continue
                done[key] = true
                notePaneFocus(v)
                v.activateDocument(i)
                if (!saveFile())
                    return
            }
        }
    }

    /*
     * 内容区 tab 上的右键菜单。
     *
     * 落点和菜单栏那套不一样：菜单栏是 openFor（挂在控件正下方），
     * 这里是 openAtPoint —— 菜单左上角紧贴鼠标右键的那一点。
     * 条目见 js/EditorMenus.js 的 tabMenu（关闭 / 关闭其他 / 关闭全部 + 分栏）。
     *
     * pane 是**右键点中的那一条标签栏属于哪一栏**：分栏之后两条标签栏各有
     * 自己的一组标签，"关闭其他"必须以那一栏为准。
     */
    function openTabMenu(pane, index, anchor, x, y) {
        var v = pane ? pane : activeView()
        ddMenu.openAtPoint(anchor, x, y,
                           Menus.tabMenu(v, index, shortcutOverrides(), splitState(v)))
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
    /*
     * 编辑区右键菜单里各条的动作名（自检核对用，见 src/SelfTest.cpp）。
     *
     * 走的是和 openEditorContextMenu 完全同一份构造 —— 断言里看到的条目
     * 就是菜单里弹出的条目。
     */
    function editMenuActs() {
        var items = Menus.editMenu(activeView(), shortcutOverrides(), {
            can: canPreviewMarkdown,
            on: markdownPreview,
            canFormat: currentFormatEngine() !== ""
        })
        var out = []
        for (var i = 0; i < items.length; ++i)
            out.push(items[i] && items[i].act !== undefined ? String(items[i].act)
                                                            : "separator")
        return out
    }

    /*
     * 预览里那份右键菜单的每一条（自检核对用：`分隔` 还是 `act:0/1`）。
     *
     * 走的是和 openPreviewContextMenu **完全同一份构造** —— 断言里看到的
     * 就是预览里右键弹出的那份。它和编辑区那份的区别只在于 preview: true
     * （见 js/EditorMenus.js 的 editMenu），所以这里也把 preview 写死成 true。
     */
    function editMenuPreviewActs() {
        var items = Menus.editMenu(activeView(), shortcutOverrides(), {
            can: canPreviewMarkdown,
            on: markdownPreview,
            canFormat: currentFormatEngine() !== "",
            preview: true,
            hasSelection: editor.previewHasSelection
        })
        var out = []
        for (var i = 0; i < items.length; ++i) {
            var it = items[i]
            if (!it || it.act === undefined) {
                out.push("separator")
                continue
            }
            out.push(String(it.act) + (it.disabled ? ":0" : ":1"))
        }
        return out
    }

    /*
     * tab 右键菜单的动作名（自检用）。
     *
     * pane 不给就用当前那一栏 —— 自检模拟的是"在界面上右键一条标签"，
     * 走的是和 openTabMenu 同一个构造。
     */
    function tabMenuActs(index, pane) {
        var v = pane ? pane : activeView()
        var items = Menus.tabMenu(v, index === undefined ? 0 : index,
                                  shortcutOverrides(), splitState(v))
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
        var items = Menus.settingsMenu(activeView(), menuOv())
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 设置菜单里**当前是灰的**那些条目的动作名（自检核对用）。
     *
     * 单独报一条清单，是因为"方案钉住了字号"这件事有两种坏法：一种是没置灰
     * （用户按下去看着改了，下次重推又被盖掉），另一种是全都灰了（扳机没接上，
     * 换方案时没重算）。只看生效字号分不出这两种，看这份清单能。
     */
    function settingsMenuLockedActs() {
        var items = Menus.settingsMenu(activeView(), menuOv())
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined && items[i].disabled === true)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 自检用：行高的档位表（现在只喂设置页的"行高 − / +"和这一条判据 ——
     * 菜单里那 7 档已经撤进 设置 → 字体 了，表本身还在 js/EditorMenus.js）。
     */
    function lineHeightSteps() { return Menus.lineHeightFactors() }

    /*
     * 视图菜单里各条的动作名（自检核对用，见 src/SelfTest.cpp）。
     *
     * 同样走"和弹出的那份同一个构造"：点"视图"弹出的就是 Menus.viewMenu。
     * 自检据此确认新的两条竖线开关确实进了菜单，而不是只在 C++ 里有属性。
     */
    function viewMenuActs() {
        var items = Menus.viewMenu(activeView(), shortcutOverrides())
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
        var keys = Folders.allFolderKeys(cbm.nodes)
        var open = 0
        for (var i = 0; i < keys.length; ++i)
            if (expanded[keys[i]]) ++open
        return { folderCount: keys.length,
                 openCount: open,
                 itemCount: treeRows.length,
                 entryCount: Store.entryCount,
                 rootPath: Store.rootPath,
                 importedCount: Store.importedFolders.length,
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

    /* 标题「项目 ∨」那份"看哪一份"菜单的动作名（自检核对用，同上） */
    function treeScopeMenuActs() {
        var items = Menus.treeScopeMenu(treeScope)
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 左树当前状态（自检量"全部折叠 / 全部展开 / 收起面板 / 定位"用）。
     *
     * panelWidth 是布局算出来的真实槽位宽度：面板收起来时它必须是 0，
     * 只把 folderTreeHidden 置上而宽度没跟着走，从界面上是能一眼看出来的。
     *
     * 认"当前打开的是哪一份文件"用**路径**（currentPath / selectedPath）：
     * 树上列的就是磁盘上的文件，没有条目 id 这回事了。
     */
    function treeState() {
        var keys = Folders.allFolderKeys(cbm.nodes)
        var open = 0
        for (var i = 0; i < keys.length; ++i)
            if (expanded[keys[i]]) ++open
        var files = 0
        for (var j = 0; j < cbm.nodes.length; ++j)
            files += cbm.nodes[j].files !== undefined ? cbm.nodes[j].files : 0
        return { folderCount: keys.length,
                 openFolders: open,
                 rows: treeRows.length,
                 /* 最外层那几行（日期文件夹 + 导入的根）：全部折叠时行数就是它 */
                 topLevelRows: cbm.nodes.length,
                 items: files,
                 entries: Store.entryCount,
                 hidden: folderTreeHidden,
                 width: folderTreeWidth,
                 panelWidth: folderTree.width,
                 newestFirst: newestFirst,
                 rootPath: Store.rootPath,
                 /* 定位用：当前标签对应哪个文件 / 按钮是不是可点 / 选中的是哪个 */
                 currentPath: currentTreePath(),
                 locateEnabled: canLocateCurrent(),
                 selectedPath: selectedPath,
                 /* 右键菜单正指着哪一行（灰黑底那一条） */
                 contextPath: folderTree.contextPath,
                 /* 定位的最后一步（滚进可视区）有没有真的生效 */
                 locatedVisible: folderTree.rowVisible(selectedPath),
                 /* 亮着蓝底的行：文件夹必须恒为 0，文件最多 1（见 FolderTree.highlightCounts） */
                 highlighted: folderTree.highlightCounts(),
                 /* 一级 / 二级图标各落在哪一列上（必须一样，见 FolderTree.iconColumnXs） */
                 iconColumns: folderTree.iconColumnXs(),
                 /* 树那块面板自己的 x（标题和箭头都换算成面板坐标再比，见 titleTextX） */
                 panelX: folderTree.mapToItem(null, 0, 0).x,
                 /* 标题「项目」左边缘的 x（必须和一级行的展开箭头对齐，见 titleTextX） */
                 titleTextX: folderTree.titleTextX(),
                 /* 一级那行的展开箭头在**面板坐标**里的 x（折算掉横向滚动，自检用） */
                 firstChevronPanelX: folderTree.firstRowChevronPanelX(),
                 /* 现在在看哪一份（project / projectFiles / openFiles，见 switchTreeScope） */
                 scope: treeScope,
                 /* 树里现在有几行文件夹（"项目文件"那个视图必须是 0） */
                 folderRows: folderRowCount() }
    }

    /* 当前行里有多少行是文件夹（自检用；"项目文件 / 打开的文件"两个视图里是 0） */
    function folderRowCount() {
        var n = 0
        for (var i = 0; i < treeRows.length; ++i)
            if (treeRows[i] && treeRows[i].kind === "folder")
                ++n
        return n
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
        ddMenu.openAtPoint(null, x, y, Menus.editMenu(view, shortcutOverrides(), {
            can: canPreviewMarkdown,
            on: markdownPreview,
            canFormat: currentFormatEngine() !== ""
        }))
    }

    /*
     * Markdown 预览里的右键菜单。
     *
     * 和编辑区那条**同一份构造**（Menus.editMenu），只是多传 preview: true：
     * 预览里那份是只读的渲染结果，所以"会改正文"的命令（撤销 / 剪切 / 粘贴 /
     * 删除行 / 格式化…）一律置灰，只留复制 / 全选 / 复制全文这些不改正文的 ——
     * 预览里右键最常用的就是"选中一段复制走"。
     *
     * 坐标口径和编辑区那条一样：(x, y) 是场景坐标，anchor 传 null 直接按宿主
     * 坐标用（见 DropdownMenu.openAtPoint）。
     */
    function openPreviewContextMenu(x, y) {
        ddMenu.openAtPoint(null, x, y, Menus.editMenu(view, shortcutOverrides(), {
            can: canPreviewMarkdown,
            on: markdownPreview,
            canFormat: currentFormatEngine() !== "",
            preview: true,
            hasSelection: editor.previewHasSelection
        }))
    }

    /* 自检用：模拟在预览里点右键（坐标是场景坐标，和真右键同一条路） */
    function simulatePreviewRightClick(x, y) {
        openPreviewContextMenu(x, y)
        return ddMenu.opened
    }

    /* 把预览里选中的那段复制到剪贴板（菜单里"复制"那条走它） */
    function copyPreviewSelection() {
        var t = editor.previewSelectedText
        if (t === "")
            return
        Cmd.copyText(t)
    }

    /* 全选预览里的正文（只读控件也能全选，选完可以复制） */
    function selectAllPreview() {
        editor.previewSelectAll()
    }

    /*
     * 当前这份文件能不能格式化 / 会用哪个工具（右键菜单那两条的可用状态）。
     *
     * 问的是 C++ 那侧（Fmt，见 src/Formatter.h）：只有它知道本机装没装
     * clang-format / prettier。菜单是**弹出那一刻**现算的，所以刚装完工具
     * 重开一次菜单就通了。
     */
    function currentFormatEngine() {
        var v = activeView()
        if (!v || !v.hasDocument)
            return ""
        return Fmt.engineLabel(v.language, v.filePath)
    }

    /* ------------------------------------------------------------------
     * 代码格式化（右键菜单 / Ctrl+Shift+F，见 src/Formatter.h）
     * ---------------------------------------------------------------- */

    /*
     * 格式化当前编辑器里的正文。
     *
     * 三件事按顺序做，**任何一步失败都不动正文**：
     *   1. 问 C++ 有没有可用的格式化器（本机工具 / 内置那几样）；
     *   2. 拿正文去格式化；
     *   3. 成功才写回去（EditorView.setText 把整份替换包成一步，能撤销）。
     *
     * 失败时报的是 lastError（"本机没装 clang-format…" / "第 3 行：标签没闭合"），
     * 而不是一句笼统的"格式化失败" —— 用户得知道下一步该干嘛。
     */
    function formatCurrent() {
        var v = activeView()
        if (!v || !v.hasDocument)
            return
        var engine = Fmt.engineLabel(v.language, v.filePath)
        if (engine === "") {
            notify("格式化", "这个语言没有可用的格式化器。\n\n"
                             + "装一个（C/C++ 装 clang-format，JS/JSON 装 prettier，"
                             + "Python 装 black），或者在设置 → 格式化里指定路径。")
            return
        }
        var before = v.currentText()
        var after = Fmt.format(before, v.language, v.filePath)
        if (after === before) {
            if (Fmt.lastError !== "")
                notify("格式化失败", Fmt.lastError)
            else
                notify("格式化", "已经是格式化好的样子了（用的是 " + engine + "）。")
            return
        }
        v.setText(after)
    }

    /* 把 JSON 重排一遍（不依赖任何外部工具，见 Formatter 的内置那几样） */
    function formatCurrentAsJson() {
        var v = activeView()
        if (!v || !v.hasDocument)
            return
        var before = v.currentText()
        var after = Fmt.format(before, "json", v.filePath)
        if (after === before) {
            notify("格式化 JSON", Fmt.lastError !== "" ? Fmt.lastError : "已经是排好的 JSON 了。")
            return
        }
        v.setText(after)
    }

    /* ------------------------------------------------------------------
     * 校验（中文用词 / 代码语法，见 src/Checker.h）
     * ---------------------------------------------------------------- */

    /*
     * 校验**当前那一栏**（分栏时两栏可能看着两份不同的文件，校验的是用户
     * 正在看的那一份）。
     *
     * 这是**手动的**：编辑区右键（或"编辑"菜单 / 快捷键）点了才跑，界面上不做
     * 自动校验 —— 它要联网、要花 token。结果也不弹卡片，而是直接画在正文里：
     * 出问题的地方一条波浪线，鼠标停上去弹详情（见 EditorViewItem::setCheckIssues），
     * 那句总结落在状态栏（StatusBar 直接读 Check.status）。
     * 所以这里先记住是哪一栏（checkPane）—— 结果回来时按它推给对应的编辑器。
     */
    property var checkPane: null

    function runCheck() {
        var v = activeView()
        if (!v || !v.hasDocument) {
            notify("校验", "先打开一份文件。")
            return
        }
        checkPane = v
        v.clearCheckIssues()
        Check.check(v.currentText(), v.language, v.filePath)
    }

    /*
     * 校验结果 -> 编辑区里的波浪线。
     *
     * 本地规则那部分（同步）和大模型那部分（异步）都会发 issuesChanged，
     * 所以这一份代码两边都管：整份清单推给那一栏，由编辑器自己重画波浪线
     * （见 EditorViewItem::setCheckIssues）。
     */
    Connections {
        target: Check

        function onIssuesChanged() {
            if (window.checkPane && window.checkPane.hasDocument)
                window.checkPane.setCheckIssues(Check.issues)
        }
    }

    /* ------------------------------------------------------------------
     * 文件对比（见 src/Diff.h + qml/components/DiffPane.qml）
     *
     * 一个会话 = 标签栏上多出来的一格 + 正文区那一份 DiffPane。
     * 两边记的都是**文档号**（池子里的稳定身份证，见 EditorViewItem.h 那段），
     * 不是标签下标：对比页里改字就是改那份文档，切回普通标签看到的也是它。
     *
     * 同一时刻只把一份会话显示出来（diffIndex 指着它），其余的留在列表里，
     * 点标签再换上去 —— DiffPane 只有一份，换的是它绑的那两个文档。
     * ---------------------------------------------------------------- */

    property var diffSessions: []
    property bool diffMode: false
    property int diffIndex: -1

    readonly property var diffTabTitles: diffSessions.map(function (s) { return s.title })

    function diffSessionOf(index) {
        return (index >= 0 && index < diffSessions.length) ? diffSessions[index] : null
    }

    function compareCurrentWithFile() {
        var v = activeView()
        compareTabWithFile(v ? v.currentIndex : -1, v)
    }

    /*
     * 拿第 index 个标签去和另一个文件比。
     *
     * 左边用**编辑器里那份文档**（可能有没存盘的改动，用户想看的正是这个）；
     * 右边是选中的那个文件 —— 文件框里按取消就退化成一份空白文档。
     */
    function compareTabWithFile(index, pane) {
        var v = pane ? pane : activeView()
        if (!v)
            return
        var docs = v.documents
        var doc = (index >= 0 && index < docs.length) ? docs[index] : null
        if (!doc) {
            notify("文件对比", "先打开一份文件。")
            return
        }
        var other = Cmd.chooseFileDialog(
            "选择要比对的文件（按取消 = 右边用一份空白文件）",
            "文本文件 (*.txt *.md *.cpp *.h *.py *.js *.json);;所有文件 (*.*)")
        openDiffSession(doc.docId, doc.title, other)
    }

    /* 新开一份对比：左边用当前文件，右边给一份空白文档（BC 里的 "New comparison"） */
    function compareWithBlank() {
        var v = activeView()
        var doc = v ? v.documents[v.currentIndex] : null
        if (!doc) {
            notify("文件对比", "先打开一份文件。")
            return
        }
        openDiffSession(doc.docId, doc.title, "")
    }

    function openDiffSession(leftDocId, leftTitle, rightPath) {
        var right = editor.diffPage.prepareRight(rightPath)
        var s = {
            leftDocId: leftDocId,
            rightDocId: right.docId,
            leftTitle: leftTitle,
            rightTitle: right.title,
            /* 右边是这次会话自己打开的那份文件时，关会话要把它从右栏收回去 */
            rightOwned: true,
            title: leftTitle + " ↔ " + right.title
        }
        var list = diffSessions.slice()
        list.push(s)
        diffSessions = list
        showDiffSession(diffSessions.length - 1)
    }

    function showDiffSession(index) {
        if (index < 0 || index >= diffSessions.length)
            return
        /* 换会话之前把上一对文档身上那层画的东西还回去 */
        if (diffMode)
            editor.diffPage.unbindPanes()
        diffIndex = index
        diffMode = true
        /* 预览和分栏抢这块正文区，进对比页先把它们让开 */
        if (markdownPreview)
            markdownPreview = false
        editor.diffPage.rebind()
    }

    function exitDiffMode() {
        if (!diffMode)
            return
        editor.diffPage.unbindPanes()
        diffMode = false
    }

    function closeDiffSession(index) {
        if (index < 0 || index >= diffSessions.length)
            return
        var wasCurrent = (index === diffIndex)
        var list = diffSessions.slice()
        var s = list.splice(index, 1)[0]
        diffSessions = list
        if (diffIndex > index)
            diffIndex -= 1
        if (diffSessions.length === 0) {
            exitDiffMode()
            diffIndex = -1
        } else if (wasCurrent) {
            diffIndex = Math.min(index, diffSessions.length - 1)
            editor.diffPage.rebind()
        }
        /* 右边那份是会话自己打开的：把标签从右栏收回去（文档本身留在池子里） */
        if (s && s.rightOwned)
            editor.diffPage.releaseRight()
    }

    /* 关掉所有对比会话（自检 / 关标签时用） */
    function closeAllDiffSessions() {
        exitDiffMode()
        diffSessions = []
        diffIndex = -1
    }

    /*
     * 自检用：对比页现在是个什么状态（见 src/SelfTestTools.cpp 的"对比页"那一节）。
     *
     * 单独一个函数不塞进 uiState()：那边是"界面摆没摆对"的一大堆量，
     * 这一节要的是两栏各自的行数和同一处差异的 y，凑在一起读不出重点。
     */
    function diffState() {
        var page = editor.diffPage
        return {
            mode: diffMode,
            index: diffIndex,
            tabs: diffSessions.length,
            titles: diffTabTitles,
            rows: Differ.rows.length,
            changes: Differ.changes.length,
            summary: Differ.summary,
            error: Differ.lastError,
            leadLeft: Differ.leadGapLeft,
            leadRight: Differ.leadGapRight,
            left: page.paneInfo(0),
            right: page.paneInfo(1),
            scroll: page.scrollState()
        }
    }

    /* 自检用：第 index 处差异在两栏各落在哪个 y（两个数相等才算对齐） */
    function diffChangeYs(index) {
        return editor.diffPage.changeYs(index)
    }

    /* 自检用：直接开一个会话（不弹文件框），两边都是已经开着的文档号 */
    function diffOpenForTest(leftDocId, rightDocId, leftTitle, rightTitle) {
        var list = diffSessions.slice()
        list.push({
            leftDocId: leftDocId, rightDocId: rightDocId,
            leftTitle: leftTitle, rightTitle: rightTitle,
            rightOwned: false,
            title: leftTitle + " ↔ " + rightTitle
        })
        diffSessions = list
        showDiffSession(diffSessions.length - 1)
    }

    /* 自检用：把左栏滚到第 line 显示行（验两栏同步） */
    function diffScrollLeftForTest(line) {
        editor.diffPage.pane(0).setFirstVisibleLine(line)
    }

    /* 自检用：跳到第 index 处差异，并回报"当前那一处亮不亮"的像素数 */
    function diffGotoForTest(index) {
        editor.diffPage.gotoChangeForTest(index)
        return editor.diffPage.currentBandStats()
    }

    /* 自检用：把第 0 处差异从一侧搬到另一侧（dir=1 左->右，-1 右->左） */
    function diffMergeForTest(dir) {
        editor.diffPage.showChange(0)
        editor.diffPage.mergeCurrent(dir)
    }

    /* 演示口子（SMARTCLIP_DIFF_DEMO=1，见 src/main.cpp）：两份现成的文件开成一页 */
    function diffDemo(pathA, pathB) {
        openTreeFile(pathA)
        var idA = view.currentDocId()
        openTreeFile(pathB)
        var idB = view.currentDocId()
        diffOpenForTest(idA, idB,
                        pathA.substring(pathA.lastIndexOf("/") + 1),
                        pathB.substring(pathB.lastIndexOf("/") + 1))
        /*
         * 把右边改一个脏：演示要能看见题头那个"保存"（它只在改过之后才出现）。
         * 顺带也多一处差异，正好看看三处的时候导航条长什么样。
         */
        editor.diffPage.pane(1).replaceLines(4, 1, ["第 5 行：演示用，这一行被改过。"])
        /* 摆位要等布局落定，晚一拍再打（只在演示这条路上跑） */
        Qt.createQmlObject('import QtQuick
            Timer { interval: 500; repeat: false; running: true
              onTriggered: {
                  console.log("DIFFDEMO " + JSON.stringify(window.diffState()))
                  /* 跳到第 2 处：演示要能看见"当前这一处亮一档"长什么样 */
                  window.diffGotoForTest(1)
              } }',
            window)
    }

    /*
     * 滚动条上的右键菜单。
     *
     * 和编辑区右键同一个组件（DropdownMenu），只是条目换成"滚动到这里 /
     * 边缘 / 翻页 / 滚一行"那七条 —— 原来这里是 Qt 自带的浅色英文菜单，
     * 和界面完全不搭（见 EditorViewItem::scrollBarContextMenuRequested）。
     */
    function openScrollBarContextMenu(horizontal, x, y) {
        ddMenu.openAtPoint(null, x, y, Menus.scrollBarMenu(horizontal))
    }

    /*
     * 滚动条右键菜单里各条的动作名（自检核对用，见 src/SelfTest.cpp）。
     * 和弹出的那份同一个构造。
     */
    function scrollMenuActs(horizontal) {
        var items = Menus.scrollBarMenu(horizontal === undefined ? true : horizontal)
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
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
        if (!activeView() || !activeView().hasDocument)
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
        var v = activeView()
        if (!v || !v.hasDocument)
            return
        askInput("转到行", "行号（1 - " + v.lineCount + "）", v.cursorLine,
                 { intMode: true, min: 1, max: v.lineCount },
                 function (text) {
                     var line = parseInt(text)
                     if (!(line >= 1 && line <= v.lineCount))
                         return
                     v.gotoLine(line)
                     v.requestEditorFocus()
                 })
    }

    /* 换行 / 行号 / 空白字符是**视图级**设置，两栏一起改（见 applyToPanes） */
    function toggleWrap() {
        var on = !view.wrapEnabled
        applyToPanes(function (v) { v.wrapEnabled = on })
        Cmd.remember("wrap", on ? "1" : "0")
    }

    function toggleLineNumbers() {
        var on = !view.lineNumbersVisible
        applyToPanes(function (v) { v.lineNumbersVisible = on })
        Cmd.remember("lineNumbers", on ? "1" : "0")
    }

    function toggleWhitespace() {
        var on = !view.whitespaceVisible
        applyToPanes(function (v) { v.whitespaceVisible = on })
        Cmd.remember("whitespace", on ? "1" : "0")
    }

    function showShortcuts() {
        settingsPanel.openSection("shortcuts")
    }

    /* 设置面板的"存储"那一栏（保存位置 / 导入的文件夹，见 SettingsPanel.qml） */
    function showStorage() {
        settingsPanel.openSection("storage")
    }

    /* 设置面板的"字体"那一栏 —— 顶栏菜单里那四组（字号/注释字号/字体/行高）撤到这里了 */
    function showFont() {
        settingsPanel.openSection("font")
    }

    /*
     * 自检用：点菜单栏某一栏（走 TopBar::activateTab，和鼠标同一个入口）。
     *
     * 「帮助」那一栏改成"直接开关于"之后，要有一条检查钉住它 —— 但不能在自检里
     * 另写一套触发方式，那样验的不是用户走的路。所以从这儿转一手。
     *
     * anchor 传 null：自检手里没有"被点的那一栏"，TopBar 会退回用应用图标当锚点
     * （鼠标点的时候传的是那一栏自己，见那边的 activateTab）。
     */
    function activateMenuTab(label) {
        topBar.activateTab(label, null)
    }

    /*
     * 自检用：某一栏在宿主窗口里的左边（见 TopBar::tabLeft）。
     *
     * 自检拿它和"菜单实际画在哪"比 —— 菜单要挂在被点的那一栏下方，不是窗口
     * 最左边（锚点写死成 appBadge 的时候就是整条偏到最左边）。
     */
    function topBarTabLeft(label) {
        return topBar.tabLeft(label)
    }

    /* 自检用：某一栏在宿主窗口里的上边（和 topBarTabLeft 配成一对，见 TopBar::tabTop） */
    function topBarTabTop(label) {
        return topBar.tabTop(label)
    }

    /*
     * 自检用：**像鼠标那样**点某一栏 —— 把那一栏自己当锚点传进去。
     *
     * 和 activateMenuTab 的区别：那个不传锚点（走"没有那一栏"的退路，拿应用图标
     * 当锚点，是给 `menu:xx` 命令用的）。要验"菜单挂在被点的那一栏下方"就必须
     * 走这一条 —— 否则验的是另一条路，锚点写错了也照样绿。
     */
    function clickMenuTab(label) {
        topBar.activateTab(label, topBar.tabItem(label))
    }

    /* 自检收尾用：关掉设置面板，别让它挂到进程退出那一刻再拆 */
    function closeSettings() {
        settingsPanel.hide()
    }

    function showAbout() {
        settingsPanel.openSection("about")
    }

    /* ------------------------------------------------------------------
     * 文档识别（见 src/DocImport.h + qml/components/DocCard.qml）
     * ------------------------------------------------------------------ */

    /*
     * 挑一个文档来识别。
     *
     * 走和"选择保存位置…"同一条路：**先让设置面板让开**再开文件框 —— 面板是
     * 置顶的原生窗口，不让开的话 Windows 的文件选择框会被它整块盖住
     * （见 withSettingsPanelAway 上面那段）。
     */
    function importDocumentDialog() {
        var path = withSettingsPanelAway(function () {
            return Cmd.chooseFileDialog("选择要识别的文档", Doc.fileFilter())
        })
        if (path === "")
            return
        importDocuments([path])
    }

    /*
     * 把一批文件交给识别队列（菜单、拖拽、以后别的入口都走这里）。
     *
     * 这里只做**能不能开始**这一层判断：命令配好了没有、文件认得认不了。
     * 真正的报错（缺依赖、认到一半失败）由 Doc 那边的 status / error 报出来。
     */
    function importDocuments(paths) {
        if (!paths || paths.length === 0)
            return

        var problem = Doc.runnerProblem()
        if (problem !== "") {
            Cmd.alert("识别程序还没配好",
                      problem + "\n\n设置 → 识别 里可以换一条命令；\n"
                      + "默认那条要装：pip install rapid-doc")
            settingsPanel.openSection("document")
            return
        }

        Doc.clearResult()
        Doc.enqueue(paths)
    }

    /*
     * "便签"那一格的右键菜单（新建 / 排列 / 显示全部 / 收起全部）。
     *
     * 落点和别的右键菜单一样是 openAtPoint：菜单左上角紧贴鼠标那一点。
     * 条目见 js/EditorMenus.js 的 notesMenu —— 走的是界面里共用那份
     * DropdownMenu（深色 + 图标 + 快捷键），和"帮助"那份同一个长相。
     */
    function openNotesCellMenu(anchor, x, y) {
        ddMenu.openAtPoint(anchor, x, y, Menus.notesMenu(notesMenuState(), shortcutOverrides()))
    }

    /* 那份菜单要的当前状态：便签总数 / 正摆在桌面上的条数（见 Menus.notesMenu） */
    function notesMenuState() {
        return { count: Notes.count, visible: Notes.visibleCount }
    }

    /*
     * 自检用：便签那格右键菜单的动作名（见 src/SelfTest.cpp）。
     * 和界面上弹出的是**同一份构造**（Menus.notesMenu），断言看到的就是用户能点的。
     */
    function notesMenuActs() {
        var items = Menus.notesMenu(notesMenuState(), shortcutOverrides())
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 左树一行上按右键：文件给"打开 / 重命名 / 删除 / 在文件夹中显示"，
     * 文件夹给"新建 / 刷新 / 在文件夹中显示"（导入的目录多一条移除）。
     *
     * 顺手把"菜单指着哪一行"记到树上（folderTree.contextPath）—— 那一行会画一层
     * 灰黑底，不然右键一个没打开的文件时，光看菜单看不出动的是哪一行。
     * 菜单收起时清掉（见下面 ddMenu 的 onClosed）。
     */
    function openTreeRowMenu(row, x, y) {
        if (!row)
            return
        folderTree.contextPath = row.path !== undefined ? row.path : ""
        var items = (row.kind === "file") ? Menus.fileContextMenu(row)
                                          : Menus.folderContextMenu(row)
        ddMenu.openAtPoint(null, x, y, items)
    }

    /*
     * 自检用：某个下拉菜单（"文件" / "编辑" …）里的动作名。
     * 和界面上弹的是**同一份**构造（TopBar.menuItems -> Menus.menuItems）。
     */
    function topMenuActs(label) {
        var items = topBar.menuItems(label)
        var out = []
        for (var i = 0; i < items.length; ++i) {
            if (items[i] && items[i].act !== undefined)
                out.push(String(items[i].act))
        }
        return out
    }

    /*
     * 自检用：像真的右键那样，为某一类行（"file" / "folder"）弹出右键菜单，
     * 返回那一行的路径。和界面走的是同一个 openTreeRowMenu。
     */
    function openTreeRowMenuFor(kind) {
        for (var i = 0; i < treeRows.length; ++i) {
            if (treeRows[i].kind !== kind)
                continue
            openTreeRowMenu(treeRows[i], 120, 200)
            return treeRows[i].path !== undefined ? String(treeRows[i].path) : ""
        }
        return ""
    }

    /*
     * 左树某一类行的右键菜单动作名（自检核对用，见 src/SelfTest.cpp）。
     *
     * kind 传 "file" / "folder"：找树里第一个这一类行，用它构造菜单 ——
     * 和界面上右键弹出的是**同一个构造**（Menus.fileContextMenu /
     * folderContextMenu），所以断言看到的就是用户能点的。
     */
    function treeRowMenuActs(kind) {
        for (var i = 0; i < treeRows.length; ++i) {
            var row = treeRows[i]
            if (row.kind !== kind)
                continue
            var items = (kind === "file") ? Menus.fileContextMenu(row)
                                          : Menus.folderContextMenu(row)
            var out = []
            for (var j = 0; j < items.length; ++j) {
                if (items[j] && items[j].act !== undefined)
                    out.push(String(items[j].act))
            }
            return out
        }
        return []
    }

    function dispatch(act) {
        if (act === undefined || act === null || act === "" || act === "none")
            return

        /*
         * 方案钉住的那几项在这儿再拦一道。菜单条目本身是置灰的（走不到这里），
         * 但字号/行高这些还有**快捷键**——QAction 在 C++ 侧注册，直接发命令过来，
         * 绕得过界面那排灰按钮。不拦的话按一下就真改设置（还会写回注册表），
         * 下一次重推又被方案盖掉，看着像"设置存不住"。
         */
        var lockKey = ""
        if (act.indexOf("fontSize:") === 0)
            lockKey = "size"
        else if (act.indexOf("commentFontSize:") === 0)
            lockKey = "commentSize"
        else if (act.indexOf("font:") === 0)
            lockKey = "family"
        else if (act.indexOf("lineHeight:") === 0 || act === "lineHeightDown"
                 || act === "lineHeightUp")
            lockKey = "lineHeight"
        else if (act === "toggleWrap")
            lockKey = "wrap"
        if (lockKey !== "" && fontLocked(lockKey))
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
                applyToPanes(function (v) { v.commentFontPixelSize = cpx })
            return
        }
        if (act.indexOf("font:") === 0) {
            var fam = act.substring(5)
            applyToPanes(function (v) { v.fontFamily = fam })
            return
        }
        if (act.indexOf("lineHeight:") === 0) {
            var lhf = parseFloat(act.substring(11))
            /* 越界由 C++ 侧夹住（1.0 ~ 3.0） */
            if (!isNaN(lhf))
                applyToPanes(function (v) { v.lineHeightFactor = lhf })
            return
        }
        /* 设置面板上的"行高 − / 行高 +"：按档位表走一格（表在 js/EditorMenus.js） */
        if (act === "lineHeightDown" || act === "lineHeightUp") {
            var nextLh = Menus.stepLineHeight(view.lineHeightFactor,
                                              act === "lineHeightUp" ? 1 : -1)
            applyToPanes(function (v) { v.lineHeightFactor = nextLh })
            return
        }
        /*
         * 终端字体这两条：改的是面板的**基线**（terminal.termFamily / termSize）并立刻落盘。
         * 配色方案钉住这两项时，设置里那两行是灰的、走不到这里；没钉的时候，
         * TerminalView 会自己重算"实际用来画字的那个值"（applySchemeFont）。
         */
        if (act.indexOf("termFont:") === 0) {
            terminal.termFamily = act.substring(9)
            Cmd.remember("termFontFamily", terminal.termFamily)
            return
        }
        if (act.indexOf("termFontSize:") === 0) {
            var tpx = parseInt(act.substring(13))
            if (!isNaN(tpx) && tpx >= 8 && tpx <= 40) {
                terminal.termSize = tpx
                Cmd.remember("termFontSize", String(tpx))
            }
            return
        }
        /*
         * 语言 / 编码 / 换行符是**按文档**设的（Scintilla 的样式表在文档里），
         * 所以只作用在"当前那一栏当前那一份"上 —— 另一栏看的是别的文件时
         * 不该被顺手改掉。
         */
        if (act.indexOf("lang:") === 0) { activeView().language = act.substring(5); return }
        if (act.indexOf("encoding:") === 0) {
            activeView().encoding = act.substring(9)
            return
        }
        if (act.indexOf("eol:") === 0) { activeView().eolMode = act.substring(4); return }
        if (act.indexOf("menu:") === 0) { topBar.openGroup(act.substring(5)); return }
        if (act.indexOf("folder:") === 0) { activateFolder(act.substring(7)); return }

        /*
         * 滚动条右键那七条：scroll:<h|v>:<动作>（见 js/EditorMenus.js 的
         * scrollBarMenu）。动作直接落到 QScrollBar 那一套上，见
         * EditorViewItem::scrollBarAction。滚的是**弹菜单那一栏**。
         */
        if (act.indexOf("scroll:") === 0) {
            var scrollParts = act.split(":")
            if (scrollParts.length === 3)
                activeView().scrollBarAction(scrollParts[1], scrollParts[2])
            return
        }

        /*
         * 带路径的文件动作（左树右键菜单用，见 js/EditorMenus.js 的
         * fileContextMenu / folderContextMenu）。路径里带盘符冒号，
         * 所以只能按前缀长度切，不能按 ':' 分。
         */
        if (act.indexOf("fileOpen:") === 0) { openTreeFile(act.substring(9)); return }
        if (act.indexOf("fileRename:") === 0) { renameTreeFile(act.substring(11)); return }
        if (act.indexOf("fileDelete:") === 0) { deleteTreeFile(act.substring(11)); return }
        if (act.indexOf("fileReveal:") === 0) { revealPath(act.substring(11)); return }
        if (act.indexOf("folderReveal:") === 0) { revealPath(act.substring(13)); return }
        if (act.indexOf("removeImport:") === 0) {
            removeImportedFolder(act.substring(13))
            return
        }
        /* ---- 左侧项目树（标题栏那排按钮 / 标题上的"更多"菜单） ---- */
        if (act === "treeNew") { newEntry(); return }
        /* "看哪一份"：标题「项目 ∨」那个菜单（见 js/EditorMenus.js 的 treeScopeMenu） */
        if (act.indexOf("treeScope:") === 0) { switchTreeScope(act.substring(10)); return }
        if (act === "treeLocate") { locateCurrentItem(); return }
        if (act === "treeExpandAll") { setAllFolders(true); return }
        if (act === "treeCollapseAll") { setAllFolders(false); return }
        if (act === "treeHide") { toggleFolderTree(); return }
        if (act === "treeSortNewest") { setNewestFirst(true); return }
        if (act === "treeSortOldest") { setNewestFirst(false); return }
        if (act === "treeImportFolder") { importFolder(); return }
        if (act === "treeOpenRoot") { revealPath(Store.rootPath); return }
        if (act === "treeChooseRoot") { chooseStorageRoot(); return }

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
        /* 截图：抓屏 -> 框选 -> 加文字 / 复制 / 保存 / 固定到桌面（见 src/Screenshot.h） */
        if (act === "shot") { Shot.beginCapture(); return }
        /*
         * 便签（见 src/StickyNotes.h / qml/notes/StickyNoteWindow.qml）。
         *
         * 四条命令的落点都在 Notes 那个单例上：新建会自己找一块没被占用的
         * 桌面摆好，排列是按便签所在那块屏的工作区摆成网格。这里不做任何
         * 二次处理 —— 托盘菜单点的是同一份实现，两边行为必须一模一样。
         */
        if (act === "note") { Notes.createNote(); return }
        if (act === "notesArrange") { Notes.arrangeAll(); return }
        if (act === "notesShowAll") { Notes.showAll(); return }
        if (act === "notesHideAll") { Notes.hideAll(); return }
        /*
         * 翻译卡片（见 src/Translate.h / qml/translate/TranslateCard.qml）。
         *
         * 只有一张卡片：叫出来 = 没有就建、有就 show + 置前（Trans.showCard）。
         * 图标条 / 托盘 / 快捷键落到的都是这一个入口。
         */
        if (act === "translate") { Trans.showCard(); return }
        if (act === "save") { saveFile(); return }
        if (act === "saveAs") { saveFileAs(); return }
        if (act === "saveAll") { saveAll(); return }
        /*
         * 关标签这一类（菜单栏 / 快捷键 / 标签右键菜单）作用在**当前那一栏**上
         * —— 分栏之后两栏各有自己的一组标签，"关当前标签"就是关前面那一栏里
         * 当前那一个。
         */
        if (act === "closeTab") { closeTab(activeView().currentIndex); return }
        if (act === "closeOtherTabs") { closeOtherTabs(); return }
        if (act === "closeAllTabs") { closeAllTabs(); return }
        if (act === "print") { activeView().printDocument(); return }
        /* 刷新 = 重扫磁盘（文件可能在别的程序里被改过 / 删过），再重建左树 */
        if (act === "refresh") { Store.rescan(); refresh(); return }
        /* 退出 = 真退出进程（理由见上面 quitAsk 那段：有便签时关窗口退不掉） */
        if (act === "quit") { Win.quitApp(); return }
        if (act === "clearsearch") { searchText = ""; topBar.clearSearch(); return }

        /* ---- 编辑（发给"当前编辑器"，见 activeView） ---- */
        if (act === "undo") { activeView().undo(); return }
        if (act === "redo") { activeView().redo(); return }
        if (act === "cut") { activeView().cut(); return }
        if (act === "copy") { activeView().copy(); return }
        if (act === "paste") { activeView().paste(); return }
        if (act === "selectAll") { activeView().selectAll(); return }
        if (act === "copyLine") { activeView().copyCurrentLine(); return }
        if (act === "copyAll") { activeView().copyAll(); return }
        if (act === "deleteLine") { activeView().deleteLine(); return }
        if (act === "duplicateLine") { activeView().duplicateLine(); return }
        if (act === "toggleComment") { activeView().toggleComment(commentPrefix()); return }
        /* 只读是**视图级**的，两栏一起切（免得只有一栏打不了字，像坏了） */
        if (act === "toggleReadOnly") {
            var ro = !view.readOnly
            applyToPanes(function (v) { v.readOnly = ro })
            return
        }
        if (act === "formatCode") { formatCurrent(); return }
        if (act === "formatJson") { formatCurrentAsJson(); return }
        if (act === "toggleMarkdownPreview") { toggleMarkdownPreview(); return }
        if (act === "checkFile") { runCheck(); return }
        /*
         * 预览里右键菜单那两条（见 js/EditorMenus.js 的 editMenu 与上面的
         * openPreviewContextMenu）：它们作用在**预览那个只读控件**上，
         * 不是编辑器，所以单独两个动作名，不复用 copy / selectAll。
         */
        if (act === "copyPreview") { copyPreviewSelection(); return }
        if (act === "selectAllPreview") { selectAllPreview(); return }

        /* ---- 文件对比 ---- */
        if (act === "compareWithFile") { compareCurrentWithFile(); return }
        if (act === "compareWithBlank") { compareWithBlank(); return }
        if (act.indexOf("compareTab:") === 0) {
            var cmpView = activeView()
            compareTabWithFile(parseInt(act.substring(11)), cmpView)
            return
        }

        /* ---- 分栏 ---- */
        if (act === "splitRight") { setSplit("right"); return }
        if (act === "splitDown") { setSplit("down"); return }
        if (act === "splitNone") { setSplit(""); return }

        /* ---- 查找 ---- */
        if (act === "find") { showFind(false); return }
        if (act === "replace") { showFind(true); return }
        if (act === "findNext") { findStep(true); return }
        if (act === "findPrev") { findStep(false); return }
        if (act === "goto") { gotoLine(); return }

        /* ---- 视图（这一组都是**视图级**设置，两栏一起改，见 applyToPanes） ---- */
        if (act === "zoomIn") { applyToPanes(function (v) { v.zoomIn() }); return }
        if (act === "zoomOut") { applyToPanes(function (v) { v.zoomOut() }); return }
        if (act === "zoomReset") { applyToPanes(function (v) { v.zoomReset() }); return }
        if (act === "toggleWrap") { toggleWrap(); return }
        if (act === "toggleTerminal") { toggleTerminalPanel(); return }
        if (act === "toggleLineNumbers") { toggleLineNumbers(); return }
        if (act === "toggleWhitespace") { toggleWhitespace(); return }
        if (act === "toggleIndentGuides") {
            var guides = !view.indentGuidesVisible
            applyToPanes(function (v) { v.indentGuidesVisible = guides })
            Cmd.remember("indentGuides", guides ? "1" : "0")
            return
        }
        if (act === "toggleGutterLine") {
            var gutter = !view.gutterLineVisible
            applyToPanes(function (v) { v.gutterLineVisible = gutter })
            Cmd.remember("gutterLine", gutter ? "1" : "0")
            return
        }
        if (act === "toggleRuler") {
            var ruler = !view.rulerVisible
            applyToPanes(function (v) { v.rulerVisible = ruler })
            Cmd.remember("rulerVisible", ruler ? "1" : "0")
            return
        }
        /* 字数参考线列号：菜单里的固定档位（rulerColumn:80）走这条 */
        if (act.indexOf("rulerColumn:") === 0) {
            var rc = parseInt(act.substring(12))
            if (!isNaN(rc) && rc >= 1 && rc <= 2000)
                applyToPanes(function (v) { v.rulerColumn = rc })
            return
        }
        /* 自定义列号：弹一张带输入框的自绘卡片（见 window.askInput） */
        if (act === "rulerColumnAsk") {
            askInput("字数参考线", "在第几个字后面画竖线（1 - 500）", view.rulerColumn,
                     { intMode: true, min: 1, max: 500 },
                     function (text) {
                         var picked = parseInt(text)
                         if (picked >= 1 && picked <= 500)
                             applyToPanes(function (v) { v.rulerColumn = picked })
                     })
            return
        }
        if (act === "toggleFolding") {
            var fold = !view.foldingEnabled
            applyToPanes(function (v) { v.foldingEnabled = fold })
            return
        }
        if (act === "foldAll") { applyToPanes(function (v) { v.foldAll() }); return }
        if (act === "unfoldAll") { applyToPanes(function (v) { v.unfoldAll() }); return }

        /* ---- 其它 ---- */
        if (act === "shortcuts") { showShortcuts(); return }
        if (act === "settings") { showShortcuts(); return }
        /* 设置面板的"存储"栏：保存位置 / 导入的文件夹 */
        if (act === "storage") { showStorage(); return }
        /* 顶栏「设置」菜单里那条"字体、字号、行高…" */
        if (act === "settingsFont") { showFont(); return }
        /* 设置面板的"翻译"栏：模型怎么配（翻译卡片上的提示会指到这儿） */
        if (act === "settingsTranslate") { settingsPanel.openSection("translate"); return }
        /* 设置 → 模型里那两个「选择…」（面板先让开，再开系统文件框） */
        if (act === "translateChooseExe") { chooseTranslateLocalFile("exe"); return }
        if (act === "translateChooseModel") { chooseTranslateLocalFile("model"); return }
        if (act === "translateChooseMmproj") { chooseTranslateLocalFile("mmproj"); return }
        if (act === "about") { showAbout(); return }
        /* 设置面板的"识别"栏：文档识别用哪条命令 */
        if (act === "settingsDocument") { settingsPanel.openSection("document"); return }
        /*
         * 设置 → 识别 里那个「选择…」（挑 venv 里的 python.exe）。
         *
         * 这条以前没有：面板那个按钮写的是 Cmd.chooseDocPython()，而
         * EditorController 上没这个函数 —— 点一次抛一次 TypeError，按钮是死的。
         * 函数一直在窗口根上躺着（见上面 chooseDocPython），只是没人接。
         */
        if (act === "docChoosePython") { chooseDocPython(); return }
        /*
         * 识别文档（文件菜单那一条）。文件框由 Main.qml 开、不由设置面板开 ——
         * 面板是置顶原生窗口，会把系统文件框整个盖住（见 withSettingsPanelAway）。
         */
        if (act === "docImport") { importDocumentDialog(); return }
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
     * 自检用：把宿主窗口挪一段（见 WindowHelper::moveHostForTest）。
     *
     * 走的是 QML 这一侧，不直接从 C++ 拿 WindowHelper 那个对象：
     * engine->singletonInstance 拿到的实例和 QML 里用的不是同一个
     * （见 src/SelfTest.cpp 里的记录）。量的是"宿主窗口一移动，菜单会不会收起来"。
     */
    function moveHostForTest(dx, dy) { return Win.moveHostForTest(dx, dy) }

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
            /* 下拉菜单：条目总高 / 画出来的高 / 上限（= 宿主可用高度），
               规则是"内容多高画多高，超过上限才出滚动条"（见 DropdownMenu.maxMenuHeight） */
            menuOpened: ddMenu.opened,
            /*
             * 弹窗"露出来之后"被挪过几次（见 DropdownMenu.openShifts）。
             *
             * 这个是给自检用的**违规计数**：规则是"位置在开之前就定死，之后只许
             * 长高长宽、不许挪" —— 挪一下在 Windows 上就是"旧画面按新位置合成
             * 一帧"（闪）。正常应该是 0。
             */
            menuOpenShifts: ddMenu.openShifts,
            /*
             * 主窗口的系统转场动画关掉了没有（见 src/WindowHelper.h）。
             * 关掉之前，最大化那一下系统会把上一次那张画面缩放过来 ——
             * 看着就是"窗口先跑到右边、还在放大"。
             */
            transitionsDisabled: Win.transitionsDisabled,
            /* 圆角是系统裁的（抗锯齿）还是自己遮罩裁的（1-bit、有台阶） */
            dwmRound: Win.dwmRound,
            menuHeight: ddMenu.menuHeight,
            menuContentHeight: ddMenu.entriesHeight,
            menuScrollable: ddMenu.scrollable,
            /* 菜单的上限（= 宿主可用高度，见 DropdownMenu.maxMenuHeight）：
               自检要拿它核"画出来的高度 = min(内容高, 上限)"这条规则 */
            menuMaxHeight: ddMenu.maxMenuHeight,
            /* 宿主可用高 / 子栏当场的上限：自检核"画出来的高 = min(内容高, 上限)" */
            menuHostHeight: ddMenu.hostHeight,
            submenuCap: ddMenu.submenuCap,
            /* 有图标的菜单：图标在左、快捷键在右（工具栏已取消） */
            menuHasIcons: ddMenu.hasIcons,
            /* 弹窗里已经建出来的条目数（"露出来之后再补内容"那条自检读它） */
            menuItemCount: ddMenu.itemCount,

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
            /* 往上翻了没有 + 面板自己的上下沿（不翻时 panelTop == submenuTop） */
            submenuFlipped: ddMenu.submenuFlipped,
            submenuPanelTop: ddMenu.submenuPanelTop,
            submenuBottom: ddMenu.submenuBottom,
            submenuRoomBelow: ddMenu.submenuRoomBelow,
            submenuRoomAbove: ddMenu.submenuRoomAbove,
            submenuNaturalHeight: ddMenu.submenuNaturalHeight,
            menuUsableBottom: ddMenu.usableBottom,
            submenuInset: ddMenu.submenuInset,
            submenuTop: ddMenu.submenuTop,
            submenuRowY: ddMenu.submenuRowY,
            menuPaneWidth: ddMenu.paneWidth,
            menuItemHeight: ddMenu.itemHeight,
            menuPaneGap: ddMenu.paneGap,
            menuTotalWidth: ddMenu.width,
            menuTotalHeight: ddMenu.height,
            /* 弹窗"该有多大"和"实际有多大"：不一致就是 Popup 没跟着长（子栏会被裁掉） */
            menuImplicitHeight: ddMenu.implicitHeight,

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
             * 诊断用：同一个弹窗顶边的三种读数，摆在一起才看得出它们是**三个坐标系**。
             *
             *   menuPlacedY  openFor 定下来的宿主内容区坐标（算剩余空间只能用这个）
             *   menuRawY     Popup.y —— open() **之前**和上面同一个数，之后被 Qt 改成
             *                屏幕坐标（实测宿主 y=30 时它读 635，宿主内容区顶边在屏幕 605）
             *   menuY        background.mapToGlobal 反推的宿主内位置（现算，不会过期）
             *
             * 自检"子栏不许压成 0 高"那条红就是拿第二个数当第一个数用踩出来的。
             */
            menuPlacedY: ddMenu.placedY,
            menuPlacedX: ddMenu.placedX,
            menuRawX: ddMenu.x,
            menuRawY: ddMenu.y,
            menuRawH: ddMenu.height,
            subEntriesH: ddMenu.subEntriesHeight,
            openCapNow: ddMenu.openCap,
            /*
             * 这次菜单锚在哪个控件上（宽度 / 高度 / 名字）。
             *
             * 自检据此确认"菜单挂在**被点的那一栏**下方"，而不是窗口最左边那个
             * 应用图标下面（锚点写错就是这么暴露的：菜单整条偏到最左边）。
             */
            menuAnchorWidth: window.lastMenuAnchor ? window.lastMenuAnchor.width : -1,
            menuAnchorHeight: window.lastMenuAnchor ? window.lastMenuAnchor.height : -1,

            /* 设置面板（存储那一栏的绑定会在打开时才算出来，自检据此确认它没报错） */
            settingsOpened: settingsPanel.opened,
            settingsSection: settingsPanel.section,
            /* 点面板外面的空白处还会不会把它收掉（见 SettingsPanel.closePolicy） */
            settingsClosesOnOutside: settingsPanel.closesOnOutsidePress,

            /*
             * 文档识别那张小卡片（见 qml/components/DocCard.qml）。
             *
             * 它是**独立原生窗口**，不在主窗口的场景树里 —— `findChild` 那种按
             * objectName 找 Item 的办法对它是没用的。所以照菜单 / 设置面板那套，
             * 把几何报出来给自检量。
             *
             * docCardBottomOverlap 是那条关键判据：卡片底边**有没有压到底部
             * 状态栏**。>0 就是压上了。
             */
            docCardOpened: docCard.visible,
            docCardX: docCard.x,
            docCardY: docCard.y,
            docCardWidth: docCard.width,
            docCardHeight: docCard.height,
            docCardShowing: docCard.showing,
            docCardShouldShow: docCard.shouldShow,
            docWindowUsable: Doc.windowUsable,
            storageRoot: Store.rootPath,
            /* 内容实际在哪儿（<保存位置>/剪贴板，见 ClipboardStore::contentRoot） */
            storageContentRoot: Store.contentRoot,
            storageFiles: Store.fileCount,
            storageEntries: Store.entryCount,
            storageImported: Store.importedFolders.length,

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
            /* 分栏（自检核对 dispatch 和两栏各自的文档） */
            splitMode: editor.splitMode,
            /* Markdown 预览（自检核对开关和那份右键菜单） */
            markdownPreview: window.markdownPreview,
            previewSelection: editor.previewSelectedText,
            canPreviewMarkdown: window.canPreviewMarkdown,
            previewFile: window.activeView() ? String(window.activeView().filePath) : "",
            /*
             * 两栏各自的正文 / 标题 / 当前文档号（自检核对"两栏是独立的
             * 标签栏，但同一份文档改一边另一边也变"）。
             */
            mirrorText: editor.mirrorView ? editor.mirrorView.currentText() : "",
            mainText: window.view ? window.view.currentText() : "",
            mainTabCount: window.view ? window.view.documents.length : 0,
            mirrorTabCount: editor.mirrorView ? editor.mirrorView.documents.length : 0,
            mainDocId: window.view ? window.view.currentDocId() : -1,
            mirrorDocId: editor.mirrorView ? editor.mirrorView.currentDocId() : -1,
            mainViewPath: window.view ? String(window.view.filePath) : "",
            mirrorViewPath: editor.mirrorView ? String(editor.mirrorView.filePath) : "",
            /*
             * 两栏**对象本身**（不是下标 / 拷贝）。
             *
             * 自检要按"哪一栏"发命令时得拿到那个 EditorViewItem：标签栏点小叉
             * 走的就是 tabCloseRequested(pane, index) → closeTab(index, pane)，
             * 自检要复现"点右栏那个小叉"就必须传同一个对象进去。
             */
            mainPane: editor.mainView,
            mirrorPane: editor.mirrorView,
            splitLayout: editor.splitLayoutState(),

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
             * 问答卡片开着的是哪一块（见 AskCard.qml）。
             * 自检据此确认"该弹的时候弹了、答完就收"—— 卡片是独立原生窗口，
             * 光看顶层窗口列表分不出是哪一块。
             */
            quitAskOpened: quitAsk.opened,
            saveAskOpened: saveAsk.opened,
            confirmAskOpened: confirmAsk.opened,
            noticeAskOpened: noticeAsk.opened,
            /*
             * 带输入框那张卡片（重命名 / 转到行 / 参考线列）。
             *
             * 自检要拿它确认：叫出来的是这张卡片，不是原生 QInputDialog；
             * 再按一次「确定」看值有没有真的回到逻辑里（回调没接上就是点了没反应）。
             */
            inputAskOpened: inputAsk.opened,
            inputAskTitle: inputAsk.title,
            inputAskHasField: inputAsk.inputSpec !== null,

            /*
             * 分隔线热区的纵向范围（自检里量它有没有越界）。
             *
             * splitterTop / splitterBottom 必须和中间行（midRow）的上下边界
             * 对齐：高了会压住顶栏菜单，低了会压住底部状态栏，
             * 那两条上也就能拖动左树宽度（改之前就是这个毛病）。
             */
            splitterTop: splitterMouse.y,
            splitterBottom: splitterMouse.y + splitterMouse.height,
            /*
             * 抓手的水平位置 vs 树面板**实时**的右边缘。
             *
             * splitterCenterX 读的是 splitterMouse.x 那条绑定的当前值，
             * treeRightLive 是当场算一遍 mapToItem 的现值。两者必须重合 ——
             * 绑定停在旧坐标（mapToItem 不给绑定建依赖）时这两个会差出去，
             * 用户看到的就是"编辑区中间一移动鼠标就变 <->"。
             */
            splitterCenterX: splitterMouse.x + splitterMouse.width / 2,
            treeRightLive: folderTree.mapToItem(window.contentItem, folderTree.width, 0).x,
            /*
             * 抓手的左边缘和那条缝的宽度：自检据此卡"抓手不许往左压到树面板的
             * 滚动条上"（左边缘必须 ≥ 面板右边缘）、以及"抓手别宽过那条缝太多"。
             */
            splitterLeftX: splitterMouse.x,
            splitterWidth: splitterMouse.width,
            /*
             * 自检要拿这两个数当判据：
             *   splitterEnabled —— 最大化之后那条缝还让不让拖（2026-09-23 他要的）
             *   splitterCursor  —— 悬在缝上时是什么光标。要的是标准系统光标
             *     Qt.SizeHorCursor（Windows 上就是 IDC_SIZEWE），和终端面板顶边那条
             *     拖高度的 Qt.SizeVerCursor 同一套字形；Qt.SplitHCursor 在 Windows 上
             *     走的是 Qt 自己画的 pixmap（qwindowscursor.cpp 里 Split* 不进
             *     standardCursors 表），两个挨着看就是一粗一细不配套。
             */
            splitterEnabled: splitterMouse.enabled,
            splitterCursor: splitterMouse.cursorShape,
            /* 自检按完那一下立刻读它：true = 按下确实落到这条抓手上了（分清"没送到"和"送了没效果"） */
            splitterPressed: splitterMouse.pressed,
            /* 命中的是哪个 item：自检拿它和 splitterLeftX/Width 对号 */
            splitterHitX: splitterMouse.x,
            splitterHitW: splitterMouse.width,
            gapWidth: splitterGap.width,
            midRowTop: window.mapFromItem(midRow, 0, 0).y,
            midRowBottom: window.mapFromItem(midRow, 0, 0).y + midRow.height,
            /*
             * 左边图标条和它底部那一组的位置（自检量"终端展开 / 最大化时
             * 那一格会不会跟着跑"，见 src/SelfTestTerminal.cpp）。
             *
             * 一律 mapToItem 当场算：写成属性绑定会被缓存（mapToItem 不给
             * 绑定建依赖，别处吃过这个亏），这里要的就是"每次读都是现值"。
             */
            stripVisible: navStrip.visible,
            midRowVisible: midRow.visible,
            stripTop: navStrip.mapToItem(window.contentItem, 0, 0).y,
            stripHeight: navStrip.height,
            termCellY: navTerminalCell.mapToItem(window.contentItem, 0, 0).y,
            termCellX: navTerminalCell.mapToItem(window.contentItem, 0, 0).x,
            themeCellY: themeCell.mapToItem(window.contentItem, 0, 0).y,
            navSlotWidth: navStripSlot.width,
            themeLight: Theme.light,
            themeChrome: String(Theme.c("#313335", Theme.rev)),
            /*
             * 字体生效值 + 方案钉住了哪几项（自检读这两组，见 src/SelfTest.cpp）。
             * 量的是 view 上**当下真正生效**的那个字号 / 行高 / 家族，不是注册表里
             * 存的数 —— 注册表那两份方案永远不许写脏，这条要自己证明给自检看。
             */
            fontPixelSize: view.fontPixelSize,
            fontFamilyNow: view.fontFamily,
            lineHeightNow: view.lineHeightFactor,
            wrapNow: view.wrapEnabled,
            fontLockedSize: Theme.fontOverride.size !== undefined,
            fontLockedFamily: Theme.fontOverride.family !== undefined,
            fontLockedLine: Theme.fontOverride.lineHeight !== undefined,
            fontLockedWrap: Theme.fontOverride.wrap !== undefined,
            /*
             * 浅色档下"挨着的两层"各自实际解析成什么颜色。
             *
             * 查表机制有个固有缺陷：两个不同的深色值可以映射到同一个浅色值，
             * 于是本来靠明度差分开的两层在白色档里塌成一片（页签条底 #1e1f22
             * 和选中页签底 #2b2d30 都翻成 #ffffff，就是"选中 tab 没背景"那次）。
             * 这里报的是**界面上真正在用的那层**（不是查表前的原始值），
             * 所以按角色写死过的位置也查得到。判据见 src/SelfTest.cpp 的 layerPairs。
             */
            layerChrome: String(contentRoot.color),
            layerTabStrip: String(editor.tabStripColor),
            layerTabActive: String(editor.tabActiveBg),
            layerPaper: String(editor.editorBg),
            layerSelection: String(Theme.c("#2f659c", Theme.rev)),
            layerNavSelected: String(Theme.c("#3a4a5a", Theme.rev)),
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
            tree: window.treeState(),

            /*
             * 便签（见 src/StickyNotes.h）。
             *
             * 便签是**独立顶层窗口**，不在主窗口这棵树里 —— 自检从别处
             * （StickyNotes::windowState）量它们的几何，这里量的是"主界面
             * 这一侧知不知道有那么几块"，也就是图标条那一格的状态。
             */
            noteCount: Notes.count,
            noteVisibleCount: Notes.visibleCount,
            noteFile: Notes.notesFilePath,

            /*
             * 翻译卡片（见 src/Translate.h）。
             *
             * 和便签一样是**独立顶层窗口**，这里量的是"主界面这一侧知不知道
             * 它摆着没有"—— 也就是图标条那一格的状态。
             */
            translateVisibleCount: Trans.visibleCount
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
         * 左树：宽度 / 是不是收起来 / 文件排序都按上次的样子回来。
         * 必须在 refresh() 之前 —— 列表就是按 newestFirst 排的。
         * 宽度照夹一遍：设置文件被手改成 3 这种值，面板会窄到连按钮都放不下。
         */
        var treeW = parseFloat(Cmd.recall("treeWidth", "300"))
        if (!isNaN(treeW))
            folderTreeWidth = Math.max(folderTreeMinWidth,
                                       Math.min(folderTreeMaxWidth, treeW))
        folderTreeHidden = Cmd.recall("treeHidden", "0") === "1"
        newestFirst = Cmd.recall("treeNewestFirst", "1") === "1"

        /*
         * 底部终端面板：高度照上次的样子回来，但**默认永远是收起的**。
         *
         * 故意不记开合状态：这是个会起 shell 的面板，开机就弹一个终端出来
         * 不是"恢复上次状态"，是打扰 —— 而且上一次那个 shell 早没了，
         * 展开只会是一个全新的会话。
         */
        var termH = parseFloat(Cmd.recall("termHeight", "260"))
        if (!isNaN(termH))
            terminalHeight = Math.max(terminalMinHeight, Math.min(700, termH))
        terminalHidden = true

        /* 终端字体的基线（方案钉住时上面那两条只是"没钉才用"的那份） */
        var tf = Cmd.recall("termFontFamily", "Cascadia Mono")
        if (tf !== "")
            terminal.termFamily = tf
        var ts = parseInt(Cmd.recall("termFontSize", "13"))
        if (!isNaN(ts) && ts >= 8 && ts <= 40)
            terminal.termSize = ts

        /*
         * Markdown 预览那个偏好（上次退出时看的是预览还是源码）。
         *
         * 只读偏好，不在这里 applyMarkdownPreference()：这一步跑在恢复标签之前，
         * 当时还没有任何文档，算了也是白算 —— 真正落地是文档变了的时候。
         */
        markdownPreviewPreferred = Cmd.recall("markdownPreview", "0") === "1"

        /* 分栏不在启动时恢复：开局就是单栏（见上面那段说明） */

        /*
         * 展开状态：设置里存的是"哪些文件夹开着"（一行一个 key）。
         *
         * 注意顺序：refresh() 要先跑 —— "第一次启动默认展开今天那一组"要知道
         * 今天那个日期目录的 key，而 key 是数据里带出来的，得先有数据。
         */
        refresh()

        var openKeys = Cmd.recall("treeExpanded", "")
        var map = ({})
        if (openKeys !== "") {
            var openList = openKeys.split("\n")
            for (var t = 0; t < openList.length; ++t)
                if (openList[t] !== "")
                    map[openList[t]] = true
        } else {
            /* 第一次启动：一进来就看见最新的那一组，而不是一排折着的空文件夹 */
            var todayKey = Folders.firstDateKey(cbm.nodes)
            if (todayKey !== "")
                map[todayKey] = true
        }
        expanded = map
        rebuild()

        /*
         * 恢复上次的字号 / 换行 / 行号 / 空白字符设置。
         * 字号出厂默认 12（见 EditorArea.editorFontSize 与 EditorViewItem）。
         * 这些是**视图级**设置，两栏一起设（第二栏这会儿还没建出来，
         * 它在 EditorArea 里直接绑 editorFontSize / 自己读同一份设置，
         * 见分栏那一段）。
         */
        /*
         * 先原样收下注册表那几项（越界/写坏就当没设过，用出厂默认），再让
         * applyFontScheme() 把方案的 font 段盖上去。顺序不能反：方案那一档以后
         * 改动或删掉，回落的是这里记下的 storedFont，不是出厂默认。
         */
        var size = parseInt(Cmd.recall("fontSize", "12"))
        var commentSize = parseInt(Cmd.recall("commentFontSize", "0"))
        var lineHeight = parseFloat(Cmd.recall("lineHeight", "1"))
        var family = Cmd.recall("fontFamily", "Consolas")
        storedFont.size = (!isNaN(size) && size >= 6 && size <= 72) ? size : 12
        storedFont.commentSize = (!isNaN(commentSize) && commentSize >= 0 && commentSize <= 72)
                                 ? commentSize : 0
        storedFont.lineHeight = isNaN(lineHeight) ? 1.0 : lineHeight
        storedFont.family = family !== "" ? family : "Consolas"
        storedFont.wrap = Cmd.recall("wrap", "0") === "1"
        applyFontScheme()
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

        /* 上面这些是"两栏一起"的设置，第二栏建得比这里晚，补设一遍 */
        applyToPanes(function (v) {
            v.commentFontPixelSize = view.commentFontPixelSize
            v.fontFamily = view.fontFamily
            v.lineHeightFactor = view.lineHeightFactor
            v.wrapEnabled = view.wrapEnabled
            v.lineNumbersVisible = view.lineNumbersVisible
            v.whitespaceVisible = view.whitespaceVisible
            v.indentGuidesVisible = view.indentGuidesVisible
            v.gutterLineVisible = view.gutterLineVisible
            v.rulerVisible = view.rulerVisible
            v.rulerColumn = view.rulerColumn
        })
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
            /*
             * 方案钉住的那一项，注册表里存的**永远是用户自己设的那个值**。
             * 不这么挡一下，方案推下来的字号会被当成"用户改的"记回去 —— 换了方案
             * 那一项就再也回不去了（而且用户那份被悄悄改脏，比看不见还难查）。
             *
             * 这里没用一个"正在推"的标志位：C++ 侧那一下 fontChanged 常常是
             * **下一拍**才发的（applyStyle 排在事件队列里），推的函数早返回了、
             * 标志位已经落回去，照样写脏。按"这一项被不被方案钉着"判就没这个时间窗。
             */
            Cmd.remember("fontSize", fontLocked("size") ? String(storedFont.size)
                                                        : String(editor.view.fontPixelSize))
            Cmd.remember("commentFontSize",
                         fontLocked("commentSize") ? String(storedFont.commentSize)
                                                   : String(editor.view.commentFontPixelSize))
            Cmd.remember("fontFamily", fontLocked("family") ? storedFont.family
                                                           : editor.view.fontFamily)
            Cmd.remember("lineHeight", fontLocked("lineHeight") ? String(storedFont.lineHeight)
                                                                : String(editor.view.lineHeightFactor))
        }
        function onWrapChanged() {
            Cmd.remember("wrap", fontLocked("wrap") ? (storedFont.wrap ? "1" : "0")
                                                    : (editor.view.wrapEnabled ? "1" : "0"))
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
        /*
         * 编辑区里保存了一份文件之后，重扫一遍元数据。
         *
         * 内容改了之后条目摘要 / 计数就变了（数据库里存的是元数据，不是正文），
         * 不重扫的话左树右边那个"几条"还是旧的，搜索也搜不到新写进去的东西。
         *
         * 用 Qt.callLater 推到下一拍再扫：这个信号是在 C++ 的保存流程里发出来的，
         * 当场就在里面扫盘 + 重建左树，等于在保存到一半的时候再扎回去一圈。
         */
        function onSaved(path) {
            if (path !== "")
                Qt.callLater(function () { Store.rescan() })
            /* 存过盘之后正文那份文件变了，预览跟着刷新一遍 */
            if (window.markdownPreview)
                Qt.callLater(function () { window.refreshMarkdown() })
        }

        /*
         * 正文长度变了（打字 / 粘贴 / 撤销都会走到 statsChanged）：
         * 预览开着就重渲染。
         *
         * 用 statsChanged 而不是"专门的正文变化信号"：这个类里跟正文内容有关
         * 的通知就它一个（行列 / 字数 / 撤销状态那一组）。重渲染很便宜
         * （一份 md 几毫秒），多触发几次只是白花点 CPU；漏一次用户看到的就是
         * "改了源码预览不跟着变"。
         */
        function onStatsChanged() {
            if (window.markdownPreview && editor.view.hasDocument)
                window.refreshMarkdown()
        }

        /*
         * 文档池变了（打开 / 关闭 / 切标签）：预览要按新文档重算。
         *
         * 分栏**不在这里**跟着开 / 关：布局只由 setSplit（用户点的）和
         * "第二栏被关空"（见 leaveSplitForEmptyMirror）决定 —— 原来这里有一条
         * "第二栏空着就补一份主栏当前文档"，它正是"右边小叉点了没反应"的原因。
         */
        function onDocumentsChanged() {
            window.applyMarkdownPreference()
            /* "打开的文件"那个视图列的就是这些标签：开 / 关一个都要跟着重排 */
            if (window.treeScope === "openFiles")
                window.rebuild()
        }

        /* 换了文档（切标签）：预览按当前这份重算（从 md 切到 .cpp 时自动关掉） */
        function onCurrentChanged() {
            window.applyMarkdownPreference()
        }

        /* 某一栏被点了：把"当前编辑器"切过去（状态栏那几项跟着变） */
        function onPaneFocused() { window.notePaneFocus(editor.view) }

    }

    /*
     * 分栏时**镜像那一栏**被点了（主栏那个是上面 Connections 里的）。
     * 两栏都要接，否则点右边那一栏时命令还落在主栏上。
     */
    Connections {
        target: editor.mirrorView
        function onPaneFocused() { window.notePaneFocus(editor.mirrorView) }
        /* 第二栏开 / 关标签也要重排"打开的文件"那个视图（两栏都算） */
        function onDocumentsChanged() {
            if (window.treeScope === "openFiles")
                window.rebuild()
        }
    }

    /*
     * 第二栏那几个"跟着主栏走"的设置变化时也要照样记下来（两边是同一套值，
     * 只是信号是从第二栏发出来的 —— 比如用户在第二栏按了 Ctrl+滚轮缩放）。
     */
    Connections {
        target: editor.mirrorView
        function onFontChanged() {
            Cmd.remember("fontSize", String(editor.view.fontPixelSize))
        }
        function onWrapChanged() {
            Cmd.remember("wrap", editor.view.wrapEnabled ? "1" : "0")
        }
    }

    Connections {
        target: editor.view

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

        /* 主栏里按下右键：弹 QML 那套"编辑"菜单（见 openEditorContextMenu） */
        function onContextMenuRequested(x, y) {
            window.notePaneFocus(editor.view)
            window.openEditorContextMenu(x, y)
        }

        /* 滚动条上按下右键：同一个组件，条目换成滚动那七条 */
        function onScrollBarContextMenuRequested(horizontal, x, y) {
            window.notePaneFocus(editor.view)
            window.openScrollBarContextMenu(horizontal, x, y)
        }

        /*
         * 主栏那一份正文变了：第二栏如果正看着**同一份文档**，它的画面也得
         * 重画（两栏是同一个底层文档，Scintilla 不会自己通知另一个视图）。
         *
         * 这就是"改一边另一边立刻变"。m_syncing 那类防回环在这里不需要：
         * mirrorPane 的 refreshSharedDocument() 只做 viewport 重画，不写正文。
         */
        function onTabsChanged() {
            if (editor.splitting && editor.mirrorView)
                editor.mirrorView.refreshSharedDocument()
        }
    }

    Connections {
        target: editor.mirrorView

        function onErrorOccurred(message) {
            if (!Cmd.selfTestMode)
                Cmd.alert("出错了", message)
        }

        function onContextMenuRequested(x, y) {
            window.notePaneFocus(editor.mirrorView)
            window.openEditorContextMenu(x, y)
        }

        function onScrollBarContextMenuRequested(horizontal, x, y) {
            window.notePaneFocus(editor.mirrorView)
            window.openScrollBarContextMenu(horizontal, x, y)
        }

        function onTabsChanged() {
            if (editor.mainView)
                editor.mainView.refreshSharedDocument()
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
        /* 自检按名字找它那块原生窗（见 src/SelfTest.cpp 的"露出来之后不许再变"） */
        objectName: "dropdownMenu"
        parent: window
        onSelected: (act) => window.dispatch(act)
        /*
         * 菜单一收，左树那一行的"右键底色"也跟着撤（见 openTreeRowMenu）。
         * 别的菜单（tab / 编辑区 / 滚动条）收起时这里清的是个空串，无害。
         */
        onClosed: folderTree.contextPath = ""
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
     * 自检用：一个贴着窗口右边缘的小格子 + 挂在它上面的提示框。
     *
     * 量的是"居中会不会越界"：原来 AppToolTip 的 x 是 (父宽 - 气泡宽)/2，
     * 父项本身贴右边缘时气泡右半边就跑到窗口外面了（标签栏那个
     * "源码 / 预览"开关实测被切掉半截）。这里造一个同样的位置让自检量得到。
     */
    Item {
        id: tipEdgeProbe

        width: 24
        height: 24
        anchors.right: parent.right
        anchors.top: parent.top

        AppToolTip {
            id: tipRightProbe

            text: "预览 Markdown（Ctrl+Shift+V）—— 这段文字够长，居中就会越界"
            visible: false
        }
    }

    /*
     * 自检用：量"提示框弹出来之后有没有越过窗口左右两边"。
     *
     * 必须真的 open 一次：Popup 收着的时候 x 读出来是旧值（实测写 -486 进去，
     * 关着的时候读回 0，open 之后才落地），所以不能只调 place() 再看属性。
     * open 走的就是真路径 —— onAboutToShow 里那个 place() 会被触发。
     * 透明度置 0 免得自检时右上角闪一块气泡。
     */
    function tipEdgeProbeState() {
        tipRightProbe.opacity = 0
        tipRightProbe.open()
        const btn = tipEdgeProbe.mapToItem(null, 0, 0)
        const state = {
            windowWidth: window.width,
            tipLeft: btn.x + tipRightProbe.x,
            tipRight: btn.x + tipRightProbe.x + tipRightProbe.width,
            buttonLeft: btn.x,
            buttonRight: btn.x + tipEdgeProbe.width,
            tipWidth: tipRightProbe.width
        }
        tipRightProbe.close()
        tipRightProbe.opacity = 1
        return state
    }

    /*
     * 设置面板（快捷键 / 关于）。
     *
     * 和下拉菜单一样是**原生弹窗**（见 qml/components/SettingsPanel.qml 开头）：
     * 左边是操作步骤、右边是具体内容，编辑区那个原生子窗口盖不住它。
     */
    SettingsPanel {
        id: settingsPanel
        /* 自检按名字找它那块原生窗（同上） */
        objectName: "settingsPanel"
        /* 它现在是一块顶层 Window，不能再当子项挂 parent；居中用的宿主矩形现读 Win.hostScreenGeometry() */
        view: window.view
        entries: window.shortcutItems
        /* 设置 → 字体 那两行终端的基线（生效值由面板自己合：方案钉住就用方案的） */
        termFamilyNow: terminal.termFamily
        termSizeNow: terminal.termSize
        onCommandRequested: (act) => window.dispatch(act)
        /* 「配色方案」那一栏的"另存为…"要一个带输入框的弹框，卡片挂在窗口这一侧 */
        askText: function (title, hint, value, then) {
            window.askInput(title, hint, value, {}, then)
        }
    }

    /*
     * 校验结果没有卡片：它直接画在正文里（波浪线 + 悬浮详情），见上面
     * runCheck 那段和 EditorViewItem::setCheckIssues。
     */

    /*
     * 文件对比的正文页在 editor 里面（qml/components/DiffPane.qml）；
     * 它只负责摆两栏和画差异，会话列表在上面的"文件对比"那一节。
     */

    /*
     * 文档识别要打开某份新笔记（卡片上那个"打开笔记"按钮，见 DocImport::openLast）。
     *
     * 打开笔记要动左树 + 编辑器（都要 Main.qml 这一层的东西），所以识别那边只
     * 报"该打开这个文件了"，这里接住 —— 和 RecognitionCard 把动作交回给
     * CaptureOverlay 是同一个分工。
     */
    Connections {
        target: Doc
        function onOpenRequested(path) {
            window.openTreeFile(path)
        }
    }

    /*
     * 主窗口现在可不可用 —— 识别那张卡片据此决定收不收起来（见 DocCard.qml）。
     *
     * 卡片是**独立置顶的原生窗口**，不跟着主窗口隐藏，所以要有人告诉它"主窗口
     * 没了"。自检里主窗口是故意不显示的，那种情况下 Doc.windowUsable 由 C++ 侧
     * 强制打开（见 DocImport 的构造）—— 这里的绑定会被那一次赋值打断，
     * 正是要的效果。
     */
    Binding {
        target: Doc
        property: "windowUsable"
        value: window.visible
    }

    /*
     * 把文档拖进主窗口 = 识别它。
     *
     * 拖拽热区铺满整个窗口（DropArea 不参与布局，只收拖放事件）。做成整窗而不是
     * 某一块：用户手里拿着一个 PDF 往回拖的时候，不会去瞄"该放在哪个格子里"。
     *
     * 只收文件（hasUrls）—— 从浏览器里拖一段选中的文字过来也走拖放，那种不该
     * 触发识别（那是剪贴板那条路的事）。
     */
    DropArea {
        id: docDrop
        anchors.fill: parent
        onDropped: (drop) => {
            if (!drop.hasUrls)
                return
            var files = []
            for (var i = 0; i < drop.urls.length; ++i) {
                /*
                 * 只认本地文件。
                 *
                 * 路径不能自己从 "file:///C:/a.pdf" 上切字符串 —— "file:///"
                 * 是 8 个字符，但 "file:///C:/…" 里那个 C 前面还有个斜杠，
                 * 切 8 位会把盘符第一个字母吃掉。交给 Qt 转最稳。
                 */
                var local = drop.urls[i].toLocalFile()
                if (local !== "")
                    files.push(local)
            }
            if (files.length > 0)
                window.importDocuments(files)
        }
    }

    /*
     * 四块问答卡片：关闭窗口 / 未保存改动 / 确认 / 提示。
     *
     * 都是同一个组件（qml/components/AskCard.qml），也是和上面两个一样的
     * Popup.Window：一个只占自己一小块的原生小窗，底下的界面原封不动
     * （不压暗、不遮住、不挡鼠标）。为什么不做成 C++ 的 QMessageBox，见
     * AskCard.qml 开头。
     */
    AskCard {
        id: quitAsk
        /* 自检按名字找它那块原生窗（见 src/SelfTest.cpp 里"问句是一块小卡片"） */
        objectName: "quitAskCard"
        parent: window
        onAnswered: (choice) => {
            if (choice === 0) Win.hideToTray()
            /* 「完全退出」= 真退出（见 WindowHelper::quitApp）：桌面上摆着便签时，
               关主窗口是退不掉进程的 —— 便签都是各自的顶层窗口，Qt"最后一个窗口
               关掉就退出"那条规则不成立，看着就是"只收进了托盘" */
            else if (choice === 1) Win.quitApp()
        }
    }

    /* 未保存改动：保存 / 不保存 / 取消（答完接着走关标签那条队列） */
    AskCard {
        id: saveAsk
        parent: window
        onAnswered: (choice) => window.answerSaveAsk(choice)
    }

    /* 删除 / 移除这类确认：确定 / 取消 */
    AskCard {
        id: confirmAsk
        parent: window
        onAnswered: (choice) => window.answerConfirm(choice)
    }

    /* 出错提示：只有一个「知道了」 */
    AskCard {
        id: noticeAsk
        parent: window
    }

    /*
     * 要用户敲字的那三类（重命名 / 转到行 / 字数参考线列）。
     *
     * 以前是原生 QInputDialog：系统标题栏 + 英文 OK/Cancel，和界面两套观感
     * （用户拿三张截图对过：「这两个弹框，换成退出这样的自绘框」）。现在和
     * 退出那张共用 AskCard，只是多一个输入框。三处都用这同一个实例 ——
     * 同一时刻只可能开着一个是自然的（它们都由菜单/快捷键触发）。
     */
    AskCard {
        id: inputAsk
        /* 自检按名字找它那块原生窗（见 src/SelfTest.cpp 里"问句是一块小卡片"） */
        objectName: "inputAskCard"
        parent: window
        /* 答完（含 Esc / 点外面）把键盘焦点还给编辑区 —— 只在此前它确实在编辑区时 */
        onAnswered: {
            if (!window.inputAskBackToEditor)
                return
            window.inputAskBackToEditor = false
            var v = window.activeView()
            if (v)
                v.requestEditorFocus()
        }
    }

    /* 弹输入卡片之前，编辑区有没有拿着键盘焦点（决定答完要不要还回去） */
    property bool inputAskBackToEditor: false

    /*
     * 弹一张带输入框的卡片。
     *
     * then 是 function(文字)：点「确定」回内容（已 trim），取消 / Esc 回空串。
     * 卡片是异步的，所以调用方原来那句 `var x = Cmd.askText(...)` 之后的逻辑
     * 全得搬进这个函数里。
     */
    function askInput(title, hint, value, opts, then) {
        /*
         * 先把编辑区那个原生子窗口的键盘焦点摘掉，不然卡片里打不进字。
         *
         * 编辑区是真的 QScintilla 子窗口（不是 QML 画的），它拿着焦点时按键
         * 根本进不了 QML —— 这条在查找栏上踩过一次（见 FindBar.focusField 的
         * 记录：打开查找栏后打字全进了正文），换成自绘输入卡片又踩一次：
         * 用户报的是"退格删不掉、数字也打不进"。
         * 以前那个原生 QInputDialog 是 QtWidgets 的窗口，焦点切换是 QDialog
         * 自己做的，换成卡片就得自己做。
         */
        var v = activeView()
        window.inputAskBackToEditor = !!(v && v.hasEditorFocus())
        if (v)
            v.releaseEditorFocus()
        inputAsk.askInput(title, hint, value, opts,
                          [ { label: "确定", primary: true },
                            { label: "取消" } ],
                          then)
    }

    /* 关闭键那个问句：完全退出，还是收进托盘 */
    function openQuitAsk() {
        quitAsk.ask("完全退出，还是收进托盘？",
                    "收进托盘：程序继续运行，托盘图标右键能截图，截图快捷键也还能用。\n"
                    + "完全退出：所有功能停止（截图、剪贴板都不再用）。",
                    [ { label: "收进托盘", primary: true },
                      { label: "完全退出" },
                      { label: "取消" } ])
    }

    /* 自检用：直接收掉那块卡片（弹窗是原生窗口，外面不好直接操作） */
    function closeQuitAsk() { quitAsk.close() }

    /*
     * 自检用：当作用户点了「未保存」卡片上的第 index 个按钮。
     *
     * 走 card.answer() 而不是直接调 answerSaveAsk()：前者才是真的那条路
     * （收卡片 -> 发 answered -> 流程接着走），后者只模拟了后半截。
     */
    function clickSaveAsk(index) { saveAsk.answer(index) }

    Connections {
        target: Win
        function onQuitRequested() { window.openQuitAsk() }
    }

    /* C++ 那侧要弹"出错提示"（Cmd.alert，见 src/EditorController.h） */
    Connections {
        target: Cmd
        function onAlertRequested(title, text) { window.notify(title, text) }
    }

    /* 提示卡片：只有一行说明 + 一个「知道了」 */
    function notify(title, text) {
        noticeAsk.ask(title, text, [ { label: "知道了", primary: true } ])
    }

    /*
     * 确认卡片：点了「确定」才跑 onYes。
     *
     * 以前 Cmd.confirm() 是同步返回真假的，卡片换成了异步的原生小窗，
     * 所以"确定之后做什么"得当成回调传进来。
     */
    property var confirmThen: null

    function askConfirm(title, text, onYes) {
        confirmThen = onYes
        confirmAsk.ask(title, text, [ { label: "确定", primary: true },
                                      { label: "取消" } ])
    }

    function answerConfirm(choice) {
        var then = confirmThen
        confirmThen = null
        if (choice === 0 && then)
            then()
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
         * 最大化 / 还原这一下**不再做任何淡入**了。
         *
         * 原来的"压暗 -> 回全亮"是为了盖住那次尺寸跳变；现在跳变本身已经没有了：
         * WindowHelper 只改几何（不碰系统状态，系统那段缩放转场因此不会发生），
         * 并且在改完之后当场把新尺寸这一帧合成上屏（flushContent）—— 屏幕上
         * 只有"变完了"这一帧，没有需要遮的东西。
         *
         * 留着它反而是负担，两条都是量出来的：
         *   * from 0.45 / 150ms 时，屏幕采样看到窗口左上角 (70,68,69) -> (49,49,49)
         *     再爬回来，整窗压暗 30% 持续 130ms —— 这本身就是一眼能看见的"闪"；
         *   * 就算压到 0.86 / 90ms，它仍然要驱动约 87ms 的连续重画
         *     （日志里切换后那 11 条"帧：界面重画"就是它）。
         *
         * Win.transitioned 仍然发（谁要"知道刚切换过"可以接），只是这一层
         * 不再拿它做动画。
         */
        property bool transitioning: Win.transitioned
        opacity: 1.0

        /*
         * 最大化 / 还原那一小段里，把"图标条 / 左树 / 间隙 / 编辑区"四块的宽度
         * 一帧一记，写进窗口变化日志（见 src/WindowHelper.h 的"窗口变化日志"）。
         *
         * 用来回答一个具体问题：**这次换尺寸，QML 布局是算一拍就落定，还是要算两拍**
         * （两拍的话用户就会看到"缝在闪"）。一步到位的话，这段时间里记到的宽度
         * 只应该有两个值：旧的那组和新的那组，中间不该出现第三组。
         * 只在切换后 ~400ms 内记（50 拍 × 8ms），平时一行都不写。
         */
        Timer {
            id: layoutProbe
            /*
             * 默认不开：这个探针自己在事件循环里很吵，开着会把换尺寸那一下
             * 拖慢一个量级（实测 4K 渲染 ~5ms -> ~155ms）。
             * 要看"布局算一拍还是两拍"时：设 SMARTCLIP_LAYOUT_PROBE=1 再跑。
             */
            interval: 1
            repeat: true
            property int ticks: 0
            function dump(tag) {
                Win.traceMark("布局[" + tag + "] nav=" + navStrip.width
                              + " tree=" + folderTree.width
                              + " gap=" + splitterGap.width
                              + " editor=" + editor.width
                              + " 正文项=" + (editor.mainView ? editor.mainView.width + "x" + editor.mainView.height : "-")
                              + " 窗=" + window.width + "x" + window.height)
            }
            onTriggered: {
                if (++ticks > 120) {
                    stop()
                    return
                }
                dump("t" + ticks)
            }
        }

        Connections {
            target: Win
            /*
             * 用 maximized 而不是 transitioned 起头：maximized 是在**改几何之前**
             * 就翻过去的（见 WindowHelper::applyState），所以先记一拍"改之前"，
             * 再 1ms 一拍地跟 120 拍 —— 布局要是有"算两拍"的中间值，这里必然露出来。
             */
            function onMaximizedChanged() {
                if (!Win.probeEnabled)
                    return
                layoutProbe.dump("改之前")
                layoutProbe.ticks = 0
                layoutProbe.restart()
            }
        }

        /*
         * 文档识别的进度 / 结果卡片（主窗口右下角，见 qml/components/DocCard.qml）。
         *
         * 它是一个**独立的原生窗口**（Window），几何由它自己算 —— 这里不用给
         * anchors，也没有 z 可言（不在主窗口的场景树里）。为什么必须这样，见那个
         * 文件头的说明：编辑区是原生 QScintilla 子窗口，场景内的浮层会被它盖住。
         */
        DocCard {
            id: docCard
            objectName: "docCard"
        }

        /*
         * 卡片的位置由 C++ 推过来（见 main.cpp 里那个 cardGeo 定时器和
         * DocImport::publishCardGeometry）。
         *
         * 为什么不让 QML 自己算：QML 那个 ApplicationWindow 的 x/y 和宿主
         * QWidget 的 geometry **不是同一套坐标系** —— 实测宿主摆在 (300,160)
         * 时 QML 读出来 x=0，于是卡片被算到屏幕中间去（用户报的"不在右下角"
         * 就是这个）。只有 C++ 那边知道宿主窗口的真实几何。
         */
        Connections {
            target: Doc
            function onCardGeometryChanged(x, y) { docCard.placeAt(x, y) }
        }

        Rectangle {
            id: contentRoot

            anchors.fill: parent

            // 卡片之外那圈底（编辑区右侧 5px 间隙、左树面板左侧的留白）
            color: Theme.c("#313335", Theme.rev)

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
            fontLock: window.fontLock
            onOpenMenu: (anchor, items) => {
                /*
                 * 记下这次菜单锚在哪个控件上 —— 自检拿它验"菜单挂在被点的那一栏
                 * 正下方，而不是窗口最左边"。
                 *
                 * 这条真出过问题：TopBar::activateTab 里写死用 appBadge（左边那个
                 * 应用图标）当锚点，于是点「设置」菜单从最左边弹出来，整条偏移到
                 * 应用图标底下去了。菜单是独立原生窗口，锚点对不对只有这里知道。
                 */
                window.lastMenuAnchor = anchor
                ddMenu.openFor(anchor, items)
            }

            onSearchChanged: (text) => { window.searchText = text; window.refresh() }
            /*
             * 「帮助」那一栏是"直接执行"的：点一下开"关于 SmartClip"，
             * 不弹下拉菜单（见 TopBar.qml 的 isDirect）。
             */
            onAboutRequested: window.dispatch("about")
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

            /*
             * 必须显式给 0。
             *
             * Quick Layouts 在没写 Layout.minimumHeight 时拿 **implicitHeight** 当最小值，
             * 而这一行的 implicitHeight 是子项撑出来的（编辑区 / 左树那几百像素）——
             * 于是底部面板要 260 也拿不到：中间那一行死活不缩，面板被挤成 0 高
             * （实测：wanted=260 而 height=0）。
             */
            Layout.minimumHeight: 0

            /*
             * 终端面板"最大化"时整行让给它（和 VS Code 面板那个上箭头一个意思）。
             *
             * 用 visible 而不是把高度算成 0：Quick Layouts 会把不可见的项整个跳过，
             * 这样面板就能拿到中间那一行的全部空间，而不用去和编辑区的
             * 最小高度（implicitHeight 会被当成 Layout 的下限）抢。
             */
            visible: !window.terminalMaximized

            /*
             * 图标条的槽位：只占住最左边那 34px，本体在下面的 contentRoot 里。
             *
             * 留着它是为了让左树 / 编辑区的横向几何和以前一模一样
             * （实测左树卡片起于 x=34、终端面板那条 Layout.leftMargin: 34 也是它）。
             */
            Item {
                id: navStripSlot
                Layout.fillHeight: true; Layout.preferredWidth: 34
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
                 *
                 * 前面那两行 `void (...)` 不是废话，是**给绑定建依赖**：
                 * mapToItem() 是个函数调用，QML 不会替它记依赖，绑定里只读
                 * width 的话，"面板被布局挪了位置但宽度没变"这一类变化就唤不醒
                 * 它 —— 值会停在旧坐标上。下面文件末尾的 splitterMouse 就是吃
                 * 这个亏：抓手（9px 宽、z:2000、光标 Qt.SplitHCursor）留在旧 x 上，
                 * 用户看到的就是"编辑区中间一移动鼠标就变 <->，按住还能拖左树"
                 * （实测：树右边缘在 290，热区在 670）。
                 * x / y 和父级的位置属性都读一遍，位置一变绑定就重算。
                 */
                readonly property real panelRight: {
                    void (folderTree.x, folderTree.y, midRow.x, midRow.y)
                    return mapToItem(window.contentItem, width, 0).x
                }

                rows: window.treeRows
                selectedPath: window.selectedPath
                onFolderClicked: (key) => window.toggleFolder(key)
                onFileClicked: (row) => window.openTreeFile(row)

                /*
                 * 行上按右键：文件给"打开 / 重命名 / 删除 / 在文件夹中显示"，
                 * 文件夹给"新建 / 刷新 / 在文件夹中显示"（导入的目录多一条移除）。
                 * 坐标由委托换算成场景坐标（见 TreeDelegate 的 onClicked）。
                 */
                onRowContextMenuRequested: (row, x, y) => window.openTreeRowMenu(row, x, y)

                /* 标题栏那排按钮：动作全在 Main 这边（数据都在这儿） */
                onNewEntryRequested: window.newEntry()
                /* 刷新 = 重扫磁盘（文件可能被别的程序改过 / 删过）再重建左树 */
                onRefreshRequested: { Store.rescan(); window.refresh() }
                onLocateRequested: window.locateCurrentItem()
                onCollapseAllRequested: window.setAllFolders(false)
                onExpandAllRequested: window.setAllFolders(true)
                onHideRequested: window.toggleFolderTree()
                /* 当前标签不是左树里的文件时，准星按钮置灰（没什么可定位的） */
                locateEnabled: window.canLocateCurrent()
                /*
                 * 标题「项目 ∨」弹的是"看哪一份"（项目 / 项目文件 / 打开的文件），
                 * 右边那个 ⋯ 弹的还是那排操作 —— 和 PyCharm 一样分工。
                 */
                onScopeMenuRequested: (anchor) =>
                    ddMenu.openFor(anchor, Menus.treeScopeMenu(window.treeScope))
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
             * 这里只负责占住那条缝。
             *
             * 5 → 3：用户要"间隙再小一点"。抓手（5px）比这条缝宽 2px，
             * 多出去的那 2px 让到编辑区一侧，不往左压滚动条。
             */
            Item {
                id: splitterGap

                /*
                 * 面板收起来时这条缝也一起收掉。
                 *
                 * 留着的话收起之后编辑区左边会多出这一条和图标条同色的暗带：
                 * 展开时这一列到面板卡片左边缘为止（34px），收起后却到 37px ——
                 * 看着就是"折叠和展开左边这一列宽度不一样"。收掉之后
                 * 编辑区卡片正好顶到图标条右边，和展开时面板卡片的起点对齐。
                 */
                Layout.preferredWidth: window.folderTreeHidden ? 0 : 3
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
                 * 图片不再单独预览。
                 *
                 * 上一版图片是左树里的一条"图片条目"，点它走这块图片面板；现在
                 * 图片跟在当天的 md 里（`![](assets/xxx.png)`），左树列的只有
                 * md 文件，点开看到的就是那一段 markdown 引用。面板留着不接数据，
                 * 以后要做"md 里图片的行内预览"再从这里接。
                 */
                previewItem: null

                onTabCloseRequested: (pane, index) => window.closeTab(index, pane)
                onTabCloseAllRequested: (pane) => window.closeAllTabs(pane)
                onNewTabRequested: window.newFile()
                onClipboardRefreshRequested: window.refresh()
                /*
                 * tab 右键菜单：把"哪一栏的哪个标签 + 鼠标在标签里的坐标"
                 * 转给 openTabMenu —— 菜单左上角要落在鼠标那一点上。
                 */
                onTabContextMenuRequested: (pane, index, anchor, x, y) =>
                    window.openTabMenu(pane, index, anchor, x, y)

                /* ---- Markdown 预览（见 qml/components/MarkdownView.qml） ---- */
                markdownPreview: window.markdownPreview
                markdownHtml: window.markdownHtml
                markdownHint: window.markdownHint
                canPreviewMarkdown: window.canPreviewMarkdown
                onMarkdownToggleRequested: window.toggleMarkdownPreview()
                onMarkdownLinkActivated: (link) => Cmd.openExternal(link)
                onMarkdownContextMenuRequested: (x, y) => window.openPreviewContextMenu(x, y)

                /* ---- 分栏（见 setSplit / notePaneFocus） ---- */
                onPaneFocusRequested: (pane) => window.notePaneFocus(pane)

                /* ---- 文件对比（见 qml/components/DiffPane.qml） ---- */
                diffTabs: window.diffTabTitles
                diffSession: window.diffSessionOf(window.diffIndex)
                diffMode: window.diffMode
                diffIndex: window.diffIndex
                onDiffTabRequested: (diffIndex) => window.showDiffSession(diffIndex)
                onDiffTabClosed: (diffIndex) => window.closeDiffSession(diffIndex)
                /* 点回文档标签 = 退出对比页（点的是当前那份时 currentChanged 不会发） */
                onDocTabActivated: window.exitDiffMode()
                onDiffCopyPatchRequested: Cmd.copyText(Differ.unifiedDiff())
            }
        }

        /*
         * 底部终端面板：夹在中间那一行和状态栏之间。
         *
         * 收起时槽位给 0（不是把组件删掉）—— 会话还得活着，不然每次展开都重新
         * 起一个 shell，之前跑的东西全没了。
         */
        TerminalPanel {
            id: terminal

            /* 终端那块右键菜单的宿主：和下面 ddMenu 的 `parent: window` 同一个对象
               （面板自己够不到窗口根 Item，见 TerminalPanel.qml 里 menuHost 的说明） */
            menuHost: window
            Layout.fillWidth: true
            /*
             * 三条边距全部照抄同级那两张卡片，不手算常量：
             *   左 34 = 图标条宽度（实测左树卡片起于 x=34）
             *   右 5  = 编辑区卡片那条 Layout.rightMargin: 5（实测卡片右边缘 1454，
             *           窗口 1460，中间正好 5px 底色）
             *   上 3  = splitterGap 那条缝（实测树与编辑区之间 293..295 是 3px 窗口底色）
             * 收起时槽位给 0（不是把组件删掉）—— 会话还得活着，不然每次展开都重新
             * 起一个 shell，之前跑的东西全没了。
             */
            Layout.leftMargin: 34
            Layout.rightMargin: 5
            Layout.topMargin: 3
            Layout.preferredHeight: window.terminalHidden ? 0
                                    : (window.terminalMaximized
                                       ? Math.max(window.terminalMinHeight,
                                                  contentRoot.height - topBar.height
                                                  - statusBar.height - 3)
                                       : window.terminalHeight)
            opened: !window.terminalHidden
            maximized: window.terminalMaximized

            onDraggedTo: (h) => {
                window.terminalMaximized = false
                window.terminalHeight = Math.max(window.terminalMinHeight,
                                                 Math.min(700, h))
            }
            onDragFinished: window.rememberTerminalHeight()
            onCloseRequested: window.toggleTerminalPanel()
            onMaximizeToggled: window.terminalMaximized = !window.terminalMaximized
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
            count: Store.entryCount
        }
            }

            /*
             * 左侧工具窗口图标条（已取消边框）。
             *
             * 它**不在 midRow 的布局里**：那条行会随终端面板展开而变矮，
             * 面板最大化时更是整个 hide 掉（midRow 的 visible），图标条
             * 跟着没。这里改成从顶栏下沿一直铺到状态栏上沿，中间那一行
             * 只留一个 34px 的槽位（navStripSlot）占住宽度，几何和以前一样。
             */
            Rectangle {
                id: navStrip
                width: 34
                /*
                 * 顶到顶栏下沿、底到状态栏上沿 —— 中间那一行（midRow）展开、
                 * 收起、甚至整个 hide 掉都不影响这一条的高度。
                 *
                 * 这里**不能用 anchors.top: topBar.bottom**：topBar / statusBar
                 * 是 ColumnLayout 的孩子，和这条不是同胞，QML 直接拒掉
                 * （"Cannot anchor to an item that isn't a parent or sibling"），
                 * 结果是 y/height 双双留在默认值 —— 整条图标条塌成 0 高、
                 * 底部那一组跑到窗口外面去（自检量到的 y=-66 就是这么来的）。
                 */
                x: 0
                y: topBar.y + topBar.height
                height: Math.max(0, statusBar.y - topBar.y - topBar.height)
                color: Theme.c("#313335", Theme.rev)
                // 已删除 border.color 和 border.width

                IconProvider { id: stripIcons }
                Column {
                    id: navTopGroup
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.top: parent.top; anchors.topMargin: 8
                    spacing: 6
                    Repeater {
                        /*
                         * 五格，**每一格都接上了动作**：项目树 / 截图 / 便签 /
                         * 翻译 / 识别文档。终端不在这一排里 —— 它贴在**底部**、
                         * 紧挨着设置那一格（见下面 navTerminalCell）。
                         *
                         * 原来后面还挂着 file / search / play / branch 四个格子，
                         * 但都没接动作（`acts` 为假，点了没反应、光标也是普通箭头）
                         * —— 摆着不动就是噪音，用户问"这几个没用的去掉"，去掉了。
                         * 那几样东西在菜单里都有：搜索是顶栏那个框和「搜索」菜单，
                         * 分支 / 运行对"剪贴板 + 笔记"这个程序根本没有对应功能。
                         *
                         * 截图 / 便签 / 翻译 / 识别都是"叫出一个工具"——便签那一格点
                         * 一下就地新建一块，长按（右键）才是排列 / 收起那些（老用户
                         * 不会误点，新用户看一眼提示就懂）；翻译那一格是把翻译卡片
                         * 叫到桌面上；识别那一格是挑一份文档认成笔记。
                         */
                        model: [ { k: "folder", active: true }, { k: "screenshot", active: false },
                                 { k: "note", active: false },
                                 { k: "translate", active: false },
                                 { k: "ocr", active: false } ]
                        delegate: Rectangle {
                            id: navCell
                            required property var modelData
                            width: 26; height: 26; x: 4; radius: 5

                            /* 这一格点下去有没有事发生（决定光标和提示要不要给） */
                            readonly property bool acts: modelData.k === "folder"
                                                         || modelData.k === "screenshot"
                                                         || modelData.k === "note"
                                                         || modelData.k === "translate"
                                                         || modelData.k === "ocr"

                            /*
                             * 这一格算不算"当前打开的工具窗口"。
                             *
                             * 文件夹那格不再看 modelData.active：它现在是项目树
                             * 的开关，面板收起来时这一格就该是未选中的样子
                             * （和 PyCharm 左边那排工具窗口按钮一个道理）——
                             * 面板收起来之后，标题栏那排按钮跟着没了，
                             * 这里就是唯一能把树叫回来的地方。
                             *
                             * 便签那格同理：桌面上摆着便签时它才亮着 ——
                             * 新便签是**在桌面上**出现的（不在主窗口里），
                             * 这一格亮着就是"外面有那么几块"的唯一提示。
                             */
                            readonly property bool selected: {
                                if (modelData.k === "folder")
                                    return !window.folderTreeHidden
                                if (modelData.k === "note")
                                    return Notes.visibleCount > 0
                                if (modelData.k === "translate")
                                    return Trans.visibleCount > 0
                                return modelData.active
                            }

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
                                       : (selected ? Theme.c("#3a4a5a", Theme.rev) : "transparent")

                            AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: modelData.k
                                      tint: navCell.hot ? "#ffffff"
                                                        : (navCell.selected ? window.accentColor : Theme.c("#9aa0a8", Theme.rev))
                                      size: 16 }
                            MouseArea {
                                id: navHit
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: navCell.acts ? Qt.PointingHandCursor
                                                          : Qt.ArrowCursor
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: (mouse) => {
                                    if (modelData.k === "folder")
                                        window.toggleFolderTree()
                                    else if (modelData.k === "screenshot")
                                        Shot.beginCapture()
                                    else if (modelData.k === "note") {
                                        /*
                                         * 左键 = 新建一块；右键 = 排列 / 收起那些。
                                         * 便签这一格迟早要放好几条命令，但格子上
                                         * 挂不下第二个按钮，右键菜单是最省地方的做法。
                                         *
                                         * 菜单本身走共用那份 DropdownMenu（深色 +
                                         * 图标），不是 Qt Quick Controls 的 Menu ——
                                         * 那个白底、没图标，和界面里其它菜单不是一个
                                         * 长相（用户要求统一成"帮助"那份的样子）。
                                         */
                                        if (mouse.button === Qt.RightButton)
                                            window.openNotesCellMenu(navCell, mouse.x, mouse.y)
                                        else
                                            Notes.createNote()
                                    }
                                    else if (modelData.k === "translate")
                                        Trans.showCard()
                                    else if (modelData.k === "ocr")
                                        /* 挑一份文档认成笔记（见「文件 → 识别文档…」那条，同一个入口） */
                                        window.importDocumentDialog()
                                }
                            }

                            /*
                             * 接上动作的那几格给提示。
                             *
                             * 气泡里**只留名字**：不写快捷键，也不加"右键排列"这种
                             * 补充说明 —— 图标条这排是"一眼认工具"的地方，字越少越好。
                             * 快捷键在菜单里和设置面板里都写着；便签的右键菜单
                             * 自己会弹，不需要气泡先教一遍。
                             */
                            AppToolTip {
                                hovered: navHit.containsMouse && navCell.acts
                                text: {
                                    if (modelData.k === "folder")
                                        return window.folderTreeHidden ? "显示项目树" : "收起项目树"
                                    if (modelData.k === "screenshot")
                                        return "截图"
                                    if (modelData.k === "translate")
                                        return "翻译"
                                    if (modelData.k === "ocr")
                                        return "识别文档"
                                        return "便签"
                                }
                                /* 贴着窗口左沿放：默认的"居中在格子上"会往左出界 */
                                x: 2
                                y: -implicitHeight - 3
                            }
                        }
                    }
                }

                /*
                 * 底部那一组：终端 + 设置。
                 *
                 * 原来它是上面那个 Column 里的一根弹簧（高度 = 图标条高 - 300），
                 * 从**顶部**量出去的 —— 终端面板一展开，图标条矮了一截，
                 * 这两格就跟着往上跑（他报的那条）。现在单独一个 Column
                 * 钉在图标条下沿，图标条本身又铺满整列，位置就只跟窗口高度有关。
                 */
                Column {
                    id: navBottomGroup
                    anchors.left: parent.left; anchors.right: parent.right
                    anchors.bottom: parent.bottom; anchors.bottomMargin: 8
                    spacing: 6

                    /*
                     * 终端那一格：贴在底部、紧挨着设置。
                     *
                     * 原来它是上面那一排 Repeater 的最后一格，夹在工具格中间，
                     * 而终端是"常驻要看的东西"，和上面那几个"叫一次工具"的格子
                     * 不是一类（2026-09-23 他圈着这个图标要放到底部）。
                     * 配色/悬停/提示全照上面那一排的口径抄，看起来是同一套格子。
                     */
                    Rectangle {
                        id: navTerminalCell
                        width: 26; height: 26; x: 4; radius: 5
                        readonly property bool hot: navTermHit.containsMouse
                        readonly property bool selected: !window.terminalHidden
                        color: hot ? window.accentColor
                                   : (selected ? Theme.c("#3a4a5a", Theme.rev) : "transparent")

                        AppIcon { anchors.centerIn: parent; provider: stripIcons; kind: "terminal"
                                  tint: navTerminalCell.hot ? "#ffffff"
                                                            : (navTerminalCell.selected
                                                               ? window.accentColor : Theme.c("#9aa0a8", Theme.rev))
                                  size: 16 }
                        MouseArea {
                            id: navTermHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: window.toggleTerminalPanel()
                        }
                        AppToolTip {
                            text: qsTr("终端")
                            hovered: navTerminalCell.hot
                            /* 贴着窗口左沿放：默认的"居中在格子上"会往左出界 */
                            x: 2
                            y: -implicitHeight - 3
                        }
                    }

                    /*
                     * 底部这一格：换主题（2026-09-23 他点的就是这一格）。
                     *
                     * 原来它是"设置"入口（点一下开快捷键面板）。设置那条路不能断，
                     * 所以分工照便签那一格已有的口径来：**左键 = 直接执行**（切深色/
                     * 白色，立刻生效并落盘），**右键 = 那排设置命令**。
                     *
                     * 图标跟着状态换：深色档是"圆心 + 八道射线"那枚（画出来就是太阳，
                     * 表示"点一下变亮"），浅色档换成月牙。见 IconProvider.qml 的
                     * "gear" / "moon"。
                     */
                    Rectangle {
                        id: themeCell
                        width: 26; height: 26; x: 4; radius: 5
                        readonly property bool hot: themeHit.containsMouse
                        color: hot ? window.accentColor : "transparent"
                        AppIcon { anchors.centerIn: parent; provider: stripIcons
                                  kind: Theme.light ? "moon" : "gear"
                                  tint: themeCell.hot ? "#ffffff" : Theme.c("#9aa0a8", Theme.rev)
                                  size: 16 }
                        MouseArea {
                            id: themeHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onClicked: (mouse) => {
                                if (mouse.button === Qt.RightButton)
                                    ddMenu.openAtPoint(themeCell, mouse.x, mouse.y, [
                                        { label: "快捷键 / 设置", act: "settings" },
                                        { label: "存储位置", act: "storage" } ])
                                else
                                    Theme.toggle()
                            }
                        }
                        AppToolTip {
                            text: Theme.light ? "换回深色主题" : "换成白色主题"
                            hovered: themeCell.hot
                            /* 贴着窗口左沿放：默认的"居中在格子上"会往左出界 */
                            x: 2
                            y: -implicitHeight - 3
                        }
                    }
                }
            }
        }
    }

    /*
     * 圆角外侧那一圈描边。
     *
     * 无边框窗口贴到深底桌面上会显得"糊"，压一条比底色亮一点点的细线，边界才立得住。
     *
     * **走 DWM 圆角时这条要退场**：系统已经按我们给的颜色（#4b4d4f，见
     * WindowHelper::attachWidget）沿着它自己那条弧画了一条 1px 边，再叠一条
     * 就是两条线、而且半径对不齐。回退到遮罩那条路（Win10）时它还得在。
     */
    Rectangle {
        anchors.fill: parent
        z: 10
        visible: !Win.dwmRound
        color: "transparent"
        radius: window.cornerRadius
        border.width: 1
        border.color: Theme.c("#4b4d4f", Theme.rev)
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
     * 最大化时照旧能拖（2026-09-23 他要的）：这一条原来是 `enabled: !window.maximized`，
     * 全屏以后那条缝就废了。抓手的上下限由 folderTreeMinWidth/MaxWidth（240/600）夹着，
     * 和窗口多宽没关系，全屏下没有理由例外。
     */
    MouseArea {
        id: splitterMouse

        /*
         * 位置：从树面板的右边缘**往右**吃 5px（= 那条缝 + 编辑区卡片左边 2px）。
         *
         * 原来写的是 `panelRight - 4` 起、宽 9 —— 往左多出来的那 4px 正好压在左树
         * 的滚动条上（滚动条是贴着面板右边缘画的，见 FolderTree 的
         * `ScrollBar.vertical: ThinScrollBar { anchors.right: parent.right }`），
         * 结果就是"鼠标一移到滚动条上就变 <->，想拖滚动条反而在改面板宽度"。
         * 现在左边缘不许越过 panelRight，抓手整个落在缝里。
         *
         * 宽度 9 → 5：间隙本身也从 5px 收到 3px（见上面 splitterGap），抓手比缝
         * 宽 2px，多出来的那点让到编辑区那一侧 —— 那边没有可点的控件贴边，
         * 而滚动条有。面板收起时间隙是 0，这 5px 就是"从缝里往右拖把面板叫回来"
         * 的把手（见下面 onPressed）。
         */
        x: Math.max(0, folderTree.panelRight)
        width: 5

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
        /*
         * 用 SizeHorCursor，不用 SplitHCursor —— 和下面终端面板顶边那条拖高度的
         * 把手同一个家族（那边是 Qt.SizeVerCursor，见 qml/components/TerminalPanel.qml
         * 的 resizeStrip）：都是"一根轴上的双箭头"。SplitHCursor 那个带一条竖线的
         * 双箭头是另一套字形，两个挨在一起看就是一粗一细不配套（他报的第 2 条）。
         */
        cursorShape: Qt.SizeHorCursor
        acceptedButtons: Qt.LeftButton

        property real pressSceneX: 0
        property real pressWidth: 0

        onPressed: (mouse) => {
            pressSceneX = mapToItem(null, mouse.x, 0).x
            /*
             * 按住这一整段把光标钉成左右双箭头。
             *
             * cursorShape 只在鼠标停在这 5px 上时生效，拖快一点指针就跑到左树 /
             * 编辑区那边（编辑区还是另一个原生子窗口，它自己会设光标），于是
             * 按住不放的过程中光标闪回默认箭头（用户报的那条）。
             */
            Win.pushResizeCursor(Qt.SizeHorCursor)
            /*
             * 面板收起来时先把它叫回来：收起来之后标题栏那排按钮也跟着没了，
             * 这条缝（抓手这 5px）就是最自然的把手（往右拖 = 把树拉出来）。
             */
            if (window.folderTreeHidden)
                window.toggleFolderTree()
            pressWidth = window.folderTreeWidth
            mouse.accepted = true
        }

        /* 抓取被抢走（比如中途弹出别的东西）：也得还原，不然光标一直钉着 */
        onCanceled: Win.popResizeCursor()

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
            Win.popResizeCursor()
            /* 拖完才记一次宽度：拖动过程中每动一像素写一次设置太浪费 */
            window.rememberTreeWidth()
        }
    }
}
