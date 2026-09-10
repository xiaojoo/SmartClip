pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../js/TimeUtils.js" as Time
import "../utils"

Rectangle {
    id: root
    color: "#1e1f22"
    radius: 10
    clip: true
    border.width: 0

    property var item: null
    property bool showWelcome: true

    readonly property color barBg:       "#1e1f22"
    readonly property color editorBg:    "#1e1f22"
    readonly property color borderColor: "#4b4d4f"
    readonly property color tabBg:       "#45484c"
    readonly property color textBright:  "#e8e8e8"
    readonly property color textMain:    "#bbbbbb"
    readonly property color textMuted:   "#7d7d7d"
    readonly property color hintKey:     "#8b929e"
    readonly property color accentColor: "#4c96d8"
    readonly property color codeColor:   "#a9b7c6"
    readonly property color imageColor:  "#d7a85b"
    readonly property color lineNumberColor: "#606366"

    IconProvider { id: icons }

    function tabTitle() { return item ? item.title : "README.md" }

    // 正文内容（content 优先，兼容 text 字段）
    function editorText() {
        if (!root.item)
            return ""

        if (root.item.content !== undefined && root.item.content !== null)
            return String(root.item.content)

        if (root.item.text !== undefined && root.item.text !== null)
            return String(root.item.text)

        return ""
    }

    // 图片地址：剪贴板里可能存的是整段文本（甚至源码），
    // 直接拼成 file:// URL 会让 QQuickImage 拿一大段文本去加载并报错
    function imageSource() {
        if (!root.item || root.item.type !== "image")
            return ""

        var raw = root.item.content
        if (raw === undefined || raw === null)
            return ""

        var path = String(raw).replace(/\\/g, "/")

        if (path === "" || path.indexOf("\n") !== -1 || path.length > 512)
            return ""

        if (path.indexOf("file:") === 0)
            return path

        return "file:///" + path
    }

    // 行号文本：与正文同一个字体，行高自然一致
    readonly property string lineNumbers: {
        var content = textArea.text
        var count = content.length > 0 ? content.split("\n").length : 1
        var lines = []
        for (var i = 1; i <= count; ++i)
            lines.push(String(i))
        return lines.join("\n")
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // 顶部标签栏（加上顶部圆角）
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 35
            color: root.barBg
            topLeftRadius: 10
            topRightRadius: 10
            visible: !root.showWelcome

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 4
                anchors.rightMargin: 6
                spacing: 4

                Rectangle {
                    Layout.preferredWidth: 240
                    Layout.preferredHeight: 28
                    radius: 4
                    color: root.tabBg

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 8
                        spacing: 8

                        AppIcon {
                            provider: icons
                            kind: item && item.type === "image" ? "image" : "file"
                            tint: item && item.type === "image" ? root.imageColor : root.accentColor
                            size: 12
                        }
                        Label {
                            text: root.tabTitle()
                            color: root.textBright
                            font.pixelSize: 12
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        AppIcon { provider: icons; kind: "close"; tint: root.textMuted; size: 11 }
                    }
                    MouseArea { anchors.fill: parent; hoverEnabled: true }
                }

                // Rectangle {
                //     Layout.preferredWidth: 30
                //     Layout.preferredHeight: 28
                //     radius: 4
                //     color: "transparent"
                //     AppIcon { anchors.centerIn: parent; provider: icons; kind: "plus"; tint: root.textMuted; size: 15 }
                //     MouseArea { anchors.fill: parent; hoverEnabled: true }
                // }

                Item { Layout.fillWidth: true }
                AppIcon { provider: icons; kind: "grid"; tint: root.accentColor; size: 12 }
                AppIcon { provider: icons; kind: "chevron-down"; tint: root.textMuted; size: 11 }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1
                color: Qt.rgba(root.borderColor.r,
                               root.borderColor.g,
                               root.borderColor.b,
                               0.5)
            }
        }

        // 内容区（始终保持底部圆角；如果标签栏隐藏则四个角都是圆角）
        Rectangle {
            id: contentArea
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.editorBg
            clip: true
            radius: 10

            Column {
                visible: root.showWelcome
                anchors.centerIn: parent
                width: 460
                spacing: 18
                Repeater {
                    model: [ { t: "搜索全部内容", k: "Double Shift" }, { t: "刷新剪贴板", k: "F5" },
                             { t: "最近复制", k: "Ctrl+E" }, { t: "导航栏", k: "Alt+Home" } ]
                    delegate: RowLayout {
                        required property var modelData
                        width: parent.width
                        spacing: 12
                        Label { text: modelData.t; color: root.textMuted; font.pixelSize: 13; Layout.fillWidth: true }
                        Label { text: modelData.k; color: root.hintKey; font.pixelSize: 13 }
                    }
                }
                Label { text: "复制任意内容自动采集，点击左侧条目回填剪贴板"; color: root.textMuted; font.pixelSize: 13 }
            }

            Column {
                visible: !root.showWelcome && item && item.type === "image"
                anchors.fill: parent; anchors.margins: 20; spacing: 10
                RowLayout { width: parent.width; spacing: 8
                    AppIcon { provider: icons; kind: "image"; tint: root.imageColor; size: 14 }
                    Label { text: item ? item.title : ""; color: root.textMain; font.pixelSize: 13; font.bold: true; Layout.fillWidth: true }
                }
                Label { text: item ? Time.displayTime(item.createdAt) : ""; color: root.textMuted; font.pixelSize: 12 }
                Rectangle { width: parent.width; height: 1; color: root.borderColor }
                Item { width: parent.width; height: parent.height - 90
                    Image { anchors.fill: parent; source: root.imageSource()
                        fillMode: Image.PreserveAspectFit; horizontalAlignment: Image.AlignHCenter; verticalAlignment: Image.AlignVCenter }
                }
            }

            // -----------------------------------------------------------------
            // 文本编辑器
            //
            // 行号栏与正文放在同一个 Flickable 的 contentItem 里，
            // 天然共用同一个 contentY，滚动完全同步；
            // contentHeight 由正文真实高度决定，ScrollBar 才有真实滚动范围。
            // -----------------------------------------------------------------
            Flickable {
                id: editorFlick
                visible: !root.showWelcome && root.item !== null && root.item !== undefined
                         && root.item.type === "text"
                anchors.fill: parent
                clip: true
                flickableDirection: Flickable.VerticalFlick
                boundsBehavior: Flickable.StopAtBounds

                contentWidth: width
                contentHeight: Math.max(editorRow.height, height)

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AlwaysOn
                    width: 8
                }

                Row {
                    id: editorRow
                    width: editorFlick.width
                    // 短文本至少占满视口，长文本取真实内容高度
                    height: Math.max(textArea.implicitHeight, editorFlick.height)

                    // 行号栏（随内容一起滚动）
                    Item {
                        id: gutter
                        width: 50
                        height: editorRow.height
                        clip: true

                        Text {
                            id: lineNumbersText
                            x: 10
                            y: textArea.topPadding
                            width: gutter.width - 20
                            horizontalAlignment: Text.AlignRight
                            color: root.lineNumberColor
                            font.family: "Consolas"
                            font.pixelSize: 13
                            textFormat: Text.PlainText
                            text: root.lineNumbers
                        }
                    }

                    Rectangle {
                        width: 1
                        height: editorRow.height
                        color: root.borderColor
                        opacity: 0.6
                    }

                    TextArea {
                        id: textArea
                        width: Math.max(0, editorRow.width - gutter.width - 1)
                        height: editorRow.height

                        wrapMode: TextArea.Wrap
                        selectByMouse: true
                        color: "#d6d7da"
                        font.family: "Consolas"
                        font.pixelSize: 13

                        padding: 12
                        rightPadding: 20

                        placeholderText: qsTr("（内容为空）")

                        // 去掉第二层背景，背景由 contentArea 提供
                        background: null

                        text: root.editorText()

                        // 光标移出可视区域时跟随滚动
                        onCursorRectangleChanged: {
                            if (!activeFocus)
                                return

                            var y = textArea.y + cursorRectangle.y
                            var maxY = Math.max(0, editorFlick.contentHeight - editorFlick.height)

                            if (y < editorFlick.contentY)
                                editorFlick.contentY = Math.max(0, y)
                            else if (y + cursorRectangle.height > editorFlick.contentY + editorFlick.height)
                                editorFlick.contentY = Math.min(maxY, y + cursorRectangle.height - editorFlick.height)
                        }
                    }
                }
            }
        }
    }
}
