import QtQuick

/*
 * M1 spike 的 GPU 内容层（根是 Item —— 它被 QQuickView 包进窗口，不是自己当 Window）。
 * 长得和 SmartClip 那层一样：深色底 + 左边一条树 + 标题 + 几行正文。
 * 配套说明见 spike/gpu-host.cpp。
 */
Item {
    id: root

    Rectangle {
        anchors.fill: parent
        color: "#1e1f22"

        Rectangle {
            id: tree
            width: 264
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            color: "#25262a"

            Repeater {
                model: 26
                Rectangle {
                    y: 40 + index * 26
                    width: parent.width
                    height: 24
                    color: index === 3 ? "#2f5fa8" : "transparent"
                    Text {
                        x: 14
                        text: "2356" + index + ".md"
                        color: "#c8c8c8"
                        font.pixelSize: 13
                    }
                }
            }
        }

        Rectangle { width: 1; anchors.top: parent.top; anchors.bottom: parent.bottom; x: tree.width; color: "#4b4d4f" }

        Text {
            id: title
            x: tree.width + 36
            y: 34
            text: "M1 spike — GPU 内容层"
            color: "#f2f2f2"
            font.pixelSize: 34
            font.bold: true
        }

        Repeater {
            model: 14
            Text {
                x: title.x
                y: 100 + index * 26
                text: "第 " + index + " 行：这一行用来撑内容，看看最大化那一下它有没有跟着走。"
                color: "#b8b8b8"
                font.pixelSize: 15
            }
        }

        /* 右下角一个按钮：点它验"鼠标 / 键盘进不进得了 GPU 层"（它拿得到焦点就说明事件进得来） */
        Rectangle {
            id: hit
            width: 200
            height: 40
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 30
            radius: 6
            color: focusArea.activeFocus ? "#2f5fa8" : "#313335"
            border.color: "#4b4d4f"
            Text {
                anchors.centerIn: parent
                text: focusArea.activeFocus ? "GPU 层已拿到焦点" : "点我试试焦点"
                color: "#e8e8e8"
                font.pixelSize: 14
            }
            MouseArea {
                id: focusArea
                anchors.fill: parent
                focus: true
                onClicked: focusArea.forceActiveFocus()
            }
        }

        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 24
            color: "#25262a"
            Text {
                anchors.verticalCenter: parent.verticalCenter
                x: 12
                text: "spike 就绪  窗口 " + root.width + "x" + root.height
                color: "#8a8a8a"
                font.pixelSize: 12
            }
        }
    }
}
