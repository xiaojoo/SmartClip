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

    /*
     * tab 栏和顶上那条横向滚动条的几何（自检读它，见 src/SelfTest.cpp）。
     *
     * 要钉的是"横条有没有压到容器圆角上"：
     *   scrollLeft 和 width - scrollRight 都不能小于容器圆角半径，
     *   scrollBottom 要在标签上沿（stripTop）之上 —— 横条不能盖住标签。
     */
    function tabBarState() {
        return {
            height: tabBar.height,
            width: tabBar.width,
            cornerRadius: tabBar.cornerRadius,
            stripTop: tabScroll.mapToItem(tabBar, 0, 0).y,
            stripHeight: tabScroll.height,
            scrollShown: tabBar.scrollShown,
            /* 自检诊断用：横条自己算出来的比例 / Flickable 的内容宽 */
            scrollSize: tabScrollBar.size,
            scrollPosition: tabScrollBar.position,
            scrollVisible: tabScrollBar.visible,
            flickWidth: tabScroll.width,
            flickContent: tabScroll.contentWidth,
            scrollLeft: tabScrollBar.x,
            scrollRight: tabScrollBar.x + tabScrollBar.width,
            scrollTop: tabScrollBar.y,
            scrollBottom: tabScrollBar.y + tabScrollBar.height,
            tabs: root.view.documents.length
        }
    }

    /*
     * 正文卡片（contentArea）和里面那个原生编辑器的几何（自检读它）。
     *
     * 要钉的是这两条约束，它们是一对：
     *   * 编辑器底边离卡片底边只有一点点（cardBottomInset）—— 横向滚动条
     *     跟着编辑器走，留一个圆角的空档就会在横条下面空出一条；
     *   * 左右各让开**至少一个卡片圆角半径**（见 editorView 的 cardInset）——
     *     编辑器是原生子控件、矩形角是直角，让开这么多，卡片左下 / 右下那两段
     *     圆弧才整个落在它矩形之外，两角的圆角才保得住。
     */
    function editorCardState() {
        return {
            radius: contentArea.radius,
            visible: editorView.visible,
            cardWidth: contentArea.width,
            cardHeight: contentArea.height,
            viewX: editorView.x,
            viewY: editorView.y,
            viewWidth: editorView.width,
            viewHeight: editorView.height,
            viewBottom: editorView.y + editorView.height,
            viewRight: editorView.x + editorView.width,
            bottomGap: contentArea.height - (editorView.y + editorView.height),
            leftGap: editorView.x,
            rightGap: contentArea.width - (editorView.x + editorView.width)
        }
    }

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

            /*
             * 标签栏两个上角的圆角。
             *
             * 这个值同时是**顶部那条横向滚动条左右要让开的量**（见下面
             * ScrollBar 的 left / rightMargin）：横条伸进这段圆角里，
             * 就会把那两刀圆角啃成直角。两边用同一个常量，改一处就对齐。
             */
            readonly property real cornerRadius: 10

            /*
             * tab 撑满容器了吗 —— 也就是顶上那条横向滚动条要不要出现。
             *
             * 直接问滚动条自己（ThinScrollBar 里就是 visible: size < 1.0），
             * 不在这里另算一遍"总宽 > 容器宽"：同一件事算两份迟早对不上。
             */
            readonly property bool scrollShown: tabScrollBar.visible

            Layout.fillWidth: true
            /*
             * 高度始终是 35px，**滚动条出现时也不长高**。
             *
             * 那条横条是浮在标签栏最顶上 3px 里的（正好落在标签原有的 3px
             * 上边距那条缝里，谁也不碰）：不占高度，标签和下面编辑区都不会
             * 因为标签撑满了而挪一下。
             */
            Layout.preferredHeight: 35
            visible: root.hasTabs
            color: root.barBg

            /*
             * 顶部两个圆角只能由标签栏自己画。
             *
             * Qt Quick 的 clip 只按矩形裁剪、radius 不参与裁剪，
             * 所以 root 的 radius: 10 挡不住这个铺满顶部的直角矩形。
             */
            topLeftRadius: cornerRadius
            topRightRadius: cornerRadius

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

                    /*
                     * 横向滚动条：浮在标签栏最顶上，**不占高度**。
                     *
                     * 落点正好是标签原来那 3px 上边距：横条 3px 高、贴着容器
                     * 顶边，标签还是从 y=3 开始 —— 横条出现 / 消失都不会把
                     * 标签栏撑高，标签和下面的编辑区都不动。
                     *
                     * 为什么这样能挪到顶上：附加属性的自动摆放只认"父项就是
                     * Flickable"的横条 —— Qt 的 layoutHorizontal() 第一行就是
                     * `if (horizontal->parentItem() != flickable) return;`。
                     * 这里给它另指一个 parent（标签栏本身），自动摆放就此关掉、
                     * 位置归下面的 anchors；而 size / position / active / 拖动时
                     * 回写 contentX 都是另外接的信号，照样生效。
                     *
                     * 左右各让开容器圆角那么多：横条就压在圆角那一刀的高度上，
                     * 不让开就会把圆角啃成直角。
                     */
                    ScrollBar.horizontal: ThinScrollBar {
                        id: tabScrollBar

                        parent: tabBar
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.leftMargin: tabBar.cornerRadius
                        anchors.rightMargin: tabBar.cornerRadius
                        anchors.topMargin: 0
                        height: 3
                        /*
                         * 这个 0 不能省。
                         *
                         * Fusion 样式的 ScrollBar 自带 padding: 2（见
                         * .../qml/QtQuick/Controls/Fusion/ScrollBar.qml），
                         * 横条的滑块高 = 高度 - 上下 padding —— 3px 的条会被
                         * 它挤成负数（贴成一条看不见的线）。ThinScrollBar 的
                         * 滑块是自己画的一个矩形，不需要样式那圈内边距。
                         */
                        padding: 0
                    }

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

                                /*
                                 * 这一层必须压在下面的 tabHit 之上（z: 1）。
                                 *
                                 * tabHit 是"整条标签"的点击区，声明在 RowLayout 后面 ——
                                 * QML 里后声明的兄弟盖在上面，于是按下和悬停都先到
                                 * tabHit：**小叉那个 MouseArea 永远收不到点击**，
                                 * 左键点叉只会把标签切到前台，标签关不掉（实测就是这个
                                 * 毛病：连点两下叉，两个标签都还在）。
                                 *
                                 * z 只能加在这个 RowLayout 上，加在叉自己那个 MouseArea
                                 * 上没用：z 只在同一父项的子项之间比，比的始终是 tabItem
                                 * 的两个子项（RowLayout 和 tabHit）谁在上面。
                                 *
                                 * 抬上来只改"按下"落在谁身上 —— 叉没写 acceptedButtons
                                 * （默认只有左键），右键 / 中键照样穿到 tabHit 去弹菜单、
                                 * 关标签；hover 也仍然是 tabHit 收（标签的 hover 态就是
                                 * 它的 containsMouse）。
                                 */
                                RowLayout {
                                    z: 1
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

                /*
                 * 原来这里还有三个工具栏按钮（新建 / 关闭当前 / 关闭全部），
                 * 已经取消 —— 三条命令在"文件"菜单里都有，快捷键也还是
                 * Ctrl+N / Ctrl+W，标签右键菜单里还有关闭那一组。
                 * 标签栏因此整条留给标签本身，不再被按钮挤掉一截。
                 */
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
                 * 和卡片边缘留出内边距。
                 *
                 * 编辑器是原生子控件，它自己的矩形角是直角：贴着卡片的角就会把
                 * contentArea（radius: 10）画出来的圆角盖成直角。四边留一点就行，
                 * 留多少只看**那边有没有滚动条** —— 滚动条是贴在编辑器边缘上的：
                 *
                 *   右边 / 底边各 2px：竖向、横向滚动条就在右边缘 / 下边缘上，
                 *  让开一个圆角（10px）等于让滚动条离卡片边一个圆角，白空一条。
                 *  2px 既保证方角压不到卡片的圆角上（两边底色本来就是同一个，
                 *  见下面 paperColor: root.editorBg），又给坐标取整留了余量
                 *  （EditorViewItem::applyGeometry 里是 qRound）。
                 *  实测（截图逐像素比对）：2px 和原来 10px 画出来的圆角一模一样。
                 *
                 *   左边 10px：正文别贴着卡片左边缘，行号栏外面留点气。
                 */
                readonly property int cardInset: 10
                readonly property int cardRightInset: 2
                readonly property int cardBottomInset: 2

                anchors.leftMargin: cardInset
                anchors.rightMargin: cardRightInset
                anchors.bottomMargin: cardBottomInset

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
