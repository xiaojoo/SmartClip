pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "../utils"
import SmartClip.Globals 1.0

/*
 * 设置 / 帮助面板（左侧操作步骤，右侧具体内容）。
 *
 * ===========================================================================
 * 为什么又是 popupType: Popup.Window
 * ===========================================================================
 * 和 DropdownMenu 同一个原因：编辑区是**原生 QScintilla 子窗口**
 * （QWidget::createWindowContainer，见 src/EditorViewItem.h），
 * 原生子窗口永远画在 QQuickWidget 内容之上，场景内的浮层压不住它。
 * 所以这个面板必须自己是一个同级原生窗口。
 *
 * 面板跟着宿主窗口移动/缩放：openFor() 里按宿主几何摆位置，
 * 之后每次打开都重新摆一次（用户拖走的位置不持久化 —— 下次还是居中）。
 *
 * ===========================================================================
 * 快捷键为什么在这里改，而不是改 js/EditorMenus.js
 * ===========================================================================
 * 菜单里的快捷键只是**显示**，真正按键生效的是 C++ 侧挂在宿主 QWidget 上的
 * QAction（见 src/EditorController.h 开头的说明：焦点在原生编辑区里，
 * QML 的 Shortcut 收不到）。所以改键必须改 QAction，改完落 QSettings，
 * 顺手 emit shortcutsChanged() —— 菜单标签再通过 ShortcutOverrides 重算。
 *
 * 采样键盘用 TextInput 的 Keys.onPressed，不是全局 KeyboardHandler：
 * 这个面板是活动原生窗口时宿主窗口不是活动的，那一串 QAction 不会触发，
 * 所以能放心把 Ctrl+N 这类组合原样抓下来。
 */

Popup {
    id: root

    /* 外边（Main.qml）传进来的"可改键清单"，每项见 EditorController.h */
    property var entries: []

    /* 编辑器状态（Main.qml 的 window.view），关于栏要读当前字号 */
    property var view: null

    /* 面板里的"字号 + / 自动换行"这类按钮，转回 Main.qml 的命令分发 */
    signal commandRequested(string act)

    /* 每次 Cmd.shortcutsChanged 自增，用来强制 delegate 重读条目数据 */
    property int revision: 0

    /* 当前正在采键的命令名（空串 = 没在采） */
    property string capturing: ""

    /* 改键失败的原因（撞键 / 不认识的键），显示在右栏底部 */
    property string hint: ""

    /* 显示的栏目：shortcuts / about */
    property string section: "shortcuts"

    /* 顶部标题栏文案（"=" 栏目名），以及它在拖拽时的偏移 */
    property real dragDeltaX: 0
    property real dragDeltaY: 0

    readonly property color bgColor:      "#2b2d30"
    readonly property color sidebarColor: "#26282b"
    readonly property color headerColor:  "#33363a"
    readonly property color borderColor:  "#4b4d4f"
    /*
     * 面板最外圈那一道边框。
     *
     * borderColor（#4b4d4f）是内部那几条分隔线的颜色，直接拿来当外框太暗：
     * 面板是浮在深色桌面上的独立窗口，1px 的分隔线色在截图里几乎看不见，
     * 整个框像是没有边界。所以外框单独提一档亮、加粗到 2px，
     * 宽度统一收在 frameWidth 上（背景圆角和标题栏圆角都跟着它走）。
     */
    readonly property color frameColor:   "#5c6066"
    readonly property int frameWidth:     2
    readonly property color rowHover:     "#34373b"
    readonly property color rowSel:       "#2f3a44"
    readonly property color textColor:    "#c8ccd1"
    readonly property color textBright:   "#e8e8e8"
    readonly property color mutedColor:   "#8a9098"
    readonly property color accentColor:  "#4c96d8"
    readonly property color warnColor:    "#c8503c"

    readonly property int rowHeight: 26

    /* 栏目表：左边"操作步骤"那一列 */
    readonly property var navItems: [
        { key: "shortcuts", label: "快捷键", icon: "gear" },
        { key: "about",     label: "关于",   icon: "info" }
    ]

    function titleFor(key) {
        for (var i = 0; i < navItems.length; ++i)
            if (navItems[i].key === key)
                return navItems[i].label
        return key
    }

    /* 打开面板并定位到某一栏 */
    function show(sectionKey) {
        if (sectionKey)
            section = sectionKey
        capturing = ""
        hint = ""
        placeOverHost()
        open()
        forceActiveFocus()
    }

    /* 居中到宿主窗口（DropdownMenu 里说的坐标系问题同样适用：用内容区坐标） */
    function placeOverHost() {
        var host = root.parent
        if (!host)
            return
        root.x = Math.max(8, Math.round((host.width - root.width) / 2))
        root.y = Math.max(8, Math.round((host.height - root.height) / 2))
    }

    /*
     * 采键结果落库。Cmd.setShortcut 返回空串 = 成功（并在 C++ 侧 emit 了
     * shortcutsChanged()，条目数据下一帧就是新的），非空 = 撞键之类的原因。
     */
    function commitKey(name, keyText) {
        var error = Cmd.setShortcut(name, keyText)
        hint = error ? error : ""
        capturing = ""
    }

    /* Qt.PortableText 这套写法：命令里的修饰位对应字符串里的 Ctrl/Alt/Shift/Meta */
    function keyTextFromEvent(event) {
        var text = ""
        if (event.modifiers & Qt.ControlModifier)
            text += "Ctrl+"
        if (event.modifiers & Qt.AltModifier)
            text += "Alt+"
        if (event.modifiers & Qt.ShiftModifier)
            text += "Shift+"
        if (event.modifiers & Qt.MetaModifier)
            text += "Meta+"

        var key = event.key
        /* 只按了修饰键：继续等真正的那个键 */
        if (key === Qt.Key_Control || key === Qt.Key_Shift
                || key === Qt.Key_Alt || key === Qt.Key_Meta
                || key === Qt.Key_AltGr)
            return ""

        /*
         * 字符键用 event.text 的原样字符：'/'、'='、'-' 这类
         * 在 PortableText 里就是字面量，拿 Key_ slash 之类的枚举名反而对不上。
         * Ctrl+Shift+= 出来的 '+' 也正好是我们要的（和默认键位一致）。
         */
        if (event.text !== "" && event.text.charCodeAt(0) >= 0x20
                && key !== Qt.Key_Escape && key !== Qt.Key_Tab
                && key !== Qt.Key_Backtab && key !== Qt.Key_Backspace
                && key !== Qt.Key_Return && key !== Qt.Key_Enter
                && key !== Qt.Key_Delete && key !== Qt.Key_Insert
                && key < Qt.Key_F1) {
            text += event.text.toUpperCase()
        } else {
            text += eventNativeName(key)
        }
        return text
    }

    /* 非字符键：用 QKeySequence 认得的枚举名（F1…F35 / Del / Ins / Home…） */
    function eventNativeName(key) {
        if (key >= Qt.Key_F1 && key <= Qt.Key_F35)
            return "F" + (key - Qt.Key_F1 + 1)

        switch (key) {
        case Qt.Key_Delete:    return "Del"
        case Qt.Key_Insert:    return "Ins"
        case Qt.Key_Home:      return "Home"
        case Qt.Key_End:       return "End"
        case Qt.Key_PageUp:    return "PgUp"
        case Qt.Key_PageDown:  return "PgDown"
        case Qt.Key_Left:      return "Left"
        case Qt.Key_Right:     return "Right"
        case Qt.Key_Up:        return "Up"
        case Qt.Key_Down:      return "Down"
        case Qt.Key_Tab:       return "Tab"
        case Qt.Key_Backtab:   return "Backtab"
        case Qt.Key_Backspace: return "Backspace"
        case Qt.Key_Return:
        case Qt.Key_Enter:     return "Return"
        case Qt.Key_Space:     return "Space"
        default:               return ""
        }
    }

    width: 820
    height: Math.min(560, (root.parent ? root.parent.height : 640) - 60)
    padding: 0
    margins: 0
    modal: false
    focus: true
    popupType: Popup.Window
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onClosed: {
        capturing = ""
        hint = ""
    }

    IconProvider { id: icons }

    background: Rectangle {
        color: root.bgColor
        radius: 6
        border.color: root.frameColor
        border.width: root.frameWidth
    }

    /*
     * contentItem 套一层 Item 并显式给宽高。
     *
     * Popup 默认会按 contentItem 的 implicitWidth/Height 反过来定自己的尺寸，
     * 这里宽高已经由我们自己定死（面板是固定大小的），显式铺满可以避免
     * Popup 的 contentWidth/contentHeight 绑定循环。
     */
    contentItem: Item {
        implicitWidth: root.width
        implicitHeight: root.height

        Column {
            /*
             * 四边各让出 frameWidth：内容层是"整个窗口那么大"（标题栏默认铺满
             * 整宽、左栏默认铺满整高），不留边的话它会把背景上的边框盖住 ——
             * 顶边和左边就是这样被标题栏和左栏吃掉的，只有右边、下边能看见框线。
             */
            x: root.frameWidth
            y: root.frameWidth
            width: parent.width - root.frameWidth * 2
            height: parent.height - root.frameWidth * 2
            spacing: 0

            /* ---------------- 标题栏（可拖动） ---------------- */
            Rectangle {
                id: header
                width: parent.width
                height: 36
                color: root.headerColor
                topLeftRadius: 4
                topRightRadius: 4
    
                AppIcon {
                    id: headerIcon
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    provider: icons
                    kind: "gear"
                    size: 15
                    tint: "#9aa0a8"
                }
    
                Text {
                    anchors.left: headerIcon.right
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: "SmartClip — 设置"
                    color: root.textBright
                    font.pixelSize: 13
                }
    
                /* 右上角关闭按钮：hover 走同一套红（和窗口那个关闭按钮一致） */
                Rectangle {
                    id: closeCell
                    width: 30; height: 30
                    anchors.right: parent.right
                    anchors.rightMargin: 5
                    anchors.verticalCenter: parent.verticalCenter
                    radius: 5
                    color: closeHit.containsMouse ? "#c8503c" : "transparent"
    
                    AppIcon {
                        anchors.centerIn: parent
                        provider: icons
                        kind: "close"
                        size: 13
                        tint: closeHit.containsMouse ? "#ffffff" : "#9aa0a8"
                    }
    
                    MouseArea {
                        id: closeHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.close()
                    }
                }
    
                /* 拖标题栏移动面板 */
                MouseArea {
                    anchors.left: parent.left
                    anchors.right: closeCell.left
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    cursorShape: Qt.SizeAllCursor
                    property real pressSceneX: 0
                    property real pressSceneY: 0
    
                    onPressed: (mouse) => {
                        pressSceneX = mapToItem(null, mouse.x, mouse.y).x
                        pressSceneY = mapToItem(null, mouse.x, mouse.y).y
                        mouse.accepted = true
                    }
                    onPositionChanged: (mouse) => {
                        if (!(mouse.buttons & Qt.LeftButton))
                            return
                        var p = mapToItem(null, mouse.x, mouse.y)
                        root.x = root.x + (p.x - pressSceneX)
                        root.y = root.y + (p.y - pressSceneY)
                        mouse.accepted = true
                    }
                }
            }
    
            /* ---------------- 主体：左栏 + 右栏 ---------------- */
            Row {
                width: parent.width
                height: parent.height - header.height
                spacing: 0
    
                /* ---- 左栏：操作步骤 ---- */
                Rectangle {
                    id: sidebar
                    width: 210
                    height: parent.height
                    color: root.sidebarColor
                    bottomLeftRadius: 4
    
                    Column {
                        anchors.top: parent.top
                        anchors.topMargin: 10
                        width: parent.width
                        spacing: 1
    
                        Repeater {
                            model: root.navItems
    
                            delegate: Rectangle {
                                id: navRow
                                required property var modelData
    
                                width: sidebar.width - 12
                                x: 6
                                height: 30
                                radius: 5
                                readonly property bool active: root.section === navRow.modelData.key
                                readonly property bool hot: navHit.containsMouse
                                color: navRow.active ? root.rowSel
                                                     : (navRow.hot ? root.rowHover : "transparent")
    
                                AppIcon {
                                    id: navIcon
                                    anchors.left: parent.left
                                    anchors.leftMargin: 9
                                    anchors.verticalCenter: parent.verticalCenter
                                    provider: icons
                                    kind: navRow.modelData.icon
                                    size: 14
                                    tint: navRow.active ? root.accentColor
                                                        : (navRow.hot ? root.textBright : "#9aa0a8")
                                }
    
                                Text {
                                    anchors.left: navIcon.right
                                    anchors.leftMargin: 8
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: navRow.modelData.label
                                    color: navRow.active ? root.textBright
                                                         : (navRow.hot ? root.textBright
                                                                       : root.textColor)
                                    font.pixelSize: 13
                                    font.bold: navRow.active
                                    elide: Text.ElideRight
                                }
    
                                MouseArea {
                                    id: navHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.section = navRow.modelData.key
                                        root.capturing = ""
                                        root.hint = ""
                                    }
                                }
                            }
                        }
                    }
                }
    
                /* 一条竖分隔线，和主界面的卡片缝一个色 */
                Rectangle {
                    width: 1
                    height: parent.height
                    color: root.borderColor
                }
    
                /* ---- 右栏：具体操作 ---- */
                Item {
                    id: content
                    width: parent.width - sidebar.width - 1
                    height: parent.height
    
                    /* ============ 快捷键 ============ */
                    Column {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8
                        visible: root.section === "shortcuts"
    
                        /* 标题 + 一句用法（用 x 定位，不用 Row —— 见下面"操作列"的注释） */
                        Item {
                            width: parent.width
                            height: 22
    
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                text: "快捷键"
                                color: root.textBright
                                font.pixelSize: 14
                                font.bold: true
                            }
                            Text {
                                x: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "点右边“修改”后按下新的组合键；Esc 取消"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                        }
    
                        /* 表头 */
                        Rectangle {
                            width: parent.width
                            height: 24
                            color: "transparent"
    
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: "命令"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 250
                                anchors.verticalCenter: parent.verticalCenter
                                text: "快捷键"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                            Text {
                                anchors.left: parent.left
                                anchors.leftMargin: 430
                                anchors.verticalCenter: parent.verticalCenter
                                text: "操作"
                                color: root.mutedColor
                                font.pixelSize: 11
                            }
                            Rectangle {
                                anchors.bottom: parent.bottom
                                width: parent.width
                                height: 1
                                color: root.borderColor
                            }
                        }
    
                        /*
                         * 条目表。
                         *
                         * 不用 ListView 的 delegate 缓存来存"正在采键"，
                         * 状态一律放 root.capturing（命令名）—— 滚动时 delegate
                         * 会被复用/销毁，状态放里边会丢。
                         */
                        ListView {
                            id: keyList
                            width: parent.width
                            height: parent.height - 22 - 24 - 8 - 24 - 8
                            clip: true
                            model: Cmd.shortcutItems
                            boundsBehavior: Flickable.StopAtBounds
                            ScrollBar.vertical: ThinScrollBar { }
    
                            delegate: Item {
                                id: keyRow
                                required property var modelData
                                width: keyList.width
                                height: root.rowHeight
    
                                readonly property string cmdName: modelData.name
                                readonly property bool capturing: root.capturing === keyRow.cmdName
                                readonly property bool hot: keyHit.containsMouse
    
                                Rectangle {
                                    anchors.fill: parent
                                    color: keyRow.hot ? root.rowHover : "transparent"
    
                                    /* 前一条的分隔线 */
                                    Rectangle {
                                        anchors.top: parent.top
                                        width: parent.width
                                        height: 1
                                        color: root.borderColor
                                        opacity: 0.45
                                    }
    
                                    /* 命令名 */
                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 230
                                        text: keyRow.modelData.label
                                        color: root.textColor
                                        font.pixelSize: 13
                                        elide: Text.ElideRight
                                    }
    
                                    /* 当前组合键（采键时变成提示文字） */
                                    Rectangle {
                                        id: keyCell
                                        anchors.left: parent.left
                                        anchors.leftMargin: 250
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 168
                                        height: 20
                                        radius: 4
                                        color: keyRow.capturing ? "#1e2023" : "transparent"
                                        border.width: keyRow.capturing ? 1 : 0
                                        border.color: root.accentColor
    
                                        Text {
                                            anchors.fill: parent
                                            anchors.leftMargin: 6
                                            verticalAlignment: Text.AlignVCenter
                                            text: keyRow.capturing
                                                  ? "按下新的组合键…"
                                                  : (keyRow.modelData.shortcut === ""
                                                     ? "（未设置）"
                                                     : keyRow.modelData.shortcut)
                                            color: keyRow.capturing ? root.accentColor
                                                  : (keyRow.modelData.shortcut === "" ? root.mutedColor
                                                                                      : (keyRow.modelData.conflict
                                                                                         ? root.warnColor
                                                                                         : root.textBright))
                                            font.pixelSize: 12
                                            font.family: "Consolas"
                                            elide: Text.ElideRight
                                        }
    
                                        /* 采键输入：只在正在采的这一行可见/可用 */
                                        TextInput {
                                            id: keyInput
                                            anchors.fill: parent
                                            visible: keyRow.capturing
                                            focus: keyRow.capturing
                                            color: "transparent"
                                            cursorVisible: false
                                            selectByMouse: false
                                            activeFocusOnTab: false
    
                                            Keys.onPressed: (event) => {
                                                event.accepted = true
                                                if (event.key === Qt.Key_Escape) {
                                                    root.capturing = ""
                                                    root.hint = ""
                                                    return
                                                }
                                                if (event.key === Qt.Key_Backspace
                                                        || event.key === Qt.Key_Delete) {
                                                    root.commitKey(keyRow.cmdName, "")
                                                    return
                                                }
                                                var text = root.keyTextFromEvent(event)
                                                if (text === "")
                                                    return
                                                root.commitKey(keyRow.cmdName, text)
                                            }
                                        }
                                    }
    
                                    /*
                                     * 操作列。
                                     *
                                     * 这里不能用 Row：Row 会给子项重算 x，
                                     * 而子项上的 anchors 又是非法组合
                                     * （运行时会刷 "Cannot specify left … inside Row"）。
                                     * 三个动作的落点本来就是固定的，直接给 x。
                                     */
                                    Text {
                                        id: modifyLabel
                                        x: 430
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: keyRow.capturing ? "取消" : "修改"
                                        color: keyHit.containsMouse ? root.accentColor
                                                                    : root.mutedColor
                                        font.pixelSize: 12
    
                                        MouseArea {
                                            id: keyHit
                                            anchors.fill: parent
                                            anchors.margins: -6
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                root.hint = ""
                                                root.capturing = keyRow.capturing
                                                                 ? ""
                                                                 : keyRow.cmdName
                                            }
                                        }
                                    }
    
                                    Text {
                                        x: modifyLabel.x + modifyLabel.implicitWidth + 22
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: keyRow.modelData.custom
                                        text: "恢复默认"
                                        color: resetHit.containsMouse ? root.accentColor
                                                                      : root.mutedColor
                                        font.pixelSize: 12
    
                                        MouseArea {
                                            id: resetHit
                                            anchors.fill: parent
                                            anchors.margins: -6
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: {
                                                root.capturing = ""
                                                Cmd.resetShortcut(keyRow.cmdName)
                                                root.hint = ""
                                            }
                                        }
                                    }
    
                                    /* 撞键告警：红点 + 悬浮说明（深色提示框，见 AppToolTip） */
                                    AppIcon {
                                        x: modifyLabel.x + modifyLabel.implicitWidth + 106
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: keyRow.modelData.conflict === true
                                        provider: icons
                                        kind: "info"
                                        size: 13
                                        tint: root.warnColor
                                        AppToolTip {
                                            hovered: conflictHit.containsMouse
                                            text: "这个组合键和另一个命令重复，实际不会生效"
                                        }
                                        MouseArea {
                                            id: conflictHit
                                            anchors.fill: parent
                                            anchors.margins: -5
                                            hoverEnabled: true
                                        }
                                    }
                                }
                            }
                        }
    
                        /* 底部：提示 / 全部恢复默认 / 关闭 */
                        Item {
                            width: parent.width
                            height: 24
    
                            Text {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: parent.width - 260
                                text: root.hint
                                color: root.warnColor
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
    
                            Row {
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 8
    
                                /* 小按钮：恢复全部默认 */
                                Rectangle {
                                    width: resetAllLabel.implicitWidth + 20
                                    height: 22
                                    radius: 4
                                    color: resetAllHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
    
                                    Text {
                                        id: resetAllLabel
                                        anchors.centerIn: parent
                                        text: "全部恢复默认"
                                        color: resetAllHit.containsMouse ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }
    
                                    MouseArea {
                                        id: resetAllHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            root.capturing = ""
                                            Cmd.resetAllShortcuts()
                                            root.hint = ""
                                        }
                                    }
                                }
    
                                Rectangle {
                                    width: 76
                                    height: 22
                                    radius: 4
                                    color: okHit.containsMouse ? "#3a3e42" : "#33363a"
                                    border.width: 1
                                    border.color: okHit.containsMouse ? root.accentColor : root.borderColor
    
                                    Text {
                                        anchors.centerIn: parent
                                        text: "完成"
                                        color: root.textBright
                                        font.pixelSize: 12
                                    }
    
                                    MouseArea {
                                        id: okHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.close()
                                    }
                                }
                            }
                        }
                    }
    
                    /* ============ 关于 ============ */
                    Column {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10
                        visible: root.section === "about"
    
                        Text {
                            text: "关于 SmartClip"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }
    
                        Rectangle {
                            width: parent.width
                            height: aboutColumn.height + 28
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor
    
                            Column {
                                id: aboutColumn
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 7
    
                                Repeater {
                                    model: [
                                        { k: "名称",     v: "SmartClip" },
                                        { k: "版本",     v: "0.1.0" },
                                        { k: "作用",     v: "剪贴板历史 + 内置编辑器" },
                                        { k: "编辑器内核", v: "QScintilla 2.14（GPLv3）" },
                                        { k: "界面内核",  v: "Qt " + Qt.version + " / QML" },
                                        { k: "当前字号",  v: (view ? view.fontPixelSize : 12) + " px（设置里可改）" },
                                        /*
                                         * 行高：显示**实际像素**，倍数是设置里那一档。
                                         * 两个数都从编辑器读（view.lineHeight / lineHeightFactor），
                                         * 换字体 / 换字号之后这里跟着变。
                                         */
                                        { k: "当前行高",  v: (view ? view.lineHeight : 15) + " px"
                                                              + (view && view.lineHeightFactor > 1.0
                                                                 ? "（" + view.lineHeightFactor + " 倍）"
                                                                 : "（跟随字体）") }
                                    ]
    
                                    delegate: Row {
                                        required property var modelData
                                        spacing: 12
    
                                        Text {
                                            width: 74
                                            text: modelData.k
                                            color: root.mutedColor
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            text: modelData.v
                                            color: root.textColor
                                            font.pixelSize: 12
                                        }
                                    }
                                }
                            }
                        }
    
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "撤销 / 重做 / 剪切 / 复制 / 粘贴 / 全选（Ctrl+Z / Ctrl+Y / Ctrl+X / "
                                  + "Ctrl+C / Ctrl+V / Ctrl+A）由编辑器内核自己处理，不在上面的可改清单里。"
                        }
    
                        /* 编辑器字号这些既有开关，顺手给个入口，省得翻菜单 */
                        Text {
                            text: "编辑器"
                            color: root.textBright
                            font.pixelSize: 13
                            font.bold: true
                        }
    
                        Row {
                            spacing: 10
    
                            Repeater {
                                model: [
                                    { label: "字号 −", act: "fontSize:11" },
                                    { label: "字号 +", act: "fontSize:14" },
                                    { label: "行高 −", act: "lineHeightDown" },
                                    { label: "行高 +", act: "lineHeightUp" },
                                    { label: "重置缩放", act: "zoomReset" },
                                    { label: "自动换行", act: "toggleWrap" }
                                ]
    
                                delegate: Rectangle {
                                    required property var modelData
                                    width: 86
                                    height: 24
                                    radius: 4
                                    color: aboutBtnHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
    
                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: aboutBtnHit.containsMouse ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }
    
                                    MouseArea {
                                        id: aboutBtnHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.commandRequested(modelData.act)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
