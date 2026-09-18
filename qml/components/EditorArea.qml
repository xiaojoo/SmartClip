pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../js/TimeUtils.js" as Time
import "../utils"
import SmartClip.Editor 1.0

/*
 * 编辑区：标签栏 + 查找栏 + 正文。
 *
 * 正文是原生 QScintilla（见 src/EditorViewItem.h），所以这里只负责外壳：
 *   * 标签栏 —— 多文档切换 / 关闭 / 新建；
 *   * 查找栏 —— 一条独立的栏（不能做成浮层，见 FindBar.qml 开头）；
 *   * 空状态（欢迎页）和图片预览。
 *
 * 分栏（第二版）：**两个对等的编辑组**，各自一条标签栏、各自看不同的文件，
 * 但共用一份文档池 —— 同一份文件在两栏里就是同一个底层文档，在任意一栏改，
 * 另一栏立刻就变（和 VS Code 的 split editor 一个意思）。
 * 文档池在 C++ 那边（见 EditorViewItem.h 里那段说明），这里只管布局：
 *   * 左右分栏 = 两栏并排，两条标签栏也在顶上并排；
 *   * 上下分栏 = 两栏摞起来，标签栏跟着各自那一栏走。
 * 分隔条在布局**外面**（splitterHandle），纵向范围从标签栏下沿一直到
 * 卡片底边 —— 拖的时候整块一起动，不是只在正文里拖。
 *
 * 注意 EditorView 必须被"真正隐藏"（visible: false）而不是只被盖住：
 * 它是独立的原生子窗口，只要 show 着就会盖在 QML 任何内容之上。
 * Main.qml / 工具栏拿到的编辑器句柄是 root.view。
 */
Rectangle {
    id: root

    color: "#1e1f22"
    radius: 10
    clip: true
    border.width: 0

    /* 图片条目预览（没有文件、也不是文本的东西走这里） */
    property var previewItem: null

    /*
     * 编辑器本体。
     *
     * Main.qml 通过 root.view 调命令、绑状态；工具栏绑的也是它。
     */
    readonly property alias view: editorView
    readonly property alias findBar: find

    /*
     * 标签栏请求（关闭要先问"要不要保存"，所以交给 Main.qml）。
     *
     * 分栏之后有两条标签栏，所以两个信号的第一个参数是**哪一栏**
     * （pane = editorView / mirrorPane）—— 关闭 / 右键菜单都得作用在
     * 点中的那一栏上，不能想当然用"当前栏"。
     */
    signal tabCloseRequested(var pane, int index)
    signal tabCloseAllRequested(var pane)
    signal newTabRequested()
    signal clipboardRefreshRequested()

    /*
     * tab 上的右键菜单（由 Main.qml 的 openTabMenu 弹出）。
     *
     * menuAnchor 传的是**那个标签本身**（不是整个 tab 栏），(x, y) 是右键那一点
     * 在标签里的本地坐标：菜单要按它们换算成窗口坐标，让左上角紧贴鼠标那一点
     * （见 DropdownMenu.openAtPoint 与 openFor 的坐标口径说明）。
     */
    signal tabContextMenuRequested(var pane, int index, var menuAnchor, real x, real y)

    readonly property color barBg: "#1e1f22"
    readonly property color editorBg: "#1e1f22"
    readonly property color borderColor: "#4b4d4f"
    readonly property color tabBg: "#45484c"
    readonly property color tabActiveBg: "#2b2d30"
    readonly property color textBright: "#e8e8e8"
    readonly property color textMain: "#bbbbbb"
    readonly property color textMuted: "#7d7d7d"
    readonly property color hintKey: "#8b929e"
    readonly property color accentColor: "#4c96d8"
    readonly property color imageColor: "#d7a85b"
    readonly property color lineNumberColor: "#606366"
    readonly property color dangerColor: "#e06c75"
    /*
     * 编辑区里那些竖线（行号右边那条分隔线 / 字数参考线 / 缩进参考线）的颜色，
     * 和 src/EditorViewItem.cpp 里的 kGuideLine 是同一个值。
     *
     * 分栏那条拖动的分隔线也用它 —— 用户要的是"和序号线一样的一条"。
     */
    readonly property color gutterLineColor: "#333840"

    /*
     * 正文默认字号（12）。
     *
     * 写成可写属性而不是常量：启动时会按上次保存的设置覆盖它（Main.qml），
     * "设置"菜单里改字号也改这里 —— EditorView 的 fontPixelSize 一直绑着它，
     * 免得直接给 fontPixelSize 赋值把绑定打断。
     */
    property int editorFontSize: 12
    readonly property bool hasDocument: root.view.hasDocument
    readonly property bool hasTabs: root.hasDocument || root.previewItem !== null

    /* ---- Markdown 预览（见 qml/components/MarkdownView.qml） ---- */

    /* 正文显示的是预览还是源码（由 Main.qml 决定，这里只是接过来摆哪一页） */
    property bool markdownPreview: false
    /* 已经渲染好的 HTML（Main.qml 从 Cmd.markdownHtml 取来） */
    property string markdownHtml: ""
    /* 渲染结果为空时的提示语 */
    property string markdownHint: "没有可预览的内容"
    /* 当前这份文档能不能预览（不是 .md 就是假，标签栏那个开关据此显不显示） */
    property bool canPreviewMarkdown: false

    /* Markdown 预览里点了一个链接（交给 Main.qml -> Cmd.openExternal） */
    signal markdownLinkActivated(string link)
    /* 标签栏最右边那个"源码 / 预览"开关被点了 */
    signal markdownToggleRequested()
    /*
     * 在预览里按了右键。
     *
     * 坐标是场景坐标（和编辑区 contextMenuRequested 同一个口径）—— Main.qml
     * 收到就弹**程序自己那套**下拉菜单（见 openPreviewContextMenu）。
     */
    signal markdownContextMenuRequested(real x, real y)

    /* 预览里选中的那段文字（正文那个只读 TextEdit 自己有 selectedText，见 MarkdownView） */
    readonly property string previewSelectedText: markdownView.selectedText
    readonly property bool previewHasSelection: markdownView.selectedText !== ""
    function previewSelectAll() { markdownView.selectAll() }

    /* ---- 分栏（两个对等的编辑组，见文件头那段） ---- */

    /*
     *   ""      不分区（只有一个编辑组）
     *   "right" 左右两栏
     *   "down"  上下两栏
     *
     * 两栏是**两个 EditorViewItem 实例**，共用一份文档池（正文 / 路径 /
     * 修改标记只有一份，见 src/EditorViewItem.h）；各自打开哪几个标签、
     * 当前看的是哪一份、光标和滚动位置则是各管各的 —— 这就是"像 VS Code
     * 那样，两栏是独立的 tab，但信息共用"。
     */
    property string splitMode: ""
    /* 第二栏占多少比例（拖动分隔条改，夹在 0.2 ~ 0.8） */
    property real splitRatio: 0.5
    readonly property bool splitting: root.splitMode !== "" && root.view.hasDocument
    /*
     * 两栏之间那条分隔带的抓取宽度（也是两栏正文各自让开的宽度）。
     *
     * 这个值不能按"看着合适"给：两栏正文都是**原生子窗口**（QScintilla，
     * 见 src/EditorViewItem.h），QML 那层热区在原生窗口盖住的范围内一个鼠标
     * 事件都收不到 —— 抓手只有落在两栏正文都让开的那条缝里才算数。
     * 6px 那版实测很难抓住（缝被两个编辑器各 2px 的内缩吃掉一半，只剩 4px），
     * 现在缝就是这条带的宽度（下面 splitPaneInset 按它算），整条带子都能抓。
     */
    readonly property real splitHandleSize: 10
    /*
     * 分栏时两栏正文各自朝分隔线让开多少：半条带 + 1px 取整余量。
     * 让够了缝，那条 1px 的分隔线才落在一整条能收到鼠标的缝正中。
     */
    readonly property real splitPaneInset: root.splitHandleSize / 2 + 1
    /* 第二栏占多少像素；不分栏 / 没有文档时是 0 */
    readonly property real splitSize: {
        if (!splitting || !contentArea)
            return 0
        return root.splitMode === "down" ? contentArea.height * (1 - splitRatio)
                                         : contentArea.width * (1 - splitRatio)
    }

    /* 两个编辑组（Main.qml 拿它决定命令发给谁） */
    readonly property var mainView: editorView
    readonly property var mirrorView: mirrorPane
    /* 某一栏被点了（Main.qml 接住，把"当前编辑器"切过去） */
    signal paneFocusRequested(var pane)

    /*
     * 分栏开始时把主栏当前那一份拉进第二栏。
     *
     * 第二栏开局是空的（池子里的东西不会自动进新栏的标签栏），不叫这一下的
     * 话分出来的是一条空标签栏，看着像分栏没生效。
     */
    function openMirrorWithCurrent() {
        if (!mirrorPane || !mainView)
            return
        var id = mainView.currentDocId()
        if (id < 0)
            return
        /*
         * 打开并**切过去**：只把标签加进第二栏而不激活的话，刚分出来的
         * 那一栏正文区是空的（用户会以为分栏没生效）。
         */
        mirrorPane.openPoolDocument(id)
    }

    /* 取消分栏：第二栏把它打开的标签放回池子（文档本身不删） */
    function unbindMirror() {
        if (mirrorPane)
            mirrorPane.unbind()
    }

    /*
     * 分栏布局状态（自检读它）。
     *
     *   * 两条标签栏各摆在哪 —— 左右分栏时它们必须**贴在一起**（中间只有那条
     *     1px 的线，没有缝）；上下分栏时下面那条必须**紧贴在下面那一栏正文上面**
     *     （用户报过"这个 tab 没有显示在下面一栏紧贴"）。
     *   * 两栏正文（原生编辑器）的位置 / 尺寸。
     *
     * 几何全是 QML 算的，只有这里知道最终值。
     */
    function splitLayoutState() {
        function rect(it) {
            if (!it)
                return ({ x: -1, y: -1, w: 0, h: 0 })
            var p = it.mapToItem(null, 0, 0)
            return ({ x: Math.round(p.x), y: Math.round(p.y),
                      w: Math.round(it.width), h: Math.round(it.height) })
        }
        /* 有效可见性：自己 visible 为真、并且没有哪一层祖先被收起来 */
        function shown(it) {
            for (var p = it; p; p = p.parent)
                if (p.visible === false)
                    return false
            return true
        }
        var mirrorStrip = (root.splitMode === "down") ? mirrorTabStripDown : mirrorTabStrip
        return {
            mode: root.splitMode,
            sideBySide: tabBars.sideBySide,
            mainTab: rect(mainTabStrip),
            mirrorTab: rect(mirrorStrip),
            mainView: rect(editorView),
            mirrorView: rect(mirrorPane),
            content: rect(contentArea),
            /*
             * 每条标签栏上"源码 / 预览"开关在不在（这条栏带 actions 且当前能预览），
             * 以及它**有效可见**没有（祖先被收起来就算没了）。
             *
             * 自检钉的是：开着预览时（正文那两块整块收起）总得有一条**看得见的**
             * 标签栏带着这个开关 —— 不然切不回源码（用户报的："md 文档预览，
             * 不能切换了，按钮不见了"）。
             */
            mainTabActions: mainTabStrip.paneActions && mainTabStrip.markdownToggleVisible,
            mirrorTabActions: mirrorStrip.paneActions && mirrorStrip.markdownToggleVisible,
            mainTabShown: shown(mainTabStrip),
            mirrorTabShown: shown(mirrorStrip)
        }
    }

    IconProvider { id: icons }

    /*
     * tab 栏和顶上那条横向滚动条的几何（自检读它，见 src/SelfTest.cpp）。
     *
     * 要钉的是"横条有没有压到容器圆角上"：
     *   scrollLeft 和 width - scrollRight 都不能小于容器圆角半径，
     *   scrollBottom 要在标签上沿（stripTop）之上 —— 横条不能盖住标签。
     *
     * 分栏之后有两条标签栏，这里报的是**第一栏**那条（不分栏时就是唯一那条，
     * 分栏时两条的几何是同一套算法算出来的）。
     */
    function tabBarState() {
        return mainTabStrip.barState()
    }

    /*
     * 正文卡片（contentArea）和里面那个原生编辑器的几何（自检读它）。
     *
     * 要钉的是这两条约束，它们是一对：
     *   * 编辑器底边离卡片底边只有一点点（cardBottomInset）—— 横向滚动条
     *     跟着编辑器走，留一个圆角的空档就会在横条下面空出一条；
     *   * 左右各让开一点点（见 editorView 的 cardLeftInset / cardRightInset）——
     *     编辑器是原生子控件、矩形角是直角，让开这一点，卡片左下 / 右下那两段
     *     圆弧就整个落在它矩形之外，两角的圆角保得住（实测 2px 就够，
     *     见下面 editorView 那段注释里的逐像素比对）。
     */
    function editorCardState() {
        return {
            radius: contentArea.radius,
            visible: editorView.visible,
            cardWidth: contentArea.width,
            cardHeight: contentArea.height,
            viewX: editorView.x,
            viewY: editorView.y,
            viewWidth: editorView.width,
            viewHeight: editorView.height,
            viewBottom: editorView.y + editorView.height,
            viewRight: editorView.x + editorView.width,
            bottomGap: contentArea.height - (editorView.y + editorView.height),
            leftGap: editorView.x,
            rightGap: contentArea.width - (editorView.x + editorView.width)
        }
    }

    function imageSource() {
        if (!root.previewItem || root.previewItem.type !== "image")
            return ""
        var raw = root.previewItem.content
        if (raw === undefined || raw === null)
            return ""
        var path = String(raw).replace(/\\/g, "/")
        if (path === "" || path.indexOf("\n") !== -1 || path.length > 512)
            return ""
        if (path.indexOf("file:") === 0)
            return path
        return "file:///" + path
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        /*
         * ================= 标签栏 =================
         *
         * 左右分栏：**两条并排**，各自绑自己那一栏，宽度按 splitRatio 走下面正文
         * 那一套数 —— 中间**不留缝**：交界处只有一条 1px 的线，两条标签栏在分界
         * 那个 x 上直接接上（用户要的"两栏 tab 之间不要有间隙"）。
         *
         * 上下分栏：这一行**只有上面那一栏**的标签栏；下面那一栏的标签栏在它自己
         * 那一栏正文的正上方（见 mirrorPaneHolder 里的 mirrorTabStripDown）——
         * 标签属于栏，两条都摆在顶上就会出现"下面那一栏的 tab 离它那一栏八丈远"
         * （用户报的："这个 tab 没有显示在下面一栏紧贴"）。
         */
        Item {
            id: tabBars

            /* 一条标签栏有多高（上下分栏那条也用它） */
            readonly property real stripHeight: 35
            /* 两条并排只发生在左右分栏 */
            readonly property bool sideBySide: root.splitting && root.splitMode === "right"

            Layout.fillWidth: true
            /*
             * 高度钉死在 35，**不许布局改它**。
             *
             * `Layout.fillHeight: false` 这一条不能少：Item 没有 implicitHeight，
             * fillHeight 为真时 ColumnLayout 会把"剩下的高度"全塞给它 ——
             * 实测那样标签栏会涨到 818px、把正文压成一条缝；而只写
             * preferredHeight 又会被布局按比例压缩（实测只拿到 29px，
             * 标签整个塌掉）。两条一起给才是"固定高度"。
             */
            Layout.fillHeight: false
            Layout.preferredHeight: stripH
            /*
             * 自己也写一份 height。
             *
             * `Layout.preferredHeight` 只是交给布局的"请求"，而里面标签栏的高度
             * 是**按这层壳的高度**算的：不给它一个确定值，布局和子项之间就会互相等
             * （实测标签栏读到 height=0，标签整条消失）。两个都写，值来自同一个
             * 常量公式，不绕回自己。
             */
            height: stripH
            visible: root.hasTabs
            clip: true

            /* 有标签就是一条的高度：并排 / 上下分栏都不往这一行里摞第二条 */
            readonly property real stripH: root.hasTabs ? stripHeight : 0

            Item {
                id: tabBarRow

                /*
                 * 两栏标签栏的宽度和下面两块正文**用同一套数**：都按 splitRatio
                 * 直接切整条宽度，中间一个像素也不扣。
                 *
                 * 原来这里先扣掉那条 6px 分隔带再按比例分，于是右栏那条标签栏
                 * 整体比正文的分界往右挪了 6(1-splitRatio) px —— 用户报的
                 * "两栏 tab 之间有一条缝"就是它（再加上两条标签栏朝里的那两个
                 * 圆角，交界处看着更像个缺口）。
                 */
                readonly property real activeTabWidth: tabBars.sideBySide
                                                       ? width * root.splitRatio
                                                       : width
                /*
                 * 两条标签栏交界处那条 1px 的线（和正文里那条分隔线同色）：
                 * 两条标签栏现在是贴在一起的，没有这条线就看不出分到哪儿。
                 * 它压在左栏那条标签栏的最后一列上，右栏标签栏从分界线开始
                 * —— 这样标签栏的分界和正文的分界是同一个 x。
                 */
                readonly property real dividerX: Math.max(0, activeTabWidth - 1)

                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom

                /* 第一栏的标签（左右分栏时在左半边，上下分栏时在最上面） */
                TabStrip {
                    id: mainTabStrip

                    pane: editorView
                    iconProvider: icons
                    /*
                     * "源码 / 预览"开关在这一条上（分栏时只出现一次）。
                     *
                     * 它必须放在**预览时也看得见**的那一条上：预览一开，
                     * 正文那两块（paneHolder）整块收起来，上下分栏时下面那条
                     * 标签栏是挂在 paneHolder 里的 —— 开关放在那儿就会跟着消失，
                     * 用户就"切不回源码"了（用户报的："md 文档预览，不能切换了，
                     * 按钮不见了"）。所以上下分栏时放在**上面这条**（它在
                     * paneHolder 外面，预览时照样在），左右分栏时放在右栏那条。
                     */
                    paneActions: !root.splitting || root.splitMode === "down"
                    markdownToggleVisible: root.canPreviewMarkdown
                    markdownPreview: root.markdownPreview
                    /*
                     * 分栏时朝里的那个角不能是圆角：两条标签栏是贴着的，
                     * 各自留一个圆角就会在交界处挖出一个缺口（"有缝"的观感）。
                     * 上下分栏时它是整条顶边，两个角都圆。
                     */
                    roundTopRight: !tabBars.sideBySide

                    anchors.left: parent.left
                    anchors.top: parent.top
                    width: tabBarRow.activeTabWidth
                    height: tabBars.stripH

                    onTabCloseRequested: (pane, index) =>
                        root.tabCloseRequested(pane, index)
                    onTabContextMenuRequested: (pane, index, anchor, x, y) =>
                        root.tabContextMenuRequested(pane, index, anchor, x, y)
                    onMarkdownToggleRequested: root.markdownToggleRequested()
                }

                /* 第二栏的标签：**只有左右分栏**在这一行（上下分栏见 mirrorTabStripDown） */
                TabStrip {
                    id: mirrorTabStrip

                    visible: tabBars.sideBySide
                    pane: mirrorPane
                    iconProvider: icons
                    /* 左右分栏时开关在这条（它在 paneHolder 外面，预览时也在） */
                    paneActions: root.splitMode === "right"
                    markdownToggleVisible: root.canPreviewMarkdown
                    markdownPreview: root.markdownPreview
                    /* 朝里的那个角抹平（它贴着左栏那条） */
                    roundTopLeft: false
                    roundTopRight: true

                    /*
                     * 贴着左栏那条标签栏，中间不留缝。左右两条边都由锚定决定
                     * （左 = 左栏标签栏的右边缘，右 = 容器右边），所以这里
                     * **不给 width**：三个一起给是冲突的。
                     */
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.left: mainTabStrip.right
                    anchors.leftMargin: 0
                    height: tabBars.stripH

                    onTabCloseRequested: (pane, index) =>
                        root.tabCloseRequested(pane, index)
                    onTabContextMenuRequested: (pane, index, anchor, x, y) =>
                        root.tabContextMenuRequested(pane, index, anchor, x, y)
                    onMarkdownToggleRequested: root.markdownToggleRequested()
                }

                /* 两条标签栏中间那条 1px 的线（压在左栏那条的最后一列上） */
                Rectangle {
                    visible: tabBars.sideBySide
                    color: root.gutterLineColor
                    x: tabBarRow.dividerX
                    y: 0
                    width: 1
                    height: tabBars.stripH
                    z: 2
                }
            }
        }

        /* ================= 查找 / 替换栏 ================= */
        FindBar {
            id: find
            Layout.fillWidth: true
            view: root.view
        }

        /* ================= 正文 ================= */
        Rectangle {
            id: contentArea

            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.editorBg
            clip: true
            radius: 10

            /* ---- 空状态：没有任何标签时显示 ---- */
            Column {
                id: welcomePanel

                visible: !root.hasTabs
                width: 460
                anchors.centerIn: parent
                spacing: 16

                Text {
                    width: parent.width
                    text: "SmartClip 编辑器"
                    color: root.textBright
                    font.pixelSize: 20
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Repeater {
                    model: [
                        { title: "新建文件", shortcut: "Ctrl+N" },
                        { title: "打开文件", shortcut: "Ctrl+O" },
                        { title: "保存", shortcut: "Ctrl+S" },
                        { title: "查找 / 替换", shortcut: "Ctrl+F / Ctrl+H" },
                        { title: "转到行", shortcut: "Ctrl+G" },
                        { title: "撤销 / 重做", shortcut: "Ctrl+Z / Ctrl+Y" },
                        { title: "刷新剪贴板", shortcut: "F5" }
                    ]

                    delegate: Row {
                        required property var modelData

                        width: parent.width
                        height: 26
                        spacing: 12

                        Text {
                            width: 200
                            text: modelData.title
                            color: root.textMain
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                        }

                        Text {
                            text: modelData.shortcut
                            color: root.hintKey
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }

            /* ---- 图片预览 ---- */
            Rectangle {
                id: imagePanel

                visible: root.previewItem !== null
                         && root.previewItem !== undefined
                         && root.previewItem.type === "image"

                anchors.fill: parent
                anchors.margins: 20
                color: root.editorBg
                radius: 8

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36

                        AppIcon {
                            provider: icons; kind: "image"; tint: root.imageColor
                            size: 20
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.previewItem ? root.previewItem.title : ""
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }

                        Text {
                            /*
                             * 用 displayTime，不是 formatTime ——
                             * js/TimeUtils.js 里没有 formatTime 这个函数
                             * （旧代码一直写错，选图片条目时会刷
                             *  "Property 'formatTime' ... is not a function"）。
                             */
                            text: root.previewItem && root.previewItem.createdAt
                                  ? Time.displayTime(root.previewItem.createdAt) : ""
                            color: root.textMuted
                            font.pixelSize: 11
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: root.borderColor
                        opacity: 0.6
                    }

                    Item {
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        Image {
                            id: previewImage
                            anchors.fill: parent
                            source: root.imageSource()
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            smooth: true
                            visible: source !== ""
                        }

                        Column {
                            anchors.centerIn: parent
                            spacing: 10
                            visible: previewImage.source === ""

                            AppIcon {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 32
                                height: 32
                                provider: icons; kind: "image"; tint: root.imageColor
                                size: 32
                            }

                            Text {
                                text: "无法预览图片"
                                color: root.textMuted
                                font.pixelSize: 13
                            }
                        }
                    }
                }
            }

            /* ---- 两个编辑组（每一组是一个原生编辑器 + 自己那份标签） ---- */
            Item {
                id: paneHolder

                anchors.fill: parent
                visible: !root.markdownPreview

                /*
                 * 主栏**只是一个占位壳**：它的几何就是"这块正文区"（不分栏时是
                 * 整块，分栏时是左 / 上半边），编辑器本体在里面 anchors.fill。
                 *
                 * 为什么要这一层：不分栏时这一层和 contentArea 一样大、位置也
                 * 一样，所以编辑器的 2px 内缩和卡片圆角**和加这个功能之前完全
                 * 一致**（自检里那几条几何断言量到的就是这个）。
                 */
                Item {
                    id: mainPane

                    /*
                     * 主栏占"上 / 左"那一块：位置只钉住左上角，大小由 width / height
                     * 给全（左右分栏 = 左半边满高，上下分栏 = 上半边满宽）。
                     *
                     * 同样不用 anchors（原因见下面 mirrorPaneHolder 那段：贴下边的
                     * 锚点会把 height 的绑定打掉，上下分栏就展不开）。
                     */
                    x: 0
                    y: 0
                    width: Math.max(120, parent.width
                                         - (root.splitMode === "down" ? 0 : root.splitSize))
                    height: Math.max(60, parent.height
                                         - (root.splitMode === "down" ? root.splitSize : 0))

                    /* ---- 正文编辑器（原生 QScintilla） ---- */
                    EditorView {
                        id: editorView

                        /*
                         * 这一份是**主编辑器**："当前编辑器是谁"（EditorViewItem::instance）
                         * 开局指它。分栏之后工程里有两个 EditorViewItem，靠"谁最后构造"
                         * 来定并不可靠（QML 的构造顺序和声明顺序不一致 —— 实测镜像
                         * 那个反而在后），所以由这里显式指定；用户点了哪一栏，
                         * 它会跟着走（见下面的 onPaneFocused）。
                         */
                        mainEditor: true

                        anchors.fill: parent

                        /*
                         * 和卡片边缘留出内边距。
                         *
                         * 编辑器是原生子控件，它自己的矩形角是直角：贴着卡片的角就会把
                         * contentArea（radius: 10）画出来的圆角盖成直角。四边留一点就行，
                         * 留多少只看**那边有没有滚动条** —— 滚动条是贴在编辑器边缘上的：
                         *
                         *   右边 / 底边各 2px：竖向、横向滚动条就在右边缘 / 下边缘上，
                         *  让开一个圆角（10px）等于让滚动条离卡片边一个圆角，白空一条。
                         *  2px 既保证方角压不到卡片的圆角上（两边底色本来就是同一个，
                         *  见下面 paperColor: root.editorBg），又给坐标取整留了余量
                         *  （EditorViewItem::applyGeometry 里是 qRound）。
                         *  实测（截图逐像素比对）：2px 和原来 10px 画出来的圆角一模一样。
                         *
                         *   左边也收到 2px：编辑器最左边那一条就是行号栏，左边留多少，
                         *  行号就离卡片左边缘多远（用户要的是"序号贴紧左边"）。
                         *  正文不贴卡片左边缘这件事改由行号栏 + 折叠栏的宽度顶着，
                         *  不再靠这里的左边距（见下面 paddingLeft 的说明）。
                         */
                        readonly property int cardLeftInset: 2
                        readonly property int cardRightInset: 2
                        readonly property int cardBottomInset: 2

                        anchors.leftMargin: cardLeftInset
                        /*
                         * 朝分隔线那一边（左右分栏是右边，上下分栏是下边）在分栏时
                         * 让开一整个 splitPaneInset：那条 10px 的抓手只有落在两栏
                         * 正文都让开的那条缝里才收得到鼠标（原生子窗口会吃掉自己
                         * 范围内的鼠标事件）—— 让够了缝，分隔线才真的拖得动。
                         */
                        anchors.rightMargin: (root.splitting && root.splitMode !== "down")
                                             ? root.splitPaneInset : cardRightInset
                        anchors.bottomMargin: (root.splitting && root.splitMode === "down")
                                              ? root.splitPaneInset : cardBottomInset

                        /*
                         * 没有标签时**必须真的隐藏**：原生子窗口不受 QML 的
                         * 层叠影响，只要 show 着就会盖在欢迎页上面。
                         */
                        visible: root.view.hasDocument

                    /*
                     * 正文区的左右留白（Scintilla 的 SCI_SETMARGINLEFT / RIGHT）。
                     *
                     * 左边 2：它**不落在行号栏左边** —— Scintilla 画边距是从编辑器
                     * 左边缘起算的（Editor::PaintMargin 里 rcMargin.left = 0），
                     * 这个值实际落在**分隔竖线和正文之间**。原来给 12，加上折叠栏
                     * 那 14px，线和正文之间就空出 26px（用户圈着那段空白提过）；
                     * 折叠栏已经挪到线的左边（见 EditorViewItem::applyMargins），
                     * 这里收到 2 —— 和 cardLeftInset 同一个数：既让分隔线紧贴正文，
                     * 又不让字压在线上。行号贴不贴左边只由 cardLeftInset 和边距宽
                     * 决定，跟它无关。
                     *
                     * 右边 0：**当前行那层底色只铺到"文本区"的右边** —— Scintilla 把
                     * 它当正文段的底色画（EditView::DrawBackground），而文本区 =
                     * 编辑器宽 - marginRight。右边留 12px 的话，那层底色就在离卡片右
                     * 边缘 14px 的地方断掉，看着就是"当前行有背景、背景右边缺一块"。
                     * 收到 0 之后底色一直铺到编辑器右边缘（离卡片右边缘只剩
                     * cardRightInset 那 2px）；正文的右边距改由竖滚动条顶着 ——
                     * 竖条出现时它自己占掉那一条。
                     */
                        paddingLeft: 2
                        paddingRight: 0

                        fontPixelSize: root.editorFontSize
                        textColor: "#d6d7da"
                        paperColor: root.editorBg
                        gutterColor: root.editorBg
                        lineNumberColor: root.lineNumberColor

                        /* 切换标签时把"全部高亮"重新刷一遍 */
                        onDocumentsChanged: if (find.opened) find.refreshHighlight()

                        /* 点进来 = 之后的命令（工具栏 / 菜单 / 快捷键）发给这一栏 */
                        onPaneFocused: root.paneFocusRequested(editorView)
                    }
                }

                /*
                 * 第二栏：**对等**的一栏（不是只读镜像）—— 自己一条标签栏、
                 * 自己看哪一份、自己滚到哪；正文和主栏共用池子里那一份文档，
                 * 所以"改一边另一边立刻变"是天然的（见 src/EditorViewItem.h）。
                 */
                Item {
                    id: mirrorPaneHolder

                    /*
                     * 第二栏的位置：左右分栏贴右上角（右边一条、满高），
                     * 上下分栏贴**左下角**（下面一条、满宽，"向下拆分"就是拆到下面）。
                     *
                     * **不用 anchors**：x / y / width / height 四个都是普通绑定。
                     * 原来这四边用的是 anchors + width/height，其中"贴下边"那个锚点
                     * 会把 height 的绑定打掉（锚点机制写 y 的时候把 height 一起写了），
                     * height 就停在**建那一刻**那个值上、之后再没人更新它 ——
                     * 这一块的 y 又是"底边减 height"算出来的，于是它铺满整块正文区、
                     * 还压到主栏上面：上下分栏看着就是"没展开"（用户报的）。
                     * 左右分栏看不出来只是因为那一栏贴顶，多出来的一截被卡片裁掉了。
                     */
                    readonly property bool stacked: root.splitMode === "down"

                    x: stacked ? 0 : Math.max(0, parent.width - root.splitSize)
                    y: stacked ? Math.max(0, parent.height - root.splitSize) : 0
                    width: stacked ? parent.width : Math.max(0, root.splitSize)
                    height: stacked ? Math.max(0, root.splitSize) : parent.height
                    visible: root.splitting

                    /*
                     * 上下分栏时**这一栏自己的标签栏**，就压在它自己正文的正上方
                     * （用户报的："这个 tab 没有显示在下面一栏紧贴"）。
                     *
                     * 左右分栏时不用它 —— 那时候两条标签栏都要并排在顶上
                     * （见 tabBars 里那条 mirrorTabStrip）。同一个 pane 上挂两条
                     * TabStrip 不冲突：同一时刻只有一条 visible。
                     *
                     * 顶边让开 splitPaneInset：中间那条 10px 的抓手是以
                     * 分隔线为中心、上下各 5px，标签栏不缩这一下就会被抓手压住
                     * 顶上那几像素（点不中标签）。
                     */
                    TabStrip {
                        id: mirrorTabStripDown

                        visible: root.splitting && mirrorPaneHolder.stacked
                        pane: mirrorPane
                        iconProvider: icons
                        /*
                         * 开关不放这一条：它在 paneHolder 里，预览一开就跟着
                         * 正文一起收起来，切不回源码（见上栏那条的说明）。
                         */
                        paneActions: false
                        markdownToggleVisible: root.canPreviewMarkdown
                        markdownPreview: root.markdownPreview
                        /* 它夹在两栏中间，不是卡片的顶边，两个上角都不圆 */
                        roundTopLeft: false
                        roundTopRight: false

                        x: 0
                        y: root.splitPaneInset
                        width: parent.width
                        height: tabBars.stripHeight

                        onTabCloseRequested: (pane, index) =>
                            root.tabCloseRequested(pane, index)
                        onTabContextMenuRequested: (pane, index, anchor, x, y) =>
                            root.tabContextMenuRequested(pane, index, anchor, x, y)
                        onMarkdownToggleRequested: root.markdownToggleRequested()
                    }

                    EditorView {
                        id: mirrorPane

                        /* 只是让界面 / 自检认出"这是第二栏"，行为上和主栏对等 */
                        mirror: true

                        anchors.fill: parent
                        /*
                         * 朝向分隔线的那条边让开一整个 splitPaneInset（左右分栏是
                         * 左边，上下分栏是上边），另外三边还是 2px：那条 10px 的
                         * 抓手要落在"两栏正文都让开"的缝里才收得到鼠标，
                         * 两个 2px 加起来只有 4px，抓不住（见 splitHandleSize 的说明）。
                         *
                         * 上下分栏时上面还压着这一栏自己的标签栏
                         * （mirrorTabStripDown，35px），所以上边让开的是
                         * "标签栏 + 那条缝"那么多。
                         */
                        anchors.leftMargin: (root.splitting && root.splitMode !== "down")
                                            ? root.splitPaneInset : 2
                        anchors.topMargin: (root.splitting && mirrorPaneHolder.stacked)
                                           ? (root.splitPaneInset + tabBars.stripHeight) : 2
                        anchors.rightMargin: 2
                        anchors.bottomMargin: 2

                        /*
                         * 视图级设置**绑在主栏上**：两栏是同一个编辑器的两个
                         * 视图，字号 / 换行 / 行号 / 各种竖线 / 缩放必须一致
                         * —— 在设置菜单里改一次，两栏一起变（免得看着像坏了）。
                         * 文档级的东西（语言 / 编码 / 换行符）不在这里，那些
                         * 跟着文档走。
                         */
                        fontPixelSize: editorView.fontPixelSize
                        fontFamily: editorView.fontFamily
                        commentFontPixelSize: editorView.commentFontPixelSize
                        lineHeightFactor: editorView.lineHeightFactor
                        textColor: "#d6d7da"
                        paperColor: root.editorBg
                        gutterColor: root.editorBg
                        lineNumberColor: root.lineNumberColor
                        wrapEnabled: editorView.wrapEnabled
                        lineNumbersVisible: editorView.lineNumbersVisible
                        whitespaceVisible: editorView.whitespaceVisible
                        indentGuidesVisible: editorView.indentGuidesVisible
                        gutterLineVisible: editorView.gutterLineVisible
                        rulerVisible: editorView.rulerVisible
                        rulerColumn: editorView.rulerColumn
                        foldingEnabled: editorView.foldingEnabled
                        readOnly: editorView.readOnly
                        /*
                         * 缩放也跟主栏走。它是**命令式**的（zoomIn/Out 直接改
                         * Scintilla 的 zoom），光靠上面那些绑定同步不过来，
                         * 所以用这个只写不改的属性：主栏的缩放一变就写第二栏。
                         */
                        zoomPercent: editorView.zoomPercent
                        paddingLeft: 2
                        paddingRight: 0

                        onDocumentsChanged: if (find.opened) find.refreshHighlight()
                        onPaneFocused: root.paneFocusRequested(mirrorPane)
                    }

                    /*
                     * 分隔条：抓取区（鼠标移上去是改宽 / 改高的光标，拖它分栏比例）。
                     *
                     * **画出来的只有中间那 1px 的线**，颜色和编辑区里那条分隔竖线
                     * 一样（gutterLine：#333840）—— 用户要的是"和序号线一样的一条"，
                     * 所以鼠标移上去 / 拖动时也**不变色**（原来会变成强调蓝，
                     * 看着像多出来一条亮线）。
                     *
                     * 左右分栏时它贴在第二栏的左边缘（x = -半宽），上下分栏时贴在
                     * 它的上边缘 —— 正好落在两栏中间那条缝上；纵向分栏时它从标签栏
                     * 下沿一路到底（这个 holder 铺满正文区）。
                     */
                    Rectangle {
                        id: splitHandle

                        readonly property bool vertical: root.splitMode === "down"

                        width: vertical ? parent.width : root.splitHandleSize
                        height: vertical ? root.splitHandleSize : parent.height
                        x: vertical ? 0 : -root.splitHandleSize / 2
                        y: vertical ? -root.splitHandleSize / 2 : 0
                        /* 抓取区本身不画底：看得见的那条线是下面那个 1px 的矩形 */
                        color: "transparent"

                        /* 那条 1px 的线（居中压在抓取区里），颜色同行号右边那条竖线 */
                        Rectangle {
                            anchors.centerIn: parent
                            width: splitHandle.vertical ? parent.width : 1
                            height: splitHandle.vertical ? 1 : parent.height
                            color: root.gutterLineColor
                        }

                        MouseArea {
                            id: handleHit

                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: splitHandle.vertical ? Qt.SizeVerCursor
                                                              : Qt.SizeHorCursor

                            property real pressPos: 0
                            property real pressRatio: 0

                            onPressed: (mouse) => {
                                var p = mapToItem(contentArea, mouse.x, mouse.y)
                                pressPos = splitHandle.vertical ? p.y : p.x
                                pressRatio = root.splitRatio
                            }
                            onPositionChanged: (mouse) => {
                                if (!pressed || !contentArea)
                                    return
                                var p = mapToItem(contentArea, mouse.x, mouse.y)
                                var total = splitHandle.vertical ? contentArea.height
                                                                 : contentArea.width
                                if (total <= 0)
                                    return
                                var delta = (splitHandle.vertical ? p.y : p.x) - pressPos
                                /* 夹在 0.2 ~ 0.8：哪一栏都不许被拖到看不见 */
                                root.splitRatio = Math.max(0.2, Math.min(0.8,
                                                                       pressRatio + delta / total))
                            }
                        }
                    }
                }
            }

            /* ---- Markdown 预览（只读渲染，见 MarkdownView.qml） ---- */
            MarkdownView {
                id: markdownView

                anchors.fill: parent
                /* 和源码页同样的 2px 内缩：切来切去时正文不跳位置 */
                anchors.margins: 2
                visible: root.markdownPreview
                html: root.markdownHtml
                emptyHint: root.markdownHint
                onLinkActivated: (link) => root.markdownLinkActivated(link)
                onContextMenuRequested: (x, y) => root.markdownContextMenuRequested(x, y)
            }
        }
    }
}
