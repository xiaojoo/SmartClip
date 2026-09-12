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
 * 支持：区分大小写、全字匹配、正则、全部高亮、逐个替换、全部替换、
 *       匹配计数、Enter / Shift+Enter 前后跳、Esc 关闭。
 */
Rectangle {
    id: root

    implicitHeight: replaceRow.visible ? 68 : 38
    color: "#45484c"

    property var view: null
    property bool replaceVisible: false
    /* 栏是否展开（打开查找 / 替换时置 true，关闭时置 false） */
    property bool opened: false

    signal closed()

    readonly property color borderColor: "#3c3f44"
    readonly property color fieldBg: "#2b2d30"
    readonly property color textColor: "#d6d7da"
    readonly property color mutedColor: "#7d838c"
    readonly property color accent: "#4c96d8"

    /* 匹配计数，例如 "3 / 12"；没查过就是空 */
    property string matchText: ""
    property bool noMatch: false

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

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        anchors.topMargin: 4
        anchors.bottomMargin: 4
        spacing: 4

        /* ---------------- 查找行 ---------------- */
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 30
            spacing: 6

            AppIcon {
                provider: icons; kind: "search"; size: 14
                tint: root.mutedColor
                Layout.alignment: Qt.AlignVCenter
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                radius: 4
                color: root.fieldBg
                border.color: root.noMatch ? "#c75450" : root.borderColor

                TextField {
                    id: field
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
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
            }

            Label {
                Layout.preferredWidth: 84
                text: root.matchText
                color: root.noMatch ? "#e06c75" : root.mutedColor
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
                verticalAlignment: Text.AlignVCenter
            }

            ToolButton {
                provider: icons; kind: "chevron-up"; tip: "上一个"; shortcut: "Shift+F3"
                onClicked: root.findNext(false)
            }
            ToolButton {
                provider: icons; kind: "chevron-down"; tip: "下一个"; shortcut: "F3"
                onClicked: root.findNext(true)
            }

            Rectangle {
                Layout.preferredWidth: 1; Layout.preferredHeight: 18
                Layout.leftMargin: 3; Layout.rightMargin: 3
                color: root.borderColor
            }

            /* 三个开关：区分大小写 / 全字匹配 / 正则 */
            ToolButton {
                id: caseBox
                label: "Aa"
                tip: "区分大小写"
                checked: false
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
                onClicked: {
                    checked = !checked
                    root.refreshHighlight()
                }
            }

            Rectangle {
                Layout.preferredWidth: 1; Layout.preferredHeight: 18
                Layout.leftMargin: 3; Layout.rightMargin: 3
                color: root.borderColor
            }

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
        RowLayout {
            id: replaceRow

            Layout.fillWidth: true
            Layout.preferredHeight: 26
            visible: root.replaceVisible
            spacing: 6

            Item {
                Layout.preferredWidth: 14
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                radius: 4
                color: root.fieldBg
                border.color: root.borderColor

                TextField {
                    id: replaceField
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
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

            Item {
                Layout.preferredWidth: 84
            }

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

            Item {
                Layout.preferredWidth: 1 + 6 + 28 + 6 + 28 + 3 + 3 + 1 + 28
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: root.borderColor
    }
}
