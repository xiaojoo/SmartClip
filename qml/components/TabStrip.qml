pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0

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

    /*
     * 要画的那一串标签。
     *
     * 默认就是这一栏自己开着的文档；文件对比之后 EditorArea 会换成一份
     * "文档 + 对比会话"合起来的表（对比会话不是文档，池子里没有它，
     * 但它得有自己的一个标签）。每一项要么带 kind === "diff"，要么就是
     * C++ 那边那份文档记录原样。
     */
    property var tabModel: pane ? pane.documents : []

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
    /* 点中 / 关掉一个"文件对比"标签（对比会话不在文档池里，走单独这两条） */
    signal diffTabClicked(int diffIndex)
    signal diffTabCloseRequested(int diffIndex)
    /*
     * 左键点中一个文档标签。
     *
     * 切文档是标签栏自己调 activateDocument 就办完了，但"点了哪一格"这件事
     * 外面也得知道 —— 对比页开着的时候，点回文档就是要退出对比页，
     * 而点的正好是当前那一份时 currentChanged 根本不会发，没有这条信号就收不到。
     */
    signal tabActivated(var pane, int index)

/*
 * 页签条这一处**不按 hex 查表**，按角色写死。
 *
 * 表里 #1e1f22（条子底）和 #2b2d30（选中那一枚的底）在浅色档都翻成 #ffffff ——
 * 同一个深色值在这儿担着两个角色（条子底 / 卡片底），全局按 hex 映射必然撞车，
 * 结果就是"选中的 tab 背景没有了"（2026-09-23 他圈的这条）。
 * 浅色档照他参考图那套来：灰面 #f2f3f5 上压一张白卡片 #ffffff。
 */
    color: Theme.light ? "#f2f3f5" : "#1e1f22"
    topLeftRadius: roundTopLeft ? cornerRadius : 0
    topRightRadius: roundTopRight ? cornerRadius : 0

    readonly property color accentColor: "#4c96d8"
    readonly property color tabActiveBg: Theme.light ? "#ffffff" : "#2b2d30"
    readonly property color textBright: Theme.c("#e8e8e8", Theme.rev)
    readonly property color textMain: Theme.c("#bbbbbb", Theme.rev)
    readonly property color textMuted: Theme.c("#7d7d7d", Theme.rev)

    /*
     * 这一条标签栏和顶上那条横向滚动条的几何（自检读它，见 src/SelfTest.cpp）。
     * 原来这些量散在 EditorArea 里，现在标签栏独立成一个组件了，就由它自己
     * 报上来 —— 自检那边的判据（横条别压到圆角、别盖住标签）一个字没改。
     */
    function barState() {
        const t = root.activeTab()
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
            tabs: root.tabModel.length,
            /* 选中那一格在内容坐标里的位置 + 当前视口（自检据此卡"选中格没滚出屏"） */
            activeLeft: t ? t.x : -1,
            activeWidth: t ? t.width : 0,
            viewLeft: tabScroll.contentX,
            viewWidth: tabScroll.width
        }
    }

    /*
     * 选中的那一格（找不到就是 null）。
     *
     * 标签格是 Repeater 挂进 tabRow 的子项，`active` 是每一格自己按
     * modelData.active 算出来的 —— 所以按这个标记找，不按下标猜（哪一栏开着
     * 哪几份、两条标签栏各自的下标都不是一回事）。
     */
    function activeTab() {
        for (let i = 0; i < tabRow.children.length; ++i) {
            const t = tabRow.children[i]
            if (t && t.active === true)
                return t
        }
        return null
    }

    /*
     * 把选中的那一格滚进视口。
     *
     * 标签一多这条栏就是横向滚的（Flickable + 顶上那根 3px 滚动条）。不滚的话
     * 选中格可以整个停在视口外面：点右边那个标签、或者新开一份排在右边时，
     * 看着就是"点了没反应"，顶上那条滚动条的位置也和选中的标签对不上。
     *
     * 只在"选中换了 / 标签增删 / 这条栏变宽变窄"之后滚一次，**不做成绑定** ——
     * 绑上去的话用户自己拖滚动条会被立刻弹回选中格，那是另一种难用。
     * 左右各留 6px，别让选中格正好贴着视口边（贴边看着像被裁了一半）。
     */
    function scrollActiveIntoView() {
        const t = activeTab()
        if (!t || tabScroll.width <= 0)
            return
        const left = t.x
        const right = t.x + t.width
        if (left < tabScroll.contentX)
            tabScroll.contentX = Math.max(0, left - 6)
        else if (right > tabScroll.contentX + tabScroll.width)
            tabScroll.contentX = right - tabScroll.width + 6
    }

    /*
     * 三种情况要重新对一次：换了选中的、标签增删（宽度跟着变）、这条栏本身
     * 变宽变窄（窗口改大小 / 分栏）。都排到下一帧 —— 标签格的宽度是按文字
     * implicitWidth 算的，同一帧里读到的还是旧值。
     */
    Connections {
        target: root.pane

        function onCurrentChanged() { Qt.callLater(root.scrollActiveIntoView) }
        function onTabsChanged() { Qt.callLater(root.scrollActiveIntoView) }
        function onDocumentsChanged() { Qt.callLater(root.scrollActiveIntoView) }
    }

    /* 开机恢复出来一长条标签、选中的那份排在右边时，第一次摆好也要对一次 */
    Component.onCompleted: Qt.callLater(root.scrollActiveIntoView)

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
            /* 这条栏变宽变窄（窗口改大小 / 分栏）→ 选中格可能掉出视口，再对一次 */
            onWidthChanged: Qt.callLater(root.scrollActiveIntoView)

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
                /*
                 * 内容宽度变了 → **同一帧里**就把选中格对回视口，不要 callLater。
                 *
                 * callLater 是下一帧才滚，于是这一帧先"多出一个标签（在视口外面）"、
                 * 下一帧整条再横移一格 —— 两拍就是用户看到的标签栏闪烁。Row 的宽度
                 * 是在布局这一轮里变的，这时候新那格的宽度已经算好了，同步设
                 * contentX 赶得上同一帧的合成。
                 */
                onWidthChanged: root.scrollActiveIntoView()

                Repeater {
                    model: root.tabModel

                    delegate: Rectangle {
                        id: tabItem

                        required property var modelData

                        readonly property bool isDiff: modelData.kind === "diff"
                        readonly property bool active: modelData.active === true
                        readonly property bool hot: tabHit.containsMouse

                        height: tabRow.height
                        width: Math.max(120, Math.min(240, tabLabel.implicitWidth + 74))
                        radius: 5
                        color: active ? root.tabActiveBg
                                      : (hot ? Theme.c("#3a3d41", Theme.rev) : "transparent")

                        /* 这一层压在 tabHit 之上，小叉才收得到点击（见原说明） */
                        RowLayout {
                            z: 1
                            anchors.fill: parent
                            anchors.leftMargin: 9
                            anchors.rightMargin: 6
                            spacing: 6

                            AppIcon {
                                provider: root.iconProvider
                                kind: tabItem.isDiff ? "diff"
                                      : (tabItem.modelData.clipboard ? "paste" : "file")
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

                            /* 未保存：一个点；鼠标移上来变成关闭键。对比标签自己从不脏 */
                            Rectangle {
                                Layout.preferredWidth: 7
                                Layout.preferredHeight: 7
                                Layout.alignment: Qt.AlignVCenter
                                radius: 4
                                color: root.accentColor
                                visible: !tabItem.isDiff
                                         && tabItem.modelData.modified === true
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
                                    onClicked: tabItem.isDiff
                                               ? root.diffTabCloseRequested(
                                                     tabItem.modelData.diffIndex)
                                               : root.tabCloseRequested(
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
                                if (tabItem.isDiff) {
                                    /*
                                     * 对比会话不是文档：点它就是把正文区换成那一页，
                                     * 中键关掉，右键那套文档菜单（关闭其他 / 拆分 …）
                                     * 对它没有意义，所以不弹。
                                     */
                                    if (mouse.button === Qt.MiddleButton)
                                        root.diffTabCloseRequested(tabItem.modelData.diffIndex)
                                    else if (mouse.button === Qt.LeftButton)
                                        root.diffTabClicked(tabItem.modelData.diffIndex)
                                    return
                                }
                                if (mouse.button === Qt.MiddleButton)
                                    root.tabCloseRequested(root.pane,
                                                           tabItem.modelData.index)
                                else if (mouse.button === Qt.RightButton)
                                    root.tabContextMenuRequested(root.pane,
                                                                 tabItem.modelData.index,
                                                                 tabItem, mouse.x, mouse.y)
                                else {
                                    root.tabActivated(root.pane, tabItem.modelData.index)
                                    /* 只切**这一栏**：另一栏看的是它自己那一份 */
                                    root.pane.activateDocument(tabItem.modelData.index)
                                }
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
                                        : (mdHit.containsMouse ? Theme.c("#3a3d41", Theme.rev)
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
        color: Theme.c("#4b4d4f", Theme.rev)
        opacity: 0.65
    }
}
