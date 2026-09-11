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
                         */
                        Menu {
                            id: editorContextMenu

                            /*
                             * 必须挂到 Overlay。
                             *
                             * 如果直接挂到 TextArea / Flickable，
                             * 菜单会受到父级 clip 影响。
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

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: "Ctrl+Z"

                                        color:
                                            undoItem.enabled
                                            ? root.contextMenuShortcut
                                            : root.contextMenuShortcutDisabled

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        (undoItem.highlighted
                                         && undoItem.enabled)
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

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

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: "Ctrl+Y"

                                        color:
                                            redoItem.enabled
                                            ? root.contextMenuShortcut
                                            : root.contextMenuShortcutDisabled

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        (redoItem.highlighted
                                         && redoItem.enabled)
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            MenuSeparator {
                                contentItem: Rectangle {
                                    implicitWidth:
                                        editorContextMenu.width - 8

                                    implicitHeight: 1

                                    color: root.contextMenuBorder
                                }
                            }

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

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: "Ctrl+X"

                                        color:
                                            cutItem.enabled
                                            ? root.contextMenuShortcut
                                            : root.contextMenuShortcutDisabled

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        (cutItem.highlighted
                                         && cutItem.enabled)
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

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

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: "Ctrl+C"

                                        color:
                                            copyItem.enabled
                                            ? root.contextMenuShortcut
                                            : root.contextMenuShortcutDisabled

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        (copyItem.highlighted
                                         && copyItem.enabled)
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

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

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: "Ctrl+V"

                                        color:
                                            pasteItem.enabled
                                            ? root.contextMenuShortcut
                                            : root.contextMenuShortcutDisabled

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        (pasteItem.highlighted
                                         && pasteItem.enabled)
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

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
                                    if (textArea.selectedText.length > 0)
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

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: "Del"

                                        color:
                                            deleteItem.enabled
                                            ? root.contextMenuShortcut
                                            : root.contextMenuShortcutDisabled

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        (deleteItem.highlighted
                                         && deleteItem.enabled)
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }

                            MenuSeparator {
                                contentItem: Rectangle {
                                    implicitWidth:
                                        editorContextMenu.width - 8

                                    implicitHeight: 1

                                    color: root.contextMenuBorder
                                }
                            }

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

                                        verticalAlignment:
                                            Text.AlignVCenter

                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        text: "Ctrl+A"

                                        color:
                                            selectAllItem.enabled
                                            ? root.contextMenuShortcut
                                            : root.contextMenuShortcutDisabled

                                        verticalAlignment:
                                            Text.AlignVCenter
                                    }
                                }

                                background: Rectangle {
                                    radius: 4

                                    color:
                                        (selectAllItem.highlighted
                                         && selectAllItem.enabled)
                                        ? root.contextMenuHover
                                        : "transparent"
                                }
                            }
                        }

                        /*
                         * =================================================
                         * 右键点击
                         * =================================================
                         *
                         * 这里不再使用：
                         *
                         *     editorContextMenu.popup(root, x, y)
                         *
                         * 因为 Popup.open/popup 会参与一次额外的位置
                         * 计算，之后直接修改 x/y 在 Qt 6.11 下并不稳定。
                         *
                         * 改为：
                         *
                         *     1. 计算鼠标相对于 Overlay 的坐标
                         *     2. 保存为 menuOpenX/menuOpenY
                         *     3. open()
                         *     4. Menu 完成布局后再进行边界修正
                         */
                        MouseArea {
                            id: rightClickArea

                            anchors.fill: parent

                            acceptedButtons:
                                Qt.RightButton

                            cursorShape:
                                Qt.IBeamCursor

                            property real menuOpenX: 0
                            property real menuOpenY: 0

                            onPressed: function(mouse) {
                                mouse.accepted = true

                                var overlay =
                                    Overlay.overlay

                                if (!overlay)
                                    return

                                /*
                                 * 鼠标位置：
                                 *
                                 * TextArea
                                 *     ↓
                                 * EditorArea
                                 *     ↓
                                 * Overlay
                                 *
                                 * 直接转换到 Overlay，
                                 * 后面的 Menu 也使用 Overlay 坐标。
                                 */
                                var p =
                                    rightClickArea.mapToItem(
                                        overlay,
                                        mouse.x,
                                        mouse.y
                                    )

                                menuOpenX = p.x
                                menuOpenY = p.y

                                /*
                                 * 先打开。
                                 *
                                 * 此时 Menu 的最终 width / height
                                 * 才能可靠获得。
                                 */
                                editorContextMenu.open()

                                /*
                                 * 等待 Qt 完成 Popup 布局。
                                 */
                                Qt.callLater(function() {
                                    if (!editorContextMenu.visible)
                                        return

                                    if (!overlay)
                                        return

                                    /*
                                     * =================================================
                                     * EditorArea 内容区域
                                     * 转换成 Overlay 坐标
                                     * =================================================
                                     */
                                    var areaTopLeft =
                                        contentArea.mapToItem(
                                            overlay,
                                            0,
                                            0
                                        )

                                    var areaBottomRight =
                                        contentArea.mapToItem(
                                            overlay,
                                            contentArea.width,
                                            contentArea.height
                                        )

                                    /*
                                     * 内容区域边界。
                                     *
                                     * 留 6px 内边距，
                                     * 保持原来的视觉效果。
                                     */
                                    var minX =
                                        areaTopLeft.x + 6

                                    var minY =
                                        areaTopLeft.y + 6

                                    var maxX =
                                        areaBottomRight.x
                                        - editorContextMenu.width
                                        - 6

                                    var maxY =
                                        areaBottomRight.y
                                        - editorContextMenu.height
                                        - 6

                                    /*
                                     * 如果菜单比内容区域还大，
                                     * 不允许出现反向范围。
                                     */
                                    if (maxX < minX)
                                        maxX = minX

                                    if (maxY < minY)
                                        maxY = minY

                                    /*
                                     * =================================================
                                     * 计算最终位置
                                     * =================================================
                                     *
                                     * 优先使用鼠标位置。
                                     *
                                     * 如果右下方放不下，
                                     * 自动向左 / 向上移动。
                                     */
                                    var finalX =
                                        Math.max(
                                            minX,
                                            Math.min(
                                                menuOpenX,
                                                maxX
                                            )
                                        )

                                    var finalY =
                                        Math.max(
                                            minY,
                                            Math.min(
                                                menuOpenY,
                                                maxY
                                            )
                                        )

                                    /*
                                     * 最终直接设置 Overlay 坐标。
                                     */
                                    editorContextMenu.x =
                                        finalX

                                    editorContextMenu.y =
                                        finalY
                                })
                            }
                        }
                    }
                }
            }
        }
    }
}