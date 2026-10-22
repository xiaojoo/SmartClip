import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0

/*
 * 「在文件中查找」—— 在所有已索引文件的**正文**里找一段文字，双击 Shift 召唤。
 *
 * ===========================================================================
 * 为什么是独立原生窗口（Window），不是场景里的浮层
 * ===========================================================================
 * 和 SettingsPanel / DocCard / DropdownMenu 同一个原因，同一个坑：编辑区是
 * **原生 QScintilla 子窗口**（QWidget::createWindowContainer），原生子窗口
 * 永远画在 QQuickWidget 的内容之上，z 值改不动它。这扇弹框要摆在窗口正中，
 * 正好压在编辑区那块矩形上 —— 做成场景内浮层就是"看得见左边露出来的一条"。
 *
 * 为什么用 Window 而不是 Popup.Window：后者 x/y 写不进去（DocCard.qml 文件头
 * 记着那次实测：算好的坐标写下去、读回来是 0,0）。这块窗要自己管居中，
 * 所以从一开始就是 Qt.Tool 的顶层 Window，和 SettingsPanel 同一套壳。
 *
 * ===========================================================================
 * 数据从哪儿来
 * ===========================================================================
 * Store.searchContent()（见 src/ClipboardStore.cpp）—— 一张 FTS5 的 trigram
 * 索引表 clip_fts，一行一条记录，所以行号是文件里的真实行号，点一条结果
 * 直接就能跳到编辑区那一行。
 *
 * 这一扇**不碰**顶栏那个搜索框：那个是"过滤左树"（按标题 / 摘要 / 文件名），
 * 这一扇是"读正文"。两件事，两个入口，共用一套结果会互相误导。
 */
Window {
    id: root

    /* 一次最多列多少处命中。超了就如实说"只显示前 N 处"，不悄悄截断 */
    readonly property int kLimit: 300

    /* 配色：和查找栏 / 下拉菜单同一批令牌，不另起一套（蓝还是那颗 #3d78b8） */
    readonly property color panelBg:      Theme.c("#3c3f41", Theme.rev)
    readonly property color panelBorder:  Theme.c("#4b4d4f", Theme.rev)
    readonly property color fieldBg:      Theme.c("#2b2d30", Theme.rev)
    readonly property color textColor:    Theme.c("#d6d7da", Theme.rev)
    readonly property color textBright:   Theme.c("#e8e8e8", Theme.rev)
    readonly property color mutedColor:   Theme.c("#7d838c", Theme.rev)
    readonly property color accent:       Theme.c("#3d78b8", Theme.rev)
    readonly property color rowHover:     Theme.c("#34373b", Theme.rev)
    readonly property color rowSel:       Theme.c("#2f3a44", Theme.rev)
    readonly property color warnColor:    Theme.c("#c75450", Theme.rev)

    property string query: ""
    /* 区分大小写。索引本身是不分大小写的，这一档在 C++ 那侧筛回来 */
    property bool caseOn: false
    /* Store.searchContent() 的原样返回 */
    property var rows: []
    /* rows 加工成的显示行：文件头行 + 它的命中行混排（见 regroup） */
    property var display: []
    property int selected: -1
    /* 上一次查询实际花了多少毫秒（状态那一行要报真实数字，不报"约"） */
    property int elapsedMs: 0
    /*
     * 上一次查询走的是索引还是全表扫（1-2 个字只能全表扫，见 .cpp 那段注释）。
     *
     * 这里用 `.trim()` 不是 `.trimmed()`：QML 里 property string 是 JS 字符串，
     * QString 那套方法在它身上不存在 —— 写成 trimmed() 不会在编译期报错，
     * 只在运行时甩一句 "TypeError: Property 'trimmed' of object is not a function"，
     * 而这条绑定没人读就一声不吭（第一版就是这么漏过自检的）。
     */
    readonly property bool scanned: query.trim().length > 0
                                    && query.trim().length < 3

    /* 选中一条结果 -> Main.qml 打开那个文件并跳到那一行 */
    signal picked(string path, int line)

    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.NoDropShadowWindowHint
    color: "transparent"
    visible: false
    width: 760
    height: 480

    readonly property bool opened: visible

    /* ---------------------------------------------------------------- */
    /* 开 / 关 / 摆位                                                     */
    /* ---------------------------------------------------------------- */

    function openPanel() {
        /*
         * 已经开着就只把自己抬起来 —— 别把 query / rows 清掉。
         * 双击 Shift 的手势在弹框开着的时候照样会触发（那个过滤器不吃事件），
         * 用户那一下的意思是"我叫它出来"，不是"我把它清空重来"。
         */
        if (root.visible) {
            root.raise()
            root.requestActivate()
            focusField()
            return
        }
        /*
         * transientParent 必须赶在第一次 visible 之前挂，而且每次打开都挂一次：
         * 组件建出来那一刻宿主 QWidget 还没有原生窗口，那一次必定打不上
         * （同一个坑的来龙去脉记在 SettingsPanel.qml 的 openSection 里）。
         */
        Win.attachAsToolWindow(root)
        placeOverHost()
        root.visible = true
        root.raise()
        root.requestActivate()
        focusField()
        run()
    }

    function closePanel() {
        root.visible = false
    }

    function focusField() {
        Qt.callLater(function () {
            field.focus = true
            field.cursorPosition = field.text.length
        })
    }

    /* 摆到宿主窗口正中（几何现读，不存属性 —— 理由见 SettingsPanel.placeOverHost） */
    function placeOverHost() {
        var hw = Win.hostScreenGeometry()
        if (hw.width <= 0 || hw.height <= 0)
            return
        root.width = Math.max(420, Math.min(760, hw.width - 80))
        root.height = Math.max(300, Math.min(480, hw.height - 120))
        root.x = Math.max(8, hw.x + Math.round((hw.width - root.width) / 2))
        root.y = Math.max(8, hw.y + Math.round((hw.height - root.height) / 2))
    }

    /* ---------------------------------------------------------------- */
    /* 查询                                                             */
    /* ---------------------------------------------------------------- */

    /* 打字时别一个字查一遍：停 180ms 再问（1-2 字那一档是全表扫，更要挡一下） */
    Timer {
        id: debounce
        interval: 180
        onTriggered: root.run()
    }

    onQueryChanged: debounce.restart()

    onCaseOnChanged: debounce.restart()

    function run() {
        var q = root.query.trim()
        if (q === "") {
            root.rows = []
            root.display = []
            root.selected = -1
            root.elapsedMs = 0
            return
        }
        var t0 = Date.now()
        var got = Store.searchContent(q, root.kLimit, root.caseOn)
        root.elapsedMs = Date.now() - t0
        root.rows = got
        root.regroup()
    }

    /*
     * 把命中行加工成"文件头 + 它底下的命中"混排的那一份。
     *
     * 在 QML 这边分组不在 C++：C++ 只管按（文件，行号）排好序并给出每行的
     * 路径 / 行号 / 片段，分组是纯显示的事。以后要加"按修改时间排"或者
     * "只看某个文件夹"，改这里就行，不用动索引。
     */
    function regroup() {
        var out = []
        var counts = ({})
        var i
        for (i = 0; i < root.rows.length; ++i) {
            var p = root.rows[i].path
            counts[p] = (counts[p] || 0) + 1
        }
        var last = ""
        for (i = 0; i < root.rows.length; ++i) {
            var r = root.rows[i]
            if (r.path !== last) {
                out.push({ kind: "file", path: r.path, name: r.name, folder: r.folder,
                           count: counts[r.path] || 0 })
                last = r.path
            }
            out.push({ kind: "hit", path: r.path, name: r.name, folder: r.folder,
                       line: r.line, pre: r.pre, mid: r.mid, post: r.post })
        }
        root.display = out
        /* 换了一批结果，选中回到第一条命中（不是第 0 行 —— 第 0 行是文件头） */
        root.selected = firstHitFrom(-1)
    }

    /* 从 from 往后（dir=1）/ 往前（dir=-1）找第一条**命中**行；文件头行不能选 */
    function firstHitFrom(from, dir) {
        var step = dir === -1 ? -1 : 1
        for (var i = from + step; i >= 0 && i < root.display.length; i += step)
            if (root.display[i].kind === "hit")
                return i
        return -1
    }

    function move(delta) {
        var next = firstHitFrom(root.selected, delta)
        if (next < 0)
            return
        root.selected = next
        list.positionViewAtIndex(next, ListView.Contain)
    }

    function activateSelected() {
        if (root.selected < 0 || root.selected >= root.display.length)
            return
        var r = root.display[root.selected]
        if (r.kind !== "hit")
            return
        root.closePanel()
        root.picked(r.path, r.line)
    }

    /* 命中片段：把命中的那几个字着色。三段里任何一段都可能带 HTML 元字符 */
    function esc(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    function snippet(r) {
        return esc(r.pre)
               + "<span style=\"color:" + root.accent + ";font-weight:bold\">"
               + esc(r.mid) + "</span>"
               + esc(r.post)
    }

    /* ---------------------------------------------------------------- */

    /* Esc 关窗：这块窗是独立活动窗，按键先到它这儿，所以绑在这儿（同 SettingsPanel） */
    Shortcut {
        sequences: ["Escape"]
        enabled: root.visible
        onActivated: root.closePanel()
    }

    Rectangle {
        anchors.fill: parent
        color: root.panelBg
        border.color: root.panelBorder
        border.width: 1
        radius: 8

        /* 顶：标题 + 输入框 + Aa + 关闭 */
        RowLayout {
            id: head
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 10
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                radius: 5
                color: root.fieldBg
                border.color: field.activeFocus ? root.accent : root.panelBorder
                border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 9
                    anchors.rightMargin: 9
                    spacing: 7

                    AppIcon {
                        provider: icons
                        kind: "search"
                        tint: root.mutedColor
                        size: 14
                    }

                    TextField {
                        id: field
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        placeholderText: "在文件内容中查找…"
                        placeholderTextColor: root.mutedColor
                        color: root.textColor
                        font.pixelSize: 13
                        background: Item {}
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true
                        onTextChanged: root.query = text

                        /*
                         * 上下键在输入框里本来没有用处（这里只有一行），
                         * 借给结果列表移动选中；回车 = 打开选中的那一条。
                         * 都 accept 掉，别让它们再往输入框自己冒一遍。
                         */
                        Keys.onDownPressed: (event) => { event.accepted = true; root.move(1) }
                        Keys.onUpPressed: (event) => { event.accepted = true; root.move(-1) }
                        Keys.onReturnPressed: (event) => { event.accepted = true; root.activateSelected() }
                        Keys.onEnterPressed: (event) => { event.accepted = true; root.activateSelected() }
                    }
                }
            }

            /* Aa：区分大小写。开 = 蓝底，关 = 和输入框同底（和查找栏那三档一个画法） */
            Rectangle {
                Layout.preferredWidth: 34
                Layout.preferredHeight: 30
                radius: 5
                color: root.caseOn ? root.accent : root.fieldBg
                border.color: root.caseOn ? root.accent : root.panelBorder
                Text {
                    anchors.centerIn: parent
                    text: "Aa"
                    font.pixelSize: 12
                    font.bold: true
                    color: root.caseOn ? "#ffffff" : root.mutedColor
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.caseOn = !root.caseOn
                        root.focusField()
                    }
                }
                Accessible.name: "区分大小写"
                Accessible.role: Accessible.Button
            }

            Rectangle {
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                radius: 5
                color: closeMouse.containsMouse ? root.rowHover : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: "✕"
                    font.pixelSize: 14
                    color: root.mutedColor
                }
                MouseArea {
                    id: closeMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.closePanel()
                }
                Accessible.name: "关闭"
                Accessible.role: Accessible.Button
            }
        }

        /* 中：结果列表 */
        ListView {
            id: list
            objectName: "fileSearchList"
            anchors.top: head.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: foot.top
            anchors.topMargin: 8
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            anchors.bottomMargin: 6
            clip: true
            spacing: 1
            model: root.display

            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Item {
                width: list.width
                height: modelData.kind === "file" ? fileRow.implicitHeight + 8
                                                   : snippetText.implicitHeight + 6

                /* 文件头那一行 */
                RowLayout {
                    id: fileRow
                    visible: modelData.kind === "file"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 7

                    AppIcon {
                        provider: icons
                        kind: "markdown"
                        tint: root.mutedColor
                        size: 13
                    }
                    Text {
                        text: modelData.name
                        font.pixelSize: 12
                        font.bold: true
                        color: root.textBright
                    }
                    /*
                     * 目录这一段不许截断：路径是"这块界面能不能走回去"的唯一凭据，
                     * 打上省略号就等于没做。所以让它换行（WrapAnywhere：Windows
                     * 路径里没有空格，按词换行会整段不换、直接溢出）。
                     */
                    Text {
                        Layout.fillWidth: true
                        text: modelData.folder
                        font.pixelSize: 11
                        color: root.mutedColor
                        wrapMode: Text.WrapAnywhere
                    }
                    Text {
                        text: (modelData.count || 0) + " 处"
                        font.pixelSize: 11
                        color: root.mutedColor
                    }
                }

                /* 命中那一行 */
                Rectangle {
                    id: hitRow
                    visible: modelData.kind === "hit"
                    anchors.fill: parent
                    radius: 4
                    color: root.selected === index ? root.rowSel : "transparent"

                    RowLayout {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6
                        spacing: 8

                        Text {
                            Layout.preferredWidth: 38
                            Layout.alignment: Qt.AlignTop
                            horizontalAlignment: Text.AlignRight
                            text: modelData.line
                            font.pixelSize: 11
                            font.family: "Consolas"
                            color: root.mutedColor
                        }
                        /*
                         * 片段用 RichText 是为了"命中的那几个字着色"还能换行 ——
                         * 三块 Text 横排也行，但那样要么截断要么对不齐。
                         * C++ 那边已经把片段裁到命中处左右各 60 字（见 kSnippetCtx），
                         * 所以这里不需要 maximumLineCount：加上去就是一把静默的刀。
                         */
                        Text {
                            id: snippetText
                            Layout.fillWidth: true
                            textFormat: Text.RichText
                            text: root.snippet(modelData)
                            font.pixelSize: 12
                            color: root.textColor
                            wrapMode: Text.WrapAnywhere
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.selected = index
                            root.activateSelected()
                        }
                    }
                }
            }
        }

        /* 底：计数 + 耗时 + 重建索引 */
        RowLayout {
            id: foot
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            anchors.topMargin: 8
            spacing: 10

            Text {
                objectName: "fileSearchStatus"
                Layout.fillWidth: true
                text: root.statusText()
                font.pixelSize: 11
                color: Store.searchIndexReady() ? root.mutedColor : root.warnColor
                wrapMode: Text.WordWrap
            }

            /* 索引看着不对的时候人就在这一屏，重建的入口放这儿比塞进设置面板有用 */
            Rectangle {
                Layout.preferredWidth: rebuildText.implicitWidth + 18
                Layout.preferredHeight: 24
                radius: 5
                color: rebuildMouse.containsMouse ? root.rowHover : "transparent"
                border.color: root.panelBorder
                border.width: 1
                Text {
                    id: rebuildText
                    anchors.centerIn: parent
                    text: "重建索引"
                    font.pixelSize: 11
                    color: root.textColor
                }
                MouseArea {
                    id: rebuildMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        Store.rebuildSearchIndex()
                        root.run()
                    }
                }
            }

            Text {
                text: "↑↓ 选择 · Enter 打开 · Esc 关闭"
                font.pixelSize: 11
                color: root.mutedColor
            }
        }
    }

    /*
     * 状态那一行写的必须是它量到的那件事（不许写"命中很多"这种话）。
     *
     * 三件事分开报：命中几处 / 几个文件、是否被 kLimit 截断、这次花了多少毫秒
     * 以及**走的是索引还是全表扫** —— 1-2 个字那一档慢是量出来的事实，
     * 藏着不如写着。
     */
    function statusText() {
        if (!Store.searchIndexReady())
            return "全文索引没有建起来（FTS5 不可用），这一扇搜不了正文"
        var q = root.query.trim()
        if (q === "")
            return "索引里 " + Store.searchIndexRows() + " 行"
        var hits = root.rows.length
        var files = 0
        for (var i = 0; i < root.rows.length; ++i)
            if (i === 0 || root.rows[i].path !== root.rows[i - 1].path)
                ++files
        var s = "命中 " + hits + " 处 · " + files + " 个文件 · " + root.elapsedMs + " ms · "
                + (root.scanned ? "1-2 字走全表扫" : "走 trigram 索引")
        if (hits >= root.kLimit)
            s += "（只显示前 " + root.kLimit + " 处，缩小关键字再看）"
        return s
    }

    IconProvider { id: icons }
}
