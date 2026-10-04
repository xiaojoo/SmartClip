pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0

/*
 * 查找 / 替换栏。
 *
 * 为什么是"一条独立的栏"而不是浮在编辑区上的浮层：
 * 编辑区是**原生子窗口**（QScintilla 通过 createWindowContainer 挂在宿主
 * QWidget 上），它永远画在 QML 内容之上 —— 浮层一旦和编辑区重叠就被它盖住。
 * 所以做成标签栏下面的一条（编辑区自己往下让位，几何变化由
 * EditorViewItem::applyGeometry 跟着走）。
 *
 * ===========================================================================
 * 排法（2026-10-04 照用户给的参考图重做）
 * ===========================================================================
 * 五列，从左到右：
 *   1. 输入框列：查找框 / 替换框，两框一样长（同一列自然等宽，不用互相绑宽度）
 *   2. 三个带文字的勾选框：全词匹配 / 区分大小写 / 使用正则表达式
 *      （这三档唯一的入口就在这里 —— 参考图上输入框右端那颗 "Aa ▾" 和替换按钮
 *       旁边的 ▾ 弹层，用户 2026-10-05 点名不要了，所以这一扇没有弹层）
 *   3. 上一个 / 下一个（描边次级按钮）+ 右边 "当前 / 总数" 计数
 *   4. 蓝色实心「替换」；它下面是「全部替换」
 *   5. ✕ 收整扇栏（钉右上角，和「替换」同一行，不垂直居中）
 *
 * 两态（只查找 / 查找+替换）**同一套列**，只是第 2 列在收起态横排、展开态竖排：
 * 参考图那张是展开态（三个勾选框竖着排在两行输入框的高度里）。收起时如果还竖排，
 * 第二行就是空的 —— 与其留个洞，不如让三个勾选框横过来，栏高也就还是 40。
 *
 * 颜色用本应用自己的令牌（蓝 = #3d78b8，就是别处 checked 态那颗），没有抄参考图
 * 的 #2c87fc：抄过来会变成"查找栏的蓝和顶栏的蓝不一样"。
 * 尺寸同理保留本应用的 28 / 12px 那一档 —— 参考图那张是 43px 高的框、14px 的字，
 * 量不清它是 100% 还是 130% 缩放的截图（勾选框方块 19×20、字高 14），
 * 照抄会把这扇本来"和 VS Code 一个密度"的栏撑粗一档。
 */
Rectangle {
    id: root

    /* 自检按这个名字抓这一扇（findField / replaceField 是里面那两个输入框） */
    objectName: "findBar"

    /*
     * 栏高 = 面板上下各 2px + 面板内留白 + 内容。
     *   收起：2 + 4 + 28 + 4 + 2 = 40
     *   展开：内容 60（两行 28 + 4 行距；三个竖排勾选框正好也是 60）→ 72
     */
    implicitHeight: root.replaceVisible ? 72 : 40
    /* 整条透明：看上去"浮"在正文上方的是里面那块圆角面板 */
    color: "transparent"

    property var view: null
    property bool replaceVisible: false
    /* 栏是否展开（打开查找 / 替换时置 true，关闭时置 false） */
    property bool opened: false

    /* 三个匹配开关。勾选框、正文里的高亮、计数读的都是这三份 */
    property bool wordOn: false
    property bool caseOn: false
    property bool regexOn: false

    /*
     * 换档就重扫。
     *
     * 挂在根属性的 changed 信号上而不是挂在每个勾选框的 onToggled 里：这一档
     * 有两个入口（收起态横排、展开态竖排各一份实例），写在处理器里迟早漏一个。
     */
    onWordOnChanged: root.refreshHighlight()
    onCaseOnChanged: root.refreshHighlight()
    onRegexOnChanged: root.refreshHighlight()

    /* 计数："第 current 处 / 共 total 处"；没查过是 0/0（和参考图那个 "0/0" 同口径） */
    property int matchTotal: 0
    property int matchCurrent: 0

    signal closed()

    readonly property color panelBg: Theme.c("#3c3f41", Theme.rev)        /* 和下拉菜单一套 */
    readonly property color panelBorder: Theme.c("#4b4d4f", Theme.rev)
    readonly property color borderColor: Theme.c("#3c3f44", Theme.rev)
    readonly property color fieldBg: Theme.c("#2b2d30", Theme.rev)
    readonly property color textColor: Theme.c("#d6d7da", Theme.rev)
    readonly property color mutedColor: Theme.c("#7d838c", Theme.rev)
    readonly property color accent: "#3d78b8"

    readonly property bool noMatch: root.matchTotal === 0 && field.text !== ""

    /*
     * 自检用（见 Main.qml 的 uiState）：两个输入框的实际宽度、圆角面板左右各留了
     * 多少间隙、面板圆角多大。用户要的是"两个框一样长、有圆角、左右有间隙"，
     * 这三样都得能量出来，光看代码不算数。
     */
    readonly property real findFieldWidth: fieldBox.width
    readonly property real replaceFieldWidth: replaceBox.width
    readonly property real panelLeftGap: panel.x
    readonly property real panelRightGap: root.width - (panel.x + panel.width)
    readonly property real panelRadius: panel.radius
    /* 自检还要看栏本身有没有高度：高度绑错（比如绑到不存在的 id）会静悄悄变成 0，
       整个查找栏就什么都不画了 —— 实测踩过一次 */
    readonly property real barHeight: root.height
    /* 新排法的两件事：勾选框现在摆成几行、计数那句原文 */
    readonly property int optionRowCount: root.replaceVisible ? 3 : 1
    readonly property string counterText: root.matchLabel()
    /*
     * ✕ 和「替换」的垂直中线（都换算到 panel 坐标）。
     * 自检拿这两个数判"✕ 钉在第一行、和替换对平"：两个中线相等是 Δ0 的事，
     * 比"看着在同一行"可靠；而 ✕ 的中线要小于栏高的一半，才说明它真的不再
     * 垂直居中了（居中的话它正好落在栏高一半那条线上）。
     */
    readonly property real closeCenterY: closeButton.mapToItem(panel, 0, closeButton.height / 2).y
    readonly property real replaceCenterY: replaceGo.mapToItem(panel, 0, replaceGo.height / 2).y

    IconProvider { id: icons }

    function matchLabel() {
        if (field.text === "")
            return "0 / 0"
        return (root.matchCurrent > 0 ? root.matchCurrent : 0) + " / " + root.matchTotal
    }

    function openFind() {
        replaceVisible = false
        root.opened = true
        focusField(field)
        field.selectAll()
        refreshHighlight()
    }

    function openReplace() {
        replaceVisible = true
        root.opened = true
        focusField(field)
        field.selectAll()
        refreshHighlight()
    }

    /*
     * 把键盘焦点交给查找输入框。
     *
     * 关键一步是 view.releaseEditorFocus()：编辑区是原生 QScintilla 子窗口，
     * 它拿着焦点的时候按键根本进不了 QML（只 forceActiveFocus 是没用的，
     * 实测打开查找栏后打字全进了正文）。releaseEditorFocus() 会清掉原生
     * 控件的焦点并把 QQuickWidget 设为焦点控件，QML 这边的 forceActiveFocus
     * 才真正生效。
     */
    function focusField(target) {
        if (root.view)
            root.view.releaseEditorFocus()
        target.forceActiveFocus()
    }

    function close() {
        root.opened = false
        root.matchTotal = 0
        root.matchCurrent = 0
        if (root.view) {
            root.view.clearHighlights()
            root.view.requestEditorFocus()
        }
        root.closed()
    }

    function options() {
        return {
            text: field.text,
            cs: root.caseOn,
            ww: root.wordOn,
            re: root.regexOn
        }
    }

    function currentText() { return field.text }

    /*
     * 重画高亮 + 重数"当前 / 总数"。
     *
     * 这里扫了两遍全文（highlightMatches 一遍、matchStats 一遍）。是有意的取舍：
     * 指示器必须由 highlightMatches 画（它管 indicator 编号），而"当前第几处"
     * 只有 matchStats 知道；合成一个函数就得让画圈的那个顺手回传序号，
     * 那等于把"数"这件事塞进绘图路径里，两边都更难测。
     * 代价是敲一个字符扫两遍 —— 和原来比是 2 倍，但这条只在人敲键时跑。
     */
    function refreshHighlight() {
        if (!root.view || !root.view.hasDocument || field.text === "") {
            root.matchTotal = 0
            root.matchCurrent = 0
            if (root.view)
                root.view.clearHighlights()
            return
        }
        var o = options()
        root.view.highlightMatches(o.text, o.cs, o.ww, o.re)
        var st = root.view.matchStats(o.text, o.cs, o.ww, o.re)
        root.matchTotal = st.total
        root.matchCurrent = st.current
    }

    function findNext(forward) {
        if (!root.view || !root.view.hasDocument || field.text === "")
            return
        var o = options()
        root.view.find(o.text, o.cs, o.ww, o.re, forward)
        /* 跳完重数一次：选区落在哪一处只有跳完才知道 */
        refreshHighlight()
    }

    function doReplace() {
        if (!root.view || !root.view.hasDocument)
            return
        var o = options()
        root.view.replaceCurrent(o.text, replaceField.text, o.cs, o.ww, o.re)
        findNext(true)
    }

    function doReplaceAll() {
        if (!root.view || !root.view.hasDocument)
            return
        var o = options()
        root.view.replaceAll(o.text, replaceField.text, o.cs, o.ww, o.re)
        refreshHighlight()
    }

    /* 那颗蓝色主按钮：收起态它负责"把替换行打开"，展开态才是"替换这一处" */
    function primaryReplace() {
        if (!root.replaceVisible) {
            root.replaceVisible = true
            root.focusField(replaceField)
            return
        }
        root.doReplace()
    }

    visible: root.opened

    /* ---------------- 圆角面板（左右各留 8px 间隙） ---------------- */
    Rectangle {
        id: panel

        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        anchors.topMargin: 2
        anchors.bottomMargin: 2

        radius: 8
        color: root.panelBg
        border.color: root.panelBorder
        border.width: 1

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            anchors.topMargin: 4
            anchors.bottomMargin: 4
            spacing: 8

            /* ============ 1. 输入框列（查找 / 替换一样长） ============ */
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 4

                Rectangle {
                    id: fieldBox

                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    radius: 6
                    color: root.fieldBg
                    border.color: root.noMatch ? Theme.c("#c75450", Theme.rev) : root.borderColor

                    AppIcon {
                        id: findLead
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        provider: icons; kind: "search"; size: 13
                        tint: root.mutedColor
                    }

                    TextField {
                        id: field

                        objectName: "findField"
                        anchors.left: findLead.right
                        anchors.right: parent.right
                        anchors.leftMargin: 6
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        placeholderText: "查找内容"
                        placeholderTextColor: root.mutedColor
                        color: root.textColor
                        font.pixelSize: 12
                        background: Item {}
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true

                        onTextChanged: root.refreshHighlight()

                        Keys.onReturnPressed: (event) => {
                            root.findNext(!(event.modifiers & Qt.ShiftModifier))
                            event.accepted = true
                        }
                        Keys.onEnterPressed: (event) => {
                            root.findNext(!(event.modifiers & Qt.ShiftModifier))
                            event.accepted = true
                        }
                        Keys.onEscapePressed: (event) => { root.close(); event.accepted = true }
                    }
                }

                Rectangle {
                    id: replaceBox

                    Layout.fillWidth: true
                    Layout.preferredHeight: 28
                    visible: root.replaceVisible
                    radius: 6
                    color: root.fieldBg
                    border.color: root.borderColor

                    AppIcon {
                        id: replaceLead
                        x: 8
                        anchors.verticalCenter: parent.verticalCenter
                        provider: icons; kind: "replace"; size: 13
                        tint: root.mutedColor
                    }

                    TextField {
                        id: replaceField

                        anchors.left: replaceLead.right
                        anchors.right: parent.right
                        anchors.leftMargin: 6
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        placeholderText: "替换为"
                        placeholderTextColor: root.mutedColor
                        color: root.textColor
                        font.pixelSize: 12
                        background: Item {}
                        verticalAlignment: TextInput.AlignVCenter
                        selectByMouse: true

                        Keys.onReturnPressed: (event) => { root.doReplace(); event.accepted = true }
                        Keys.onEnterPressed: (event) => { root.doReplace(); event.accepted = true }
                        Keys.onEscapePressed: (event) => { root.close(); event.accepted = true }
                    }
                }
            }

            /*
             * ============ 2. 三个带文字的勾选框 ============
             * 展开态竖排（正好占两行输入框那 60px），收起态横排。
             * 两份实例读的是 root.wordOn / caseOn / regexOn **同一份状态**，
             * 所以从菜单里改一档，这里的方框立刻跟着变（反过来也一样）。
             */
            Column {
                id: checksCol
                visible: root.replaceVisible
                Layout.alignment: Qt.AlignVCenter
                spacing: 3

                OptionCheck { label: "全词匹配";     on: root.wordOn;   onToggled: root.wordOn = !root.wordOn }
                OptionCheck { label: "区分大小写";   on: root.caseOn;   onToggled: root.caseOn = !root.caseOn }
                OptionCheck { label: "使用正则表达式"; on: root.regexOn; onToggled: root.regexOn = !root.regexOn }
            }

            Row {
                id: checksRow
                visible: !root.replaceVisible
                Layout.alignment: Qt.AlignVCenter
                spacing: 10

                OptionCheck { label: "全词匹配";     on: root.wordOn;   onToggled: root.wordOn = !root.wordOn }
                OptionCheck { label: "区分大小写";   on: root.caseOn;   onToggled: root.caseOn = !root.caseOn }
                OptionCheck { label: "使用正则表达式"; on: root.regexOn; onToggled: root.regexOn = !root.regexOn }
            }

            /* ============ 3. 上一个 / 下一个 + 当前第几处 ============ */
            Column {
                Layout.alignment: Qt.AlignVCenter
                spacing: 4

                Row {
                    spacing: 4

                    ToolButton {
                        label: "上一个"
                        provider: icons
                        kind: "chevron-up"
                        iconSize: 12
                        framed: true
                        implicitHeight: 28
                        tip: "上一个匹配"; shortcut: "Shift+F3"
                        onClicked: root.findNext(false)
                    }
                    ToolButton {
                        label: "下一个"
                        provider: icons
                        kind: "chevron-down"
                        iconSize: 12
                        framed: true
                        implicitHeight: 28
                        tip: "下一个匹配"; shortcut: "F3"
                        onClicked: root.findNext(true)
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.matchLabel()
                        color: root.noMatch ? Theme.c("#e06c75", Theme.rev) : root.mutedColor
                        font.pixelSize: 11
                    }
                }

                /* 展开态第二行这里空着（参考图也是空着：计数只跟第一行） */
                Item {
                    width: 1
                    height: 28
                    visible: root.replaceVisible
                }
            }

            /* ============ 4. 蓝色「替换」/ 全部替换 ============ */
            Column {
                Layout.alignment: Qt.AlignVCenter
                spacing: 4

                ToolButton {
                    id: replaceGo
                    label: "替换"
                    primary: true
                    implicitHeight: 28
                    tip: root.replaceVisible ? "替换当前匹配" : "展开替换行"
                    onClicked: root.primaryReplace()
                }

                ToolButton {
                    label: "全部替换"
                    framed: true
                    implicitHeight: 28
                    visible: root.replaceVisible
                    tip: "替换全部匹配"
                    onClicked: root.doReplaceAll()
                }
            }

            /*
             * ============ 5. ✕ ============
             * 钉在第一行那一档（右上角），不再垂直居中：这一列和「替换」那颗同为
             * 28 高，所以 AlignTop 之后两者的中线自然对平。
             */
            ToolButton {
                id: closeButton
                provider: icons; kind: "close"; tip: "关闭"; shortcut: "Esc"
                Layout.alignment: Qt.AlignTop
                onClicked: root.close()
            }
        }
    }

    /*
     * 自绘勾选框 + 文字（原生 CheckBox 一律不用，和设置面板那条规矩一致）。
     * 方块 14px：参考图那颗量出来 19×20，但那张图整体比我们的字号档大一档
     * （见文件头"尺寸"那段），所以这里跟着我们自己的 12px 字走。
     *
     * 外壳必须是 Rectangle 而不是 Row：Row 的**直接子项**不许用 anchors.fill
     * （实测报 "Cannot specify left, right, horizontalCenter, fill or centerIn
     * anchors for items inside Row. Row will not function."，然后整列勾选框
     * 一个都不画 —— 界面上一片空白，自检那边却照样"3 行"，因为对象是建出来的）。
     */
    component OptionCheck: Rectangle {
        id: oc

        property string label: ""
        property bool on: false
        signal toggled

        implicitWidth: inner.implicitWidth
        implicitHeight: 18
        color: "transparent"

        Row {
            id: inner
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6

            Rectangle {
                id: box
                width: 14
                height: 14
                radius: 3
                anchors.verticalCenter: parent.verticalCenter
                color: oc.on ? Theme.c("#3d78b8", Theme.rev) : Theme.c("#26282b", Theme.rev)
                border.width: 1
                border.color: oc.on ? Theme.c("#3d78b8", Theme.rev) : Theme.c("#565a60", Theme.rev)

                AppIcon {
                    anchors.centerIn: parent
                    provider: icons; kind: "check"; size: 10
                    tint: "#ffffff"
                    visible: oc.on
                }
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: oc.label
                color: oc.on ? root.textColor : root.mutedColor
                font.pixelSize: 11
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: oc.toggled()
        }
    }
}
