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

    readonly property color barBg: "#1e1f22"
    readonly property color editorBg: "#1e1f22"
    readonly property color borderColor: "#4b4d4f"
    readonly property color tabBg: "#45484c"
    readonly property color textBright: "#e8e8e8"
    readonly property color textMain: "#bbbbbb"
    readonly property color textMuted: "#7d7d7d"
    readonly property color hintKey: "#8b929e"
    readonly property color accentColor: "#4c96d8"
    readonly property color codeColor: "#a9b7c6"
    readonly property color imageColor: "#d7a85b"
    readonly property color lineNumberColor: "#606366"

    readonly property int editorFontSize: 13
    readonly property int editorPadding: 12
    readonly property int gutterWidth: 50

    readonly property color selectionBg: "#3d78b8"
    readonly property color selectionText: "#ffffff"

    readonly property color contextMenuBg: "#2b2d30"
    readonly property color contextMenuBorder: "#45484c"
    readonly property color contextMenuHover: "#3d78b8"
    readonly property color contextMenuText: "#e6e7e9"
    readonly property color contextMenuDisabled: "#686b70"
    readonly property color contextMenuShortcut: "#969ba3"
    readonly property color contextMenuShortcutDisabled: "#55585d"

    IconProvider {
        id: icons
    }

    function tabTitle() {
        return item ? item.title : "README.md"
    }

    function editorText() {
        if (!root.item)
            return ""

        if (root.item.content !== undefined
                && root.item.content !== null) {
            return String(root.item.content)
        }

        if (root.item.text !== undefined
                && root.item.text !== null) {
            return String(root.item.text)
        }

        return ""
    }

    function imageSource() {
        if (!root.item || root.item.type !== "image")
            return ""

        var raw = root.item.content

        if (raw === undefined || raw === null)
            return ""

        var path = String(raw).replace(/\\/g, "/")

        if (path === ""
                || path.indexOf("\n") !== -1
                || path.length > 512) {
            return ""
        }

        if (path.indexOf("file:") === 0)
            return path

        return "file:///" + path
    }

    /*
     * 每一行的实际起始字符位置。
     *
     * 不能单纯使用固定 lineHeight，
     * 因为 TextArea 开启 Wrap 后，
     * 一行代码可能占两行甚至更多行。
     */
    readonly property var lineStartPositions: {
        var content = textArea.text
        var result = [0]

        for (var i = 0; i < content.length; ++i) {
            if (content.charAt(i) === "\n")
                result.push(i + 1)
        }

        return result
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        /*
         * 顶部 Tab
         */
        Rectangle {
            id: tabBar

            Layout.fillWidth: true
            Layout.preferredHeight: 35

            visible: !root.showWelcome

            color: root.barBg

            RowLayout {
                anchors.fill: parent

                anchors.leftMargin: 8
                anchors.rightMargin: 8

                spacing: 6

                Rectangle {
                    Layout.preferredWidth: 240
                    Layout.preferredHeight: 28

                    color: root.tabBg
                    radius: 5

                    RowLayout {
                        anchors.fill: parent

                        anchors.leftMargin: 10
                        anchors.rightMargin: 6

                        spacing: 8

                        AppIcon {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16

                            provider: icons

                            kind: root.item
                                  && root.item.type === "image"
                                  ? "image"
                                  : "file"

                            tint: root.item
                                  && root.item.type === "image"
                                  ? root.imageColor
                                  : root.accentColor

                            size: 16

                            Layout.alignment: Qt.AlignVCenter
                        }

                        Text {
                            Layout.fillWidth: true

                            text: root.tabTitle()

                            color: root.textBright

                            font.pixelSize: 12

                            elide: Text.ElideRight

                            verticalAlignment:
                                Text.AlignVCenter
                        }

                        AppIcon {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16

                            provider: icons
                            kind: "close"
                            tint: root.textMuted
                            size: 16

                            opacity: 0.8

                            MouseArea {
                                anchors.fill: parent

                                cursorShape:
                                    Qt.PointingHandCursor

                                onClicked: {
                                    root.item = null
                                    root.showWelcome = true
                                }
                            }
                        }
                    }
                }

                Item {
                    Layout.fillWidth: true
                }

                AppIcon {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18

                    provider: icons
                    kind: "grid"
                    tint: root.textMuted
                    size: 18

                    opacity: 0.75
                }

                AppIcon {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18

                    provider: icons
                    kind: "chevronDown"
                    tint: root.textMuted
                    size: 18

                    opacity: 0.75
                }
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom

                height: 1

                color: root.borderColor
                opacity: 0.65
            }
        }

        /*
         * 主内容区域
         */
        Rectangle {
            id: contentArea

            Layout.fillWidth: true
            Layout.fillHeight: true

            color: root.editorBg

            clip: true

            radius: 10

            /*
             * 欢迎页
             */
            Column {
                id: welcomePanel

                visible: root.showWelcome

                width: 460

                anchors.centerIn: parent

                spacing: 18

                Repeater {
                    model: [
                        {
                            title: "搜索全部内容",
                            shortcut: "Double Shift"
                        },
                        {
                            title: "刷新剪贴板",
                            shortcut: "F5"
                        },
                        {
                            title: "最近复制",
                            shortcut: "Ctrl+E"
                        },
                        {
                            title: "导航栏",
                            shortcut: "Alt+Home"
                        }
                    ]

                    delegate: Row {
                        required property var modelData

                        width: parent.width
                        height: 28

                        spacing: 12

                        Text {
                            width: 180

                            text: modelData.title

                            color: root.textMain

                            font.pixelSize: 13

                            verticalAlignment:
                                Text.AlignVCenter
                        }

                        Text {
                            text: modelData.shortcut

                            color: root.hintKey

                            font.pixelSize: 12

                            verticalAlignment:
                                Text.AlignVCenter
                        }
                    }
                }

                Text {
                    width: parent.width

                    text: "复制任意内容自动采集，点击左侧条目回填剪贴板"

                    color: root.textMuted

                    font.pixelSize: 12

                    horizontalAlignment:
                        Text.AlignHCenter

                    verticalAlignment:
                        Text.AlignVCenter

                    wrapMode:
                        Text.WordWrap
                }
            }

            /*
             * 图片预览
             */
            Rectangle {
                id: imagePanel

                visible:
                    root.item !== null
                    && root.item !== undefined
                    && root.item.type === "image"

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
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20

                            provider: icons
                            kind: "image"
                            tint: root.imageColor
                            size: 20
                        }

                        Text {
                            Layout.fillWidth: true

                            text: root.tabTitle()

                            color: root.textBright

                            font.pixelSize: 14
                            font.bold: true

                            elide: Text.ElideRight

                            verticalAlignment:
                                Text.AlignVCenter
                        }

                        Text {
                            text:
                                root.item
                                && root.item.timestamp
                                ? Time.formatTime(
                                      root.item.timestamp
                                  )
                                : ""

                            color: root.textMuted

                            font.pixelSize: 11

                            verticalAlignment:
                                Text.AlignVCenter
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

                            fillMode:
                                Image.PreserveAspectFit

                            asynchronous: true
                            cache: true
                            smooth: true

                            visible: source !== ""
                        }

                        Column {
                            anchors.centerIn: parent

                            spacing: 10

                            visible:
                                previewImage.source === ""

                            AppIcon {
                                anchors.horizontalCenter:
                                    parent.horizontalCenter

                                width: 32
                                height: 32

                                provider: icons
                                kind: "image"
                                tint: root.imageColor
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

            /*
             * 文本编辑器
             */
            Flickable {
                id: editorFlick

                visible:
                    !root.showWelcome
                    && root.item !== null
                    && root.item !== undefined
                    && root.item.type === "text"

                anchors.fill: parent

                clip: true

                flickableDirection:
                    Flickable.VerticalFlick

                boundsBehavior:
                    Flickable.StopAtBounds

                contentWidth: width

                contentHeight:
                    Math.max(
                        editorRow.height,
                        height
                    )

                ScrollBar.vertical: ScrollBar {
                    id: editorScrollBar

                    /*
                     * 只有真正超出视口时才显示。
                     *
                     * 不使用 AlwaysOn，
                     * 避免页面切换时出现闪一下的滚动条。
                     */
                    policy: ScrollBar.AsNeeded

                    width: 8

                    interactive: true

                    visible:
                        editorFlick.contentHeight
                        > editorFlick.height + 1

                    opacity:
                        visible ? 1.0 : 0.0

                    Behavior on opacity {
                        NumberAnimation {
                            duration: 80
                        }
                    }
                }

                Row {
                    id: editorRow

                    width: editorFlick.width

                    height:
                        Math.max(
                            textArea.implicitHeight,
                            editorFlick.height
                        )

                    /*
                     * 行号栏
                     */
                    Rectangle {
                        id: gutter

                        width: root.gutterWidth

                        height: editorRow.height

                        color: root.editorBg

                        clip: true

                        Repeater {
                            model: root.lineStartPositions

                            delegate: Item {
                                id: lineNumberDelegate

                                required property int index
                                required property int modelData

                                width: gutter.width

                                /*
                                 * 使用 TextArea 的实际字符坐标，
                                 * 而不是固定 lineHeight。
                                 *
                                 * 这样换行后的代码也能保持行号
                                 * 与第一行文字顶部严格对应。
                                 */
                                property rect lineRect: {
                                    if (!textArea.text) {
                                        return Qt.rect(
                                            0,
                                            root.editorPadding,
                                            textArea.width,
                                            textArea.font.pixelSize
                                        )
                                    }

                                    return textArea.positionToRectangle(
                                        modelData
                                    )
                                }

                                y: lineRect.y

                                height:
                                    Math.max(
                                        lineRect.height,
                                        root.editorFontSize
                                    )

                                Text {
                                    anchors.fill: parent

                                    anchors.rightMargin: 10

                                    text:
                                        String(
                                            lineNumberDelegate.index + 1
                                        )

                                    color:
                                        root.lineNumberColor

                                    font.family: "Consolas"

                                    font.pixelSize:
                                        root.editorFontSize

                                    horizontalAlignment:
                                        Text.AlignRight

                                    verticalAlignment:
                                        Text.AlignVCenter
                                }
                            }
                        }
                    }

                    /*
                     * 行号 / 编辑器分隔线
                     */
                    Rectangle {
                        width: 1

                        height: editorRow.height

                        color: root.borderColor

                        opacity: 0.6
                    }

                    /*
                     * 正文
                     */
                    TextArea {
                        id: textArea

                        width:
                            Math.max(
                                0,
                                editorRow.width
                                - gutter.width
                                - 1
                            )

                        height: editorRow.height

                        wrapMode:
                            TextArea.Wrap

                        selectByMouse: true

                        color: "#d6d7da"

                        /*
                         * 选中文字：
                         * 蓝色背景 + 白色文字
                         */
                        selectionColor:
                            root.selectionBg

                        selectedTextColor:
                            root.selectionText

                        font.family: "Consolas"

                        font.pixelSize:
                            root.editorFontSize

                        topPadding:
                            root.editorPadding

                        bottomPadding:
                            root.editorPadding

                        leftPadding:
                            root.editorPadding

                        /*
                         * 给右侧滚动条留空间。
                         */
                        rightPadding: 20

                        placeholderText:
                            qsTr("（内容为空）")

                        background: null

                        text: root.editorText()

                        onTextChanged: {
                            if (root.item) {
                                if (root.item.content !== undefined)
                                    root.item.content = text
                            }
                        }

                        /*
                         * =================================================
                         * 右键菜单
                         * =================================================
                         *
                         * 不再使用 Menu.delegate + 自定义 shortcutText。
                         *
                         * 每个 MenuItem 自己直接绘制快捷键，
                         * 避免 Qt 6 Menu delegate 属性传递问题。
                         */
                        Menu {
                            id: editorContextMenu

                            /*
                             * 挂到 Overlay，
                             * 避免被 TextArea / Flickable clip。
                             */
                            parent: Overlay.overlay

                            popupType: Popup.Item

                            width: 210

                            padding: 4

                            closePolicy:
                                Popup.CloseOnEscape
                                | Popup.CloseOnPressOutside

                            background: Rectangle {
                                color:
                                    root.contextMenuBg

                                radius: 6

                                border.width: 1

                                border.color:
                                    root.contextMenuBorder
                            }

                            /*
                             * 撤销
                             */
                            MenuItem {
                                id: undoItem

                                width:
                                    editorContextMenu.width - 8

                                height: 30

                                text: qsTr("撤销")

                                enabled:
                                    textArea.canUndo

                                padding: 0

                                onTriggered: {
                                    textArea.undo()
                                }

                                contentItem: RowLayout {
                                    anchors.fill: parent

                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10

                                    spacing: 12

                                    Text {
                                        Layout.fillWidth: true

                                        text:
                                            undoItem.text

                                        color:
                                            undoItem.enabled
                                            ? (
                                                undoItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuText
                                              )
                                            : root.contextMenuDisabled

                                        font.pixelSize: 13

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    Text {
                                        text: "Ctrl+Z"

                                        color:
                                            undoItem.enabled
                                            ? (
                                                undoItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuShortcut
                                              )
                                            : root.contextMenuShortcutDisabled

                                        font.pixelSize: 11

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        horizontalAlignment:
                                            Text.AlignRight
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        undoItem.highlighted
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            /*
                             * 重做
                             */
                            MenuItem {
                                id: redoItem

                                width:
                                    editorContextMenu.width - 8

                                height: 30

                                text: qsTr("重做")

                                enabled:
                                    textArea.canRedo

                                padding: 0

                                onTriggered: {
                                    textArea.redo()
                                }

                                contentItem: RowLayout {
                                    anchors.fill: parent

                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10

                                    spacing: 12

                                    Text {
                                        Layout.fillWidth: true

                                        text:
                                            redoItem.text

                                        color:
                                            redoItem.enabled
                                            ? (
                                                redoItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuText
                                              )
                                            : root.contextMenuDisabled

                                        font.pixelSize: 13

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    Text {
                                        text: "Ctrl+Y"

                                        color:
                                            redoItem.enabled
                                            ? (
                                                redoItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuShortcut
                                              )
                                            : root.contextMenuShortcutDisabled

                                        font.pixelSize: 11

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        horizontalAlignment:
                                            Text.AlignRight
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        redoItem.highlighted
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            MenuSeparator {
                                topPadding: 3
                                bottomPadding: 3
                            }

                            /*
                             * 剪切
                             */
                            MenuItem {
                                id: cutItem

                                width:
                                    editorContextMenu.width - 8

                                height: 30

                                text: qsTr("剪切")

                                enabled:
                                    textArea.selectedText.length > 0

                                padding: 0

                                onTriggered: {
                                    textArea.cut()
                                }

                                contentItem: RowLayout {
                                    anchors.fill: parent

                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10

                                    spacing: 12

                                    Text {
                                        Layout.fillWidth: true

                                        text:
                                            cutItem.text

                                        color:
                                            cutItem.enabled
                                            ? (
                                                cutItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuText
                                              )
                                            : root.contextMenuDisabled

                                        font.pixelSize: 13

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    Text {
                                        text: "Ctrl+X"

                                        color:
                                            cutItem.enabled
                                            ? (
                                                cutItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuShortcut
                                              )
                                            : root.contextMenuShortcutDisabled

                                        font.pixelSize: 11

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        horizontalAlignment:
                                            Text.AlignRight
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        cutItem.highlighted
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            /*
                             * 复制
                             */
                            MenuItem {
                                id: copyItem

                                width:
                                    editorContextMenu.width - 8

                                height: 30

                                text: qsTr("复制")

                                enabled:
                                    textArea.selectedText.length > 0

                                padding: 0

                                onTriggered: {
                                    textArea.copy()
                                }

                                contentItem: RowLayout {
                                    anchors.fill: parent

                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10

                                    spacing: 12

                                    Text {
                                        Layout.fillWidth: true

                                        text:
                                            copyItem.text

                                        color:
                                            copyItem.enabled
                                            ? (
                                                copyItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuText
                                              )
                                            : root.contextMenuDisabled

                                        font.pixelSize: 13

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    Text {
                                        text: "Ctrl+C"

                                        color:
                                            copyItem.enabled
                                            ? (
                                                copyItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuShortcut
                                              )
                                            : root.contextMenuShortcutDisabled

                                        font.pixelSize: 11

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        horizontalAlignment:
                                            Text.AlignRight
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        copyItem.highlighted
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            /*
                             * 粘贴
                             */
                            MenuItem {
                                id: pasteItem

                                width:
                                    editorContextMenu.width - 8

                                height: 30

                                text: qsTr("粘贴")

                                enabled:
                                    textArea.canPaste

                                padding: 0

                                onTriggered: {
                                    textArea.paste()
                                }

                                contentItem: RowLayout {
                                    anchors.fill: parent

                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10

                                    spacing: 12

                                    Text {
                                        Layout.fillWidth: true

                                        text:
                                            pasteItem.text

                                        color:
                                            pasteItem.enabled
                                            ? (
                                                pasteItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuText
                                              )
                                            : root.contextMenuDisabled

                                        font.pixelSize: 13

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    Text {
                                        text: "Ctrl+V"

                                        color:
                                            pasteItem.enabled
                                            ? (
                                                pasteItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuShortcut
                                              )
                                            : root.contextMenuShortcutDisabled

                                        font.pixelSize: 11

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        horizontalAlignment:
                                            Text.AlignRight
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        pasteItem.highlighted
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            /*
                             * 删除
                             */
                            MenuItem {
                                id: deleteItem

                                width:
                                    editorContextMenu.width - 8

                                height: 30

                                text: qsTr("删除")

                                enabled:
                                    textArea.selectedText.length > 0

                                padding: 0

                                onTriggered: {
                                    textArea.remove(
                                        textArea.selectionStart,
                                        textArea.selectionEnd
                                    )
                                }

                                contentItem: RowLayout {
                                    anchors.fill: parent

                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10

                                    spacing: 12

                                    Text {
                                        Layout.fillWidth: true

                                        text:
                                            deleteItem.text

                                        color:
                                            deleteItem.enabled
                                            ? (
                                                deleteItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuText
                                              )
                                            : root.contextMenuDisabled

                                        font.pixelSize: 13

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    Text {
                                        text: "Delete"

                                        color:
                                            deleteItem.enabled
                                            ? (
                                                deleteItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuShortcut
                                              )
                                            : root.contextMenuShortcutDisabled

                                        font.pixelSize: 11

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        horizontalAlignment:
                                            Text.AlignRight
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        deleteItem.highlighted
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            MenuSeparator {
                                topPadding: 3
                                bottomPadding: 3
                            }

                            /*
                             * 全选
                             */
                            MenuItem {
                                id: selectAllItem

                                width:
                                    editorContextMenu.width - 8

                                height: 30

                                text: qsTr("全选")

                                enabled:
                                    textArea.length > 0

                                padding: 0

                                onTriggered: {
                                    textArea.selectAll()
                                }

                                contentItem: RowLayout {
                                    anchors.fill: parent

                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 10

                                    spacing: 12

                                    Text {
                                        Layout.fillWidth: true

                                        text:
                                            selectAllItem.text

                                        color:
                                            selectAllItem.enabled
                                            ? (
                                                selectAllItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuText
                                              )
                                            : root.contextMenuDisabled

                                        font.pixelSize: 13

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }

                                    Text {
                                        text: "Ctrl+A"

                                        color:
                                            selectAllItem.enabled
                                            ? (
                                                selectAllItem.highlighted
                                                ? "#ffffff"
                                                : root.contextMenuShortcut
                                              )
                                            : root.contextMenuShortcutDisabled

                                        font.pixelSize: 11

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        horizontalAlignment:
                                            Text.AlignRight
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        selectAllItem.highlighted
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }
                        }

                        /*
                         * 右键捕获区域
                         *
                         * 只接收右键，不影响 TextArea
                         * 正常的左键选择、拖动和编辑。
                         */
                        MouseArea {
                            id: contextMouseArea

                            anchors.fill: parent

                            acceptedButtons:
                                Qt.RightButton

                            cursorShape:
                                Qt.IBeamCursor

                            onPressed: function(mouse) {
                                if (mouse.button !== Qt.RightButton)
                                    return

                                /*
                                 * 没有选区时：
                                 * 把光标放到右键位置。
                                 *
                                 * 有选区时：
                                 * 保留当前选区。
                                 */
                                if (textArea.selectedText.length === 0) {
                                    var position =
                                        textArea.positionAt(
                                            mouse.x,
                                            mouse.y
                                        )

                                    textArea.cursorPosition =
                                        position
                                }

                                /*
                                 * TextArea 坐标 -> Overlay 坐标
                                 */
                                var p =
                                    textArea.mapToItem(
                                        Overlay.overlay,
                                        mouse.x,
                                        mouse.y
                                    )

                                editorContextMenu.x =
                                    p.x

                                editorContextMenu.y =
                                    p.y

                                /*
                                 * 打开菜单。
                                 */
                                editorContextMenu.open()

                                /*
                                 * 等 Menu 完成布局后，
                                 * 再检查窗口边界。
                                 */
                                Qt.callLater(function() {
                                    if (!editorContextMenu.visible)
                                        return

                                    var overlay =
                                        Overlay.overlay

                                    if (!overlay)
                                        return

                                    var maxX =
                                        Math.max(
                                            6,
                                            overlay.width
                                            - editorContextMenu.width
                                            - 6
                                        )

                                    var maxY =
                                        Math.max(
                                            6,
                                            overlay.height
                                            - editorContextMenu.height
                                            - 6
                                        )

                                    editorContextMenu.x =
                                        Math.max(
                                            6,
                                            Math.min(
                                                editorContextMenu.x,
                                                maxX
                                            )
                                        )

                                    editorContextMenu.y =
                                        Math.max(
                                            6,
                                            Math.min(
                                                editorContextMenu.y,
                                                maxY
                                            )
                                        )
                                })

                                mouse.accepted = true
                            }

                            onReleased: function(mouse) {
                                mouse.accepted = true
                            }
                        }

                        /*
                         * 光标自动跟随。
                         *
                         * 编辑较长文本时，
                         * 光标进入视口外自动滚动。
                         */
                        onCursorRectangleChanged: {
                            if (!activeFocus)
                                return

                            var y =
                                textArea.y
                                + cursorRectangle.y

                            var maxY =
                                Math.max(
                                    0,
                                    editorFlick.contentHeight
                                    - editorFlick.height
                                )

                            if (
                                y
                                < editorFlick.contentY
                            ) {
                                editorFlick.contentY =
                                    Math.max(
                                        0,
                                        y
                                    )
                            } else if (
                                y
                                + cursorRectangle.height
                                >
                                editorFlick.contentY
                                + editorFlick.height
                            ) {
                                editorFlick.contentY =
                                    Math.min(
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