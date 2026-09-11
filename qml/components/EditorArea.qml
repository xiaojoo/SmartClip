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
                    Item {
                        id: editorSurface

                        width:
                            Math.max(
                                0,
                                editorRow.width
                                - gutter.width
                                - 1
                            )

                        height: editorRow.height

                        /*
                         * 正文编辑器。
                         *
                         * 这里特意用 Item 包一层：
                         * 右键拦截层必须是 TextArea 的“兄弟层”，
                         * 而不是 TextArea 的子 MouseArea。
                         *
                         * 这样 Qt Quick Controls 的 TextArea 内部
                         * 默认右键菜单就不会再收到这个右键事件，
                         * 从根源上消除白色默认菜单偶发闪现。
                         */
                        TextArea {
                            id: textArea

                            // 分隔线到正文第一个字符严格保持 10px。
                            x: 10
                            y: 0
                            width: Math.max(0, parent.width - 10)
                            height: parent.height

                            wrapMode:
                                TextArea.Wrap

                            selectByMouse: true

                            // Qt 6.9+：彻底关闭 TextArea 自带的默认右键菜单。
                            // 自绘 Popup 是唯一的右键菜单。
                            ContextMenu.menu: null

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

                            leftPadding: 0

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

                        /*
                         * =====================================================
                         * 右键专用拦截层
                         * =====================================================
                         *
                         * 关键点：
                         *
                         * 1. 它是 TextArea 的兄弟 Item；
                         * 2. z = 100，保证右键先被这里拿到；
                         * 3. acceptedButtons 只有 RightButton，
                         *    所以左键仍然完全交给 TextArea；
                         * 4. 不使用 Qt.callLater；
                         * 5. 菜单在 open() 之前就已经计算好最终位置。
                         *
                         * 因此不会出现：
                         * “先显示白色/初始菜单 -> 再移动到正确位置”的闪现。
                         */
                        MouseArea {
                            id: contextMouseArea

                            anchors.fill: parent

                            z: 100

                            acceptedButtons:
                                Qt.RightButton

                            preventStealing: true

                            propagateComposedEvents: false

                            cursorShape:
                                Qt.IBeamCursor

                            onPressed: function(mouse) {
                                if (mouse.button !== Qt.RightButton)
                                    return

                                mouse.accepted = true

                                /*
                                 * 保持和普通编辑器一致：
                                 * 没有选区时，右键位置成为光标位置；
                                 * 已有选区时，不破坏当前选区。
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

                                textArea.forceActiveFocus()

                                var p =
                                    contextMouseArea.mapToItem(
                                        contentArea,
                                        mouse.x,
                                        mouse.y
                                    )

                                editorSurface.openEditorContextMenu(
                                    p.x,
                                    p.y
                                )
                            }

                            onReleased: function(mouse) {
                                if (mouse.button === Qt.RightButton)
                                    mouse.accepted = true
                            }
                        }

                        /*
                         * =====================================================
                         * 自绘右键菜单
                         * =====================================================
                         *
                         * 这里不用 Menu。
                         *
                         * 原来的 Menu 会经过 Qt Quick Controls 的
                         * Menu/Popup 默认布局和样式流程，在 TextArea
                         * 右键事件与 Popup 打开时序叠加后，可能短暂出现
                         * 默认白色菜单。
                         *
                         * 改成纯 Popup + 手工 Column 后：
                         * - 背景永远是自定义深色；
                         * - 菜单尺寸固定；
                         * - padding 固定为 6px；
                         * - 每个菜单项高度固定 30px；
                         * - 左右内容边距严格 10px；
                         * - 菜单外边距上下左右统一 6px。
                         */
                        Popup {
                            id: editorContextMenu

                            parent: contentArea

                            popupType: Popup.Item

                            width: 210

                            /*
                             * 7 个菜单项 × 30px
                             * + 2 条分隔线 × 1px
                             * + 上下 padding 6px
                             * = 224px。
                             *
                             * 必须在 open() 前就有确定的高度，
                             * 否则第一次右键时可能拿到 height=0，
                             * 导致菜单先出现在错误位置再跳动。
                             */
                            height: 224

                            padding: 6

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

                            contentItem: Column {
                                width:
                                    editorContextMenu.availableWidth

                                spacing: 0

                                MenuItem {
                                    id: undoItem

                                    width: parent.width
                                    height: 30

                                    text: qsTr("撤销")

                                    enabled:
                                        textArea.canUndo

                                    padding: 0

                                    onTriggered: {
                                        textArea.undo()
                                        editorContextMenu.close()
                                    }

                                    contentItem: RowLayout {
                                        anchors.fill: parent

                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10

                                        spacing: 12

                                        Text {
                                            Layout.fillWidth: true

                                            text: undoItem.text

                                            color:
                                                !undoItem.enabled
                                                ? root.contextMenuDisabled
                                                : undoItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuText

                                            font.pixelSize: 13

                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }

                                        Text {
                                            text: "Ctrl+Z"

                                            color:
                                                !undoItem.enabled
                                                ? root.contextMenuShortcutDisabled
                                                : undoItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuShortcut

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
                                            undoItem.enabled
                                            && undoItem.hovered
                                            ? root.contextMenuHover
                                            : "transparent"
                                    }
                                }

                                MenuItem {
                                    id: redoItem

                                    width: parent.width
                                    height: 30

                                    text: qsTr("重做")

                                    enabled:
                                        textArea.canRedo

                                    padding: 0

                                    onTriggered: {
                                        textArea.redo()
                                        editorContextMenu.close()
                                    }

                                    contentItem: RowLayout {
                                        anchors.fill: parent

                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10

                                        spacing: 12

                                        Text {
                                            Layout.fillWidth: true

                                            text: redoItem.text

                                            color:
                                                !redoItem.enabled
                                                ? root.contextMenuDisabled
                                                : redoItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuText

                                            font.pixelSize: 13

                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }

                                        Text {
                                            text: "Ctrl+Y"

                                            color:
                                                !redoItem.enabled
                                                ? root.contextMenuShortcutDisabled
                                                : redoItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuShortcut

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
                                            redoItem.enabled
                                            && redoItem.hovered
                                            ? root.contextMenuHover
                                            : "transparent"
                                    }
                                }

                                Rectangle {
                                    width: parent.width - 20
                                    height: 1

                                    x: 10

                                    color:
                                        root.contextMenuBorder

                                    opacity: 0.75
                                }

                                MenuItem {
                                    id: cutItem

                                    width: parent.width
                                    height: 30

                                    text: qsTr("剪切")

                                    enabled:
                                        textArea.selectedText.length > 0

                                    padding: 0

                                    onTriggered: {
                                        textArea.cut()
                                        editorContextMenu.close()
                                    }

                                    contentItem: RowLayout {
                                        anchors.fill: parent

                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10

                                        spacing: 12

                                        Text {
                                            Layout.fillWidth: true

                                            text: cutItem.text

                                            color:
                                                !cutItem.enabled
                                                ? root.contextMenuDisabled
                                                : cutItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuText

                                            font.pixelSize: 13

                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }

                                        Text {
                                            text: "Ctrl+X"

                                            color:
                                                !cutItem.enabled
                                                ? root.contextMenuShortcutDisabled
                                                : cutItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuShortcut

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
                                            cutItem.enabled
                                            && cutItem.hovered
                                            ? root.contextMenuHover
                                            : "transparent"
                                    }
                                }

                                MenuItem {
                                    id: copyItem

                                    width: parent.width
                                    height: 30

                                    text: qsTr("复制")

                                    enabled:
                                        textArea.selectedText.length > 0

                                    padding: 0

                                    onTriggered: {
                                        textArea.copy()
                                        editorContextMenu.close()
                                    }

                                    contentItem: RowLayout {
                                        anchors.fill: parent

                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10

                                        spacing: 12

                                        Text {
                                            Layout.fillWidth: true

                                            text: copyItem.text

                                            color:
                                                !copyItem.enabled
                                                ? root.contextMenuDisabled
                                                : copyItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuText

                                            font.pixelSize: 13

                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }

                                        Text {
                                            text: "Ctrl+C"

                                            color:
                                                !copyItem.enabled
                                                ? root.contextMenuShortcutDisabled
                                                : copyItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuShortcut

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
                                            copyItem.enabled
                                            && copyItem.hovered
                                            ? root.contextMenuHover
                                            : "transparent"
                                    }
                                }

                                MenuItem {
                                    id: pasteItem

                                    width: parent.width
                                    height: 30

                                    text: qsTr("粘贴")

                                    enabled:
                                        textArea.canPaste

                                    padding: 0

                                    onTriggered: {
                                        textArea.paste()
                                        editorContextMenu.close()
                                    }

                                    contentItem: RowLayout {
                                        anchors.fill: parent

                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10

                                        spacing: 12

                                        Text {
                                            Layout.fillWidth: true

                                            text: pasteItem.text

                                            color:
                                                !pasteItem.enabled
                                                ? root.contextMenuDisabled
                                                : pasteItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuText

                                            font.pixelSize: 13

                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }

                                        Text {
                                            text: "Ctrl+V"

                                            color:
                                                !pasteItem.enabled
                                                ? root.contextMenuShortcutDisabled
                                                : pasteItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuShortcut

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
                                            pasteItem.enabled
                                            && pasteItem.hovered
                                            ? root.contextMenuHover
                                            : "transparent"
                                    }
                                }

                                MenuItem {
                                    id: deleteItem

                                    width: parent.width
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
                                        editorContextMenu.close()
                                    }

                                    contentItem: RowLayout {
                                        anchors.fill: parent

                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10

                                        spacing: 12

                                        Text {
                                            Layout.fillWidth: true

                                            text: deleteItem.text

                                            color:
                                                !deleteItem.enabled
                                                ? root.contextMenuDisabled
                                                : deleteItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuText

                                            font.pixelSize: 13

                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }

                                        Text {
                                            text: "Delete"

                                            color:
                                                !deleteItem.enabled
                                                ? root.contextMenuShortcutDisabled
                                                : deleteItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuShortcut

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
                                            deleteItem.enabled
                                            && deleteItem.hovered
                                            ? root.contextMenuHover
                                            : "transparent"
                                    }
                                }

                                Rectangle {
                                    width: parent.width - 20
                                    height: 1

                                    x: 10

                                    color:
                                        root.contextMenuBorder

                                    opacity: 0.75
                                }

                                MenuItem {
                                    id: selectAllItem

                                    width: parent.width
                                    height: 30

                                    text: qsTr("全选")

                                    enabled:
                                        textArea.length > 0

                                    padding: 0

                                    onTriggered: {
                                        textArea.selectAll()
                                        editorContextMenu.close()
                                    }

                                    contentItem: RowLayout {
                                        anchors.fill: parent

                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 10

                                        spacing: 12

                                        Text {
                                            Layout.fillWidth: true

                                            text: selectAllItem.text

                                            color:
                                                !selectAllItem.enabled
                                                ? root.contextMenuDisabled
                                                : selectAllItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuText

                                            font.pixelSize: 13

                                            verticalAlignment:
                                                Text.AlignVCenter
                                        }

                                        Text {
                                            text: "Ctrl+A"

                                            color:
                                                !selectAllItem.enabled
                                                ? root.contextMenuShortcutDisabled
                                                : selectAllItem.hovered
                                                  ? "#ffffff"
                                                  : root.contextMenuShortcut

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
                                            selectAllItem.enabled
                                            && selectAllItem.hovered
                                            ? root.contextMenuHover
                                            : "transparent"
                                    }
                                }
                            }
                        }

                        /*
                         * 在 Popup 打开前计算最终坐标。
                         * 不延迟，不二次修正，因此不会闪现初始位置。
                         */
                        function openEditorContextMenu(mouseX, mouseY) {
                            /*
                             * 菜单和 contentArea 使用同一个坐标系。
                             *
                             * 这里的 10px 有两个作用：
                             *
                             * 1. 上 / 右 / 下：距离 contentArea 边缘 10px；
                             * 2. 左：距离编辑器中间那根竖线 10px。
                             *
                             * 注意左侧不能直接使用 contentArea.left，
                             * 因为 contentArea 最左边还有 50px 行号栏。
                             */
                            var margin = 10

                            /*
                             * Popup 已经固定为最终尺寸，
                             * 因此这里不会出现第一次打开时 height=0。
                             */
                            var menuWidth = editorContextMenu.width
                            var menuHeight = editorContextMenu.height

                            /*
                             * 竖线位置：
                             * gutter.width + 1px separator。
                             * 菜单再向右留 10px。
                             */
                            var minX =
                                gutter.width
                                + 1
                                + margin

                            var minY = margin

                            /*
                             * 右边和下边同样保留 10px。
                             */
                            var maxX = Math.max(
                                minX,
                                contentArea.width
                                - menuWidth
                                - margin
                            )

                            var maxY = Math.max(
                                minY,
                                contentArea.height
                                - menuHeight
                                - margin
                            )

                            var finalX = Math.max(
                                minX,
                                Math.min(mouseX, maxX)
                            )

                            var finalY = Math.max(
                                minY,
                                Math.min(mouseY, maxY)
                            )

                            /*
                             * 先关闭旧菜单，再设置最终位置，
                             * 最后才 open()。
                             *
                             * 整个过程不使用 Qt.callLater，
                             * 不进行第二次移动，因此不会闪出
                             * 一个“初始位置”的菜单。
                             */
                            editorContextMenu.close()

                            editorContextMenu.x =
                                Math.round(finalX)

                            editorContextMenu.y =
                                Math.round(finalY)

                            editorContextMenu.open()
                        }

                    }
                }
            }
        }
    }
}