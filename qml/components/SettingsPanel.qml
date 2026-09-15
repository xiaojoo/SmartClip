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

    /* 显示的栏目：shortcuts / storage / translate / about */
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

    /*
     * 右栏快捷键表里"快捷键"这一列的位置。
     *
     * keyColumnX 是这一列的左边界：表头文字和行里那个组合键文字都从这条线
     * 开始，所以两边只读这一个值，不会再错开（用户报过中间这列比表头右移
     * 6px —— 那 6px 是格子给"采键高亮框"留的左内边距，格子本身得往左让出来，
     * 文字才落在线上）。
     */
    readonly property real keyColumnX: 250
    readonly property real keyCellInset: 6

    /* 栏目表：左边"操作步骤"那一列 */
    readonly property var navItems: [
        { key: "shortcuts", label: "快捷键", icon: "gear" },
        { key: "storage",   label: "存储",   icon: "folder" },
        { key: "translate", label: "翻译",   icon: "translate" },
        { key: "document",  label: "识别",   icon: "ocr" },
        { key: "about",     label: "关于",   icon: "info" }
    ]

    /*
     * 文档识别用哪个引擎 —— 从**那条命令**上认，不另存一个"引擎名"。
     *
     * 为什么不单独存一个字段：命令本身就是唯一的真相（用户可以直接编辑那一行
     * 改成任何东西）。存两份的话，改了命令而没改字段，界面上的高亮就骗人了。
     */
    function docEngineKey() {
        var cmd = Doc.runner
        if (cmd.indexOf("doc_runner_paddleocr_vl") >= 0)
            return "paddle"
        if (cmd.indexOf("doc_runner_granite") >= 0)
            return "granite"
        if (cmd.indexOf("doc_runner_rapid") >= 0)
            return "rapid"
        return ""
    }

    /* 三个按钮各自对应的那条命令（脚本路径和默认那条同一个目录） */
    function runnerForEngine(key) {
        var script = key === "paddle" ? "doc_runner_paddleocr_vl.py"
                   : (key === "granite" ? "doc_runner_granite.py"
                                        : "doc_runner_rapid.py")
        return Doc.commandForScript(script)
    }

    /*
     * 把界面那个命令框刷成 C++ 那边当前的值。
     *
     * 换引擎 / 选完解释器之后要调一次：那个框里的 text 是用户点进来时抄的一份，
     * C++ 改了它不会自己跟着变（TextField 的 text 不是绑定 —— 绑定会被用户输入
     * 打断，也会把正在敲的字弹回去）。
     */
    function refreshDocRunner() {
        docRunnerField.text = Doc.runner
    }

    /*
     * 语言下拉的条目（[ { label, act, checked } ]，见 DropdownMenu 的说明）。
     * 语言清单在 C++ 侧那一份（Llm.languages / Llm.targetLanguages），界面不另抄。
     */
    function languageEntries(list, current) {
        var out = []
        for (var i = 0; i < (list ? list.length : 0); ++i)
            out.push({ label: list[i], act: list[i], checked: list[i] === current })
        return out
    }

    /*
     * 把"选中的本地推理程序 / 模型文件"填进对应那个框。
     *
     * 对话框由 Main.qml 开，不由这个面板自己调 —— 面板是置顶的原生窗口，
     * Windows 的文件选择框会被它压住。那边先让面板让开再开框，选完从这里
     * 回来填（同一条路见 Main.qml 的 withSettingsPanelAway / chooseStorageRoot）。
     */
    function setLocalPath(kind, path) {
        if (kind === "exe")
            exeField.text = path
        else if (kind === "mmproj")
            mmprojField.text = path
        else
            modelField.text = path
    }

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
    /*
     * 只认"主动关"：标题栏那个 ✕，还有 Esc。
     *
     * 原来带着 CloseOnPressOutside —— 点面板外面的空白处就收起来了。而面板里
     * 那几个按钮（选择保存位置… / 导入文件夹…）弹的是**系统文件对话框**，
     * 用户去点那个对话框，就是在点面板外面，面板先一步自己收掉，看着像
     * "打开文件夹选择框把设置面板弄没了"。
     *
     * 另一件事也靠这个改：CloseOnPressOutside 会让 Qt 把这块窗口建成
     * Qt::Popup（Windows 上位这类窗口是**置顶**的），所以那个系统文件对话框
     * 一出来就被压在面板下面（用户截图报的正是这个）。去掉之后它是普通
     * Qt::Tool 窗口，对话框正常盖在它上面。
     */
    closePolicy: Popup.CloseOnEscape

    /*
     * 自检用：这条策略里还带着"点外面就收"没有。
     *
     * 在 QML 这侧按 Popup 自己的枚举判，不拿到 C++ 去手写那个位掩码 ——
     * 同一个枚举两边各记一份，迟早对不上。
     */
    readonly property bool closesOnOutsidePress:
        (closePolicy & Popup.CloseOnPressOutside) !== 0

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
    
                        /*
                         * 表头。
                         *
                         * 底下**不画那条横线**（原来这里有一条 borderColor 的
                         * 1px 线）：表头和第一条是同一张表，那条线把标题和内容
                         * 割成两半，看着像多出来的一行。行与行之间的细线照旧留着
                         * （见下面 delegate 里的分隔线），第一条自己那条也关掉了，
                         * 不然表头底下还是会冒一条出来。
                         */
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
                                anchors.leftMargin: root.keyColumnX
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
                                /* 表头底下不画线，所以第一条自己那条也关掉（见上面表头） */
                                required property int index
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
                                        visible: keyRow.index > 0
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
                                        /*
                                         * 格子往左让出 keyCellInset：文字（下面
                                         * Text 的 leftMargin 就是它）才会正好落在
                                         * keyColumnX 那条线上、和表头对齐。
                                         */
                                        anchors.leftMargin: root.keyColumnX - root.keyCellInset
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 168
                                        height: 20
                                        radius: 4
                                        color: keyRow.capturing ? "#1e2023" : "transparent"
                                        border.width: keyRow.capturing ? 1 : 0
                                        border.color: root.accentColor
    
                                        Text {
                                            anchors.fill: parent
                                            anchors.leftMargin: root.keyCellInset
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
    
                    /* ============ 存储 ============ */
                    /*
                     * 剪贴板内容现在是**磁盘上的 md 文件**（日期目录 / 时分秒.md），
                     * 数据库里只剩元数据。这一栏就是把"东西到底存哪儿了"讲清楚，
                     * 顺带给两个入口：换保存位置、把别的文件夹挂上来看。
                     */
                    Column {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10
                        visible: root.section === "storage"

                        Text {
                            text: "剪贴板存储"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Rectangle {
                            width: parent.width
                            height: storageInfo.height + 28
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor

                            Column {
                                id: storageInfo
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.right: parent.right
                                anchors.rightMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 7

                                Repeater {
                                    model: [
                                        { k: "保存位置", v: Store.rootPath },
                                        /*
                                         * 内容实际躺在「保存位置/剪贴板」下面 —— 保存位置
                                         * 是用户可以换的那个根（默认「文档/SmartClip」），
                                         * 剪贴板内容是它下面的一个子目录（见
                                         * ClipboardStore::contentRoot）。单独列一行，
                                         * 用户才找得到文件在哪。
                                         */
                                        { k: "内容目录", v: Store.contentRoot },
                                        { k: "文件",     v: Store.fileCount + " 份 md" },
                                        { k: "内容",     v: Store.entryCount + " 条" },
                                        { k: "切分",     v: "每份 md 写满 20K 就另起一份，名字取那一条的时间" },
                                        { k: "图片",     v: "PNG 存在当天的 assets/ 里，md 里用相对路径引用" }
                                    ]

                                    delegate: Row {
                                        required property var modelData
                                        width: parent.width
                                        spacing: 12

                                        Text {
                                            width: 74
                                            text: modelData.k
                                            color: root.mutedColor
                                            font.pixelSize: 12
                                        }
                                        Text {
                                            width: parent.width - 86
                                            text: modelData.v
                                            color: root.textColor
                                            font.pixelSize: 12
                                            elide: Text.ElideMiddle
                                        }
                                    }
                                }
                            }
                        }

                        Row {
                            spacing: 10

                            Repeater {
                                model: [
                                    { label: "选择保存位置…", act: "treeChooseRoot" },
                                    { label: "打开保存位置",   act: "treeOpenRoot" },
                                    { label: "刷新列表",       act: "refresh" }
                                ]

                                delegate: Rectangle {
                                    required property var modelData
                                    width: 118
                                    height: 24
                                    radius: 4
                                    color: storageBtnHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: modelData.label
                                        color: storageBtnHit.containsMouse ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }

                                    MouseArea {
                                        id: storageBtnHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: root.commandRequested(modelData.act)
                                    }
                                }
                            }
                        }

                        Text {
                            text: "导入的文件夹"
                            color: root.textBright
                            font.pixelSize: 13
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "挂上来看的：里面的 md / txt 会显示在左树上、也能搜到，"
                                  + "但新复制的内容永远只写进上面的保存位置，不会动这些文件夹。"
                        }

                        Rectangle {
                            width: parent.width
                            height: Math.max(30, importedColumn.height + 16)
                            radius: 6
                            color: root.rowHover
                            border.width: 1
                            border.color: root.borderColor

                            Column {
                                id: importedColumn
                                anchors.left: parent.left
                                anchors.leftMargin: 14
                                anchors.right: parent.right
                                anchors.rightMargin: 14
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Text {
                                    visible: Store.importedFolders.length === 0
                                    text: "（还没导入任何文件夹）"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }

                                Repeater {
                                    model: Store.importedFolders

                                    delegate: Row {
                                        required property string modelData
                                        width: parent.width
                                        spacing: 10

                                        Text {
                                            width: parent.width - 90
                                            text: modelData
                                            color: root.textColor
                                            font.pixelSize: 12
                                            elide: Text.ElideMiddle
                                        }
                                        Rectangle {
                                            width: 70
                                            height: 22
                                            radius: 4
                                            color: removeHit.containsMouse ? root.rowHover : "transparent"
                                            border.width: 1
                                            border.color: root.borderColor

                                            Text {
                                                anchors.centerIn: parent
                                                text: "移除"
                                                color: removeHit.containsMouse ? root.textBright : root.textColor
                                                font.pixelSize: 11
                                            }
                                            MouseArea {
                                                id: removeHit
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.commandRequested("removeImport:" + modelData)
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            width: 118
                            height: 24
                            radius: 4
                            color: importHit.containsMouse ? root.rowHover : "transparent"
                            border.width: 1
                            border.color: root.borderColor

                            Text {
                                anchors.centerIn: parent
                                text: "导入文件夹…"
                                color: importHit.containsMouse ? root.textBright : root.textColor
                                font.pixelSize: 12
                            }
                            MouseArea {
                                id: importHit
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.commandRequested("treeImportFolder")
                            }
                        }
                    }

                    /* ============ 翻译（LLM 模型怎么配） ============ */
                    Column {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 9
                        visible: root.section === "translate"

                        Text {
                            text: "翻译 / AI 模型"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "翻译卡片（左侧图标条上那个地球图标，或 Ctrl+Alt+T）用这里的模型翻译。"
                                  + "接口按 OpenAI 兼容格式填 —— OpenAI / DeepSeek / 通义 / Kimi / "
                                  + "Ollama / LM Studio 都是这个格式；密钥只存在本机设置里。"
                        }

                        /* ---- 两种模式：现成的 API 服务 / 本机自己启动一个 ---- */
                        Row {
                            spacing: 8

                            Repeater {
                                model: [ { k: "api", label: "API 模型" },
                                         { k: "local", label: "本地模型（本程序启动）" } ]

                                delegate: Rectangle {
                                    id: modeCell
                                    required property var modelData

                                    readonly property bool active: Llm.mode === modeCell.modelData.k

                                    width: modeLabel.implicitWidth + 22
                                    height: 26
                                    radius: 4
                                    color: modeHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: modeCell.active ? root.accentColor : root.borderColor

                                    Text {
                                        id: modeLabel
                                        anchors.centerIn: parent
                                        text: modeCell.modelData.label
                                        color: modeCell.active ? root.textBright : root.textColor
                                        font.pixelSize: 12
                                    }

                                    MouseArea {
                                        id: modeHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: Llm.mode = modeCell.modelData.k
                                    }
                                }
                            }
                        }

                        /* ---- API 模式：地址 / 密钥 / 模型名 ---- */
                        Column {
                            width: parent.width
                            spacing: 6
                            visible: Llm.mode === "api"

                            Repeater {
                                model: [ { k: "apiBase", label: "接口地址",
                                           hint: "https://api.deepseek.com/v1" },
                                         { k: "apiKey",  label: "密钥",
                                           hint: "sk-…（Ollama 这类本地服务可以留空）" },
                                         { k: "model",   label: "模型名",
                                           hint: "deepseek-chat / gpt-4o-mini / qwen-plus …" },
                                         /*
                                          * 截图识别要**能看图**的模型（多模态）。
                                          * 和上面那个翻译模型分开填：翻译用便宜的
                                          * 文本模型、识别用视觉模型，是常见配法；
                                          * 只配了一个视觉模型的话这项留空就行
                                          * （留空 = 用上面那个模型名）。
                                          */
                                         { k: "ocrModel", label: "识别模型",
                                           hint: "留空 = 用上面那个；要能看图的，如 qwen-vl-max / glm-4v / gpt-4o" } ]

                                delegate: Row {
                                    id: apiRow
                                    required property var modelData
                                    spacing: 8

                                    Text {
                                        width: 62
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: apiRow.modelData.label
                                        color: root.mutedColor
                                        font.pixelSize: 12
                                    }

                                    TextField {
                                        id: apiField
                                        width: 420
                                        height: 26
                                        text: Llm[apiRow.modelData.k]
                                        placeholderText: apiRow.modelData.hint
                                        color: root.textColor
                                        placeholderTextColor: root.mutedColor
                                        font.pixelSize: 12
                                        selectByMouse: true
                                        leftPadding: 7
                                        rightPadding: 7
                                        /* 改完（或按回车）就落盘：Llm 的属性 setter 自己写 QSettings */
                                        onEditingFinished: Llm[apiRow.modelData.k] = text
                                        /*
                                         * 路径 / 地址是从存档填进来的，光标默认落在末尾 ——
                                         * 不聚焦时会显示成"…尾巴那一截"。这里把它拨回开头，
                                         * 看着才是完整的一条（自己敲字时不动它）。
                                         */
                                        onTextChanged: if (!activeFocus) cursorPosition = 0
                                        background: Rectangle {
                                            color: "#26282b"
                                            border.color: root.borderColor
                                            border.width: 1
                                            radius: 4
                                        }
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "已经在跑 Ollama / LM Studio 的话，选这种模式、地址填 "
                                      + "http://127.0.0.1:11434/v1（Ollama）或它给的地址就行，不用下面那套。"
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "截图的时候还能认字：框好区域点工具条上的「识别」，"
                                      + "图会交给这里的视觉模型（识别模型，留空就用上面那个模型名），"
                                      + "结果摆在选区旁边那张卡片上，可以复制、也可以直接贴到截图上。"
                                      + "要翻译的话选一下目标语言 —— 认和翻在同一次请求里做完。"
                            }
                        }

                        /* ---- 本地模式：自己启动一个 OpenAI 兼容的推理服务 ---- */
                        Column {
                            width: parent.width
                            spacing: 6
                            visible: Llm.mode === "local"

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "服务程序"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                TextField {
                                    id: exeField
                                    width: 320
                                    height: 26
                                    text: Llm.localExe
                                    placeholderText: "llama-server.exe（llama.cpp 那个）"
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.localExe = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: "#26282b"
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                                Rectangle {
                                    width: 58; height: 26; radius: 4
                                    anchors.verticalCenter: exeField.verticalCenter
                                    color: exePickHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: "选择…"
                                        color: root.textColor
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: exePickHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        /* 交给 Main.qml 开框（面板要先让开，见 setLocalPath） */
                                        onClicked: root.commandRequested("translateChooseExe")
                                    }
                                }
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "模型文件"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                TextField {
                                    id: modelField
                                    width: 320
                                    height: 26
                                    text: Llm.localModel
                                    placeholderText: "*.gguf 模型文件"
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.localModel = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: "#26282b"
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                                Rectangle {
                                    width: 58; height: 26; radius: 4
                                    anchors.verticalCenter: modelField.verticalCenter
                                    color: modelPickHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: "选择…"
                                        color: root.textColor
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: modelPickHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        /* 同上：走 Main.qml，面板先让开 */
                                        onClicked: root.commandRequested("translateChooseModel")
                                    }
                                }
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "多模态投影"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                TextField {
                                    id: mmprojField
                                    width: 320
                                    height: 26
                                    text: Llm.localMmproj
                                    placeholderText: "mmproj-model-f16.gguf（纯文本模型留空）"
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.localMmproj = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: "#26282b"
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                                Rectangle {
                                    width: 58; height: 26; radius: 4
                                    anchors.verticalCenter: mmprojField.verticalCenter
                                    color: mmprojPickHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: root.borderColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: "选择…"
                                        color: root.textColor
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: mmprojPickHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        /* 同上：走 Main.qml，面板先让开 */
                                        onClicked: root.commandRequested("translateChooseMmproj")
                                    }
                                }
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "端口"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                TextField {
                                    id: portField
                                    width: 90
                                    height: 26
                                    text: String(Llm.localPort)
                                    color: root.textColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    validator: IntValidator { bottom: 1; top: 65535 }
                                    onEditingFinished: Llm.localPort = parseInt(text)
                                    background: Rectangle {
                                        color: "#26282b"
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }

                                Rectangle {
                                    width: 118; height: 26; radius: 4
                                    anchors.verticalCenter: portField.verticalCenter
                                    color: startHit.containsMouse ? root.rowHover : "transparent"
                                    border.width: 1
                                    border.color: Llm.localRunning ? root.warnColor : root.accentColor
                                    Text {
                                        anchors.centerIn: parent
                                        text: Llm.localRunning ? "停止本地模型" : "启动本地模型"
                                        color: Llm.localRunning ? root.warnColor : root.textBright
                                        font.pixelSize: 12
                                    }
                                    MouseArea {
                                        id: startHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            if (Llm.localRunning)
                                                Llm.stopLocal()
                                            else
                                                Llm.startLocal()
                                        }
                                    }
                                }

                                /*
                                 * 提示：本地服务第一次要加载模型，几秒到几十秒都有可能 ——
                                 * 文案跟着 Llm.status 走（"模型加载中…" / "已就绪" / 失败原因）。
                                 */
                                Text {
                                    anchors.verticalCenter: portField.verticalCenter
                                    width: Math.max(60, parent.width - 62 - 90 - 118 - 32)
                                    text: Llm.status
                                    color: root.mutedColor
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }

                            /* 服务程序的输出（启动失败时唯一能看的地方，最多三行） */
                            Text {
                                width: parent.width
                                visible: Llm.mode === "local" && Llm.localLog !== ""
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 3
                                elide: Text.ElideRight
                                color: root.mutedColor
                                font.pixelSize: 10
                                font.family: "Consolas"
                                text: Llm.localLog
                            }

                            /*
                             * 真正会执行的那条命令行（和 C++ 拼出来的是同一份）。
                             * 模型加载不起来时，先把这条复制出去在终端里跑一遍，
                             * 报错信息比这里的日志全。
                             */
                            Text {
                                width: parent.width
                                visible: Llm.localCommand !== ""
                                wrapMode: Text.WrapAnywhere
                                maximumLineCount: 2
                                elide: Text.ElideRight
                                color: root.mutedColor
                                font.pixelSize: 10
                                font.family: "Consolas"
                                text: Llm.localCommand
                            }

                            /*
                             * 这句是给用户吃定心丸的：本地模型不用手动点启动
                             * （点翻译时会自己拉起来，见 LlmClient::post）。
                             */
                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "配好之后不用每次手动启动：翻译卡片上一点「翻译」，"
                                      + "本地模型会自己起来（加载要几秒到几十秒，卡片上会显示进展）。"
                            }
                        }

                        /* ---- 两种模式共用：测试一下 ---- */
                        Row {
                            spacing: 10

                            Rectangle {
                                width: 88; height: 24; radius: 4
                                color: testHit.containsMouse ? root.rowHover : "transparent"
                                border.width: 1
                                border.color: root.borderColor
                                Text {
                                    anchors.centerIn: parent
                                    text: "测试连接"
                                    color: testHit.containsMouse ? root.textBright : root.textColor
                                    font.pixelSize: 12
                                }
                                MouseArea {
                                    id: testHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Llm.probe()
                                }
                            }

                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.max(60, parent.width - 100)
                                text: Llm.busy ? "正在测试…" : Llm.status
                                color: (Llm.status.indexOf("正常") >= 0) ? root.accentColor
                                                                        : root.mutedColor
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        /* ---- 图上选字：贴图上用鼠标选字时，用哪个引擎 ---- */
                        Column {
                            width: parent.width
                            spacing: 6

                            Text {
                                text: "图上选字（贴图上按住拖，把图里的字选出来）"
                                color: root.textColor
                                font.pixelSize: 12
                            }

                            Row {
                                spacing: 6

                                Repeater {
                                    model: [ { k: "windows", label: "Windows 自带" },
                                             { k: "ppocr",   label: "PP-OCRv6" } ]

                                    delegate: Rectangle {
                                        id: engCell
                                        required property var modelData
                                        readonly property bool active: Llm.pinOcrEngine === engCell.modelData.k

                                        width: 96
                                        height: 24
                                        radius: 4
                                        color: engCell.active ? "#2f3a44"
                                                              : (engHit.containsMouse ? root.rowHover
                                                                                      : "transparent")
                                        border.width: 1
                                        border.color: engCell.active ? root.accentColor : root.borderColor

                                        Text {
                                            anchors.centerIn: parent
                                            text: engCell.modelData.label
                                            color: engCell.active ? root.accentColor : root.textColor
                                            font.pixelSize: 11
                                        }
                                        MouseArea {
                                            id: engHit
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: Llm.pinOcrEngine = engCell.modelData.k
                                        }
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "Windows 自带：离线、不用配（默认）。PP-OCRv6：跑下面这条本机命令，"
                                      + "框更准，但要先装好。"
                            }

                            Row {
                                spacing: 8

                                Text {
                                    width: 62
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "OCR 程序"
                                    color: root.mutedColor
                                    font.pixelSize: 12
                                }
                                TextField {
                                    id: ocrRunnerField
                                    width: 420
                                    height: 26
                                    text: Llm.pinOcrRunner
                                    placeholderText: "python \"…\\ppocr_runner.py\""
                                    color: root.textColor
                                    placeholderTextColor: root.mutedColor
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    leftPadding: 7
                                    rightPadding: 7
                                    onEditingFinished: Llm.pinOcrRunner = text
                                    onTextChanged: if (!activeFocus) cursorPosition = 0
                                    background: Rectangle {
                                        color: "#26282b"
                                        border.color: root.borderColor
                                        border.width: 1
                                        radius: 4
                                    }
                                }
                            }

                            Text {
                                width: parent.width
                                wrapMode: Text.WordWrap
                                color: root.mutedColor
                                font.pixelSize: 11
                                text: "PP-OCR 那条路要装：pip install rapidocr onnxruntime（RapidOCR 3.9 "
                                      + "起内置 PP-OCRv6，模型第一次跑会自动下）。随包那个脚本第一次用到时会"
                                      + "落到 %APPDATA%/SmartClip/SmartClip/ppocr_runner.py，可以自己改。"
                                      + "选档：命令后面再接一个词 —— tiny / small / medium（默认 small，"
                                      + "medium 最准也最慢）；想指到自己下的 ONNX 文件，就把一份 params JSON "
                                      + "的路径接在后面当第三个参数。"
                            }
                        }
                    }

                    /* ============ 识别（文档 / 图片 -> Markdown） ============ */
                    Column {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 10
                        visible: root.section === "document"

                        Text {
                            text: "文档识别（PDF / 图片 / Office → Markdown 笔记）"
                            color: root.textBright
                            font.pixelSize: 14
                            font.bold: true
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "把文件拖进主窗口（或者用「文件 → 识别文档…」）就会认一遍，"
                                  + "认出来的 Markdown 存成一份新笔记，文档里的插图一起落到笔记目录的 "
                                  + "assets/ 下。识别跑在后台，右下角那张小卡片显示进展。"
                        }

                        /* ---- 引擎：三个可选项，换的就是下面那条命令 ---- */
                        Text {
                            text: "引擎"
                            color: root.textColor
                            font.pixelSize: 12
                        }

                        Row {
                            spacing: 6

                            Repeater {
                                model: [ { k: "rapid",    label: "RapidDoc（默认）" },
                                         { k: "paddle",   label: "PaddleOCR-VL" },
                                         { k: "granite",  label: "Granite-Docling" } ]

                                delegate: Rectangle {
                                    id: docEngCell
                                    required property var modelData
                                    readonly property bool active: docEngineKey() === docEngCell.modelData.k

                                    width: 138
                                    height: 24
                                    radius: 4
                                    color: docEngCell.active ? "#2f3a44"
                                                             : (docEngHit.containsMouse ? root.rowHover
                                                                                        : "transparent")
                                    border.width: 1
                                    border.color: docEngCell.active ? root.accentColor : root.borderColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: docEngCell.modelData.label
                                        color: docEngCell.active ? root.accentColor : root.textColor
                                        font.pixelSize: 11
                                    }
                                    MouseArea {
                                        id: docEngHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            Doc.runner = runnerForEngine(docEngCell.modelData.k)
                                            refreshDocRunner()
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
                            text: "RapidDoc：纯 ONNX，模型随包（约 800MB），CPU 就够，装 "
                                  + "pip install rapid-doc 即可 —— 默认这个。\n"
                                  + "PaddleOCR-VL 1.6：精度最高（版面 / 表格 / 公式 / 109 种语言），"
                                  + "要装 paddlepaddle + paddleocr[doc-parser]，权重约 1.8GB，建议有独显。\n"
                                  + "Granite-Docling 258M：最省资源（权重约 515MB），表格公式不错，"
                                  + "但语言以英文为主 —— 中文材料别选它。"
                        }

                        /* ---- 档位 ---- */
                        Row {
                            spacing: 8

                            Text {
                                width: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "档位"
                                color: root.mutedColor
                                font.pixelSize: 12
                            }

                            Repeater {
                                model: [ { k: "fast",     label: "快" },
                                         { k: "balanced", label: "均衡" },
                                         { k: "best",     label: "最准" } ]

                                delegate: Rectangle {
                                    id: tierCell
                                    required property var modelData
                                    readonly property bool active: Doc.tier === tierCell.modelData.k

                                    width: 60
                                    height: 24
                                    radius: 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: tierCell.active ? "#2f3a44"
                                                           : (tierHit.containsMouse ? root.rowHover
                                                                                    : "transparent")
                                    border.width: 1
                                    border.color: tierCell.active ? root.accentColor : root.borderColor

                                    Text {
                                        anchors.centerIn: parent
                                        text: tierCell.modelData.label
                                        color: tierCell.active ? root.accentColor : root.textColor
                                        font.pixelSize: 11
                                    }
                                    MouseArea {
                                        id: tierHit
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: Doc.tier = tierCell.modelData.k
                                    }
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "「快」关掉表格和公式识别（一份 PDF 往往快好几倍）；「均衡」是默认；"
                                  + "「最准」一次处理更多页、更激进的排版还原。"
                        }

                        /* ---- 用哪个 Python ---- */
                        Row {
                            spacing: 8

                            Text {
                                width: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "Python"
                                color: root.mutedColor
                                font.pixelSize: 12
                            }
                            TextField {
                                id: docPythonField
                                width: 420
                                height: 26
                                text: Doc.pythonPath
                                placeholderText: "留空 = 用 PATH 里的 python"
                                color: root.textColor
                                placeholderTextColor: root.mutedColor
                                font.pixelSize: 12
                                selectByMouse: true
                                leftPadding: 7
                                rightPadding: 7
                                onEditingFinished: Doc.pythonPath = text
                                onTextChanged: if (!activeFocus) cursorPosition = 0
                                background: Rectangle {
                                    color: "#26282b"
                                    border.color: root.borderColor
                                    border.width: 1
                                    radius: 4
                                }
                            }
                            Rectangle {
                                width: 62
                                height: 26
                                radius: 4
                                anchors.verticalCenter: parent.verticalCenter
                                color: pyHit.containsMouse ? root.rowHover : "transparent"
                                border.width: 1
                                border.color: root.borderColor

                                Text {
                                    anchors.centerIn: parent
                                    text: "选择…"
                                    color: pyHit.containsMouse ? root.textBright : root.textColor
                                    font.pixelSize: 11
                                }
                                MouseArea {
                                    id: pyHit
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    /* 文件框由 Main.qml 开（面板是置顶窗口，会盖住它） */
                                    onClicked: Cmd.chooseDocPython()
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "识别包装在虚拟环境里的话，这里要指到那个 venv 的 python.exe —— "
                                  + "PATH 里的 python 往往是系统那个，会报「No module named 'rapid_doc'」。"
                        }

                        /* ---- 那条命令 ---- */
                        Row {
                            spacing: 8

                            Text {
                                width: 62
                                anchors.verticalCenter: parent.verticalCenter
                                text: "识别程序"
                                color: root.mutedColor
                                font.pixelSize: 12
                            }
                            TextField {
                                id: docRunnerField
                                width: 420
                                height: 26
                                text: Doc.runner
                                placeholderText: "python \"…\\doc_runner_rapid.py\""
                                color: root.textColor
                                placeholderTextColor: root.mutedColor
                                font.pixelSize: 12
                                selectByMouse: true
                                leftPadding: 7
                                rightPadding: 7
                                onEditingFinished: Doc.runner = text
                                onTextChanged: if (!activeFocus) cursorPosition = 0
                                background: Rectangle {
                                    color: "#26282b"
                                    border.color: root.borderColor
                                    border.width: 1
                                    radius: 4
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: root.mutedColor
                            font.pixelSize: 11
                            text: "这条命令就是「用哪个引擎」唯一的接口：前面是程序，后面接到脚本的档位词"
                                  + "由上面那个「档位」自动接上。随包那三个脚本第一次用到时会落到 "
                                  + "%APPDATA%/SmartClip/SmartClip/ 下，可以自己看、自己改。"
                        }

                        /* ---- 识别程序的健康状况 ---- */
                        Text {
                            width: parent.width
                            wrapMode: Text.WordWrap
                            color: Doc.runnerProblem() === "" ? root.accentColor : root.mutedColor
                            font.pixelSize: 11
                            text: Doc.runnerProblem() === "" ? "识别程序就绪。"
                                                             : ("用不了：" + Doc.runnerProblem())
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
