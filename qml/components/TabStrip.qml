pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"

/*
 * 一个编辑组的标签栏。
 *
 * 分栏之后界面上有**两条**这样的标签栏（左右 / 上下各一条），每条绑自己的
 * 那一栏（pane = EditorView / mirrorPane）—— 这就是"像 VS Code 那样，两栏
 * 是独立的 tab"：两边各自列自己打开的文件，点谁切谁。
 *
 * 这里只管"画标签 + 把点击转出去"：
 *   * 切标签    -> pane.activateDocument(i)（**只切自己那一栏**，另一栏不动）
 *   * 关闭      -> tabCloseRequested(pane, i)（窗口那一层才知道要不要先问保存）
 *   * 右键      -> tabContextMenuRequested(pane, i, anchor, x, y)
 */
Rectangle {
    id: root

    /* 这一条标签栏属于哪一栏（EditorViewItem） */
    required property var pane

    /* 分栏时两条是并排的，靠 paneActions 决定"最右边那个开关"只画一次 */
    property bool paneActions: true
    /* "源码 / 预览"开关（只有最右边那一条须要） */
    property bool markdownToggleVisible: false
    property bool markdownPreview: false
    /* 预览里的图标资源（和编辑区那套同一份，见 IconProvider.qml） */
    property var iconProvider: null

    /* 标签全撑满时顶上会出现一条 3px 的横向滚动条；两个上角的圆角要让开它 */
    readonly property real cornerRadius: 10
    readonly property bool scrollShown: tabScrollBar.visible

    /*
     * 两个上角要不要圆。
     *
     * 不分栏时两边都圆（于是标签栏顶上一左一右两个圆角，和编辑区卡片一致）；
     * 分栏之后两条标签栏是**贴在一起**的，朝里的那个角必须抹平 ——
     * 各留一个圆角会在交界处挖出一个缺口，看着就是"两栏 tab 之间有一条缝"
     * （用户报的就是这个）。谁朝里由 EditorArea 按分栏方向指定。
     */
    property bool roundTopLeft: true
    property bool roundTopRight: true

    signal tabCloseRequested(var pane, int index)
    signal tabContextMenuRequested(var pane, int index, var menuAnchor, real x, real y)
    signal markdownToggleRequested()

    color: "#1e1f22"
    topLeftRadius: roundTopLeft ? cornerRadius : 0
    topRightRadius: roundTopRight ? cornerRadius : 0

    readonly property color accentColor: "#4c96d8"
    readonly property color tabActiveBg: "#2b2d30"
    readonly property color textBright: "#e8e8e8"
    readonly property color textMain: "#bbbbbb"
    readonly property color textMuted: "#7d7d7d"

    /*
     * 这一条标签栏和顶上那条横向滚动条的几何（自检读它，见 src/SelfTest.cpp）。
     * 原来这些量散在 EditorArea 里，现在标签栏独立成一个组件了，就由它自己
     * 报上来 —— 自检那边的判据（横条别压到圆角、别盖住标签）一个字没改。
     */
    function barState() {
        return {
            height: root.height,
            width: root.width,
            cornerRadius: root.cornerRadius,
            stripTop: tabScroll.mapToItem(root, 0, 0).y,
            stripHeight: tabScroll.height,
            scrollShown: root.scrollShown,
            scrollSize: tabScrollBar.size,
            scrollPosition: tabScrollBar.position,
            scrollVisible: tabScrollBar.visible,
            flickWidth: tabScroll.width,
            flickContent: tabScroll.contentWidth,
            scrollLeft: tabScrollBar.x,
            scrollRight: tabScrollBar.x + tabScrollBar.width,
            scrollTop: tabScrollBar.y,
            scrollBottom: tabScrollBar.y + tabScrollBar.height,
            tabs: root.pane ? root.pane.documents.length : 0
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        spacing: 4

        Flickable {
            id: tabScroll

            Layout.fillWidth: true
            /*
             * 高度**直接算**，不靠 RowLayout 分配（`Layout.fillHeight: true`）。
             *
             * 这里踩过一次：RowLayout 的自己高度是从子项推出来的，而这个
             * Flickable 的 implicitHeight 是 0（它自己伸给内容），
             * 于是行高算成 0 —— `fillHeight` 把 0 填成 0，标签整条消失
             * （实测：标签栏 35px，这一条读出来 0）。直接用父项高度减上下那
             * 各 3px 的边距，和布局时序无关。
             */
            Layout.fillHeight: false
            Layout.preferredHeight: parent ? parent.height - 6 : 0
            height: parent ? parent.height - 6 : 0

            contentWidth: tabRow.width
            contentHeight: height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentWidth > width

            /*
             * 横向滚动条：浮在标签栏最顶上、**不占高度**（落在标签原来那 3px
             * 上边距里）。左右各让开容器圆角那么多，否则会把圆角啃成直角。
             * 说明见原来那份（EditorArea.qml）里的长注，口径没变。
             */
            ScrollBar.horizontal: ThinScrollBar {
                id: tabScrollBar

                parent: root
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: root.cornerRadius
                anchors.rightMargin: root.cornerRadius
                anchors.topMargin: 0
                height: 3
                padding: 0
            }

            Row {
                id: tabRow
                height: parent.height
                spacing: 3

                Repeater {
                    model: root.pane.documents

                    delegate: Rectangle {
                        id: tabItem

                        required property var modelData

                        readonly property bool active: modelData.active === true
                        readonly property bool hot: tabHit.containsMouse

                        height: tabRow.height
                        width: Math.max(120, Math.min(240, tabLabel.implicitWidth + 74))
                        radius: 5
                        color: active ? root.tabActiveBg
                                      : (hot ? "#3a3d41" : "transparent")

                        /* 这一层压在 tabHit 之上，小叉才收得到点击（见原说明） */
                        RowLayout {
                            z: 1
                            anchors.fill: parent
                            anchors.leftMargin: 9
                            anchors.rightMargin: 6
                            spacing: 6

                            AppIcon {
                                provider: root.iconProvider
                                kind: tabItem.modelData.clipboard ? "paste" : "file"
                                size: 14
                                tint: tabItem.active ? root.accentColor : root.textMuted
                                Layout.alignment: Qt.AlignVCenter
                            }

                            Label {
                                id: tabLabel
                                Layout.fillWidth: true
                                text: tabItem.modelData.title
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
                                provider: root.iconProvider
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
                                    onClicked: root.tabCloseRequested(
                                                   root.pane, tabItem.modelData.index)
                                }
                            }
                        }

                        MouseArea {
                            id: tabHit
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                                             | Qt.RightButton
                            cursorShape: Qt.PointingHandCursor
                            onClicked: (mouse) => {
                                if (mouse.button === Qt.MiddleButton)
                                    root.tabCloseRequested(root.pane,
                                                           tabItem.modelData.index)
                                else if (mouse.button === Qt.RightButton)
                                    root.tabContextMenuRequested(root.pane,
                                                                 tabItem.modelData.index,
                                                                 tabItem, mouse.x, mouse.y)
                                else
                                    /* 只切**这一栏**：另一栏看的是它自己那一份 */
                                    root.pane.activateDocument(tabItem.modelData.index)
                            }
                        }
                    }
                }
            }
        }

        /*
         * 最右边那个"源码 / 预览"开关。
         *
         * 分栏时只画在**第二栏**那条标签栏上（paneActions），免得同一个开关
         * 在两条栏上各出现一次。它不在的时候标签栏整条留给标签本身。
         */
        Rectangle {
            id: mdToggle

            Layout.preferredWidth: 28
            Layout.preferredHeight: 22
            Layout.alignment: Qt.AlignVCenter
            radius: 5
            visible: root.paneActions && root.markdownToggleVisible
            color: root.markdownPreview ? root.accentColor
                                        : (mdHit.containsMouse ? "#3a3d41"
                                                               : "transparent")

            AppIcon {
                anchors.centerIn: parent
                provider: root.iconProvider
                kind: root.markdownPreview ? "markdown" : "preview"
                size: 15
                tint: root.markdownPreview ? "#ffffff" : root.textMain
            }

            MouseArea {
                id: mdHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.markdownToggleRequested()
            }

            AppToolTip {
                hovered: mdHit.containsMouse
                text: root.markdownPreview ? "回到源码（Ctrl+Shift+V）"
                                           : "预览 Markdown（Ctrl+Shift+V）"
            }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 1
        color: "#4b4d4f"
        opacity: 0.65
    }
}
