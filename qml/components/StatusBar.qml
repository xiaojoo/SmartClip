pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

/*
 * 底部状态栏。
 *
 * 左边是"这是什么"，右边是"现在什么状态" —— 和主流编辑器一样：
 * 行列号、选中长度、字符数、行数、语法语言、编码、换行符、缩放比例。
 * 这些值全部来自 EditorView（原生 QScintilla），QML 只是显示。
 */
Rectangle {
    id: root

    implicitHeight: 26
    color: "#313335"

    /* 剪贴板条目数 */
    property int count: 0

    /* 编辑器状态（Main.qml 把 EditorView 传进来） */
    property var view: null

    readonly property bool hasDoc: root.view !== null && root.view !== undefined
                                   && root.view.hasDocument

    readonly property color mutedColor: "#77808c"
    readonly property color textColor:  "#7d7d7d"
    readonly property color accentColor: "#4c96d8"

    IconProvider { id: icons }

    component Divider: Rectangle {
        Layout.preferredWidth: 1
        Layout.preferredHeight: 12
        Layout.alignment: Qt.AlignVCenter
        color: "#4b4d4f"
    }

    component Info: Label {
        color: root.textColor
        font.pixelSize: 11
        Layout.alignment: Qt.AlignVCenter
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 10
        anchors.rightMargin: 10
        spacing: 10

        /* ---- 左：应用 / 当前文件 ---- */
        Text {
            text: "SmartClip"
            color: root.mutedColor
            font.pixelSize: 11
            Layout.alignment: Qt.AlignVCenter
        }

        Divider {}

        Label {
            text: root.hasDoc ? root.view.displayName : "就绪"
            color: root.hasDoc ? "#bbbbbb" : root.textColor
            font.pixelSize: 11
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideRight
            Layout.maximumWidth: 320
        }

        Label {
            visible: root.hasDoc && root.view.modified
            text: "● 未保存"
            color: root.accentColor
            font.pixelSize: 11
            Layout.alignment: Qt.AlignVCenter
        }

        Label {
            visible: root.hasDoc && root.view.filePath !== ""
            text: root.hasDoc ? root.view.filePath : ""
            color: root.mutedColor
            font.pixelSize: 11
            Layout.alignment: Qt.AlignVCenter
            elide: Text.ElideMiddle
            Layout.fillWidth: true
            Layout.maximumWidth: 520
        }

        Item { Layout.fillWidth: true }

        /* ---- 右：编辑器状态 ---- */
        Info {
            visible: root.hasDoc
            text: "行 " + (root.hasDoc ? root.view.cursorLine : 1)
                  + "，列 " + (root.hasDoc ? root.view.cursorColumn : 1)
        }

        Divider { visible: root.hasDoc }

        Info {
            visible: root.hasDoc && root.view.selectionLength > 0
            text: "选中 " + (root.hasDoc ? root.view.selectionLength : 0)
        }

        Info {
            visible: root.hasDoc
            text: (root.hasDoc ? root.view.charCount : 0) + " 字符"
        }

        Info {
            visible: root.hasDoc
            text: (root.hasDoc ? root.view.lineCount : 0) + " 行"
        }

        Divider { visible: root.hasDoc }

        Info {
            visible: root.hasDoc
            text: root.hasDoc ? root.view.languageLabel(root.view.language) : ""
            color: "#9aa0a8"
        }

        Info {
            visible: root.hasDoc
            text: root.hasDoc ? root.view.encoding : ""
        }

        Info {
            visible: root.hasDoc
            text: root.hasDoc ? root.view.eolMode : ""
        }

        Info {
            visible: root.hasDoc
            text: (root.hasDoc ? root.view.zoomPercent : 100) + "%"
        }

        Divider {}

        Info { text: root.count + " 项剪贴板" }
    }
}
