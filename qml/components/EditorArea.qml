pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../js/TimeUtils.js" as Time
import "../utils"
import SmartClip.Editor 1.0

/*
 * 编辑区：标签栏 + 查找栏 + 正文。
 *
 * 正文是原生 QScintilla（见 src/EditorViewItem.h），所以这里只负责外壳：
 *   * 标签栏 —— 多文档切换 / 关闭 / 新建；
 *   * 查找栏 —— 一条独立的栏（不能做成浮层，见 FindBar.qml 开头）；
 *   * 空状态（欢迎页）和图片预览。
 *
 * 注意 EditorView 必须被"真正隐藏"（visible: false）而不是只被盖住：
 * 它是独立的原生子窗口，只要 show 着就会盖在 QML 任何内容之上。
 * Main.qml / 工具栏拿到的编辑器句柄是 root.view。
 */
Rectangle {
    id: root

    color: "#1e1f22"
    radius: 10
    clip: true
    border.width: 0

    /* 图片条目预览（没有文件、也不是文本的东西走这里） */
    property var previewItem: null

    /*
     * 编辑器本体。
     *
     * Main.qml 通过 root.view 调命令、绑状态；工具栏绑的也是它。
     */
    readonly property alias view: editorView
    readonly property alias findBar: find

    /* 标签栏请求（关闭要先问"要不要保存"，所以交给 Main.qml） */
    signal tabCloseRequested(int index)
    signal tabCloseAllRequested()
    signal newTabRequested()
    signal clipboardRefreshRequested()

    /*
     * tab 上的右键菜单（由 Main.qml 的 openTabMenu 弹出）。
     *
     * menuAnchor 传的是**那个标签本身**（不是整个 tab 栏），(x, y) 是右键那一点
     * 在标签里的本地坐标：菜单要按它们换算成窗口坐标，让左上角紧贴鼠标那一点
     * （见 DropdownMenu.openAtPoint 与 openFor 的坐标口径说明）。
     */
    signal tabContextMenuRequested(int index, var menuAnchor, real x, real y)

    readonly property color barBg: "#1e1f22"
    readonly property color editorBg: "#1e1f22"
    readonly property color borderColor: "#4b4d4f"
    readonly property color tabBg: "#45484c"
    readonly property color tabActiveBg: "#2b2d30"
    readonly property color textBright: "#e8e8e8"
    readonly property color textMain: "#bbbbbb"
    readonly property color textMuted: "#7d7d7d"
    readonly property color hintKey: "#8b929e"
    readonly property color accentColor: "#4c96d8"
    readonly property color imageColor: "#d7a85b"
    readonly property color lineNumberColor: "#606366"
    readonly property color dangerColor: "#e06c75"

    /*
     * 正文默认字号（12）。
     *
     * 写成可写属性而不是常量：启动时会按上次保存的设置覆盖它（Main.qml），
     * "设置"菜单里改字号也改这里 —— EditorView 的 fontPixelSize 一直绑着它，
     * 免得直接给 fontPixelSize 赋值把绑定打断。
     */
    property int editorFontSize: 12
    readonly property bool hasDocument: root.view.hasDocument
    readonly property bool hasTabs: root.hasDocument || root.previewItem !== null

    IconProvider { id: icons }

    function imageSource() {
        if (!root.previewItem || root.previewItem.type !== "image")
            return ""
        var raw = root.previewItem.content
        if (raw === undefined || raw === null)
            return ""
        var path = String(raw).replace(/\\/g, "/")
        if (path === "" || path.indexOf("\n") !== -1 || path.length > 512)
            return ""
        if (path.indexOf("file:") === 0)
            return path
        return "file:///" + path
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        /* ================= 标签栏 ================= */
        Rectangle {
            id: tabBar

            Layout.fillWidth: true
            Layout.preferredHeight: 35
            visible: root.hasTabs
            color: root.barBg

            /*
             * 顶部两个圆角只能由标签栏自己画。
             *
             * Qt Quick 的 clip 只按矩形裁剪、radius 不参与裁剪，
             * 所以 root 的 radius: 10 挡不住这个铺满顶部的直角矩形。
             */
            topLeftRadius: 10
            topRightRadius: 10

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                spacing: 4

                /* ---- 打开的文档（横向可滚动） ---- */
                Flickable {
                    id: tabScroll

                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.topMargin: 3
                    Layout.bottomMargin: 3

                    contentWidth: tabRow.width
                    contentHeight: height
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    interactive: contentWidth > width

                    Row {
                        id: tabRow
                        height: parent.height
                        spacing: 3

                        Repeater {
                            model: root.view.documents

                            delegate: Rectangle {
                                id: tabItem

                                required property var modelData

                                readonly property bool active: modelData.active === true
                                readonly property bool hot: tabHit.containsMouse

                                height: tabRow.height
                                width: Math.max(132, Math.min(250,
                                                              tabLabel.implicitWidth + 74))

                                radius: 5
                                color: active ? root.tabActiveBg
                                              : (hot ? "#3a3d41" : "transparent")

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 9
                                    anchors.rightMargin: 6
                                    spacing: 6

                                    AppIcon {
                                        provider: icons
                                        kind: tabItem.modelData.clipboard ? "paste" : "file"
                                        size: 14
                                        tint: tabItem.active ? root.accentColor : root.textMuted
                                        Layout.alignment: Qt.AlignVCenter
                                    }

                                    Label {
                                        id: tabLabel
                                        Layout.fillWidth: true
                                        text: tabItem.modelData.filePath !== ""
                                              ? tabItem.modelData.title
                                              : tabItem.modelData.title
                                        color: tabItem.active ? root.textBright : root.textMain
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                        verticalAlignment: Text.AlignVCenter
                                    }

                                    /* 未保存：一个点；鼠标移上来变成关闭键 */
                                    Rectangle {
                                        Layout.preferredWidth: 7
                                        Layout.preferredHeight: 7
                                        Layout.alignment: Qt.AlignVCenter
                                        radius: 4
                                        color: root.accentColor
                                        visible: tabItem.modelData.modified === true
                                                 && !tabItem.hot
                                    }

                                    AppIcon {
                                        provider: icons
                                        kind: "close"
                                        size: 13
                                        tint: tabItem.hot ? root.textBright : root.textMuted
                                        Layout.alignment: Qt.AlignVCenter
                                        visible: tabItem.hot
                                                 || tabItem.modelData.modified !== true

                                        MouseArea {
                                            anchors.fill: parent
                                            anchors.margins: -4
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: root.tabCloseRequested(tabItem.modelData.index)
                                        }
                                    }
                                }

                                MouseArea {
                                    id: tabHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    /*
                                     * 右键也收：tab 上的右键菜单（关闭 / 关闭其他 /
                                     * 关闭全部）走下面 onClicked 的 RightButton 分支。
                                     *
                                     * 关掉那个小叉的 MouseArea 不用管：它没写
                                     * acceptedButtons（默认只有左键），右键在它上面
                                     * 不会被吃掉，照样落到这一层来。
                                     */
                                    acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                                     | Qt.RightButton
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: (mouse) => {
                                        if (mouse.button === Qt.MiddleButton)
                                            root.tabCloseRequested(tabItem.modelData.index)
                                        else if (mouse.button === Qt.RightButton)
                                            /*
                                             * 坐标用的是事件里的 mouse.x / mouse.y：
                                             * tabHit 是 anchors.fill 标签的，两者同一套
                                             * 坐标，所以直接把标签当锚点传出去。
                                             */
                                            root.tabContextMenuRequested(
                                                tabItem.modelData.index, tabItem,
                                                mouse.x, mouse.y)
                                        else
                                            root.view.activateDocument(tabItem.modelData.index)
                                    }
                                }
                            }
                        }
                    }
                }

                /* ---- 右侧：新建 / 关闭当前 ---- */
                ToolButton {
                    provider: icons; kind: "new"; tip: "新建文件"; shortcut: "Ctrl+N"
                    onClicked: root.newTabRequested()
                }
                ToolButton {
                    provider: icons; kind: "close"; tip: "关闭当前标签"; shortcut: "Ctrl+W"
                    enabled: root.hasDocument
                    onClicked: root.tabCloseRequested(root.view.currentIndex)
                }
                ToolButton {
                    provider: icons; kind: "trash"; tip: "关闭全部标签"
                    enabled: root.view.documents.length > 0
                    onClicked: root.tabCloseAllRequested()
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

        /* ================= 查找 / 替换栏 ================= */
        FindBar {
            id: find
            Layout.fillWidth: true
            view: root.view
        }

        /* ================= 正文 ================= */
        Rectangle {
            id: contentArea

            Layout.fillWidth: true
            Layout.fillHeight: true
            color: root.editorBg
            clip: true
            radius: 10

            /* ---- 空状态：没有任何标签时显示 ---- */
            Column {
                id: welcomePanel

                visible: !root.hasTabs
                width: 460
                anchors.centerIn: parent
                spacing: 16

                Text {
                    width: parent.width
                    text: "SmartClip 编辑器"
                    color: root.textBright
                    font.pixelSize: 20
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                }

                Repeater {
                    model: [
                        { title: "新建文件", shortcut: "Ctrl+N" },
                        { title: "打开文件", shortcut: "Ctrl+O" },
                        { title: "保存", shortcut: "Ctrl+S" },
                        { title: "查找 / 替换", shortcut: "Ctrl+F / Ctrl+H" },
                        { title: "转到行", shortcut: "Ctrl+G" },
                        { title: "撤销 / 重做", shortcut: "Ctrl+Z / Ctrl+Y" },
                        { title: "刷新剪贴板", shortcut: "F5" }
                    ]

                    delegate: Row {
                        required property var modelData

                        width: parent.width
                        height: 26
                        spacing: 12

                        Text {
                            width: 200
                            text: modelData.title
                            color: root.textMain
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                        }

                        Text {
                            text: modelData.shortcut
                            color: root.hintKey
                            font.pixelSize: 12
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                Text {
                    width: parent.width
                    text: "点左侧列表可把剪贴板内容载入编辑器"
                    color: root.textMuted
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                }
            }

            /* ---- 图片预览 ---- */
            Rectangle {
                id: imagePanel

                visible: root.previewItem !== null
                         && root.previewItem !== undefined
                         && root.previewItem.type === "image"

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
                            provider: icons; kind: "image"; tint: root.imageColor
                            size: 20
                            Layout.preferredWidth: 20
                            Layout.preferredHeight: 20
                        }

                        Text {
                            Layout.fillWidth: true
                            text: root.previewItem ? root.previewItem.title : ""
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                            elide: Text.ElideRight
                            verticalAlignment: Text.AlignVCenter
                        }

                        Text {
                            /*
                             * 用 displayTime，不是 formatTime ——
                             * js/TimeUtils.js 里没有 formatTime 这个函数
                             * （旧代码一直写错，选图片条目时会刷
                             *  "Property 'formatTime' ... is not a function"）。
                             */
                            text: root.previewItem && root.previewItem.createdAt
                                  ? Time.displayTime(root.previewItem.createdAt) : ""
                            color: root.textMuted
                            font.pixelSize: 11
                            verticalAlignment: Text.AlignVCenter
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
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            cache: true
                            smooth: true
                            visible: source !== ""
                        }

                        Column {
                            anchors.centerIn: parent
                            spacing: 10
                            visible: previewImage.source === ""

                            AppIcon {
                                anchors.horizontalCenter: parent.horizontalCenter
                                width: 32
                                height: 32
                                provider: icons; kind: "image"; tint: root.imageColor
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

            /* ---- 正文编辑器（原生 QScintilla） ---- */
            EditorView {
                id: editorView

                anchors.fill: parent

                /*
                 * 和卡片边缘留出内边距，下面两角的圆角交给卡片自己。
                 *
                 * 编辑器是原生子控件，它自己的矩形角是直角，会盖住
                 * contentArea（radius: 10）的圆角。给它自己裁角会把竖向
                 * 滚动条一起裁掉（滑块点不到）；撑到窗口底边又会超出容器
                 * 压住状态栏。留内边距是最稳的。
                 */
                readonly property int cardInset: 10

                anchors.leftMargin: cardInset
                anchors.rightMargin: cardInset
                anchors.bottomMargin: cardInset

                /*
                 * 没有标签时**必须真的隐藏**：原生子窗口不受 QML 的
                 * 层叠影响，只要 show 着就会盖在欢迎页上面。
                 */
                visible: root.view.hasDocument

                paddingLeft: 12
                paddingRight: 12

                fontPixelSize: root.editorFontSize
                textColor: "#d6d7da"
                paperColor: root.editorBg
                gutterColor: root.editorBg
                lineNumberColor: root.lineNumberColor

                /* 切换标签时把"全部高亮"重新刷一遍 */
                onDocumentsChanged: if (find.opened) find.refreshHighlight()
            }
        }
    }
}
