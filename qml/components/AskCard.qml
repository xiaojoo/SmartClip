pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

/*
 * 一块"问一句"的小卡片：标题 + 说明 + 一排按钮。
 *
 * 界面上所有要打断用户的地方都用它 —— 关闭窗口（完全退出 / 收进托盘）、
 * 有未保存改动（保存 / 不保存 / 取消）、删除之类的确认（确定 / 取消）、
 * 出错提示（知道了）。四处的长相和交互只有这一份（用法见 Main.qml 里的
 * quitAsk / saveAsk / confirmAsk / noticeAsk）。
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
 * 还留在 QtWidgets 那边的只剩"要用户敲字"的输入框（重命名 / 转到行 /
 * 参考线列，见 EditorController 的 askText 等）和文件对话框。
 *
 * 行为：Esc / 点卡片外面 = 不作答（answered(-1)），由调用方当成"取消"处理。
 */

Popup {
    id: root

    readonly property color cardColor:   "#3c3f41"
    readonly property color borderColor: "#4b4d4f"
    readonly property color textColor:   "#e8e8e8"
    readonly property color mutedColor:  "#9aa0a6"
    readonly property color hoverColor:  "#46484a"
    readonly property color pressedColor: "#2f3234"
    readonly property color accentColor: "#4c96d8"

    /* 标题（加粗那一行）和说明（可以带 \n） */
    property string title: ""
    property string body: ""

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
        settled = false
        openCentered()
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
        answered(index)
    }

    /* Esc / 点外面：没作答，收起来也得说一声，否则调用方一直在等 */
    onClosed: {
        if (!settled) {
            settled = true
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
                                                                : (btn.modelData.primary ? "#484c50" : "transparent"))
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
