pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import SmartClip.Globals 1.0

/*
 * 关闭键那个问句：完全退出，还是收进托盘。
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
 * 行为：Esc / 点卡片外面 = 取消（点外面这一下按 popups 的老规矩只负责收起本卡片，
 * 和项目里菜单的行为一致）。
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

    width: 420
    /* 高度交给内容 + padding 自己算（Popup 的隐式尺寸就是这么来的） */
    implicitHeight: contentColumn.implicitHeight + padding * 2

    modal: false
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    popupType: Popup.Window
    padding: 18
    margins: 0

    background: Rectangle {
        color: root.cardColor
        border.color: root.borderColor
        radius: 10
    }

    /* 弹出时贴着宿主窗口居中（和原来的对话框一致） */
    function openCentered() {
        if (parent) {
            x = Math.round((parent.width - width) / 2)
            y = Math.round((parent.height - height) / 2)
        }
        open()
    }

    contentItem: Column {
        id: contentColumn
        spacing: 10

        Text {
            width: contentColumn.width
            text: qsTr("完全退出，还是收进托盘？")
            color: root.textColor
            font.pixelSize: 15
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            width: contentColumn.width
            text: qsTr("收进托盘：程序继续运行，托盘图标右键能截图，截图快捷键也还能用。\n"
                       + "完全退出：所有功能停止（截图、剪贴板都不再用）。")
            color: root.mutedColor
            font.pixelSize: 13
            lineHeight: 1.25
            wrapMode: Text.WordWrap
        }

        Row {
            anchors.right: parent.right
            spacing: 8
            topPadding: 4

            component AskButton: Rectangle {
                id: btn
                property string label: ""
                property bool primary: false
                width: Math.max(86, btnLabel.implicitWidth + 26)
                height: 30
                radius: 4
                color: mouse.pressed ? root.pressedColor
                                     : (mouse.containsMouse ? root.hoverColor
                                                            : (btn.primary ? "#484c50" : "transparent"))
                border.color: btn.primary ? root.accentColor : root.borderColor

                Text {
                    id: btnLabel
                    anchors.centerIn: parent
                    text: btn.label
                    color: root.textColor
                    font.pixelSize: 13
                }

                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: btn.clicked()
                }

                signal clicked()
            }

            AskButton {
                label: qsTr("收进托盘")
                primary: true
                onClicked: {
                    root.close()
                    Win.hideToTray()
                }
            }

            AskButton {
                label: qsTr("完全退出")
                onClicked: {
                    root.close()
                    Win.closeWindow()
                }
            }

            AskButton {
                label: qsTr("取消")
                onClicked: root.close()
            }
        }
    }
}
