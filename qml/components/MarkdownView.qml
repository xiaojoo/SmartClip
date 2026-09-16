pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

/*
 * Markdown 预览（只读渲染）。
 *
 * 用途：打开一份 .md 时，可以把它当**文章**看，而不是当代码看
 * （见 EditorArea 正文那几页，以及 Main.qml 的 toggleMarkdownPreview）。
 *
 * 为什么渲染在 C++ 里做（Renderer 传进来的是 HTML，不是原文）：
 *   Qt 的 markdown 渲染器就在 C++ 侧（QTextDocument::setMarkdown），QML 里
 *   那个 Text.MarkdownText 只认 Markdown 的一个子集（表格、行内代码、
 *   删除线都不支持），拿它渲染出来的文章会缺东西。所以原文交给
 *   EditorController::markdownHtml()，转成一份**受控的小 HTML** 再喂给这里的
 *   TextEdit（RichText）—— 格式、配色都在那一份里定死（见 .cpp 的 kMarkdownCss）。
 *
 * 为什么是只读的 TextEdit，而不是 TextArea：
 *   预览要的就一件事 —— "选中一段复制走"，而这件事不需要 Control 那层壳。
 *   TextArea 的代价是它从 Qt 6.9 起**自带一套右键菜单**（Fusion / Basic 的
 *   TextArea.qml 里写着 `ContextMenu.menu: TextEditingContextMenu`），要关
 *   只能往 `ContextMenu.menu` 上挂东西。挂一个空 `Menu { }` 是关不掉的：
 *   那块空面板照样会 popup —— Fusion 的 Menu 底是个写死 200x20 的矩形
 *   （见 Fusion/Menu.qml 的 background：implicitWidth: 200 / implicitHeight: 20，
 *   颜色取 palette.base），配的是**样式自己那份浅色调色板**。实测表现就是
 *   用户在预览里点一下右键，界面上多出一块白方块（用户报的就是这个）。
 *
 *   TextEdit 是纯 Item，**根本没有 ContextMenu 这回事**，右键交给下面那个
 *   MouseArea 报给 Main.qml（真正要弹的那份菜单在 openPreviewContextMenu，
 *   程序自绘、深色、带图标列）。选它还有两样白拿的：
 *     * selectByMouse：拖选、双击选词、Ctrl+C 全都在；
 *     * selectedText：选中那段文字直接读，不用再从 selectionStart /
 *       selectionEnd 上切一刀（TextArea 那侧得 getText()）。
 *   那个 `Text` Item 不行：它没有 selectByMouse（实测加上这一句会直接让整个
 *   EditorArea 加载失败 —— "Cannot assign to non-existent property
 *   selectByMouse"）。
 *
 * 这里**不做**编辑：要改内容请切回源码（Ctrl+Shift+V），改完预览会跟着刷新。
 */
Rectangle {
    id: root

    /*
     * 已经渲染好的 HTML（由 Main.qml 从 Cmd.markdownHtml(path, source) 取来）。
     * 空串 = 还没渲染出来（或原文本来就是空的），下面显示那句提示。
     */
    property string html: ""

    /* 渲染结果为空时的提示语（空文件 / 还没渲染出来） */
    property string emptyHint: "没有可预览的内容"

    /* 点了一个链接（Main.qml 接住，交给系统默认程序打开） */
    signal linkActivated(string link)

    /*
     * 在预览里按了右键（Main.qml 接住，弹**本程序自己那套**下拉菜单）。
     *
     * x / y 是**场景坐标**（= 窗口内容区坐标）：和编辑区那条右键同一个口径
     * （见 EditorViewItem::contextMenuRequested），Main.qml 拿它去 openAtPoint。
     */
    signal contextMenuRequested(real x, real y)

    /* 预览里选中的那段文字（TextEdit 自己有 selectedText，见文件头） */
    readonly property string selectedText: body.selectedText

    /* 全选（只读控件也能全选，选完可以复制） */
    function selectAll() {
        body.select(0, body.length)
    }

    readonly property color textColor: "#d6d7da"

    color: "transparent"
    clip: true

    ScrollView {
        id: scroller

        anchors.fill: parent
        anchors.margins: 2
        /* 左右多留一点：正文一行贴到边上很难读 */
        contentWidth: availableWidth
        clip: true
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        TextEdit {
            id: body

            /* 自检靠这个名字找预览正文、核它是不是裸 TextEdit（见 src/SelfTestTools.cpp） */
            objectName: "markdownBody"

            /*
             * 只读 + 可以用鼠标选（见文件头那段：这就是选它不选 Text 的原因）。
             * readOnly 的 TextEdit 不接受键盘输入，但选中和复制照样能用。
             */
            readOnly: true
            selectByMouse: true
            persistentSelection: true

            text: root.html
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            color: root.textColor
            /*
             * 选中那段的底色 / 字色。
             *
             * 原来挂在 TextArea 上时这两个值来自 Control 的 palette（Fusion 的
             * TextArea.qml 里写的 `selectionColor: control.palette.highlight`）。
             * 裸 TextEdit 不走 Control 那套，这里照同一份调色板写死 —— 深色界面
             * 里选中一段不能变成系统那种浅蓝底黑字。
             */
            selectionColor: root.palette.highlight
            selectedTextColor: root.palette.highlightedText
            /* 字号在渲染出来的 HTML 里是相对的（em），基准字号在这里 */
            font.pixelSize: 14
            /* 内边距：四周留白，像一篇文章而不是一块日志 */
            leftPadding: 24
            rightPadding: 24
            topPadding: 18
            bottomPadding: 18

            /*
             * 点链接：RichText 里的 <a> 被点中时 TextEdit 会把 href 报在这里
             * （只读 + 有链接时它照样处理点击）。
             */
            onLinkActivated: (link) => root.linkActivated(link)
        }
    }

    /*
     * 右键按下就到这一层来处理。
     *
     * 盖在正文上面、只收右键（acceptedButtons: Qt.RightButton）——
     * 左键和滚轮全部穿下去给 TextEdit 自己（"没写 acceptedButtons 的
     * MouseArea 不吃那些键"这件事，EditorArea 的标签栏注释里也踩过）。
     */
    MouseArea {
        id: rightClickCatcher

        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        /*
         * hover 交给下面的 TextEdit 自己：这一层只吃右键按下，鼠标移到链接上
         * 那个"手型"光标还是正文控件的 hover 处理画出来的（这里开 hoverEnabled
         * 会把那些事件截住）。
         */
        hoverEnabled: false

        onClicked: (mouse) => {
            /*
             * 把本层的坐标换到窗口那一层（和编辑区那条路同一个口径）。
             * anchor 传 null，所以 Main.qml 直接按宿主坐标用这两个数。
             */
            var p = rightClickCatcher.mapToItem(null, mouse.x, mouse.y)
            root.contextMenuRequested(p.x, p.y)
            mouse.accepted = true        /* 吃掉：别让下面再冒一次原生菜单 */
        }
    }

    /* 没内容 / 还没渲染出来时那块提示 */
    Text {
        anchors.centerIn: parent
        width: Math.min(parent.width - 60, 420)
        visible: root.html === ""
        text: root.emptyHint
        color: "#7d7d7d"
        font.pixelSize: 13
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WordWrap
    }
}
