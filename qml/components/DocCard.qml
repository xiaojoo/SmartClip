import QtQuick
import QtQuick.Controls
import SmartClip.Globals 1.0
import "../utils"

/*
 * 文档识别的进度 / 结果小卡片（主窗口右下角）。
 *
 * ===========================================================================
 * 为什么是一个独立的原生窗口（Window），不是场景里的浮层
 * ===========================================================================
 * 编辑区是**原生 QScintilla 子窗口**（QWidget::createWindowContainer，见
 * src/EditorViewItem.h），原生子窗口永远画在 QQuickWidget 的内容之上 ——
 * 这不是 z 值能改的事。卡片原来就是个场景内的 Rectangle，于是被编辑区整个盖住：
 * 用户看到的只有"从编辑区左边缘往左露出来的那一小条"，剩下的全没了（实测报的
 * 就是这个）。同样的坑在 DropdownMenu.qml / SettingsPanel.qml 的文件头里写着。
 *
 * 所以卡片必须自己是**一个独立的原生窗口**，由窗口管理器保证它盖在编辑区之上。
 *
 * 为什么用 Window 而不是那两个用的 Popup.Window：Popup.Window 的 x/y
 * **写不进去**。实测把算好的坐标写下去（甚至硬写 111,222）再读回来还是 0,0 ——
 * Qt 那边按父 item 管着它的位置，不接受外部指定。而菜单 / 设置面板是"跟着鼠标
 * 点弹出来"的，Popup 自带那套定位正合适；卡片要的是"贴主窗口右下角、跟着主窗口
 * 走"，得自己管几何，所以用 Window。
 *
 * 于是窗口标志也自己写：Qt.Tool（不进任务栏）+ 无边框。不要置顶 —— 和设置面板
 * 那条注释里说的一样，Qt::Popup 会在 Windows 上变成置顶窗口，把别的程序也压住。
 *
 * ===========================================================================
 * 为什么不做成"贴图 / 翻译卡片"那种 QWidget
 * ===========================================================================
 * 那种是"用户要摆在桌面上的独立卡片"，关了还记得位置、还能拖动、有圆角遮罩
 * 那一套。这个不一样：它是"跟着主窗口的一条进度提示"，主窗口去哪它去哪、
 * 主窗口一收它就没了。内容全是 QML（图标 / 进度条 / 按钮），用 QQuickWidget
 * 再包一层 QWidget 只是为了借位，不值当。
 *
 * 它**不发起识别**：Doc.enqueue() 由 Main.qml 调（拖拽 / 菜单都走那儿）。
 * 这里只把 Doc 的状态摆出来 —— 和 RecognitionCard 的分工一样。
 *
 * 数据全绑 Doc 这个单例：
 *   busy      在忙（转圈）
 *   status    一句话进展
 *   pending   还剩几个
 *   error     出错原因（红字）
 *   created   建出来的笔记路径（认完列出来）
 */
Window {
    id: root

    readonly property color cardColor:   "#2b2d30"
    readonly property color borderColor: "#3c4043"
    readonly property color textColor:   "#c8ccd0"
    readonly property color textBright:  "#e8eaed"
    readonly property color mutedColor:  "#8a9096"
    readonly property color accentColor: "#4c9aff"
    readonly property color errorColor:  "#e06c75"
    /* 按钮的 hover 底色（取消 / 打开那两个） */
    readonly property color hoverColor:  "#33373b"

    /*
     * 卡片宽度。
     *
     * 位置和"离右边 / 底边留多少"都不在这儿 —— 那些由 C++ 推过来（见 placeAt 和
     * DocImport::publishCardGeometry），那几个常量也都在 DocImport 里
     * （cardWidth / cardHeightHint / cardMargin / cardBottomGap），别在这儿再抄
     * 一份：抄两份改一个忘一个就错位。
     *
     * 这个 width 和那边 cardWidth() 是同一个数，改要一起改。
     */
    readonly property int cardWidth: 340

    /*
     * 什么都不用显示时整个收起来。
     * "认完了但一份都没成"也要显示（那是要报错的），所以判据是
     * "在忙 或 有错 或 有结果"，不是单看 busy。
     */
    readonly property bool showing: Doc.busy || Doc.error !== "" || Doc.created.length > 0

    /*
     * 最终该不该显示 = 有东西要显示 **且** 主窗口现在可用。
     *
     * 主窗口不可用（收进托盘 / 最小化）时要收掉：卡片是**独立的置顶窗口**，
     * 不跟着主窗口隐藏 —— 不管的话主窗口进了托盘，桌面上会孤零零留一张
     * "识别完成"的小卡片，点它还会把主窗口叫出来。
     *
     * 判据走 Doc.windowUsable 而不是直接读 parent.visible：自检里主窗口是故意
     * 不显示的，那时候"不可见"是正常的（见 DocImport::windowUsable 的说明）。
     */
    readonly property bool shouldShow: showing && Doc.windowUsable

    /*
     * 窗口标志：不进任务栏（Qt.Tool）、没有系统边框。
     *
     * **不置顶**：置顶会压住别的程序（设置面板那条注释里说清了 Qt::Popup 在
     * Windows 上就是置顶的）。它是"主窗口的一块"，跟着主窗口就够了。
     */
    flags: Qt.Tool | Qt.FramelessWindowHint
    /* 底色由下面那个 Rectangle 自己画，窗口这层留透明 */
    color: "transparent"
    /*
     * 该显示就显示（不再用 Popup 的 open/close —— 见文件头为什么不用 Popup）。
     * 两个都写上：`visible` 是窗口自己的，`shouldShow` 是驱动它的逻辑值（自检
     * 量后者 —— visible 到生效之间有一拍）。
     */
    visible: shouldShow

    width: cardWidth
    /* 高度跟着内容长（Column 的隐式高度 + 上下各 12 的边距） */
    height: content.implicitHeight + 24

    /*
     * 位置：**屏幕坐标**，由 C++ 推过来（见 Main.qml 的 Connections 和
     * DocImport::publishCardGeometry）。
     *
     * 这里栽过三次，每次都是"自检绿了但眼睛一看不对"，所以把结论写清楚：
     *
     * 1) Popup.Window + `x: parent.x + …` —— x/y **写不进去**（算好的坐标写下去、
     *    读回来是 0,0），卡片贴在左上角。
     * 2) Window + 在 reposition() 里**赋值** —— 赋值会把绑定打断，主窗口一移动
     *    就再没人重算，卡片钉在原地不动。
     * 3) Window + 绑定 `window.x + window.width - …` —— **坐标系不对**：QML 那个
     *    ApplicationWindow 的 x/y 和宿主 QWidget 的 geometry 不是一套（宿主摆在
     *    (300,160) 时 QML 读出来 x=0），算出来是屏幕中间。
     *
     * 所以位置不由 QML 算，由 C++ 那边（唯一知道宿主真实几何的地方）推过来，
     * 这里只负责摆。`placeAt` 用赋值是有意的 —— 这个值就该被外部驱动。
     */
    x: 0
    y: 0

    /*
     * C++ 说"卡片该在屏幕的 (px,py)"（见 DocImport::publishCardGeometry）。
     *
     * 只挪位置、不动尺寸 —— 高度是内容决定的，C++ 那边给的 y 是按估算高度
     * （118）算的，这里再按**实际**高度校一次，卡片底边才真的贴在状态栏上方。
     */
    property int placeY: 0
    function placeAt(px, py) {
        root.x = px
        placeY = py
        root.y = py - (root.height - 118)
    }
    /* 内容长高了（多一条笔记），y 也要跟着改（见 placeAt 的说明） */
    onHeightChanged: root.placeAt(root.x, root.placeY)

    /* 该显示就开、不该显示就收（开了就不再重复 open，会闪） */
    onShouldShowChanged: syncVisibility()
    /* 组件建出来时可能已经是"该显示"的状态 */
    Component.onCompleted: syncVisibility()

    /*
     * 摆位 / 收起来。
     *
     * `visible` 已经绑在 shouldShow 上了，为什么还要这个函数：
     *
     *   * **摆位**：x/y 得在主窗口几何定下来之后算（而且卡片高度要等内容量完），
     *     所以推到下一轮（Qt.callLater）再摆。
     *   * 内容变了（比如又多了一条笔记）时高度会变，得重新贴右下角。
     *
     * 踩过的坑记在这儿：一开始用 Popup.Window，指望它的 x/y 能干这事 ——
     * 结果**写不进去**（算好的 1104,830 写下去、读回来是 0,0，硬写 111,222 也
     * 一样），卡片就一直贴在左上角。换成 Window 之后 x/y 归自己管，才算数。
     */
    function syncVisibility() {
        /* 位置由 C++ 那边推（见 placeAt），这里只负责显示 / 收起 */
    }

    Connections {
        target: Doc
        function onStateChanged() { root.syncVisibility() }
        function onCreatedChanged() { root.syncVisibility() }
        function onWindowUsableChanged() { root.syncVisibility() }
    }

    /* 主窗口动了 / 改大小了由 C++ 那边推过来（见 main.cpp 的 cardGeo 定时器） */

    /* 底：卡片本体（圆角 + 边框 + 深色底） */
    Rectangle {
        anchors.fill: parent
        color: root.cardColor
        radius: 8
        border.width: 1
        border.color: root.borderColor
    }

    Column {
        id: content
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 12
        spacing: 8

        /* ---- 标题行：在干什么 + 关闭 ---- */
        Item {
            width: parent.width
            height: 20

            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 24
                text: Doc.busy ? "正在识别文档"
                               : (Doc.error !== "" ? "识别出错了" : "识别完成")
                color: (Doc.error !== "" && !Doc.busy) ? root.errorColor : root.textBright
                font.pixelSize: 12
                font.bold: true
                elide: Text.ElideRight
            }

            /* 关闭：把状态清掉（卡片跟着收起来） */
            Item {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                visible: !Doc.busy

                AppIcon {
                    anchors.centerIn: parent
                    provider: cardIcons
                    kind: "close"
                    size: 12
                    tint: closeHit.containsMouse ? root.textBright : root.mutedColor
                }
                MouseArea {
                    id: closeHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Doc.clearResult()
                }
            }
        }

        /* ---- 状态那句 ---- */
        Text {
            width: parent.width
            visible: Doc.status !== ""
            text: Doc.status
            color: root.textColor
            font.pixelSize: 11
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        }

        /*
         * ---- 进展条 ----
         *
         * 进度是"认完几个 / 一共几个"——一份文档要几秒到几十秒，没有更细的
         * 进展可报（脚本是黑盒，它不往回报）。所以这里是个按份数走的条。
         */
        Item {
            width: parent.width
            height: 3
            visible: Doc.busy && Doc.total > 0

            Rectangle {
                anchors.fill: parent
                radius: 1.5
                color: "#1e2022"
            }
            Rectangle {
                height: parent.height
                radius: 1.5
                color: root.accentColor
                width: parent.width * Math.max(0.04,
                                               (Doc.total - Doc.pending) / Math.max(1, Doc.total))
                Behavior on width { NumberAnimation { duration: 180 } }
            }
        }

        /* ---- 出错原因（红字） ---- */
        Text {
            width: parent.width
            visible: Doc.error !== ""
            text: Doc.error
            color: root.errorColor
            font.pixelSize: 11
            wrapMode: Text.WrapAtWordBoundaryOrAnywhere
            maximumLineCount: 4
            elide: Text.ElideRight
        }

        /* ---- 认完的清单 ---- */
        Column {
            width: parent.width
            spacing: 3
            visible: !Doc.busy && Doc.created.length > 0

            Repeater {
                model: Math.min(Doc.created.length, 3)

                delegate: Text {
                    required property int index
                    width: parent.width
                    text: "· " + Cmd.fileNameOf(Doc.created[index])
                    color: root.mutedColor
                    font.pixelSize: 11
                    elide: Text.ElideMiddle
                }
            }

            Text {
                width: parent.width
                visible: Doc.created.length > 3
                text: "…还有 " + (Doc.created.length - 3) + " 份"
                color: root.mutedColor
                font.pixelSize: 11
            }
        }

        /* ---- 动作行 ---- */
        Row {
            spacing: 8
            visible: Doc.busy || Doc.created.length > 0

            /* 取消（只在忙的时候有） */
            Rectangle {
                width: 62
                height: 22
                radius: 4
                visible: Doc.busy
                color: cancelHit.containsMouse ? root.hoverColor : "transparent"
                border.width: 1
                border.color: root.borderColor

                Text {
                    anchors.centerIn: parent
                    text: "取消"
                    color: cancelHit.containsMouse ? root.textBright : root.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: cancelHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Doc.cancel()
                }
            }

            /* 打开刚建出来的笔记 */
            Rectangle {
                width: 88
                height: 22
                radius: 4
                visible: !Doc.busy && Doc.created.length > 0
                color: openHit.containsMouse ? "#34404d" : "transparent"
                border.width: 1
                border.color: openHit.containsMouse ? root.accentColor : root.borderColor

                Text {
                    anchors.centerIn: parent
                    text: "打开笔记"
                    color: openHit.containsMouse ? root.accentColor : root.textColor
                    font.pixelSize: 11
                }
                MouseArea {
                    id: openHit
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: Doc.openLast()
                }
            }
        }
    }

    /* 图标（关闭那个叉）：AppIcon 要一个 provider 才画 */
    IconProvider { id: cardIcons }
}
