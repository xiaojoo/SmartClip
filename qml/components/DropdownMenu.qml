pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../utils"

/*
 * IDE 风格下拉菜单。
 *
 * ===========================================================================
 * 为什么是原生弹窗（popupType: Popup.Window），不是场景里的浮层
 * ===========================================================================
 * 编辑区是**原生 QScintilla 子窗口**（QWidget::createWindowContainer，见
 * src/EditorViewItem.h）。原生子窗口永远画在 QQuickWidget 的内容之上，
 * 这不是 z 值能改的事：QML 场景内的任何浮层（Item 叠加层、Popup.Item）
 * 只要和编辑区重叠，就会被编辑区整个盖掉 —— 实测表现就是
 * "语言下拉点出来什么都看不见，被内容区盖住了"。
 *
 * 所以菜单必须自己是一个**同级原生窗口**，由窗口管理器保证它盖在编辑区之上。
 * Qt 6.8 起 Popup 支持 popupType 属性：
 *   Popup.Item   —— 场景内浮层（默认，会被原生子窗口盖住，不能用）
 *   Popup.Window —— 独立原生窗口，内容还是这份 QML（本文件用的）
 *   Popup.Native —— 平台原生菜单
 *
 * 条目格式（见 js/EditorMenus.js）：
 *   { label, act, shortcut, checked, disabled }   普通项
 *   { separator: true }                           分隔线
 */
Popup {
    id: root

    property var entries: []
    signal selected(string act)

    readonly property color bgColor:     "#3c3f41"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor:   "#bbbbbb"
    readonly property color textHot:     "#e8e8e8"
    readonly property color hoverColor:  "#46484a"
    readonly property color mutedColor:  "#6f737a"
    readonly property color accentColor: "#4c96d8"

    readonly property real itemHeight: 28
    readonly property real separatorHeight: 9
    readonly property real menuWidth: 244

    /* 所有条目撑开后的总高度（分隔线更矮）。注意不能叫 contentHeight —— Popup 自己已经有这个名字的 FINAL 属性 */
    readonly property real entriesHeight: {
        var h = 8
        for (var i = 0; i < (entries ? entries.length : 0); ++i)
            h += entries[i] && entries[i].separator ? separatorHeight : itemHeight
        return h
    }

    /*
     * 菜单最高能有多高。
     *
     * 语言菜单有 27 项，不限高就是 764px —— 会盖住左侧导航栏和大半个窗口。
     * 主流编辑器的长菜单都是限高 + 内部滚动，这里照做：最多到宿主窗口高度的
     * 一半左右，超出部分用滚轮 / 右侧细滚动条看。
     *
     * 高度取宿主（Main.qml 的根 Rectangle，即整个窗口内容区）的高度：
     * Popup 本身不是 Item，拿不到 Window 附着属性，只能顺着 parent 往上问。
     */
    readonly property real hostHeight: root.parent ? root.parent.height : 800
    readonly property real maxMenuHeight: Math.max(168, Math.min(hostHeight - 24, 460))

    /* 实际画出来的高度 */
    readonly property real menuHeight: Math.min(entriesHeight, maxMenuHeight)
    readonly property bool scrollable: entriesHeight > menuHeight + 1

    /*
     * 这组菜单里有没有带图标的条目。
     *
     * 语言 / 编码这种纯值列表没有图标，那就别留出图标列的位置；
     * 有图标的菜单（文件 / 编辑 / …）统一留出，保证所有文字左对齐。
     */
    readonly property bool hasIcons: {
        if (!entries)
            return false
        for (var i = 0; i < entries.length; ++i)
            if (entries[i] && entries[i].icon)
                return true
        return false
    }

    /* 勾选列 + 图标列的宽度（条目标签的左缩进就是它们之和） */
    readonly property real checkColumn: 14
    readonly property real iconColumn: hasIcons ? 17 : 0
    readonly property real leadColumn: checkColumn + iconColumn

    width: menuWidth
    height: menuHeight
    /* 4px 内缩：条目的圆角高亮块不要贴到弹窗边缘（弹窗自己也有 5px 圆角） */
    padding: 4
    margins: 0
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    /* 关键：独立原生窗口，否则会被原生编辑区盖住（见文件头注释） */
    popupType: Popup.Window

    IconProvider { id: menuIcons }

    background: Rectangle {
        color: root.bgColor
        radius: 5
        border.color: root.borderColor
    }

    /*
     * openFor 的 anchor 是菜单栏 tab / 工具栏按钮（都是 QML 里的 Item）。
     *
     * 坐标是**相对宿主窗口内容区**的，不是屏幕坐标：弹窗虽然是个独立原生
     * 窗口，Popup 的 x/y 语义没变，Qt 自己会换算成屏幕位置。实测传屏幕坐标
     * 会被再加一次窗口原点，菜单直接跑到窗口外面（出现在 (2683,1243)）。
     *
     * 下方放不下就翻到锚点上方（和主流菜单一致）。
     */
    function openFor(anchor, items) {
        entries = items
        list.contentY = 0

        var host = root.parent
        var below = anchor.mapToItem(host, 0, anchor.height + 3)
        var above = anchor.mapToItem(host, 0, 0)

        var px = Math.max(2, Math.min(below.x, host.width - menuWidth - 4))
        var py = below.y
        if (py + menuHeight > host.height - 4)
            py = Math.max(8, above.y - menuHeight - 6)

        root.x = px
        root.y = py
        root.open()
    }

    contentItem: Item {
        Flickable {
            id: list

            anchors.fill: parent
            contentWidth: width
            contentHeight: col.height
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            interactive: root.scrollable

            /* 滚轮也能滚：Flickable 自己处理 wheel 事件 */
            ScrollBar.vertical: ThinScrollBar {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom
            }

            Column {
                id: col

                /* 有滚动条时给右边让出一点位置，免得文字压在滚动条上 */
                width: list.width - (root.scrollable ? 6 : 0)
                spacing: 0

                Repeater {
                    model: root.entries

                    delegate: Item {
                        id: entry

                        required property var modelData

                        readonly property bool isSeparator: modelData
                                                            && modelData.separator === true
                        readonly property bool isDisabled: modelData
                                                           && modelData.disabled === true

                        width: col.width
                        height: entry.isSeparator ? root.separatorHeight : root.itemHeight

                        /* ---- 分隔线 ---- */
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.leftMargin: 8
                            anchors.rightMargin: 8
                            height: 1
                            color: root.borderColor
                            opacity: 0.8
                            visible: entry.isSeparator
                        }

                        /* ---- 普通条目 ---- */
                        Rectangle {
                            id: itemRect

                            anchors.fill: parent
                            visible: !entry.isSeparator
                            radius: 4
                            color: itemHit.containsMouse && !entry.isDisabled
                                   ? root.hoverColor : "transparent"

                            Row {
                                anchors.fill: parent
                                anchors.leftMargin: 8
                                anchors.rightMargin: 10
                                spacing: 6

                                /* 勾选列（当前语言 / 当前编码这类"选中项"打勾） */
                                Item {
                                    width: root.checkColumn
                                    height: parent.height

                                    AppIcon {
                                        anchors.centerIn: parent
                                        provider: menuIcons
                                        kind: "check"
                                        size: 13
                                        visible: entry.modelData
                                                 && entry.modelData.checked === true
                                        tint: root.accentColor
                                    }
                                }

                                /*
                                 * 图标列：图标在**左边**，快捷键在右边。
                                 *
                                 * 工具栏那一整行已经去掉，命令全部收进这些下拉菜单，
                                 * 图标就是它们的"脸"。
                                 */
                                Item {
                                    width: root.iconColumn
                                    height: parent.height

                                    AppIcon {
                                        anchors.left: parent.left
                                        anchors.verticalCenter: parent.verticalCenter
                                        provider: menuIcons
                                        kind: entry.modelData && entry.modelData.icon
                                              ? entry.modelData.icon : ""
                                        size: 14
                                        /* 和工具栏原来那套图标一个色：平时灰、悬停转亮 */
                                        tint: entry.isDisabled ? "#4d5157"
                                                               : (itemHit.containsMouse
                                                                  ? root.textHot
                                                                  : "#9aa0a8")
                                    }
                                }

                                Text {
                                    id: itemLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: parent.width - root.leadColumn - 6
                                            - (shortcutLabel.visible
                                               ? shortcutLabel.implicitWidth + 12 : 0)
                                    /*
                                     * 分隔线条目里没有 label / shortcut，取值前必须
                                     * 判存在：直接取会得到 undefined，赋给 QString
                                     * 属性会刷 "Unable to assign [undefined] to QString"。
                                     */
                                    text: entry.modelData
                                          && entry.modelData.label !== undefined
                                          ? entry.modelData.label : ""
                                    color: entry.isDisabled ? root.mutedColor
                                                            : (itemHit.containsMouse
                                                               ? root.textHot : root.textColor)
                                    font.pixelSize: 13
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                }

                                Text {
                                    id: shortcutLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: entry.modelData && entry.modelData.shortcut
                                          ? entry.modelData.shortcut : ""
                                    visible: text !== ""
                                    color: entry.isDisabled ? "#55585d" : root.mutedColor
                                    font.pixelSize: 11
                                    verticalAlignment: Text.AlignVCenter
                                }
                            }

                            MouseArea {
                                id: itemHit
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: entry.isDisabled ? Qt.ArrowCursor
                                                              : Qt.PointingHandCursor
                                onClicked: {
                                    if (entry.isDisabled || entry.isSeparator)
                                        return
                                    root.selected(entry.modelData.act)
                                    root.close()
                                }
                            }
                        }
                    }
                }
            }
        }

        /* 上面 / 下面还有内容时给个小箭头提示（长菜单才出现） */
        AppIcon {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 1
            provider: menuIcons
            kind: "chevron-up"
            size: 10
            tint: root.mutedColor
            visible: root.scrollable && list.contentY > 1
        }

        AppIcon {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 1
            provider: menuIcons
            kind: "chevron-down"
            size: 10
            tint: root.mutedColor
            visible: root.scrollable
                     && list.contentY < list.contentHeight - list.height - 1
        }
    }
}
