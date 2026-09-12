pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

/*
 * 查找 / 替换栏。
 *
 * 为什么是"一条独立的栏"而不是浮在编辑区上的浮层：
 * 编辑区是**原生子窗口**（QScintilla 通过 createWindowContainer 挂在宿主
 * QWidget 上），它永远画在 QML 内容之上 —— 浮层一旦和编辑区重叠就被它盖住。
 * 所以做成标签栏下面的一条（编辑区自己往下让位，几何变化由
 * EditorViewItem::applyGeometry 跟着走）。
 *
 * 外观按样例（VS Code 那个查找面板）来：
 *   * 一块**圆角面板**，左右各留 8px 间隙，不再是从左铺到右、带一条下边框的长条；
 *   * 两个输入框**一样长** —— 替换框的宽度直接绑到查找框的宽度上，
 *     而不是靠"两边各垫一个固定宽度的占位"去凑（那样按钮字一长就错开了）；
 *   * 大小写 / 全字 / 正则、上一个下一个、匹配数都放到**查找框内部右侧**
 *     （样例就是这样的），中间那两条分隔线因此也去掉了。
 *
 * 支持：区分大小写、全字匹配、正则、全部高亮、逐个替换、全部替换、
 *       匹配计数、Enter / Shift+Enter 前后跳、Esc 关闭。
 */
Rectangle {
    id: root

    /*
     * 高度 = 面板上下各 2px + 面板内的上下留白 + 行高。
     *   查找行：2 + 4 + 28 + 4 + 2 = 40
     *   加替换行：再 + 28（行）+ 4（行距）= 72
     */
    implicitHeight: root.replaceVisible ? 72 : 40
    /* 整条透明：看上去"浮"在正文上方的是里面那块圆角面板 */
    color: "transparent"

    property var view: null
    property bool replaceVisible: false
    /* 栏是否展开（打开查找 / 替换时置 true，关闭时置 false） */
    property bool opened: false

    signal closed()

    readonly property color panelBg: "#3c3f41"        /* 和下拉菜单一套 */
    readonly property color panelBorder: "#4b4d4f"
    readonly property color borderColor: "#3c3f44"
    readonly property color fieldBg: "#2b2d30"
    readonly property color textColor: "#d6d7da"
    readonly property color mutedColor: "#7d838c"
    readonly property color accent: "#4c96d8"

    /* 匹配计数，例如 "3 / 12"；没查过就是空 */
    property string matchText: ""
    property bool noMatch: false

    /*
     * 自检用（见 Main.qml 的 uiState）：两个输入框的实际宽度、圆角面板左右各留了
     * 多少间隙、面板圆角多大。用户要的是"两个框一样长、有圆角、左右有间隙"，
     * 这三样都得能量出来，光看代码不算数。
     */
    readonly property real findFieldWidth: fieldBox.width
    readonly property real replaceFieldWidth: replaceBox.width
    readonly property real panelLeftGap: panel.x
    readonly property real panelRightGap: root.width - (panel.x + panel.width)
    readonly property real panelRadius: panel.radius
    /* 自检还要看栏本身有没有高度：高度绑错（比如绑到不存在的 id）会静悄悄变成 0，
       整个查找栏就什么都不画了 —— 实测踩过一次 */
    readonly property real barHeight: root.height

    IconProvider { id: icons }

    function openFind() {
        replaceVisible = false
        root.opened = true
        focusField(field)
        field.selectAll()
        refreshHighlight()
    }

    function openReplace() {
        replaceVisible = true
        root.opened = true
        focusField(field)
        field.selectAll()
        refreshHighlight()
    }

    /*
     * 把键盘焦点交给查找输入框。
     *
     * 关键一步是 view.releaseEditorFocus()：编辑区是原生 QScintilla 子窗口，
     * 它拿着焦点的时候按键根本进不了 QML（只 forceActiveFocus 是没用的，
     * 实测打开查找栏后打字全进了正文）。releaseEditorFocus() 会清掉原生
     * 控件的焦点并把 QQuickWidget 设为焦点控件，QML 这边的 forceActiveFocus
     * 才真正生效。
     */
    function focusField(target) {
        if (root.view)
            root.view.releaseEditorFocus()
        target.forceActiveFocus()
    }

    function close() {
        root.opened = false
        root.matchText = ""
        root.noMatch = false
        if (root.view) {
            root.view.clearHighlights()
            root.view.requestEditorFocus()
        }
        root.closed()
    }

    function options() {
        return {
            text: field.text,
            cs: caseBox.checked,
            ww: wordBox.checked,
            re: regexBox.checked
        }
    }

    function currentText() { return field.text }

    function refreshHighlight() {
        if (!root.view || !root.view.hasDocument || field.text === "") {
            root.matchText = ""
            root.noMatch = false
            if (root.view)
                root.view.clearHighlights()
            return
        }
        var o = options()
        var n = root.view.highlightMatches(o.text, o.cs, o.ww, o.re)
        root.matchText = n > 0 ? (n + " 处") : "无匹配"
        root.noMatch = n === 0
    }

    function findNext(forward) {
        if (!root.view || !root.view.hasDocument || field.text === "")
            return
        var o = options()
        var line = root.view.find(o.text, o.cs, o.ww, o.re, forward)
        root.noMatch = line < 0
        root.matchText = line < 0 ? "无匹配" : ("第 " + line + " 行")
    }

    function doReplace() {
        if (!root.view || !root.view.hasDocument)
            return
        var o = options()
        root.view.replaceCurrent(o.text, replaceField.text, o.cs, o.ww, o.re)
        refreshHighlight()
        findNext(true)
    }

    function doReplaceAll() {
        if (!root.view || !root.view.hasDocument)
            return
        var o = options()
        var n = root.view.replaceAll(o.text, replaceField.text, o.cs, o.ww, o.re)
        root.matchText = n > 0 ? ("已替换 " + n + " 处") : "无匹配"
        root.noMatch = n === 0
        refreshHighlight()
    }

    visible: root.opened

    /* ---------------- 圆角面板（左右各留 8px 间隙） ---------------- */
    Rectangle {
        id: panel

        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 2
        anchors.bottomMargin: 2

        radius: 8
        color: root.panelBg
        border.color: root.panelBorder
        border.width: 1

        /*
         * 用 GridLayout 而不是两个 RowLayout：列宽是全表统一的，两个输入框
         * 都在第 1 列，宽度自然**一样长** —— 不用把一个框的宽度绑到另一个框上
         * （那样等于让布局反过来依赖布局的结果，实测直接把自检跑崩了：
         * 布局→宽度变化→绑定→再布局，转不完）。
         */
        GridLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            columns: 3
            columnSpacing: 6
            rowSpacing: 4

            /* ---------------- 查找行 ---------------- */
            AppIcon {
                Layout.row: 0; Layout.column: 0
                provider: icons; kind: "search"; size: 14
                tint: root.mutedColor
                Layout.alignment: Qt.AlignVCenter
            }

            /*
             * 查找输入框。右边的选项图标画在框**里面**（样例就是这样），
             * 所以文字区比替换框窄一点，但两个框的外框是一样长的。
             */
            Rectangle {
                id: fieldBox

                Layout.row: 0; Layout.column: 1
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                radius: 6
                color: root.fieldBg
                border.color: root.noMatch ? "#c75450" : root.borderColor

                TextField {
                    id: field

                    anchors.fill: parent
                    anchors.leftMargin: 10
                    /* 右边给里面的图标让位，别让文字压到它们下面 */
                    anchors.rightMargin: inlineRow.width + 12
                    placeholderText: "查找"
                    placeholderTextColor: root.mutedColor
                    color: root.textColor
                    font.pixelSize: 12
                    background: Item {}
                    verticalAlignment: TextInput.AlignVCenter
                    selectByMouse: true

                    onTextChanged: root.refreshHighlight()

                    Keys.onReturnPressed: (event) => {
                        root.findNext(!(event.modifiers & Qt.ShiftModifier))
                        event.accepted = true
                    }
                    Keys.onEnterPressed: (event) => {
                        root.findNext(!(event.modifiers & Qt.ShiftModifier))
                        event.accepted = true
                    }
                    Keys.onEscapePressed: (event) => { root.close(); event.accepted = true }
                }

                /* 框内右侧：匹配数 + 上一个 / 下一个 + 三个开关 */
                RowLayout {
                    id: inlineRow

                    anchors.right: parent.right
                    anchors.rightMargin: 3
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 1

                    Label {
                        Layout.rightMargin: 4
                        text: root.matchText
                        visible: text !== ""
                        color: root.noMatch ? "#e06c75" : root.mutedColor
                        font.pixelSize: 11
                        verticalAlignment: Text.AlignVCenter
                    }

                    ToolButton {
                        provider: icons; kind: "chevron-up"; tip: "上一个"; shortcut: "Shift+F3"
                        implicitHeight: 22
                        onClicked: root.findNext(false)
                    }
                    ToolButton {
                        provider: icons; kind: "chevron-down"; tip: "下一个"; shortcut: "F3"
                        implicitHeight: 22
                        onClicked: root.findNext(true)
                    }

                    /* 三个开关：区分大小写 / 全字匹配 / 正则 */
                    ToolButton {
                        id: caseBox
                        label: "Aa"
                        tip: "区分大小写"
                        checked: false
                        implicitHeight: 22
                        onClicked: {
                            checked = !checked
                            root.refreshHighlight()
                        }
                    }
                    ToolButton {
                        id: wordBox
                        label: "W"
                        tip: "全字匹配"
                        checked: false
                        implicitHeight: 22
                        onClicked: {
                            checked = !checked
                            root.refreshHighlight()
                        }
                    }
                    ToolButton {
                        id: regexBox
                        label: ".*"
                        tip: "正则表达式"
                        checked: false
                        implicitHeight: 22
                        onClicked: {
                            checked = !checked
                            root.refreshHighlight()
                        }
                    }
                }
            }

            RowLayout {
                Layout.row: 0; Layout.column: 2
                spacing: 2

                ToolButton {
                    label: root.replaceVisible ? "收起替换" : "替换"
                    tip: "显示 / 隐藏替换行"
                    onClicked: {
                        root.replaceVisible = !root.replaceVisible
                        if (root.replaceVisible)
                            root.focusField(replaceField)
                    }
                }

                ToolButton {
                    provider: icons; kind: "close"; tip: "关闭"; shortcut: "Esc"
                    onClicked: root.close()
                }
            }

            /* ---------------- 替换行 ---------------- */
            Item {
                Layout.row: 1; Layout.column: 0
                Layout.preferredWidth: 14
                visible: root.replaceVisible
            }

            Rectangle {
                id: replaceBox

                Layout.row: 1; Layout.column: 1
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                visible: root.replaceVisible
                radius: 6
                color: root.fieldBg
                border.color: root.borderColor

                TextField {
                    id: replaceField

                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    placeholderText: "替换为"
                    placeholderTextColor: root.mutedColor
                    color: root.textColor
                    font.pixelSize: 12
                    background: Item {}
                    verticalAlignment: TextInput.AlignVCenter
                    selectByMouse: true

                    Keys.onReturnPressed: (event) => { root.doReplace(); event.accepted = true }
                    Keys.onEnterPressed: (event) => { root.doReplace(); event.accepted = true }
                    Keys.onEscapePressed: (event) => { root.close(); event.accepted = true }
                }
            }

            RowLayout {
                Layout.row: 1; Layout.column: 2
                visible: root.replaceVisible
                spacing: 2

                ToolButton {
                    label: "替换"
                    tip: "替换当前匹配"
                    onClicked: root.doReplace()
                }
                ToolButton {
                    label: "全部替换"
                    tip: "替换全部匹配"
                    onClicked: root.doReplaceAll()
                }
            }
        }
    }
}
