pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import SmartClip.Globals 1.0

/*
 * 一块"问一句"的小卡片：标题 + 说明（可带一个输入框）+ 一排按钮。
 *
 * 界面上所有要打断用户的地方都用它 —— 关闭窗口（完全退出 / 收进托盘）、
 * 有未保存改动（保存 / 不保存 / 取消）、删除之类的确认（确定 / 取消）、
 * 出错提示（知道了），以及**要用户敲字的**那三类（重命名 / 转到行 /
 * 字数参考线列）。六处的长相和交互只有这一份（用法见 Main.qml 里的
 * quitAsk / saveAsk / confirmAsk / noticeAsk / inputAsk）。
 *
 * ===========================================================================
 * 为什么是 popupType: Popup.Window（和 DropdownMenu / SettingsPanel 同一类）
 * ===========================================================================
 * 需求是"底下的界面原封不动"：不压暗、不遮住、不挡着。所以不能做成铺满整窗的
 * 浮层 + 遮罩 —— 哪怕遮罩是透明的，它铺满整窗也会把鼠标全吃掉（实测：编辑区
 * 点不动了，用户的原话是"什么也不要做啊"）。
 *
 * 也不能做成新建的顶层对话框：系统会先把窗口映射出来、内容下一帧才画，那一帧
 * 空窗就是"弹框闪一下"。这个问题在 C++ 侧试过三种补法（关 DWM 淡入、提前
 * ensurePolished/adjustSize、改成宿主子控件）都治不干净。换成这个项目里
 * **一直在用、从来没闪过**的那类窗口（Popup.Window）就干净了 —— 它只占自己
 * 卡片那一小块，宿主既没被盖住、也没被禁用、更没有遮罩。
 *
 * 早先这些问句是 C++ 那侧的 QMessageBox：模态、系统自绘、和界面对不上色，
 * 用户报的是"关闭键那个框和别的地方长得不一样"。现在提示 / 确认 / 未保存
 * 三处全走这张卡片，C++ 只负责发信号（见 src/EditorController.h 的 alert）。
 *
 * 带输入框的那一类以前是原生 QInputDialog（系统标题栏 + 英文 OK/Cancel，
 * 和界面两套观感），现在也走这张卡片：`askInput(...)` 多传一个 spec，
 * 值通过回调回给调用方（原来是 `Cmd.askText()` 那种**阻塞拿返回值**的写法，
 * 卡片是异步的，所以调用方要把"拿到值之后干什么"包成函数传进来）。
 *
 * 行为：Esc / 点卡片外面 = 不作答（answered(-1)），由调用方当成"取消"处理。
 * 带输入框时回车 = 点第一个按钮（确定）。
 */

Popup {
    id: root

    readonly property color cardColor:   Theme.c("#3c3f41", Theme.light)
    readonly property color borderColor: Theme.c("#4b4d4f", Theme.light)
    readonly property color textColor:   Theme.c("#e8e8e8", Theme.light)
    readonly property color mutedColor:  Theme.c("#9aa0a6", Theme.light)
    readonly property color hoverColor:  Theme.c("#46484a", Theme.light)
    readonly property color pressedColor: Theme.c("#2f3234", Theme.light)
    readonly property color accentColor: "#4c96d8"

    /* 标题（加粗那一行）和说明（可以带 \n） */
    property string title: ""
    property string body: ""

    /*
     * 输入框那一档（null = 不带输入框，卡片就是原来的问句卡片）。
     *
     * { intMode: false, min: 1, max: 500 } —— intMode 给 true 时输入框只收整数，
     * min / max 是那个整数的范围（超出就打不进字，和以前原生那个 SpinBox 一致）。
     */
    property var inputSpec: null
    /* 输入框当前的内容（整数档也是文字，取完由调用方 parseInt） */
    property string inputValue: ""
    /* 带输入框时答完回给调用方的函数：function(文字)；取消 / Esc 回空串 */
    property var inputThen: null

    readonly property bool inputIsInt: inputSpec !== undefined && inputSpec !== null
                                       && inputSpec.intMode === true
    readonly property int inputMin: inputIsInt && inputSpec.min !== undefined ? inputSpec.min : 0
    readonly property int inputMax: inputIsInt && inputSpec.max !== undefined ? inputSpec.max : 99999

    /*
     * 按钮：[ { label: "保存", primary: true }, ... ]。
     * 点第 i 个就发 answered(i)；primary 那个画强调色边框（默认动作）。
     * 每开一次卡片都由调用方现传一份，所以卡片自己不缓存。
     */
    property var buttons: []

    /* 作答：下标 = 点了第几个按钮；-1 = Esc / 点外面（没作答） */
    signal answered(int index)

    width: 420
    /* 高度交给内容 + padding 自己算（Popup 的隐式尺寸就是这么来的） */
    implicitHeight: contentColumn.implicitHeight + padding * 2

    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    popupType: Popup.Window
    padding: 18
    margins: 0

    /*
     * 只有带输入框那一档才抢焦点 —— 另外四处（退出 / 未保存 / 确认 / 提示）
     * 没有可输入的东西，抢了反而会把编辑区原有的焦点弄丢。
     */
    focus: root.inputSpec !== null && root.inputSpec !== undefined

    /*
     * 开完之后再给焦点。
     *
     * 卡片是 popupType: Popup.Window —— 另起的一个顶层小窗。开之前那个窗口还
     * 不存在，`askInput()` 里直接 forceActiveFocus 是空打（实测：框里打不进字、
     * 退格也没反应）。onOpened 时小窗已经建好，再配一次 callLater 等这一帧的
     * 布局 / 激活落定。
     */
    onOpened: {
        if (root.inputSpec !== null && root.inputSpec !== undefined)
            Qt.callLater(root.focusInput)
    }

    /*
     * 这个问句作答了没。
     *
     * 收起卡片有三条路：点按钮（answer）、Esc、点外面（onClosed）。三条路
     * 都可能触发 onClosed，所以只认第一次 —— 不然点一下按钮会先发 answered(0)
     * 再补一刀 answered(-1)。
     */
    property bool settled: false

    background: Rectangle {
        color: root.cardColor
        border.color: root.borderColor
        radius: 10
    }

    /*
     * 弹一块新卡片：换内容 -> 居中 -> 打开。
     *
     * 内容是现传进来的，高度要下一帧才算得准（三个按钮的宽度、说明几行都
     * 还不知道），所以开完再按定下来的高度对一次中（见 onImplicitHeightChanged）。
     */
    function ask(t, b, btns) {
        title = t
        body = b
        buttons = btns
        inputSpec = null
        inputThen = null
        settled = false
        openCentered()
    }

    /*
     * 带输入框的那一类：标题 + 说明 + 一个输入框 + 按钮（一般是 确定 / 取消）。
     *
     * then 是 function(文字)：点第一个按钮回内容，取消 / Esc 回空串。
     * 卡片是异步的（不像以前 Cmd.askText 那样阻塞返回），所以"拿到值之后做什么"
     * 必须由调用方包成函数传进来。
     */
    function askInput(t, b, value, spec, btns, then) {
        title = t
        body = b
        buttons = btns
        inputSpec = spec
        inputValue = String(value)
        inputThen = then
        settled = false
        openCentered()
        /* 焦点不在这里给：小窗还没建好，见上面 onOpened 那段 */
    }

    function focusInput() {
        if (!inputField)
            return
        inputField.text = root.inputValue
        /*
         * 先把卡片自己那个小窗激活。
         *
         * Windows 的按键只投递给**活动窗口**里的焦点控件；不激活的话，
         * forceActiveFocus 只是在非活动窗口里换了个内部焦点，字还是进不来
         * （实测就是"框里打不进数字、退格也没反应"）。
         */
        if (inputField.window)
            inputField.window.requestActivate()
        inputField.forceActiveFocus(Qt.PopupFocusReason)
        inputField.selectAll()
    }

    /* 弹出时贴着宿主窗口居中（和原来的对话框一致） */
    function openCentered() {
        centerOnParent()
        open()
    }

    function centerOnParent() {
        if (parent) {
            x = Math.round((parent.width - width) / 2)
            y = Math.round((parent.height - height) / 2)
        }
    }

    function answer(index) {
        if (settled)
            return
        settled = true
        close()
        deliver(index === 0)
        answered(index)
    }

    /*
     * 带输入框的那一类把值回给调用方：确定回内容，取消回空串。
     *
     * 回调取出来就清掉 —— 卡片收起来之后 onClosed 还会再走一次，
     * 不清的话同一次作答会回两遍。
     */
    function deliver(accepted) {
        if (!inputThen)
            return
        const then = inputThen
        inputThen = null
        then(accepted ? inputValue.trim() : "")
    }

    /* Esc / 点外面：没作答，收起来也得说一声，否则调用方一直在等 */
    onClosed: {
        if (!settled) {
            settled = true
            deliver(false)
            answered(-1)
        }
    }

    /* 按钮个数 / 说明行数一变，高度就变，得重新对中（正开着的时候） */
    onImplicitHeightChanged: {
        if (opened)
            centerOnParent()
    }

    contentItem: Column {
        id: contentColumn
        spacing: 10

        Text {
            width: contentColumn.width
            text: root.title
            color: root.textColor
            font.pixelSize: 15
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            width: contentColumn.width
            text: root.body
            color: root.mutedColor
            font.pixelSize: 13
            lineHeight: 1.25
            wrapMode: Text.WordWrap
            /* 带输入框那一类里说明就是输入框上面那行字；没有就不占位 */
            visible: text.length > 0
        }

        /*
         * 输入框：只有 askInput 那一类（重命名 / 转到行 / 参考线列）才出现。
         *
         * 框本身是外面这块 Rectangle 画的（TextField 的 background 让位），
         * 因为要"没聚焦灰边、聚焦蓝边"，TextField 自己那套背景是另一套观感。
         */
        Rectangle {
            visible: root.inputSpec !== null && root.inputSpec !== undefined
            width: contentColumn.width
            height: 30
            radius: 4
            color: Theme.c("#2b2d2f", Theme.light)
            border.color: inputField.activeFocus ? root.accentColor : root.borderColor

            TextField {
                id: inputField

                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                verticalAlignment: Text.AlignVCenter
                background: Item {}
                color: root.textColor
                selectionColor: root.accentColor
                selectedTextColor: "#ffffff"
                font.pixelSize: 14
                /* 整数档：超出范围的字根本打不进去（和以前那个 SpinBox 一样） */
                validator: root.inputIsInt ? intValidator : null

                IntValidator {
                    id: intValidator

                    bottom: root.inputMin
                    top: root.inputMax
                }

                onTextChanged: root.inputValue = text
                // 回车 = 确定（第一个按钮），和原生那个对话框的习惯对齐
                onAccepted: root.answer(0)
            }
        }

        Row {
            anchors.right: parent.right
            spacing: 8
            topPadding: 4

            Repeater {
                model: root.buttons

                delegate: Rectangle {
                    id: btn

                    required property var modelData
                    required property int index

                    width: Math.max(86, btnLabel.implicitWidth + 26)
                    height: 30
                    radius: 4
                    color: mouse.pressed ? root.pressedColor
                                         : (mouse.containsMouse ? root.hoverColor
                                                                : (btn.modelData.primary ? Theme.c("#484c50", Theme.light) : "transparent"))
                    border.color: btn.modelData.primary ? root.accentColor : root.borderColor

                    Text {
                        id: btnLabel
                        anchors.centerIn: parent
                        text: btn.modelData.label
                        color: root.textColor
                        font.pixelSize: 13
                    }

                    MouseArea {
                        id: mouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.answer(btn.index)
                    }
                }
            }
        }
    }
}
