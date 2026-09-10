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
    // 不再把整个行号字符串交给一个 Text。
    //
    // 每一个行号单独创建一个 Text，
    // 它的 Y 坐标直接从 TextArea.positionToRectangle()
    // 获取。
    //
    // 这样行号实际上跟随正文 TextArea 的真实排版结果，
    // 而不是自己猜测 lineHeight。
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
                // 横向不滚动。
                // 正文宽度始终跟随视口。
                // -----------------------------------------------------
                contentWidth: width

                // -----------------------------------------------------
                // 内容高度取正文真实高度。
                //
                // 不再强制 AlwaysOn。
                // -----------------------------------------------------
                contentHeight: Math.max(
                    editorRow.height,
                    height
                )

                // =====================================================
                // 滚动条
                //
                // 重要：
                //
                // 不再使用：
                //
                //     AlwaysOn <-> AsNeeded
                //
                // 因为切换内容时会经历一次旧状态，
                // 容易出现：
                //
                //     长页面
                //       ↓
                //     短页面
                //       ↓
                //     滚动条闪一下
                //       ↓
                //     消失
                //
                // 现在 ScrollBar 始终存在，
                // 但视觉显示完全由是否真正存在滚动范围决定。
                // =====================================================
                ScrollBar.vertical: ScrollBar {
                    id: editorScrollBar

                    // 保持 ScrollBar 自身的 geometry 稳定，
                    // 避免 policy 切换造成闪烁。
                    policy: ScrollBar.AlwaysOn

                    // 真正需要滚动时才显示。
                    visible: editorFlick.contentHeight
                             > editorFlick.height + 1

                    width: 8

                    interactive: true

                    // 避免短页面时参与视觉显示。
                    opacity: visible ? 1.0 : 0.0
                }

                // =====================================================
                // 编辑器内容 Row
                // =====================================================
                Row {
                    id: editorRow

                    width: editorFlick.width

                    // -------------------------------------------------
                    // TextArea 的 implicitHeight 是正文真实排版高度。
                    //
                    // 同时保证短文本至少占满整个视口。
                    // -------------------------------------------------
                    height: Math.max(
                        textArea.implicitHeight,
                        editorFlick.height
                    )

                    // =================================================
                    // 行号栏
                    // =================================================
                    Item {
                        id: gutter

                        width: 50

                        height: editorRow.height

                        clip: true

                        // -------------------------------------------------
                        // 行号 Repeater
                        //
                        // 每个数字单独定位。
                        // -------------------------------------------------
                        Repeater {
                            model: root.lineStartPositions

                            delegate: Item {
                                required property int index
                                required property int modelData

                                width: gutter.width
                                height: textMetrics.lineHeight

                                // -------------------------------------------------
                                // 关键：
                                //
                                // 直接询问 TextArea：
                                //
                                // “这个字符实际排版到了哪里？”
                                //
                                // 所以行号 Y 坐标和正文真正的 baseline /
                                // line box 保持一致。
                                //
                                // 不再使用：
                                //
                                //     y = index * 18
                                //
                                // 也不再使用 Text.lineHeight。
                                // -------------------------------------------------
                                y: {
                                    if (!textArea.text)
                                        return root.editorPadding

                                    var rect = textArea.positionToRectangle(
                                        modelData
                                    )

                                    return rect.y
                                }

                                TextMetrics {
                                    id: textMetrics

                                    font.family: "Consolas"

                                    font.pixelSize: root.editorFontSize

                                    text: "Ag"
                                }

                                Text {
                                    anchors.right: parent.right

                                    anchors.rightMargin: 10

                                    anchors.verticalCenter: parent.verticalCenter

                                    width: parent.width - 20

                                    height: parent.height

                                    horizontalAlignment: Text.AlignRight

                                    verticalAlignment: Text.AlignVCenter

                                    color: root.lineNumberColor

                                    font.family: "Consolas"

                                    font.pixelSize: root.editorFontSize

                                    text: String(index + 1)

                                    textFormat: Text.PlainText

                                    renderType: Text.NativeRendering
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

                        // 给滚动条预留空间。
                        rightPadding: 20

                        placeholderText: qsTr("（内容为空）")

                        // 背景由外层 Rectangle 提供。
                        background: null

                        text: root.editorText()

                        // -------------------------------------------------
                        // 光标移出可视区域时自动跟随滚动
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