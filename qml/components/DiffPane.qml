pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SmartClip.Globals 1.0
import SmartClip.Editor 1.0

/*
 * 文件对比页：一栏一份正文，左右并排，两边都是**真的编辑器**（能打字、能撤销、
 * 能存盘），不是画出来的差异图。
 *
 * 为什么是真编辑器而不是自己画（原来那张弹窗卡片就是自己画的）：
 * 用户要的正是"看差异的同时顺手改两笔"。自己画的行表只能看不能改。
 *
 * 那"一行对一行"的观感怎么来的？两栏各自把自己那些**多出来的行**用 Scintilla
 * 的行注释（annotation）撑成空白带 —— 见 src/EditorViewItem.h 里 beginDiff
 * 那一组。撑完之后左右两栏的"第 k 个显示行"就是同一逻辑行，所以滚动同步
 * 只要同步行号，不用查映射表。
 *
 * 数据全在 C++ 那侧（SmartClip.Globals 的 Differ，见 src/Diff.h）：它给一张
 * 已经对齐好的行表，这里只负责把每一行涂到对应的编辑器上、以及摆控件。
 *
 * 注意两个 EditorView 是**独立的原生子窗口**：只要 show 着就会盖在 QML 任何
 * 内容之上，所以这一页不显示时必须整块 visible=false（由 EditorArea 那边管）。
 */
Item {
    id: root
    /*
     * 当前这一份对比会话：{ leftDocId, rightDocId, leftTitle, rightTitle }。
     * 换了会话就重新绑一遍两栏（同一时刻只有一份 DiffPane，见 EditorArea 那段）。
     */
    property var session: null

    /* 字号 / 主题跟着主编辑栏走（这里只读，不改它） */
    property var styleSource: null

    /* 当前在第几处差异上（-1 = 还没跳） */
    property int changeIndex: -1

    signal closed()

    readonly property color borderColor: Theme.c("#4b4d4f", Theme.rev)
    readonly property color textColor: Theme.c("#d6d7da", Theme.rev)
    readonly property color mutedColor: Theme.c("#9aa0a6", Theme.rev)
    readonly property color accentColor: "#4c96d8"
    readonly property color barBg: "#26282c"
    readonly property color delBg: Theme.c("#3a2224", Theme.rev)
    readonly property color addBg: Theme.c("#1e3524", Theme.rev)
    readonly property color modBg: Theme.c("#3a3320", Theme.rev)

    /* 两栏之间那条缝（和分栏那条同一个手感） */
    readonly property real handleSize: 10
    /*
     * 右边那条差异导航条的宽度。右栏题头里的"保存"要让开它 ——
     * 导航条是盖在右栏宿主 Item 的最右边 12px 上的，不让就被切掉半截。
     */
    readonly property real navWidth: 12
    property real splitRatio: 0.5

    /* 滚动同步的开关（BC 里叫 Scroll Sync）：关掉之后两栏各滚各的 */
    property bool syncScroll: true
    /* 正在回声的那一栏（见下面的 follow()） */
    property bool echoing: false

    readonly property string leftTitle: session && session.leftTitle ? session.leftTitle : ""
    readonly property string rightTitle: session && session.rightTitle ? session.rightTitle : ""

    /*
     * 换会话：先把上一对文档身上那层画的东西还回去，再挂到新的两份上。
     *
     * 标记 / 行注释 / 指示器都存在**文档**上（不是视图上），不还就会跟着文档
     * 回到普通标签页 —— 用户切回去看到的是满屏假高亮。
     */
    function rebind() {
        leftPane.endDiff()
        rightPane.endDiff()
        if (!session)
            return
        if (session.leftDocId !== undefined && session.leftDocId >= 0)
            leftPane.openPoolDocument(session.leftDocId)
        if (session.rightDocId !== undefined && session.rightDocId >= 0)
            rightPane.openPoolDocument(session.rightDocId)
        recompare()
    }

    onSessionChanged: Qt.callLater(rebind)

    /* 关掉这一页（Main.qml 负责把会话从列表里摘掉） */
    function unbindPanes() {
        leftPane.endDiff()
        rightPane.endDiff()
    }

    /* 关会话时把右栏那一格收回去（文档本身留在池子里，不删） */
    function releaseRight() {
        rightPane.endDiff()
        rightPane.closeCurrent()
    }

    /*
     * 用两栏**当前的正文**重算一次差异。
     *
     * 走 currentText() 而不是重新读盘：左边那份很可能有没存盘的改动，
     * 用户要比的正是"我现在这一份"。算完 Differ 发 compared，
     * 下面那个 Connections 把结果刷回两栏。
     */
    function recompare() {
        if (!session || !leftPane.hasDocument || !rightPane.hasDocument)
            return
        Differ.compare(leftPane.currentText(), rightPane.currentText(),
                       leftPane.filePath, rightPane.filePath)
    }

    /*
     * 把右边准备好，返回 { docId, title }。
     *
     * 走右栏自己那份 openFile / newDocument：文档进的是**全局池子**，
     * 但"开一个标签"只落在右栏这一格里，所以主标签栏不会凭空多出一格。
     * path 传空串 = 新建一份未命名的空白文档（可以直接往里打字再比）。
     */
    function prepareRight(path) {
        if (path === "")
            rightPane.newDocument()
        else
            rightPane.openFile(path)
        return { docId: rightPane.currentDocId(), title: rightPane.displayName }
    }

    /* ------------------------------------------------------------------
     * 自检口子（src/SelfTestTools.cpp 的"对比页"那一节读这几个）
     * ---------------------------------------------------------------- */

    function pane(i) {
        return i === 0 ? leftPane : rightPane
    }

    function paneInfo(i) {
        var p = pane(i)
        /* lineCount / charCount / modified 是**属性**不是函数（见 EditorViewItem.h） */
        var at = p.mapToItem(null, 0, 0)
        return {
            lines: p.lineCount,
            chars: p.charCount,
            dirty: p.modified,
            docId: p.currentDocId(),
            shown: p.visible,
            gap: p.diffGapAt(0),
            x: Math.round(at.x), y: Math.round(at.y),
            w: Math.round(p.width), h: Math.round(p.height),
            /* 宿主 Item 的几何：原生控件没画出来时，先看这一层是不是 0 大小 */
            hostW: Math.round(p.parent ? p.parent.width : -1),
            hostH: Math.round(p.parent ? p.parent.height : -1),
            rootW: Math.round(root.width), rootH: Math.round(root.height),
            rootShown: root.visible,
            /* 原生控件那一份几何：QML 说有位置 ≠ 那块 HWND 真在那个位置 */
            nat: p.paneGeometryForTest()
        }
    }

    /*
     * 第 index 处差异在两栏各自的高度上落在哪儿。
     *
     * 这两个数**相等**才说明对齐真的生效了 —— 底色、标记位那些都能在"逻辑上对"
     * 而"画出来错开"的情况下全绿，只有 y 量得出来错位。
     */
    function changeYs(index) {
        var list = Differ.changes
        if (index < 0 || index >= list.length)
            return ({ left: -1, right: -1 })
        var c = list[index]
        return {
            left: c.leftCount > 0 ? leftPane.lineTopY(c.leftStart) : -1,
            right: c.rightCount > 0 ? rightPane.lineTopY(c.rightStart) : -1
        }
    }

    /* 两栏的滚动位置（验同步） */
    function scrollState() {
        return { left: leftPane.firstVisibleLine(), right: rightPane.firstVisibleLine() }
    }

    /* 滚到第 index 处差异（自检用） */
    function gotoChangeForTest(index) {
        showChange(index)
    }

    /*
     * 自检用：当前那一处现在亮不亮 —— 量"被标为当前的第一行"上
     * 亮一档那个色（#63572c）占了多少像素。
     */
    function currentBandStats() {
        if (markedLeft.length > 0)
            return leftPane.diffRowStats(markedLeft[0], Theme.c("#63572c", Theme.rev))
        if (markedRight.length > 0)
            return rightPane.diffRowStats(markedRight[0], Theme.c("#63572c", Theme.rev))
        return ({ band: 0, glyph: 0, paper: 0 })
    }

    /* 把 Differ 那张行表刷到两栏上。
     *
     * 每一行做三件事：整行底色（标记）、下面撑空白带（行注释）、
     * 改过的行再标一下到底是哪几个字（指示器）。
     * 行号是 1 基、编辑器是 0 基，这里统一减一。
     */
    function applyDiff() {
        /* 这一页没开着的时候别刷：两栏是原生子窗口，画了也看不见，白花时间 */
        if (!root.visible)
            return
        var rows = Differ.rows
        leftPane.beginDiff()
        rightPane.beginDiff()
        for (var i = 0; i < rows.length; ++i) {
            var r = rows[i]
            if (r.leftNo > 0) {
                var ln = r.leftNo - 1
                leftPane.setDiffLineKind(ln, r.kind)
                if (r.leftGap > 0)
                    leftPane.setDiffGap(ln, r.leftGap)
                if (r.kind === "mod" && r.leftWords && r.leftWords.length > 0)
                    leftPane.setDiffWordMarks(ln, r.leftWords)
            }
            if (r.rightNo > 0) {
                var rn = r.rightNo - 1
                rightPane.setDiffLineKind(rn, r.kind)
                if (r.rightGap > 0)
                    rightPane.setDiffGap(rn, r.rightGap)
                if (r.kind === "mod" && r.rightWords && r.rightWords.length > 0)
                    rightPane.setDiffWordMarks(rn, r.rightWords)
            }
        }
        changeIndex = -1
        markedLeft = []
        markedRight = []
        nav.requestPaint()
    }

    /*
     * 跳到上 / 下一处差异。dir = -1 往前、1 往后；到底了绕回另一头
     * （和原来那张卡片一样：不绕的话用户会以为按钮坏了）。
     */
    function gotoChange(dir) {
        var list = Differ.changes
        if (list.length === 0)
            return
        var next = changeIndex + dir
        if (next < 0)
            next = list.length - 1
        else if (next >= list.length)
            next = 0
        showChange(next)
    }

    function showChange(index) {
        var list = Differ.changes
        if (index < 0 || index >= list.length)
            return
        changeIndex = index
        var c = list[index]
        clearCurrentMarks()
        /*
         * 整块都涂上"亮一档"，不是只标第一行 —— BC 里跳过去看到的是
         * "这一整块是当前这处"，只标头一行会以为块只有那么大。
         */
        for (var i = 0; i < c.leftCount; ++i) {
            leftPane.setDiffCurrent(c.leftStart + i, c.kind)
            markedLeft.push(c.leftStart + i)
        }
        for (var j = 0; j < c.rightCount; ++j) {
            rightPane.setDiffCurrent(c.rightStart + j, c.kind)
            markedRight.push(c.rightStart + j)
        }
        /*
         * 开头那几行有一边压根没有对应行（leadGap*，见 src/Diff.h 那段）——
         * 那一段撑不出空白带，所以两栏的显示行整体错开这么多，同步时要补回去。
         */
        var lead = Differ.leadGapRight - Differ.leadGapLeft
        jumpTo(leftPane, c.leftStart, lead)
        jumpTo(rightPane, c.rightStart, -lead)
    }

    /* 上一轮"当前"标在哪几行上（撤的时候要按行号撤，块换了就找不到原位置了） */
    property var markedLeft: []
    property var markedRight: []

    function clearCurrentMarks() {
        for (var i = 0; i < markedLeft.length; ++i)
            leftPane.setDiffCurrent(markedLeft[i], "same")
        for (var j = 0; j < markedRight.length; ++j)
            rightPane.setDiffCurrent(markedRight[j], "same")
        markedLeft = []
        markedRight = []
    }

    /* 滚到那一行，并且把它摆在视口中间偏上一点（贴顶的话看不出上下文） */
    function jumpTo(pane, docLine, leadFix) {
        if (!pane.hasDocument)
            return
        var h = pane.textLineHeight()
        if (h <= 0)
            return
        var halfRows = Math.floor(pane.height / h / 2)
        /* 文档行 -> 显示行：它前面那些空白带也得算进去 */
        var display = docLine + gapsBefore(pane === leftPane ? true : false, docLine)
        pane.setFirstVisibleLine(Math.max(0, display - halfRows + leadFix))
    }

    /*
     * 第 line 行**之前**一共撑了多少行空白。
     *
     * 行注释把空白挂在上一行下面，所以"这一行的显示行号"= 文档行号 +
     * 它前面所有 gap 之和。跳转和同步都要这个数，只有行表里有。
     */
    function gapsBefore(left, line) {
        var rows = Differ.rows
        var sum = 0
        var seen = 0
        for (var i = 0; i < rows.length; ++i) {
            var no = left ? rows[i].leftNo : rows[i].rightNo
            if (no <= 0)
                continue
            if (seen >= line)
                break
            sum += left ? (rows[i].leftGap || 0) : (rows[i].rightGap || 0)
            ++seen
        }
        return sum
    }

    /* 两栏互相跟随（同步开着才跟） */
    function follow(from, to, leadFix) {
        if (!root.syncScroll || root.echoing)
            return
        root.echoing = true
        var line = from.firstVisibleLine() + leadFix
        var x = from.viewXOffset()
        to.setFirstVisibleLine(Math.max(0, line))
        to.setViewXOffset(x)
        root.echoing = false
    }

    /* 把整份差异导出成统一格式的补丁，交给 Main.qml 写剪贴板 */
    signal copyPatchRequested()
    /* 把当前这一处差异从一侧搬到另一侧（dir=1：左 -> 右；dir=-1：右 -> 左） */
    function mergeCurrent(dir) {
        if (changeIndex < 0)
            return
        var c = Differ.changes[changeIndex]
        if (!c)
            return
        var rows = Differ.rows
        var end = c.row
        while (end < rows.length && rows[end].kind !== "same")
            ++end

        /* 整块搬：块内不再细分，BC 的"复制这一处"就是一次一整块 */
        var from = []
        for (var i = c.row; i < end; ++i) {
            if (dir > 0 ? rows[i].leftNo > 0 : rows[i].rightNo > 0)
                from.push(dir > 0 ? rows[i].left : rows[i].right)
        }

        /*
         * 目标落在哪儿：块里目标侧**第一个有行**的那一行起，连着把目标侧
         * 那些行换掉。整块目标侧一行都没有（纯删 / 纯插的那一种）就没有可换的，
         * 退成"接在上一行后面"插进去 —— 不这么处理这一种按下去会什么都不发生。
         */
        var at = -1
        var count = 0
        for (var j = c.row; j < end; ++j) {
            var no = dir > 0 ? rows[j].rightNo : rows[j].leftNo
            if (no > 0) {
                if (at < 0)
                    at = no
                ++count
            }
        }
        if (at < 0) {
            for (var k = c.row - 1; k >= 0; --k) {
                var p = dir > 0 ? rows[k].rightNo : rows[k].leftNo
                if (p > 0) {
                    at = p + 1
                    break
                }
            }
        }
        if (at < 0)
            at = 1     /* 目标整份都是空的：就写在最前面 */

        /*
         * 先接到变量上再调，不要写成 (dir > 0 ? rightPane : leftPane).replaceLines(...)：
         * 那种写法实测调不动（后面一句 console.log 照跑，方法本身像被吞了），
         * 接成变量就正常。
         */
        var target = dir > 0 ? rightPane : leftPane
        target.replaceLines(at - 1, count, from)
    }

    /*
     * 整页的底。和编辑区那张卡片同一个色（#1e1f22）—— 两栏正文的纸色也是它，
     * 所以正文之外（顶栏 / 两条题头 / 底栏）和正文之间看不出接缝。
     */
    Rectangle {
        anchors.fill: parent
        radius: 10
        color: Theme.c("#1e1f22", Theme.rev)
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        /* ================= 顶栏 ================= */
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 32
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            spacing: 6

            Text {
                text: "文件对比"
                color: root.textColor
                font.pixelSize: 12
                font.bold: true
            }

            Text {
                Layout.fillWidth: true
                text: Differ.summary
                color: root.mutedColor
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Repeater {
                model: [
                    { key: "ignoreCase", label: "忽略大小写" },
                    { key: "ignoreWhitespace", label: "忽略首尾空白" }
                ]

                delegate: Rectangle {
                    id: optChip

                    required property var modelData

                    Layout.preferredWidth: optLabel.implicitWidth + 16
                    Layout.preferredHeight: 22
                    radius: 4
                    color: optHit.containsMouse ? Theme.c("#3a3d41", Theme.rev)
                          : (Differ[modelData.key] ? Theme.c("#2c3f52", Theme.rev) : "transparent")
                    border.color: Differ[modelData.key] ? root.accentColor : root.borderColor
                    border.width: 1

                    Text {
                        id: optLabel
                        anchors.centerIn: parent
                        text: optChip.modelData.label
                        color: Differ[optChip.modelData.key] ? Theme.c("#e8e8e8", Theme.rev) : root.mutedColor
                        font.pixelSize: 11
                    }

                    MouseArea {
                        id: optHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: Differ[optChip.modelData.key] = !Differ[optChip.modelData.key]
                    }
                }
            }

            Rectangle {
                Layout.preferredWidth: syncLabel.implicitWidth + 16
                Layout.preferredHeight: 22
                radius: 4
                color: syncHit.containsMouse ? Theme.c("#3a3d41", Theme.rev)
                      : (root.syncScroll ? Theme.c("#2c3f52", Theme.rev) : "transparent")
                border.color: root.syncScroll ? root.accentColor : root.borderColor
                border.width: 1

                Text {
                    id: syncLabel
                    anchors.centerIn: parent
                    text: "同步滚动"
                    color: root.syncScroll ? Theme.c("#e8e8e8", Theme.rev) : root.mutedColor
                    font.pixelSize: 11
                }

                MouseArea {
                    id: syncHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.syncScroll = !root.syncScroll
                }
            }

            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 22
                radius: 4
                color: l2rHit.containsMouse ? Theme.c("#3a3d41", Theme.rev) : "transparent"
                border.color: root.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "左 → 右"
                    color: root.textColor
                    font.pixelSize: 11
                }

                MouseArea {
                    id: l2rHit
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: Differ.lastError === ""
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.mergeCurrent(1)
                }
            }

            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 22
                radius: 4
                color: r2lHit.containsMouse ? Theme.c("#3a3d41", Theme.rev) : "transparent"
                border.color: root.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "← 右"
                    color: root.textColor
                    font.pixelSize: 11
                }

                MouseArea {
                    id: r2lHit
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: Differ.lastError === ""
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.mergeCurrent(-1)
                }
            }

            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 22
                radius: 4
                color: patchHit.containsMouse ? Theme.c("#3a3d41", Theme.rev) : "transparent"
                border.color: root.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "复制补丁"
                    color: root.textColor
                    font.pixelSize: 11
                }

                MouseArea {
                    id: patchHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.copyPatchRequested()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: root.borderColor
            opacity: 0.6
        }

        /* ================= 两栏正文 ================= */
        Item {
            id: body

            Layout.fillWidth: true
            Layout.fillHeight: true

            readonly property real leftWidth: Math.max(120, width * root.splitRatio)

            /* ---- 左 ---- */
            Item {
                id: leftHost

                x: 0
                y: 0
                width: body.leftWidth
                height: parent.height

                RowLayout {
                    id: leftCaption

                    objectName: "diffLeftCaption"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 24
                    spacing: 6

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: root.barBg

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.leftTitle
                            color: root.textColor
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                            width: parent.width - 90
                        }

                        /*
                         * 每一边各一个"保存"。
                         *
                         * 对比页那两栏**不抢**"当前编辑器"（s_instance）—— 抢了之后
                         * 工具栏 / 菜单 / Ctrl+S 会打到这一栏上，把主编辑栏那一套
                         * 命令路由全带偏。代价就是全局的保存键管不到这里，
                         * 所以每边自己给一个。
                         */
                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            visible: leftPane.modified
                            text: "保存"
                            color: leftSaveHit.containsMouse ? root.accentColor : root.mutedColor
                            font.pixelSize: 11

                            MouseArea {
                                id: leftSaveHit
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: leftPane.saveCurrent()
                            }
                        }
                    }
                }

                EditorView {
                    id: leftPane

                    objectName: "diffLeftPane"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: leftCaption.bottom
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 2
                    anchors.rightMargin: root.handleSize / 2
                    anchors.bottomMargin: 2

                    visible: hasDocument
                    fontPixelSize: root.styleSource ? root.styleSource.fontPixelSize : 12
                    textColor: Theme.c("#d6d7da", Theme.rev)
                    paperColor: Theme.c("#1e1f22", Theme.rev)
                    gutterColor: Theme.c("#1e1f22", Theme.rev)
                    lineNumberColor: Theme.c("#606366", Theme.rev)
                    paddingLeft: 2
                    paddingRight: 0

                    onTextChanged: Qt.callLater(root.recompare)
                    onViewScrolled: root.follow(leftPane, rightPane,
                                                Differ.leadGapRight - Differ.leadGapLeft)
                }
            }

            /* ---- 右 ---- */
            Item {
                id: rightHost

                x: body.leftWidth
                y: 0
                width: Math.max(120, parent.width - body.leftWidth)
                height: parent.height

                RowLayout {
                    id: rightCaption

                    objectName: "diffRightCaption"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 24
                    spacing: 6

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        color: root.barBg

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.rightTitle
                            color: root.textColor
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                            width: parent.width - 90
                        }

                        Text {
                            anchors.right: parent.right
                            anchors.rightMargin: root.navWidth + 10
                            anchors.verticalCenter: parent.verticalCenter
                            visible: rightPane.modified
                            text: "保存"
                            color: rightSaveHit.containsMouse ? root.accentColor : root.mutedColor
                            font.pixelSize: 11

                            MouseArea {
                                id: rightSaveHit
                                anchors.fill: parent
                                anchors.margins: -4
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: rightPane.saveCurrent()
                            }
                        }
                    }
                }

                EditorView {
                    id: rightPane

                    objectName: "diffRightPane"
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: rightCaption.bottom
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: root.handleSize / 2
                    anchors.rightMargin: root.navWidth + 2
                    anchors.bottomMargin: 2

                    visible: hasDocument
                    fontPixelSize: root.styleSource ? root.styleSource.fontPixelSize : 12
                    textColor: Theme.c("#d6d7da", Theme.rev)
                    paperColor: Theme.c("#1e1f22", Theme.rev)
                    gutterColor: Theme.c("#1e1f22", Theme.rev)
                    lineNumberColor: Theme.c("#606366", Theme.rev)
                    paddingLeft: 2
                    paddingRight: 0

                    onTextChanged: Qt.callLater(root.recompare)
                    onViewScrolled: root.follow(rightPane, leftPane,
                                                Differ.leadGapLeft - Differ.leadGapRight)
                }
            }

            /* ---- 中间那条缝（拖它改两栏比例） ---- */
            MouseArea {
                id: diffHandle

                width: root.handleSize
                height: parent.height
                x: Math.max(0, body.leftWidth - root.handleSize / 2)
                cursorShape: Qt.SizeHorCursor

                Rectangle {
                    anchors.centerIn: parent
                    width: 1
                    height: parent.height
                    color: Theme.c("#333840", Theme.rev)
                }

                property real pressX: 0
                property real pressRatio: 0.5

                onPressed: (mouse) => {
                    Win.pushResizeCursor(Qt.SizeHorCursor)
                    pressX = mapToItem(body, mouse.x, mouse.y).x
                    pressRatio = root.splitRatio
                }
                onCanceled: Win.popResizeCursor()
                onReleased: Win.popResizeCursor()
                onPositionChanged: (mouse) => {
                    if (!pressed || body.width <= 0)
                        return
                    var dx = mapToItem(body, mouse.x, mouse.y).x - pressX
                    root.splitRatio = Math.max(0.2, Math.min(0.8,
                                                           pressRatio + dx / body.width))
                }
            }

            /*
             * 右边那条差异导航条：整份文件压成一条，差异处涂色块，点一下跳过去。
             *
             * 用 Canvas 而不是 Repeater：一份 5000 行的文件能有几百处差异，
             * 每个色块一个 Item 太浪费，而这条带子根本不需要单独命中。
             */
            Canvas {
                id: nav

                width: root.navWidth
                height: parent.height
                x: parent.width - width
                visible: Differ.hasResult

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    ctx.fillStyle = "#26282c"
                    ctx.fillRect(0, 0, width, height)
                    var total = Differ.rows.length
                    if (total === 0 || height <= 0)
                        return
                    var rows = Differ.rows
                    var step = height / total
                    for (var i = 0; i < total; ++i) {
                        var kind = rows[i].kind
                        if (kind === "same")
                            continue
                        ctx.fillStyle = kind === "del" ? Theme.c("#7a4448", Theme.rev)
                                    : kind === "add" ? Theme.c("#3f6b48", Theme.rev) : Theme.c("#8a7a3c", Theme.rev)
                        /* 至少 1px，否则文件一大差异就全被压没了 */
                        ctx.fillRect(1, i * step, width - 2, Math.max(1, step))
                    }
                }

                /* Canvas 自带 requestPaint()，外面改完行表直接调它 */

                MouseArea {
                    anchors.fill: parent
                    anchors.leftMargin: -3
                    anchors.rightMargin: -3
                    cursorShape: Qt.PointingHandCursor
                    onClicked: (mouse) => {
                        var total = Differ.rows.length
                        if (total <= 0 || nav.height <= 0)
                            return
                        var row = Math.floor(mouse.y / nav.height * total)
                        var list = Differ.changes
                        for (var i = 0; i < list.length; ++i) {
                            if (list[i].row >= row) {
                                root.showChange(i)
                                return
                            }
                        }
                        if (list.length > 0)
                            root.showChange(list.length - 1)
                    }
                }
            }
        }

        /* ================= 底栏 ================= */
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 26
            Layout.leftMargin: 10
            Layout.rightMargin: 10
            spacing: 8

            Text {
                Layout.fillWidth: true
                text: Differ.lastError !== "" ? Differ.lastError : leadHint()
                color: Differ.lastError !== "" ? Theme.c("#e06c75", Theme.rev) : root.mutedColor
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            Text {
                text: Differ.changes.length > 0
                      ? "差异 " + (root.changeIndex + 1) + " / " + Differ.changes.length
                      : ""
                color: root.mutedColor
                font.pixelSize: 11
            }

            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 22
                radius: 4
                color: prevHit.containsMouse ? Theme.c("#3a3d41", Theme.rev) : "transparent"
                border.color: root.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "上一个差异"
                    color: root.textColor
                    font.pixelSize: 11
                }

                MouseArea {
                    id: prevHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.gotoChange(-1)
                }
            }

            Rectangle {
                Layout.preferredWidth: 76
                Layout.preferredHeight: 22
                radius: 4
                color: nextHit.containsMouse ? Theme.c("#3a3d41", Theme.rev) : "transparent"
                border.color: root.borderColor
                border.width: 1

                Text {
                    anchors.centerIn: parent
                    text: "下一个差异"
                    color: root.textColor
                    font.pixelSize: 11
                }

                MouseArea {
                    id: nextHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.gotoChange(1)
                }
            }
        }
    }

    /* 开头那段撑不出空白带，就在底栏说清楚，别让用户以为对错了 */
    function leadHint() {
        var lead = []
        if (Differ.leadGapLeft > 0)
            lead.push("左侧开头少 " + Differ.leadGapLeft + " 行")
        if (Differ.leadGapRight > 0)
            lead.push("右侧开头少 " + Differ.leadGapRight + " 行")
        if (lead.length === 0)
            return "共 " + Differ.rows.length + " 行"
        return "共 " + Differ.rows.length + " 行 · " + lead.join("、") + "（开头那段对不齐）"
    }

    Connections {
        target: Differ
        function onCompared() {
            root.applyDiff()
        }
    }
}
