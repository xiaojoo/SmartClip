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

    // -------------------------------------------------------------
    // 编辑器统一字体参数
    // -------------------------------------------------------------
    readonly property int editorFontSize: 13
    readonly property int editorPadding: 12

    // 行号栏宽度统一定义，避免正文和行号区域计算不一致
    readonly property int gutterWidth: 50

    IconProvider {
        id: icons
    }

    // -------------------------------------------------------------
    // Tab 标题
    // -------------------------------------------------------------
    function tabTitle() {
        return item ? item.title : "README.md"
    }

    // -------------------------------------------------------------
    // 正文内容
    // -------------------------------------------------------------
    function editorText() {
        if (!root.item)
            return ""

        if (root.item.content !== undefined && root.item.content !== null)
            return String(root.item.content)

        if (root.item.text !== undefined && root.item.text !== null)
            return String(root.item.text)

        return ""
    }

    // -------------------------------------------------------------
    // 图片地址
    // -------------------------------------------------------------
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

    // =============================================================
    // 行号数据
    //
    // 只保存每个逻辑行的起始字符位置。
    //
    // 实际 Y 坐标 / 高度完全交给 TextArea.positionToRectangle()
    // 决定，避免自己计算 lineHeight。
    // =============================================================
    readonly property var lineStartPositions: {
        var content = textArea.text
        var result = [0]

        for (var i = 0; i < content.length; ++i) {
            if (content.charAt(i) === "\n")
                result.push(i + 1)
        }

        return result
    }

    // =============================================================
    // 内容区域
    // =============================================================
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // -------------------------------------------------------------
        // 顶部标签栏
        // -------------------------------------------------------------
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

                            kind: item && item.type === "image"
                                  ? "image"
                                  : "file"

                            tint: item && item.type === "image"
                                  ? root.imageColor
                                  : root.accentColor

                            size: 12
                        }

                        Label {
                            text: root.tabTitle()

                            color: root.textBright

                            font.pixelSize: 12

                            elide: Text.ElideRight

                            Layout.fillWidth: true
                        }

                        AppIcon {
                            provider: icons

                            kind: "close"

                            tint: root.textMuted

                            size: 11
                        }
                    }

                    MouseArea {
                        anchors.fill: parent

                        hoverEnabled: true
                    }
                }

                Item {
                    Layout.fillWidth: true
                }

                AppIcon {
                    provider: icons

                    kind: "grid"

                    tint: root.accentColor

                    size: 12
                }

                AppIcon {
                    provider: icons

                    kind: "chevron-down"

                    tint: root.textMuted

                    size: 11
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                height: 1

                color: Qt.rgba(
                    root.borderColor.r,
                    root.borderColor.g,
                    root.borderColor.b,
                    0.5
                )
            }
        }

        // =============================================================
        // 内容区
        // =============================================================
        Rectangle {
            id: contentArea

            Layout.fillWidth: true
            Layout.fillHeight: true

            color: root.editorBg

            clip: true

            radius: 10

            // ---------------------------------------------------------
            // Welcome
            // ---------------------------------------------------------
            Column {
                visible: root.showWelcome

                anchors.centerIn: parent

                width: 460

                spacing: 18

                Repeater {
                    model: [
                        { t: "搜索全部内容", k: "Double Shift" },
                        { t: "刷新剪贴板", k: "F5" },
                        { t: "最近复制", k: "Ctrl+E" },
                        { t: "导航栏", k: "Alt+Home" }
                    ]

                    delegate: RowLayout {
                        required property var modelData

                        width: parent.width

                        spacing: 12

                        Label {
                            text: modelData.t

                            color: root.textMuted

                            font.pixelSize: 13

                            Layout.fillWidth: true
                        }

                        Label {
                            text: modelData.k

                            color: root.hintKey

                            font.pixelSize: 13
                        }
                    }
                }

                Label {
                    text: "复制任意内容自动采集，点击左侧条目回填剪贴板"

                    color: root.textMuted

                    font.pixelSize: 13
                }
            }

            // ---------------------------------------------------------
            // 图片内容
            // ---------------------------------------------------------
            Column {
                visible: !root.showWelcome
                         && item
                         && item.type === "image"

                anchors.fill: parent

                anchors.margins: 20

                spacing: 10

                RowLayout {
                    width: parent.width

                    spacing: 8

                    AppIcon {
                        provider: icons

                        kind: "image"

                        tint: root.imageColor

                        size: 14
                    }

                    Label {
                        text: item ? item.title : ""

                        color: root.textMain

                        font.pixelSize: 13
                        font.bold: true

                        Layout.fillWidth: true
                    }
                }

                Label {
                    text: item ? Time.displayTime(item.createdAt) : ""

                    color: root.textMuted

                    font.pixelSize: 12
                }

                Rectangle {
                    width: parent.width

                    height: 1

                    color: root.borderColor
                }

                Item {
                    width: parent.width

                    height: parent.height - 90

                    Image {
                        anchors.fill: parent

                        source: root.imageSource()

                        fillMode: Image.PreserveAspectFit

                        horizontalAlignment: Image.AlignHCenter
                        verticalAlignment: Image.AlignVCenter
                    }
                }
            }

            // =========================================================
            // 文本编辑器
            // =========================================================
            Flickable {
                id: editorFlick

                visible: !root.showWelcome
                         && root.item !== null
                         && root.item !== undefined
                         && root.item.type === "text"

                anchors.fill: parent

                clip: true

                flickableDirection: Flickable.VerticalFlick

                boundsBehavior: Flickable.StopAtBounds

                // -----------------------------------------------------
                // 不允许横向滚动。
                // -----------------------------------------------------
                contentWidth: width

                // -----------------------------------------------------
                // 内容高度使用正文实际排版高度。
                // -----------------------------------------------------
                contentHeight: Math.max(
                    editorRow.height,
                    height
                )

                // =====================================================
                // 垂直滚动条
                // =====================================================
                ScrollBar.vertical: ScrollBar {
                    id: editorScrollBar

                    policy: ScrollBar.AlwaysOn

                    visible: editorFlick.contentHeight
                             > editorFlick.height + 1

                    width: 8

                    interactive: true

                    opacity: visible ? 1.0 : 0.0
                }

                // =====================================================
                // 编辑器内容 Row
                // =====================================================
                Row {
                    id: editorRow

                    width: editorFlick.width

                    height: Math.max(
                        textArea.implicitHeight,
                        editorFlick.height
                    )

                    // =================================================
                    // 行号栏
                    // =================================================
                    Item {
                        id: gutter

                        width: root.gutterWidth

                        height: editorRow.height

                        clip: true

                        Repeater {
                            model: root.lineStartPositions

                            delegate: Item {
                                required property int index
                                required property int modelData

                                width: gutter.width

                                // =================================================
                                // 关键优化：
                                //
                                // 不再使用：
                                //
                                //     FontMetrics.height
                                //
                                // 因为它不一定等于 TextArea 当前实际
                                // 使用的 line box。
                                //
                                // 现在直接使用 TextArea 的真实排版矩形。
                                // =================================================
                                property rect lineRect: {
                                    if (!textArea.text)
                                        return Qt.rect(
                                            0,
                                            root.editorPadding,
                                            textArea.width,
                                            textArea.font.pixelSize
                                        )

                                    return textArea.positionToRectangle(
                                        modelData
                                    )
                                }

                                // -------------------------------------------------
                                // Y 坐标直接跟随正文真实行框
                                // -------------------------------------------------
                                y: lineRect.y

                                // -------------------------------------------------
                                // 高度也直接使用正文真实行框高度
                                //
                                // 这样行号和代码共享同一个 line box。
                                // -------------------------------------------------
                                height: Math.max(
                                    lineRect.height,
                                    root.editorFontSize
                                )

                                Text {
                                    anchors.fill: parent

                                    anchors.rightMargin: 10

                                    horizontalAlignment: Text.AlignRight

                                    // -------------------------------------------------
                                    // 这里使用 Center 而不是自己计算 baseline。
                                    //
                                    // 因为 parent.height 已经来自正文的真实
                                    // positionToRectangle()，因此数字会落在
                                    // 同一个 line box 中。
                                    // -------------------------------------------------
                                    verticalAlignment: Text.AlignVCenter

                                    color: root.lineNumberColor

                                    font.family: "Consolas"

                                    font.pixelSize: root.editorFontSize

                                    text: String(index + 1)

                                    textFormat: Text.PlainText

                                    renderType: Text.NativeRendering

                                    antialiasing: true
                                }
                            }
                        }
                    }

                    // -------------------------------------------------
                    // 行号与正文之间的分隔线
                    // -------------------------------------------------
                    Rectangle {
                        width: 1

                        height: editorRow.height

                        color: root.borderColor

                        opacity: 0.6
                    }

                    // =================================================
                    // 正文
                    // =================================================
                    TextArea {
                        id: textArea

                        width: Math.max(
                            0,
                            editorRow.width
                            - gutter.width
                            - 1
                        )

                        height: editorRow.height

                        wrapMode: TextArea.Wrap

                        selectByMouse: true

                        color: "#d6d7da"

                        font.family: "Consolas"

                        font.pixelSize: root.editorFontSize

                        // -------------------------------------------------
                        // Padding
                        // -------------------------------------------------
                        topPadding: root.editorPadding

                        bottomPadding: root.editorPadding

                        leftPadding: root.editorPadding

                        // 给右侧滚动条留出固定空间。
                        rightPadding: 20

                        placeholderText: qsTr("（内容为空）")

                        background: null

                        text: root.editorText()

                        // -------------------------------------------------
                        // 光标自动跟随
                        // -------------------------------------------------
                        onCursorRectangleChanged: {
                            if (!activeFocus)
                                return

                            var y = textArea.y + cursorRectangle.y

                            var maxY = Math.max(
                                0,
                                editorFlick.contentHeight
                                - editorFlick.height
                            )

                            if (y < editorFlick.contentY) {
                                editorFlick.contentY = Math.max(
                                    0,
                                    y
                                )
                            } else if (
                                y + cursorRectangle.height
                                > editorFlick.contentY
                                  + editorFlick.height
                            ) {
                                editorFlick.contentY = Math.min(
                                    maxY,
                                    y
                                    + cursorRectangle.height
                                    - editorFlick.height
                                )
                            }
                        }
                    }
                }
            }
        }
    }
}