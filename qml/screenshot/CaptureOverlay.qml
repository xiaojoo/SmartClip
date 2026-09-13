pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Layouts
import "../utils"
import SmartClip.Globals 1.0

/*
 * 截图选区窗口（由 src/Screenshot.cpp 用 QQuickWidget 装进一个铺满整块屏的
 * 置顶无边框窗口里）。
 *
 * 一屏之内把这四件事做完，不再单开一个"标注编辑器"窗口：
 *
 *   1) 拖动框选区域（点一下不放 = 整屏）；
 *   2) 在框里点一下落一条文字，就地打字（见下面的文字工具）；
 *   3) 工具条上选字号 / 颜色、拖动文字摆位置；
 *   4) 复制到剪贴板 / 存成图片 / 固定到桌面（贴图窗口，见 PinWindow）。
 *
 * 为什么合成不在这里做：最终那张图（剪贴板 / 文件 / 贴图三处共用）是 C++
 * 那边 QPainter 画的（Screenshot::compose），这里只负责**预览**。两边
 * 必须长得一样，所以：文字位置就是屏幕坐标、字号也是逻辑像素，
 * C++ 侧按同样的规则乘 DPR 画一遍。
 *
 * 坐标：本窗口铺在整块屏上，所以这里所有坐标都是"被截那块屏的屏幕坐标，
 * 逻辑像素"，(0,0) 就是屏的左上角。
 */
Rectangle {
    id: root

    color: "black"

    /* ---- 选区 ---- */
    /*
     * 选区（屏幕坐标，逻辑像素）。
     *
     * selAuto = 选区还是"整屏"这个默认值（用户拖过一次框选就成 false）。
     * 为什么要有这个标志：选区窗口现在是**预建复用**的（Screenshot::prewarm），
     * 每次抓屏先复位、再 show —— 复位那一刻窗口还没被摆到屏幕尺寸上，那时读
     * width/height 拿到的是 0，sel 就成了 (0,0,0,0)：工具条跑到左上角、
     * 复制出来是空图（实测就是这么翻的车）。所以复位只把 selAuto 置 true，
     * 真正的整屏矩形交给下面两个 onXxxChanged，等尺寸落定之后再填。
     */
    property bool selAuto: true
    property rect sel: Qt.rect(0, 0, width, height)
    readonly property bool selReady: sel.width >= 8 && sel.height >= 8

    function syncAutoSel() {
        if (root.selAuto)
            root.sel = Qt.rect(0, 0, root.width, root.height)
    }

    onWidthChanged: root.syncAutoSel()
    onHeightChanged: root.syncAutoSel()

    /* ---- 标注：当前工具 / 字号 / 线宽 / 颜色 / 选中和正在打字的那一条 ---- */
    /*
     * 当前标注工具：
     *   ""        不画（鼠标用来框选）
     *   "text"    点一下落文本框（按住拖出大小）
     *   "arrow"   拖一条箭头
     *   "pencil"  按住自由画
     */
    property string tool: ""
    readonly property bool textTool: tool === "text"
    property int fontSize: 16
    /* 箭头 / 铅笔的线宽（工具条上那两个键在形状工具下改的就是它） */
    property int strokeWidth: 3
    property color annotColor: "#ff3b30"
    /*
     * selected = 选中（蓝框 + ✕，字号 / 颜色作用在它身上）
     * editing  = 正在打字（TextEdit 有焦点）
     *
     * 分成两个是为了**拖动的手感**：拖动只该动几何，不该顺手去切
     * TextInput 的可见性和焦点（按一下只改 selected，见拖动区的 onPressed）。
     * editing 一定落在 selected 上（进编辑的都是先选中的那一条）。
     */
    property int selected: -1
    property int editing: -1

    /* 正在画的形状（松开鼠标才进 shapes；拖动期间每帧只重绘画布） */
    property bool shapeDrag: false
    property string drawKind: ""
    property real drawX1: 0
    property real drawY1: 0
    property real drawX2: 0
    property real drawY2: 0
    /* 铅笔的点：扁平数组 [x,y,x,y,…]（屏幕坐标），和 C++ 那边一个格式 */
    property var drawPts: []

    /* 新增过的标注（"text" / "shape"），Ctrl+Z / 撤销按钮按它退 */
    property var history: []

    /*
     * 有没有东西可撤 —— 拿它绑"撤销"按钮，**别直接绑 history.length**。
     *
     * 这是个踩过的坑：history 是 JS 数组，`history.push(...)` 是原地改，
     * QML 不会因此重算绑定 —— 按钮的 enabled 一直停在启动时那一次求值
     * （长度 0 → 灰的），画了再多东西也一直灰着、点不动。
     * 所以下面所有增删都改成"整体重新赋值"（concat / slice），
     * 属性一换，绑定才会跟着重算。
     */
    readonly property bool undoAvailable: history.length > 0

    /*
     * 画好的形状（箭头 / 铅笔），一条一项：
     *   { kind, x1, y1, x2, y2, stroke, color, pts }
     *
     * 用**普通 JS 数组**而不是 ListModel：铅笔那一笔的点列是个数组，而
     * ListModel 的动态角色存数组会丢（实测：存进去 6 个数，读回来 0 个）。
     * 这里反正只有画布在读它 —— 画布是立即模式、每次 onPaint 自己遍历，
     * 不需要模型的变更通知（改完显式 requestPaint() 就行）。
     */
    property var shapes: []

    /* ---- 交互中间量 ---- */
    property bool dragging: false
    property bool hinted: true
    property real pressX: 0
    property real pressY: 0

    /*
     * 文字工具"拖出文本框"用：按下时先落一个空框（boxIndex），
     * 左键不松就一直按鼠标位置改它的 tx/ty/bx/bh，松手才落定并进打字。
     */
    property bool boxDrag: false
    property int boxIndex: -1
    property real boxX: 0
    property real boxY: 0

    /* 和界面其它地方同一支强调蓝（见 Main.qml 的 accentColor） */
    readonly property color accent: "#4c96d8"

    /* 文本框四个角上的图标（旋转 / 缩放 / 箭头），见 qml/utils/IconProvider.qml */
    IconProvider { id: icons }

    /* 拖一条文字时，至少留这么多像素在选区里（见拖动区的 min/max） */
    readonly property real keepVisible: 24

    /*
     * 选区那圈虚线框（和工具条、提示条）现在能不能画。
     *
     * 为什么要有这个：抓屏到选区窗口真的稳定显示，中间隔着几十毫秒（抓屏 +
     * 4K 底图上传）。这期间窗口可能已经 show() 出来了，而框选默认就是**整屏**
     * —— 于是整块屏幕会先亮起一圈"全屏选区"的边框，等窗口稳定后才有内容。
     * 用户取消截图时看到的"全屏出现了一下边框"就是这一圈（用户报的"取消截图后
     * 全屏出现了一下边框，取消不需要"）。
     *
     * 所以复位时先把它关掉，等窗口真的稳住了（C++ 那边 show() 之后发
     * settleOverlay）再打开。取消那一下子这圈框从头到尾没画过，一帧都不闪；
     * 正常截图只是晚个一两帧出现边框，用户正在框选时根本看不出来。
     */
    property bool overlayReady: false

    /* 文本框的最小宽 / 高（拖折行宽时用，避免拖成一条线） */
    readonly property real minBoxW: 40
    readonly property real minBoxH: 20

    /* 屏幕坐标 -> 选区局部坐标（文字项的 x/y 就是这么摆的） */
    function localX(screenX) { return screenX - root.sel.x }
    function localY(screenY) { return screenY - root.sel.y }

    /* 这个点在不在选区里（文字工具下"点在框里才落字"用它判断） */
    function insideSel(x, y) {
        return x >= root.sel.x && x <= root.sel.x + root.sel.width
            && y >= root.sel.y && y <= root.sel.y + root.sel.height
    }

    /*
     * 文字工具按下：先落一个"还没定尺寸"的空框。
     *
     * 这时候**不**进打字态（editing 留到松开左键那一下）—— 因为按下之后
     * 往往还要拖着把框拉大：拖动期间每一帧只改这个框的 tx/ty/bx/bh，
     * 光标进去打字是松手之后的事（见下面 rubber 的 boxDrag 那一段）。
     */
    function beginTextBox(x, y) {
        root.commitEditing()
        textModel.append({ tx: x, ty: y, txt: "", fsize: root.fontSize,
                           fcolor: root.annotColor.toString(), bx: 0, bh: 0, rot: 0 })
        root.selected = textModel.count - 1
        root.history = root.history.concat(["text"])
        return root.selected
    }

    /*
     * 把屏幕上的位移投影到框自己的一条轴上。
     *
     * 框转过角度之后，"横着拖"在屏幕上就不是横的了：改宽度要投影到框的
     * x 轴（vertical = false），改高度要投影到框的 y 轴（true）。
     */
    function axisDelta(rot, dx, dy, vertical) {
        const rad = rot * Math.PI / 180
        return vertical ? (-dx * Math.sin(rad) + dy * Math.cos(rad))
                        : (dx * Math.cos(rad) + dy * Math.sin(rad))
    }

    /*
     * 三条边手柄的拖动算法（界面和自检共用这一份，别各写一遍）。
     *
     *   edge   "left" / "right" / "bottom"
     *   start  按下那一刻：框宽高（w/h）、左上角（tx/ty）、指针位置（x/y，root 坐标）
     *   at     当前指针位置（root 坐标）
     *
     * 增量先投影到框自己的轴上（转过角度之后，屏幕上这条边不是横/竖的），
     * 再按边决定改什么：
     *   right  只改框宽，左边不动
     *   left   改框宽，同时把 tx/ty 挪正 —— 右上角那一边不动
     *   bottom 只改最小高度（bh），上边不动
     */
    function dragEdge(index, edge, start, at) {
        if (index < 0 || index >= textModel.count)
            return
        /* 上下两条边量的是框的 y 轴，左右两条量 x 轴（见 axisDelta） */
        const vertical = edge === "bottom" || edge === "top"
        const along = root.axisDelta(textModel.get(index).rot,
                                     at.x - start.x, at.y - start.y, vertical)

        if (edge === "bottom") {
            /* bh 是**最小高度**：文字比它高时框还会自己往下长 */
            const h = Math.max(root.minBoxH, Math.round(start.h + along))
            if (h !== textModel.get(index).bh)
                textModel.setProperty(index, "bh", h)
            return
        }

        if (edge === "right") {
            const w = Math.max(root.minBoxW, Math.round(start.w + along))
            if (w !== textModel.get(index).bx)
                textModel.setProperty(index, "bx", w)
            return
        }

        if (edge === "top") {
            /* 上边往下拖（along > 0）= 变矮；同时把 ty 挪正，下边那条边不动 */
            const moved = Math.min(along, start.h - root.minBoxH)
            const rad = textModel.get(index).rot * Math.PI / 180
            /* 框内 y 轴在屏幕上是 (-sin, cos) */
            textModel.setProperty(index, "tx", start.tx - moved * Math.sin(rad))
            textModel.setProperty(index, "ty", start.ty + moved * Math.cos(rad))
            const h = Math.round(start.h - moved)
            if (h !== textModel.get(index).bh)
                textModel.setProperty(index, "bh", h)
            return
        }

        /* left：往左拖（along < 0）变宽；最窄夹在 minBoxW */
        const moved = Math.min(along, start.w - root.minBoxW)
        const rad = textModel.get(index).rot * Math.PI / 180
        textModel.setProperty(index, "tx", start.tx + moved * Math.cos(rad))
        textModel.setProperty(index, "ty", start.ty + moved * Math.sin(rad))
        const w = Math.round(start.w - moved)
        if (w !== textModel.get(index).bx)
            textModel.setProperty(index, "bx", w)
    }

    /*
     * 左下角手柄：**整体放大 / 缩小** —— 字号和框一起按比例变。
     *
     * growth 是"往左下（向外）拖了多少像素"：往下 dy > 0，往左 dx < 0
     * （除以 √2 让斜着拖的 100px 就算 100px）。手感和之前那版"拖角改字号"
     * 一样（往外拖 120px ≈ 放大一倍），区别是**框跟着一起缩**：
     * 定过宽 / 高的框按同一比例改 bx/bh；没定过的（0，跟着文字自动长）
     * 不用管，字号一大它自己会长。
     */
    function scaleBox(index, start, at) {
        if (index < 0 || index >= textModel.count || start.size <= 0)
            return
        const growth = ((at.y - start.y) - (at.x - start.x)) * Math.SQRT1_2
        const size = Math.max(8, Math.min(96, Math.round(start.size * (1 + growth / 120))))
        if (size === textModel.get(index).fsize)
            return
        textModel.setProperty(index, "fsize", size)
        const factor = size / start.size
        if (start.bx > 0)
            textModel.setProperty(index, "bx",
                                  Math.max(root.minBoxW, Math.round(start.bx * factor)))
        if (start.bh > 0)
            textModel.setProperty(index, "bh",
                                  Math.max(root.minBoxH, Math.round(start.bh * factor)))
        /* 和 A− / A+ 一致：下一条的默认字号也跟着走 */
        root.fontSize = size
    }

    /* 右下角手柄：宽高分开拖（"往右下拖大"最直觉的那一个） */
    function freeResize(index, start, at) {
        if (index < 0 || index >= textModel.count)
            return
        const rot = textModel.get(index).rot
        const dx = root.axisDelta(rot, at.x - start.x, at.y - start.y, false)
        const dy = root.axisDelta(rot, at.x - start.x, at.y - start.y, true)
        textModel.setProperty(index, "bx", Math.max(root.minBoxW, Math.round(start.w + dx)))
        textModel.setProperty(index, "bh", Math.max(root.minBoxH, Math.round(start.h + dy)))
    }

    /*
     * 左下角手柄：挪位置（框身留给正文，点一下要出打字光标、拖着要选字，
     * 所以"移动"单独一个手柄）。
     *
     * 可拖范围：**至少留 keepVisible 那么宽/高在选区里** —— 不是"整条都得
     * 待在选区里"（那样文字一宽就顶死、根本拖不动），也不是随便拖出去
     * （拖没了就再也点不回来）。
     */
    function moveBox(index, start, at) {
        if (index < 0 || index >= textModel.count)
            return
        const minTx = root.sel.x + root.keepVisible - start.w
        const maxTx = root.sel.x + root.sel.width - root.keepVisible
        const minTy = root.sel.y + root.keepVisible - start.h
        const maxTy = root.sel.y + root.sel.height - root.keepVisible
        const tx = Math.max(minTx, Math.min(maxTx, start.tx + (at.x - start.x)))
        const ty = Math.max(minTy, Math.min(maxTy, start.ty + (at.y - start.y)))
        textModel.setProperty(index, "tx", Math.round(tx))
        textModel.setProperty(index, "ty", Math.round(ty))
    }

    /*
     * 拖文本框：按"按下点 -> 当前鼠标"这个矩形改框，左上角取反方向那一侧
     * （所以往左上拖也成立），并夹在选区内。松手之前每一帧都走它 ——
     * 界面和自检走的是同一个函数，别在自检里另写一份算法。
     */
    function resizeBox(index, fromX, fromY, toX, toY) {
        if (index < 0 || index >= textModel.count)
            return
        const x2 = Math.max(root.sel.x, Math.min(root.sel.x + root.sel.width, toX))
        const y2 = Math.max(root.sel.y, Math.min(root.sel.y + root.sel.height, toY))
        const left = Math.min(fromX, x2)
        const top = Math.min(fromY, y2)
        const w = Math.abs(x2 - fromX)
        const h = Math.abs(y2 - fromY)
        textModel.setProperty(index, "tx", left)
        textModel.setProperty(index, "ty", top)
        /* 小到没意义的拖动当"没拖过"：回到跟着内容自动定尺寸 */
        textModel.setProperty(index, "bx", w >= root.minBoxW ? Math.round(w) : 0)
        textModel.setProperty(index, "bh", h >= root.minBoxH ? Math.round(h) : 0)
    }

    /*
     * 不带拖动的落字（自检那条路，也是"点一下不拖"的语义）：
     * 落一个自动宽的框并立刻进入打字。
     */
    function addText(x, y) {
        const index = root.beginTextBox(x, y)
        root.editing = index
        return true
    }

    /* 结束编辑：空串的那条直接删掉（见 addText 的说明） */
    function commitEditing() {
        if (root.editing < 0)
            return
        const index = root.editing
        root.editing = -1
        if (index >= 0 && index < textModel.count && textModel.get(index).txt === "")
            root.removeAt(index)
    }

    /*
     * 删一条，并把 selected / editing 两个下标挪正。
     *
     * 删掉的是前面的一条时，后面那些条的下标会整体往前挪一格 ——
     * 不跟着挪的话"选中的"就变成隔壁那条了（选中框和 ✕ 会跳到别的字上）。
     */
    function removeAt(index) {
        if (index < 0 || index >= textModel.count)
            return
        textModel.remove(index)
        if (root.editing === index)
            root.editing = -1
        else if (root.editing > index)
            root.editing--
        if (root.selected === index)
            root.selected = -1
        else if (root.selected > index)
            root.selected--
    }

    function removeEntry(index) { root.removeAt(index) }

    /* 旋转角度：夹到 (-180, 180]，快贴到 0 / ±90 / ±180 时吸一下，方便摆正 */
    function snapAngle(deg) {        let v = deg % 360
        if (v > 180)
            v -= 360
        if (v <= -180)
            v += 360
        const near = Math.round(v / 90) * 90
        if (Math.abs(v - near) < 4)
            v = near === 180 ? -180 : near
        return v
    }

    /* 选工具：再点同一个 = 收起（回到框选）；切走时先把正在打的字收掉 */
    function pickTool(name) {
        root.commitEditing()
        root.closeMenus()
        root.tool = (root.tool === name) ? "" : name
    }

    /* 字号：改"下一条的默认值"，也改选中的那一条 */
    function bumpFont(delta) {
        /* 基准取"选中的那一条"当前的字号（没有就取默认值），
           这样拖大字号的文字之后再点 A−/A+ 不会突然跳回 16 */
        const base = root.selected >= 0 ? textModel.get(root.selected).fsize : root.fontSize
        root.setFontSize(base + delta)
    }

    function setFontSize(v) {
        const size = Math.max(8, Math.min(96, Math.round(v)))
        root.fontSize = size
        if (root.selected >= 0)
            textModel.setProperty(root.selected, "fsize", size)
    }

    /* 线宽：箭头 / 铅笔用（范围窄一档，太粗的线在小图上很丑） */
    function bumpStroke(delta) {
        root.strokeWidth = Math.max(1, Math.min(12, root.strokeWidth + delta))
    }

    /* 形状工具在用吗（决定 − / + 和那个数字是线宽还是字号） */
    readonly property bool shapeTool: tool === "arrow" || tool === "pencil" || tool === "rect"

    /* 数字框里显示的当前值 */
    readonly property int sizeValue: shapeTool ? strokeWidth
                                               : (selected >= 0 ? textModel.get(selected).fsize
                                                                : fontSize)

    /* 那两个键的标签也跟着变，免得"改线宽"的时候还写着 A+ */
    readonly property string sizeDownLabel: shapeTool ? "细 −" : "A−"
    readonly property string sizeUpLabel: shapeTool ? "粗 +" : "A+"

    /* 数字框里的候选值（点一下弹出来直接挑） */
    readonly property var sizePresets: shapeTool
        ? [1, 2, 3, 4, 5, 6, 8, 10, 12]
        : [8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 40, 48, 64, 96]

    /* "更多颜色"那一格里摆的调色板（8 列，够日常用又不至于找不着） */
    readonly property var colorPalette: [
        "#ffffff", "#d6d7da", "#8b929e", "#4b4d4f", "#000000",
        "#ff3b30", "#ff6b35", "#ffd60a", "#30d158", "#0a84ff",
        "#5e5ce6", "#bf5af2", "#ff2d55", "#a2845e", "#64d2ff",
        "#32d74b", "#ff9f0a", "#ff375f", "#ac8e68", "#00c7be",
        "#66d4cf", "#7d7aff", "#d4a5a5", "#f2f2f7"
    ]

    /* 弹出的小面板（数字框 / 更多颜色），同一时刻只开一个 */
    property bool menuSize: false
    property bool menuColor: false

    /*
     * 正开着系统取色框（QtWidgets 模态框，里面是嵌套事件循环）。
     *
     * 这期间要把窗口级快捷键全让开：实测取色框开着按 Esc，先被这边的
     * Esc Shortcut 接住、把选区窗口关了 —— 对话框的父窗口一没，它直接
     * abort（弹"Microsoft Visual C++ Runtime Library"）。让开之后 Esc
     * 才轮到对话框自己处理。C++ 那侧 endCapture 也加了同样的闸门。
     */
    property bool pickingColor: false

    /* 快捷键总闸：打字 / 开着系统对话框的时候都让开 */
    readonly property bool shortcutsOn: editing < 0 && !pickingColor

    function closeMenus() {
        root.menuSize = false
        root.menuColor = false
    }

    /* 从数字框里挑了一个值 */
    function setSizeValue(v) {
        const n = Math.round(v)
        if (root.shapeTool)
            root.strokeWidth = Math.max(1, Math.min(12, n))
        else
            root.setFontSize(n)
    }

    /* 弹出面板上的"自定义…"：开系统取色框（C++ 那边 QColorDialog） */
    function pickCustomColor() {
        root.pickingColor = true
        const picked = Shot.pickColor(root.annotColor.toString())
        root.pickingColor = false
        if (picked !== "")
            root.setColor(picked)
        root.menuColor = false
    }

    /*
     * 工具条上那两个"− / +"：跟着当前工具走 —— 形状工具改线宽，
     * 其余情况（文字工具 / 正在改某条文字）改字号。
     */
    function bumpSize(delta) {
        if (root.shapeTool)
            root.bumpStroke(delta)
        else
            root.bumpFont(delta)
    }

    function setColor(color) {
        root.annotColor = color
        if (root.selected >= 0)
            textModel.setProperty(root.selected, "fcolor", color.toString())
    }

    /*
     * 落一个形状（松开鼠标那一下走这里）。太短的（手抖点一下）直接丢掉，
     * 免得留下一个看不见的小点。
     */
    function commitShape() {
        if (root.drawKind === "pencil") {
            if (root.drawPts.length >= 4) {
                root.shapes = root.shapes.concat([{ kind: "pencil", x1: 0, y1: 0, x2: 0, y2: 0,
                                                    stroke: root.strokeWidth,
                                                    color: root.annotColor.toString(),
                                                    pts: root.drawPts.slice(0) }])
                root.history = root.history.concat(["shape"])
            }
        } else if (root.drawKind === "arrow" || root.drawKind === "rect") {
            /* 两种都是"按下点 + 松开点"定形状；太短的（手抖点一下）丢掉 */
            if (Math.abs(root.drawX2 - root.drawX1) + Math.abs(root.drawY2 - root.drawY1) >= 6) {
                root.shapes = root.shapes.concat([{ kind: root.drawKind,
                                                    x1: root.drawX1, y1: root.drawY1,
                                                    x2: root.drawX2, y2: root.drawY2,
                                                    stroke: root.strokeWidth,
                                                    color: root.annotColor.toString(), pts: [] }])
                root.history = root.history.concat(["shape"])
            }
        }
        root.drawKind = ""
        root.drawPts = []
        shapeCanvas.requestPaint()
    }

    /*
     * 撤销：按 history 退最后加的那一条。history 里可能留着已经被删掉
     * （✕ 按钮 / 空文本框收掉）的记录，所以循环里跳过对不上号的。
     *
     * 出栈也用 slice 而不是 pop —— 原地改不触发绑定，"撤销"按钮会一直
     * 停在"还有东西可撤"的状态上（见 undoAvailable 的说明）。
     */
    function undoLast() {
        while (root.history.length > 0) {
            const kind = root.history[root.history.length - 1]
            root.history = root.history.slice(0, -1)
            if (kind === "shape" && root.shapes.length > 0) {
                root.shapes = root.shapes.slice(0, -1)
                shapeCanvas.requestPaint()
                return true
            }
            if (kind === "text" && textModel.count > 0) {
                root.removeAt(textModel.count - 1)
                return true
            }
        }
        return false
    }

    /* 交给 C++ 合成的那份数据（字段名和 Screenshot::compose 一一对应） */
    function textsData() {
        const out = []
        for (let i = 0; i < textModel.count; ++i) {
            const item = textModel.get(i)
            const box = textRepeater.itemAt(i)
            out.push({
                kind: "text",
                x: item.tx, y: item.ty,
                /* 框宽 / 框高：高是 QML 这边按折行算出来的，C++ 也要拿它
                   当旋转的原点（两边绕同一个"框中心"，转出来才重合） */
                w: box ? box.width : 0,
                h: box ? box.height : 0,
                rot: item.rot,
                text: item.txt, size: item.fsize, color: item.fcolor
            })
        }
        return out
    }

    /* 形状（箭头 / 铅笔）那份数据 */
    function shapesData() {
        return root.shapes.slice(0)
    }

    /*
     * 复制 / 保存 / 贴图用的**全部标注**：形状和文字合成一份，
     * 每项带 kind，C++ 那边按 kind 分派（见 Screenshot::compose）。
     * 顺序 = 先形状后文字，和屏幕上的图层一致（文字在上面）。
     */
    function annotationsData() {
        return root.shapesData().concat(root.textsData())
    }

    /*
     * 画一个形状（画布和"正在画的那一笔"共用；数要和 C++ 那边一致：
     * 头长 max(8, 3×线宽)、张角 ±25.7°）。
     */
    function paintShape(ctx, kind, x1, y1, x2, y2, pts, width, color) {
        ctx.strokeStyle = color
        ctx.fillStyle = color
        ctx.lineWidth = width
        ctx.lineCap = "round"
        ctx.lineJoin = "round"
        if (kind === "pencil") {
            if (!pts || pts.length < 4)
                return
            ctx.beginPath()
            ctx.moveTo(pts[0] - root.sel.x, pts[1] - root.sel.y)
            for (let i = 2; i + 1 < pts.length; i += 2)
                ctx.lineTo(pts[i] - root.sel.x, pts[i + 1] - root.sel.y)
            ctx.stroke()
            return
        }
        if (kind === "rect") {
            /* 方框：左上角取反方向那一侧，所以往哪个方向拖都行 */
            ctx.strokeRect(Math.min(x1, x2) - root.sel.x, Math.min(y1, y2) - root.sel.y,
                           Math.abs(x2 - x1), Math.abs(y2 - y1))
            return
        }
        const ax = x1 - root.sel.x
        const ay = y1 - root.sel.y
        const bx = x2 - root.sel.x
        const by = y2 - root.sel.y
        ctx.beginPath()
        ctx.moveTo(ax, ay)
        ctx.lineTo(bx, by)
        ctx.stroke()
        const angle = Math.atan2(by - ay, bx - ax)
        const head = Math.max(8, width * 3)
        ctx.beginPath()
        ctx.moveTo(bx, by)
        ctx.lineTo(bx - Math.cos(angle - Math.PI / 7) * head,
                   by - Math.sin(angle - Math.PI / 7) * head)
        ctx.lineTo(bx - Math.cos(angle + Math.PI / 7) * head,
                   by - Math.sin(angle + Math.PI / 7) * head)
        ctx.closePath()
        ctx.fill()
    }

    /*
     * 工具条四个按钮 / 快捷键 / 自检走同一份动作。
     * 复制 / 保存 / 贴图做完都直接收工（选区窗口关掉、主窗口放回来）。
     */
    function runAction(act) {
        root.commitEditing()
        if (act === "cancel") {
            /*
             * 取消走 Shot.cancelCapture()，别用 endCapture()：抓屏是延时的
             * （见 Screenshot::beginCapture），用户按完快捷键马上按 Esc 时选区
             * 窗口还藏着，那一下必须把**待抓的那次**掐掉，否则延时一到它照样
             * 铺满全屏 —— 就是"取消截图时全屏框闪现一次才关闭"。
             */
            Shot.cancelCapture()
            return
        }
        const items = root.annotationsData()
        if (act === "copy") {
            if (Shot.copyResult(root.sel, items))
                Shot.endCapture()
        } else if (act === "save") {
            if (Shot.saveResultAs(root.sel, items))
                Shot.endCapture()
        } else if (act === "pin") {
            Shot.pinResult(root.sel, items)
            Shot.endCapture()
        }
    }

    /*
     * 每次抓屏前把上一次的痕迹清干净。
     *
     * 选区窗口现在是**复用**的（预建一次一直留着，见 Screenshot::prewarm），
     * 所以这里得把标注、选区、工具、各种拖动中间状态都复位 ——
     * 少复位一样，上一次的箭头就会跟到这一次的截图里。
     */
    function resetForCapture(rect) {
        root.commitEditing()
        textModel.clear()
        root.shapes = []
        root.history = []
        /*
         * 选区由**调用方**（C++ 抓屏那侧）直接给定，不靠控件尺寸去猜。
         *
         * 为什么：选区窗口是预建复用的（见 Screenshot::prewarm），摆到屏幕尺寸
         * 之后 QQuickWidget 的布局是**延迟**生效的 —— 复位那一刻读 width/height
         * 拿到的还是预热时的旧尺寸（640x480 那种），于是第一两帧按"一个小方框"
         * 画出来：用户看到的就是"屏幕中间闪一下一个方框的轮廓"，第一次抓屏尤其
         * 明显（后面尺寸已经是对的）。传进来的 rect 是权威值，selAuto 保持 true
         * 只是为了让之后的 resize 同步（那时尺寸已经对了，同步出来的值一样）。
         */
        root.selAuto = true
        if (rect !== undefined && rect.width > 0 && rect.height > 0)
            root.sel = rect
        else
            root.syncAutoSel()

        root.selected = -1
        root.editing = -1
        root.tool = ""
        root.hinted = true
        root.dragging = false
        root.boxDrag = false
        root.boxIndex = -1
        root.shapeDrag = false
        root.drawKind = ""
        root.drawPts = []
        root.menuSize = false
        root.menuColor = false
        root.pickingColor = false
        /*
         * 先别画那圈选区边框（见 overlayReady 的说明）：等窗口稳定显示之后
         * C++ 会调 settleOverlay()。取消落在"窗口刚出来"那几十毫秒里时，
         * 这圈框一帧都不会出现。
         */
        root.overlayReady = false
        shapeCanvas.requestPaint()
    }

    /*
     * 选区窗口已经稳定显示（C++ 在 show() 之后调，见 Screenshot::showOverlay）。
     * 到这一步才把选区边框放出来。
     */
    function settleOverlay() {
        root.overlayReady = true
        /* 临时诊断（和 C++ 那份同一个日志文件）：边框什么时候被放行 */
        Shot.uiTrace("settleOverlay(): border allowed")
    }

    /* ---- 自检入口（见 src/SelfTest.cpp），和界面上那几下是同一批函数 ---- */    function testSelect(x, y, w, h) {
        root.selAuto = false
        root.sel = Qt.rect(x, y, w, h)
    }
    function testTextTool(on) { root.tool = on ? "text" : "" }
    /* 自检：选某个工具 -> 按下 -> 拖 -> 松开（走界面上同一套 pointer*） */
    function testDrawWith(tool, x1, y1, x2, y2) {
        const before = root.shapes.length
        root.tool = tool
        root.pointerDown(x1, y1, false)
        root.pointerMove(x2, y2)
        root.pointerUp()
        root.tool = ""
        return root.shapes.length > before
    }
    /* 自检：画一个形状（和界面上一样"起笔 -> 落笔"，落笔走 commitShape） */
    function testAddShape(kind, x1, y1, x2, y2) {
        root.drawKind = kind
        root.drawX1 = x1
        root.drawY1 = y1
        root.drawX2 = x2
        root.drawY2 = y2
        root.drawPts = (kind === "pencil")
                       ? [x1, y1, (x1 + x2) / 2, (y1 + y2) / 2, x2, y2]
                       : [x1, y1]
        const before = root.shapes.length
        root.commitShape()
        return root.shapes.length > before
    }
    function testAddText(x, y, text) {
        if (!root.addText(x, y))
            return false
        textModel.setProperty(root.editing, "txt", text)
        root.commitEditing()
        return textModel.count > 0
    }
    /* 走一遍"文字工具按下 -> 拖着定尺寸 -> 松手"，用的就是 resizeBox */
    function testBoxDrag(x1, y1, x2, y2) {
        const index = root.beginTextBox(x1, y1)
        if (index < 0)
            return false
        root.resizeBox(index, x1, y1, x2, y2)
        root.editing = index          /* 松手那一下的动作 */
        return true
    }
    /* 把某条边拖 (dx, dy)：起点取那条边的中点（自检用的框都是没转过角度的） */
    function testEdgeDrag(index, edge, dx, dy) {
        const box = textRepeater.itemAt(index)
        const item = textModel.get(index)
        if (!box || !item)
            return false
        let sx = 0
        let sy = 0
        if (edge === "right") {
            sx = item.tx + box.width
            sy = item.ty + box.height / 2
        } else if (edge === "left") {
            sx = item.tx
            sy = item.ty + box.height / 2
        } else if (edge === "top") {
            sx = item.tx + box.width / 2
            sy = item.ty
        } else {
            sx = item.tx + box.width / 2
            sy = item.ty + box.height
        }
        root.dragEdge(index, edge,
                      { w: box.width, h: box.height, tx: item.tx, ty: item.ty, x: sx, y: sy },
                      { x: sx + dx, y: sy + dy })
        return true
    }
    /* 左下角：整体放大（走界面上同一个 scaleBox） */
    function testScaleDrag(index, dx, dy) {
        const box = textRepeater.itemAt(index)
        const item = textModel.get(index)
        if (!box || !item)
            return false
        const sx = item.tx
        const sy = item.ty + box.height
        root.scaleBox(index, { w: box.width, h: box.height, bx: item.bx, bh: item.bh,
                               size: item.fsize, x: sx, y: sy },
                      { x: sx + dx, y: sy + dy })
        return true
    }
    /* 左下角（现在的语义）：拖动整个框（走界面上同一个 moveBox） */
    function testMoveDrag(index, dx, dy) {
        const box = textRepeater.itemAt(index)
        const item = textModel.get(index)
        if (!box || !item)
            return false
        const sx = item.tx
        const sy = item.ty + box.height
        root.moveBox(index, { tx: item.tx, ty: item.ty, w: box.width, h: box.height,
                              x: sx, y: sy },
                     { x: sx + dx, y: sy + dy })
        return true
    }
    /* 右下角：宽高分开拖（走界面上同一个 freeResize） */
    function testFreeDrag(index, dx, dy) {
        const box = textRepeater.itemAt(index)
        const item = textModel.get(index)
        if (!box || !item)
            return false
        const sx = item.tx + box.width
        const sy = item.ty + box.height
        root.freeResize(index, { w: box.width, h: box.height, x: sx, y: sy },
                        { x: sx + dx, y: sy + dy })
        return true
    }

    /*
     * 自检读浮动工具条的位置（见 src/SelfTest.cpp 的"工具条贴着鼠标"那两条）。
     *
     * 为什么要在 QML 这边开个口子：工具条是**可视项**，不是 QObject 的 child，
     * C++ 那边 findChild 找不到它（visual item 的父子关系走 QQuickItem，不走
     * QObject 那棵树）。所以由根对象把这个状态报出去 —— 和 uiState / treeState
     * 那几个是同一个套路。
     */
    function barState() {
        return { x: bar.x, y: bar.y, width: bar.width, height: bar.height,
                 atScreensRight: bar.atScreensRight,
                 ready: root.overlayReady, borderVisible: selBorder.visible }
    }

    /*
     * 自检：走一遍**真实的 Esc 取消**（和上面那个 Shortcut 的 onActivated
     * 一模一样的代码），然后按毫秒盯着选区窗口到底露过面没有。
     *
     * 为什么要这么测：抓屏是延时的（见 Screenshot::beginCapture），用户按完
     * 快捷键马上按 Esc 就落在那个窗口期里 —— "取消之后窗口还是冒出来了"是
     * 用户报的问题，而它只体现在**窗口可见性**上，光调函数看状态是看不出来的。
     * 所以这里把"哪一毫秒可见"数出来，返回：
     *
     *   { frames, lastMs, beforeCancel }
     *     frames        取消之后窗口可见的采样次数（0 = 一帧都没露）
     *     lastMs        最后一次可见距取消过去了多少毫秒
     *     beforeCancel  取消之前窗口就可见了吗（prewarm 之后该是 false）
     */
    function captureCancelDuringPending(cancelAtMs) {
        const out = { frames: 0, lastMs: -1, beforeCancel: false }
        const timer = Qt.createQmlObject('import QtQuick; Timer {}', root, "cancelProbe")
        let elapsed = 0
        let cancelled = false
        let beforeCancelSeen = false
        timer.interval = 3
        timer.repeat = true
        timer.triggered.connect(function() {
            elapsed += 3
            if (!cancelled) {
                if (Shot.active)
                    beforeCancelSeen = true
                if (elapsed >= cancelAtMs) {
                    cancelled = true
                    if (root.selected >= 0)      /* 和 Shortcut 里那一段一字不差 */
                        root.selected = -1
                    else
                        Shot.cancelCapture()
                }
            } else if (Shot.active) {
                out.frames += 1
                out.lastMs = elapsed
            }
            if (elapsed >= 1000)
                timer.stop()
        })
        timer.start()
        return out
    }

    Image {
        id: shotImage

        anchors.fill: parent
        source: Shot.serial > 0 ? "image://shot/full" + Shot.serial : ""
        fillMode: Image.Stretch
        /*
         * 不进 QQuickPixmapCache：URL 里那个序号已经能绕开缓存了，
         * 这一条是双保险（截图这种大图留在缓存里也没意义）。
         */
        cache: false
    }

    /*
     * 选区之外压暗：四块，围出中间那块亮的。
     *
     * 不用"整屏压暗 + 选区再贴一张原图"：那样选区里的画面要再采样一次，
     * 高 DPI 下容易看出糊。四块矩形拼出来的洞是严丝合缝的。
     */
    Rectangle {
        x: 0; y: 0; width: root.width; height: Math.max(0, root.sel.y)
        color: "#000000"; opacity: 0.45
    }
    Rectangle {
        x: 0; y: root.sel.y + root.sel.height
        width: root.width; height: Math.max(0, root.height - (root.sel.y + root.sel.height))
        color: "#000000"; opacity: 0.45
    }
    Rectangle {
        x: 0; y: root.sel.y
        width: Math.max(0, root.sel.x); height: root.sel.height
        color: "#000000"; opacity: 0.45
    }
    Rectangle {
        x: root.sel.x + root.sel.width; y: root.sel.y
        width: Math.max(0, root.width - (root.sel.x + root.sel.width)); height: root.sel.height
        color: "#000000"; opacity: 0.45
    }

    /*
     * 框选。
     *
     * 铺满整个窗口、压在底图上面，但**在文字层和工具条下面** —— 所以
     * 点在文字上、点在按钮上时都轮不到它（同一层里后声明的在上）。
     */
    MouseArea {
        id: rubber

        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        cursorShape: root.textTool ? Qt.IBeamCursor : Qt.CrossCursor

        /*
         * 三个处理器都不干活，只转给 root.pointer*()。
         *
         * 这么绕一层是为了自检能**走同一条路**：加"方框"的时候就栽在这儿
         * —— commitShape / compose 都支持 rect，但按下时的分派还写着
         * "arrow || pencil"，于是方框工具下拖出来的是"改选区"。那种漏项
         * 光测 commitShape 是测不出来的，得能从按下开始整条走一遍。
         */
        onPressed: (mouse) => root.pointerDown(mouse.x, mouse.y,
                                               mouse.button === Qt.RightButton)
        onPositionChanged: (mouse) => root.pointerMove(mouse.x, mouse.y)
        onReleased: (mouse) => root.pointerUp()
        /*
         * 左键双击 = 取消这次截图（和 Esc 一个效果）。
         *
         * 双击只在"选区空白处"才算 —— 落在文本框上时文本编辑器会自己吃掉
         * （那是"选中一个词"），落在工具条/角标上也不会传到这里，所以不会误伤。
         */
        onDoubleClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton)
                Shot.cancelCapture()
        }
    }

    /* 按下（x/y 是选区窗口坐标，right 是右键） */
    function pointerDown(x, y, right) {
        if (right) {
            /* 右键 = 收起当前工具，回到框选 */
            root.commitEditing()
            root.tool = ""
            return
        }
        if (root.textTool) {
            root.hinted = false
            if (root.insideSel(x, y)) {
                /*
                 * 文字工具：按下就落一个空框，然后**左键不松**地拖着把
                 * 它拉大 / 缩小（和框选一个手感），松手才落定并进打字。
                 */
                root.boxIndex = root.beginTextBox(x, y)
                root.boxX = x
                root.boxY = y
                root.boxDrag = true
            }
            return
        }
        /*
         * 形状工具（箭头 / 铅笔 / 方框）：按下起笔，拖动期间只重绘画布，
         * 松手才落进 shapes（见 commitShape）。
         *
         * 判据用 root.shapeTool，别再在这里写一遍 tool 的枚举 —— 加"方框"
         * 的时候就栽在这儿：shapeTool 加了 rect、这里漏了，于是方框工具
         * 下按下去走的是下面"框选"那条路（拖出来变成改选区）。
         */
        if (root.shapeTool) {
            root.hinted = false
            if (!root.insideSel(x, y))
                return
            root.drawKind = root.tool
            root.drawX1 = x
            root.drawY1 = y
            root.drawX2 = x
            root.drawY2 = y
            root.drawPts = [x, y]
            root.shapeDrag = true
            return
        }

        root.commitEditing()
        root.closeMenus()
        root.selected = -1
        root.selAuto = false
        root.hinted = false
        root.dragging = true
        root.pressX = x
        root.pressY = y
        root.sel = Qt.rect(x, y, 0, 0)
    }

    /* 移动 */
    function pointerMove(x, y) {
        /* 画形状：点都夹在选区里（出了界画布也裁掉，但别往模型里存野点） */
        if (root.shapeDrag) {
            const px = Math.max(root.sel.x, Math.min(root.sel.x + root.sel.width, x))
            const py = Math.max(root.sel.y, Math.min(root.sel.y + root.sel.height, y))
            root.drawX2 = px
            root.drawY2 = py
            if (root.drawKind === "pencil") {
                const pts = root.drawPts
                pts.push(px, py)
                root.drawPts = pts
            }
            shapeCanvas.repaint()
            return
        }

        /* 拖文本框：框跟着鼠标走（左上角取反方向那一侧，往上左拖也成立） */
        if (root.boxDrag) {
            root.resizeBox(root.boxIndex, root.boxX, root.boxY, x, y)
            return
        }

        if (!root.dragging)
            return
        const rx = Math.min(root.pressX, x)
        const ry = Math.min(root.pressY, y)
        root.sel = Qt.rect(rx, ry, Math.abs(x - root.pressX), Math.abs(y - root.pressY))
    }

    /* 松开 */
    function pointerUp() {
        /* 形状松手 = 落笔 */
        if (root.shapeDrag) {
            root.shapeDrag = false
            root.commitShape()
            return
            }

            /* 松开左键 = 文本框落定，这会儿才把光标放进框里等打字 */
            if (root.boxDrag) {
                root.boxDrag = false
                if (root.boxIndex >= 0)
                    root.editing = root.boxIndex
                root.boxIndex = -1
                return
            }

            if (!root.dragging)
                return
            root.dragging = false
            /* 点一下（几乎没拖）= 整屏，主流截图工具都是这个手感 */
            if (!root.selReady) {
                root.sel = Qt.rect(0, 0, root.width, root.height)
                root.selAuto = true
            }
    }

    /* 选区边框（不吃鼠标：没有 MouseArea，事件照样穿到下面去） */
    Rectangle {
        id: selBorder

        x: root.sel.x
        y: root.sel.y
        width: root.sel.width
        height: root.sel.height
        color: "transparent"
        border.color: root.accent
        border.width: 1
        visible: root.selReady && root.overlayReady
        /* 临时诊断：边框真的被画出来没有 */
        onVisibleChanged: Shot.uiTrace("selBorder.visible=" + (visible ? 1 : 0))
    }

    /*
     * 形状层（箭头 / 铅笔 / 方框）。
     *
     * 用 Canvas 画：这两种都是"线的集合"，画布是立即模式，一条路径一笔带过，
     * 箭头头、铅笔折线都好写；拖动期间只要 requestPaint() 重画这一层，
     * 不像 ListModel 那样每移动一像素就得往模型里塞点。
     * 松手才把这一笔写进 shapes 数组（撤销 / 合成才有据可查）。
     *
     * 和标注层一样裁到选区内，压在正文层**下面**（文字是主角，箭头绕着它画）。
     */
    Item {
        id: shapeLayer

        x: root.sel.x
        y: root.sel.y
        width: Math.max(0, root.sel.width)
        height: Math.max(0, root.sel.height)
        clip: true

        Canvas {
            id: shapeCanvas

            anchors.fill: parent
            antialiasing: true
            renderTarget: Canvas.Image

            function repaint() { requestPaint() }

            onPaint: {
                const ctx = getContext("2d")
                ctx.clearRect(0, 0, width, height)
                for (let i = 0; i < root.shapes.length; ++i) {
                    const s = root.shapes[i]
                    root.paintShape(ctx, s.kind, s.x1, s.y1, s.x2, s.y2,
                                    s.pts, s.stroke, s.color)
                }
                /* 正在画的那一笔（还没进模型） */
                if (root.drawKind !== "")
                    root.paintShape(ctx, root.drawKind, root.drawX1, root.drawY1,
                                    root.drawX2, root.drawY2, root.drawPts,
                                    root.strokeWidth, root.annotColor.toString())
            }

            /* 选区一变尺寸就得重画（画布坐标系跟着选区走） */
            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()
            Component.onCompleted: requestPaint()
        }
    }

    /*
     * 标注层。
     *
     * 裁到选区内（clip）：选区外的部分不进最终图，预览也照裁 ——
     * C++ 那边是往"选区大小的画布"上画，两边裁法一致。
     */
    Item {
        id: textLayer

        x: root.sel.x
        y: root.sel.y
        width: Math.max(0, root.sel.width)
        height: Math.max(0, root.sel.height)
        clip: true

        ListModel { id: textModel }

        Repeater {
            id: textRepeater

            model: textModel

            delegate: Item {
                id: entry

                required property int index
                required property real tx
                required property real ty
                required property string txt
                required property int fsize
                required property string fcolor
                required property real bx      /* 框宽；0 = 跟着文字走（不折行） */
                required property real bh      /* 框高（最小高度）；0 = 跟着文字走 */
                required property real rot     /* 旋转角度（度，顺时针） */

                /* 定过宽就按那个宽折行；没定过就跟着文字长（宽 == 内容宽） */
                readonly property bool fixedWidth: entry.bx > 0
                readonly property real textW: textEditor.implicitWidth
                readonly property real textH: textEditor.implicitHeight

                x: root.localX(entry.tx)
                y: root.localY(entry.ty)
                width: entry.fixedWidth ? entry.bx : Math.max(root.minBoxW, entry.textW)
                /* 拖出来的高度是个**下限**：文字多了框自己还会长 */
                height: Math.max(root.minBoxH, Math.max(entry.bh, entry.textH))

                /*
                 * 绕**框中心**转。预览和 C++ 合成两边都用这个原点
                 * （见 Screenshot::compose 里的 translate/rotate），
                 * 所以拖出来的角度在成品图里是同一个角度。
                 */
                transform: Rotation {
                    origin.x: entry.width / 2
                    origin.y: entry.height / 2
                    angle: entry.rot
                }

                /* 选中（含正在打字）：一圈蓝框，看得见"现在改的是哪一条" */
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    color: "transparent"
                    border.color: root.accent
                    border.width: 1
                    visible: entry.index === root.selected
                }

                /*
                 * 正文：**就一个 TextEdit**，一直可编辑。
                 *
                 * 为什么不再"显示一个 / 编辑一个"（原来是 TextEdit + TextEdit
                 * 来回切）：现在要求"点一下框就出打字光标，框里的字可以用鼠标
                 * 选"。做过显示层的切换之后，鼠标点在框上会被别的层吃掉，
                 * 光标和选区都进不去 —— 那就干脆让正文层自己接鼠标：
                 *   * 点一下 → activeFocusOnPress 给它焦点 → 出光标（我们的
                 *     onActiveFocusChanged 顺手把 selected/editing 同步上）
                 *   * 拖一下 → TextEdit 自己按字符选
                 *   * 移动整个框 → 左下角那个手柄（不再靠"拖框身"）
                 * 排版仍然只有这一份，预览 / 成品不会各折各的。
                 */
                TextEdit {
                    id: textEditor

                    /*
                     * 铺满整个框（不是只盖住文字那一行）：用户点框里任何地方都
                     * 该出打字光标 —— 只盖文字高度的话，点在文字底下那片空白上
                     * 等于点在框外（实测就是"点了没反应，还把选中取消了"）。
                     * 不会成环：隐式高度只跟宽度/内容有关，跟自身高度无关。
                     */
                    anchors.fill: parent
                    wrapMode: entry.fixedWidth ? TextEdit.Wrap : TextEdit.NoWrap
                    selectByMouse: true
                    persistentSelection: true
                    activeFocusOnPress: true
                    /*
                     * 光标在谁身上 = editing 是谁。这一条不能省：鼠标点进来
                     * 有 activeFocusOnPress 顶着，但"拖出新框刚松手"那条路
                     * 没有鼠标点击，不给绑定的话 editing 已经是它了、焦点却
                     * 还在别处 —— 打字和粘贴全落空，框还是空的，下一次点别处
                     * 就被当空条目清掉（实测就是这么丢的）。
                     */
                    focus: entry.index === root.editing
                    cursorVisible: activeFocus
                    font.pixelSize: entry.fsize
                    color: entry.fcolor
                    selectionColor: root.accent
                    selectedTextColor: "#ffffff"

                    /* 打字：边打边写回模型（框高跟着内容长） */
                    onTextChanged: {
                        if (textEditor.text !== entry.txt)
                            textModel.setProperty(entry.index, "txt", textEditor.text)
                    }

                    /* 打字时回车 = 换行（那会儿 Return 的 Shortcut 是关的，见下面） */
                    Keys.onEscapePressed: root.commitEditing()

                    /*
                     * 焦点进出就是"在不在编辑这一条"：
                     * 点进来 = 选中 + 出光标；点别处 = 收工（空条目顺手清掉）。
                     * 两条都在动 editing，顺序上 A 先失焦、B 后得焦（Qt 如此），
                     * 所以"换一条时把上一条收掉"也顺带成立。
                     */
                    onActiveFocusChanged: {
                        if (activeFocus) {
                            if (root.editing !== entry.index)
                                root.commitEditing()
                            root.selected = entry.index
                            root.editing = entry.index
                        } else if (root.editing === entry.index) {
                            root.commitEditing()
                        }
                    }

                    /*
                     * 模型那边改了字（删掉一条之后下标会挪，委托就被复用成
                     * 另一条了）也要把编辑器拉回来 —— 光靠赋值会断掉绑定，
                     * 只补这一处同步，别的路径都是"编辑器往模型写"。
                     */
                    Connections {
                        target: entry
                        function onTxtChanged() {
                            if (textEditor.text !== entry.txt)
                                textEditor.text = entry.txt
                        }
                    }
                }

                /* 删除（右上角，压在拖动区上面） */
                Rectangle {
                    id: deleteHandle

                    x: parent.width - 9
                    y: -9
                    width: 18; height: 18; radius: 9
                    color: "#c8503c"
                    visible: entry.index === root.selected
                    AppIcon {
                        anchors.centerIn: parent
                        provider: icons
                        kind: "close"
                        tint: "#ffffff"
                        size: 11
                    }
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.removeEntry(entry.index)
                    }
                }

                /*
                 * 四个角各一个图标手柄（见 src/../utils/IconProvider.qml 的
                 * rotate / scale / arrow 三个字形）：
                 *
                 *   左上 ⟳ 旋转        绕框中心拖，接近 0/±90/±180 会吸一下
                 *   右上 ✕ 删除        删掉这一条
                 *   左下 ⤢ 整体放大    字号和框一起按比例缩放（文字跟着变大）
                 *   右下 ↘ 自由缩放    宽高分别拖（就是"往右下拖大"）
                 *
                 * 图标都画在圆形手柄里：圆在旋转下看不出转向，所以整块可以
                 * 跟着 entry 一起转，不用单独算它们的位置。
                 */
                Rectangle {
                    id: rotateHandle

                    x: -9
                    y: -9
                    width: 18; height: 18; radius: 9
                    color: "#2b2d30"
                    border.color: root.accent
                    border.width: 1
                    visible: entry.index === root.selected
                    AppIcon {
                        anchors.centerIn: parent
                        provider: icons
                        kind: "rotate"
                        tint: root.accent
                        size: 11
                    }

                    property real startRot: 0
                    property real startAngle: 0

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        cursorShape: Qt.CrossCursor

                        onPressed: (mouse) => {
                            /*
                             * 旋转原点 = **框中心**。
                             *
                             * 注意是从 entry 换算，不是从鼠标区换算：mouse.x/y 是
                             * 鼠标区自己的坐标，而框中心那两个数是"entry 坐标系"里
                             * 的，直接丢给鼠标区的 mapToItem 会算歪几十像素 ——
                             * 歪掉的圆心会让同样的转动算成很小的角度（实测：转了
                             * 30 度只算出 2 度，还被 snapAngle 吸回 0，看着就是
                             * "旋转手柄没反应"）。
                             */
                            const c = entry.mapToItem(root, entry.width / 2, entry.height / 2)
                            const at = mapToItem(root, mouse.x, mouse.y)
                            rotateHandle.startRot = entry.rot
                            rotateHandle.startAngle = Math.atan2(at.y - c.y, at.x - c.x)
                        }

                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return
                            const c = entry.mapToItem(root, entry.width / 2, entry.height / 2)
                            const at = mapToItem(root, mouse.x, mouse.y)
                            const now = Math.atan2(at.y - c.y, at.x - c.x)
                            const deg = rotateHandle.startRot
                                      + (now - rotateHandle.startAngle) * 180 / Math.PI
                            textModel.setProperty(entry.index, "rot", root.snapAngle(deg))
                        }
                    }
                }

                /*
                 * 左下角手柄：**拖动整个文本框**。
                 *
                 * 框身现在留给正文（点一下出光标、拖着选字），所以"挪位置"
                 * 单独给一个手柄（和 PixPin 一样）。范围见 moveBox：
                 * 至少留 keepVisible 那么宽/高在选区里，不会一拖就找不着。
                 */
                Rectangle {
                    id: moveHandle

                    x: -9
                    y: parent.height - 9
                    width: 18; height: 18; radius: 9
                    color: "#2b2d30"
                    border.color: root.accent
                    border.width: 1
                    visible: entry.index === root.selected
                    AppIcon {
                        anchors.centerIn: parent
                        provider: icons
                        kind: "move"
                        tint: root.accent
                        size: 11
                    }

                    property var start: ({ tx: 0, ty: 0, w: 0, h: 0, x: 0, y: 0 })

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        cursorShape: Qt.SizeAllCursor

                        onPressed: (mouse) => {
                            const at = mapToItem(root, mouse.x, mouse.y)
                            moveHandle.start = { tx: entry.tx, ty: entry.ty,
                                                 w: entry.width, h: entry.height,
                                                 x: at.x, y: at.y }
                            root.selected = entry.index
                        }

                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return
                            root.moveBox(entry.index, moveHandle.start,
                                         mapToItem(root, mouse.x, mouse.y))
                        }
                    }
                }

                Rectangle {
                    id: freeHandle

                    x: parent.width - 9
                    y: parent.height - 9
                    width: 18; height: 18; radius: 9
                    color: "#2b2d30"
                    border.color: root.accent
                    border.width: 1
                    visible: entry.index === root.selected
                    AppIcon {
                        anchors.centerIn: parent
                        provider: icons
                        kind: "arrow"
                        tint: root.accent
                        size: 11
                    }

                    property var start: ({ w: 0, h: 0, bx: 0, bh: 0, size: 16, x: 0, y: 0 })

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        cursorShape: Qt.SizeFDiagCursor     /* ↖↘ */

                        onPressed: (mouse) => {
                            const at = mapToItem(root, mouse.x, mouse.y)
                            freeHandle.start = { w: entry.width, h: entry.height,
                                                 bx: entry.bx, bh: entry.bh,
                                                 size: entry.fsize, x: at.x, y: at.y }
                        }

                        onPositionChanged: (mouse) => {
                            if (!pressed)
                                return
                            /*
                             * 按住 Shift = 整体放大 / 缩小（字号和框一起按比例，
                             * 原来左下角那个手柄的功能，挪到这儿当加速键 ——
                             * 用的人少，不占一个角）。
                             */
                            const at = mapToItem(root, mouse.x, mouse.y)
                            if (mouse.modifiers & Qt.ShiftModifier)
                                root.scaleBox(entry.index, freeHandle.start, at)
                            else
                                root.freeResize(entry.index, freeHandle.start, at)
                        }
                    }
                }

                /*
                 * 四条边各一个小圆点：往哪个方向拖，哪一边就跟着走。
                 *
                 *   left   往左拖 = 变宽（左边界跟着走，右上角不动）
                 *   right  往右拖 = 变宽（左边不动）
                 *   top    往上拖 = 变高（上边界跟着走，下边不动）
                 *   bottom 往下拖 = 变高（上边不动）
                 *
                 * 增量都换算到 root 坐标、再投影到框自己的轴上（见 dragEdge /
                 * axisDelta）：手柄会随着框挪位置，用局部坐标算增量会自己吃自己；
                 * 框转过角度之后，"横着拖"在屏幕上也不是横的。
                 *
                 * ponytail: 框是绕**中心**转的，所以边手柄拖动时相对的那条边
                 * 会跟着挪一点点（30 度时约为变化量的 7%）。真嫌它飘，
                 * 再把 tx/ty 按 (I-R)Δc 补一刀。
                 */
                Repeater {
                    model: [ { edge: "left",   cursor: Qt.SizeHorCursor },
                             { edge: "right",  cursor: Qt.SizeHorCursor },
                             { edge: "top",    cursor: Qt.SizeVerCursor },
                             { edge: "bottom", cursor: Qt.SizeVerCursor } ]

                    delegate: Rectangle {
                        id: edgeHandle

                        required property var modelData

                        readonly property string edge: modelData.edge
                        width: 10; height: 10; radius: 5
                        color: root.accent
                        border.color: "#ffffff"
                        border.width: 1
                        visible: entry.index === root.selected

                        /*
                         * 位置用 entry 的宽高算，别用 parent.*：Repeater 造出来的
                         * 委托在**挂上父项之前**就会求值一遍绑定，那一下 parent
                         * 还是 null（"Cannot read property 'width' of null"）。
                         * entry 是文档里的 id，什么时候求值都在。
                         */
                        x: edge === "left" ? -5
                                           : (edge === "right" ? entry.width - 5
                                                               : entry.width / 2 - 5)
                        y: edge === "top" ? -5
                                          : (edge === "bottom" ? entry.height - 5
                                                               : entry.height / 2 - 5)

                        property var start: ({ w: 0, h: 0, tx: 0, ty: 0, x: 0, y: 0 })

                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -5
                            hoverEnabled: true
                            cursorShape: edgeHandle.modelData.cursor

                            onPressed: (mouse) => {
                                const at = mapToItem(root, mouse.x, mouse.y)
                                edgeHandle.start = { w: entry.width, h: entry.height,
                                                     tx: entry.tx, ty: entry.ty,
                                                     x: at.x, y: at.y }
                            }

                            onPositionChanged: (mouse) => {
                                if (!pressed)
                                    return
                                root.dragEdge(entry.index, edgeHandle.edge, edgeHandle.start,
                                              mapToItem(root, mouse.x, mouse.y))
                            }
                        }
                    }
                }
            }
        }
    }

    /*
     * 浮动工具条：贴着选区下沿，下面放不下就翻到上面，再夹回窗口里。
     * 它自己不做位移 —— 选区一动它跟着动，用户不用去追它。
     *
     * 还没框选时（selAuto，选区是整屏）sel.x + sel.width 就是屏幕右沿，所以它先
     * 出现在**右上角** —— 用户要的就是这个位置（试过改成"贴鼠标"，被要求改回来）。
     */
    Rectangle {
        id: bar

        readonly property real wantY: root.sel.y + root.sel.height + 10
        readonly property bool flip: wantY + height > root.height
        /* 工具条这会儿是不是按"整屏选区"摆在右上角（自检要看这个状态） */
        readonly property bool atScreensRight: root.selAuto

        x: Math.max(8, Math.min(root.width - width - 8,
                                root.sel.x + root.sel.width - width))
        y: flip ? Math.max(8, root.sel.y - height - 10) : wantY
        width: barRow.implicitWidth + 20
        height: barRow.implicitHeight + 22
        radius: 6
        color: "#2b2d30"
        border.color: "#4b4d4f"
        border.width: 1
        visible: root.overlayReady

        component BarButton: Rectangle {
            id: button

            property string label: ""
            property bool active: false
            property bool danger: false

            signal clicked()

            implicitWidth: buttonLabel.implicitWidth + 18
            implicitHeight: 24
            radius: 4
            color: !button.enabled ? "transparent"
                                   : (button.active ? root.accent
                                                    : (buttonHit.containsMouse ? "#45484c"
                                                                               : "transparent"))

            Text {
                id: buttonLabel
                anchors.centerIn: parent
                text: button.label
                font.pixelSize: 12
                /* 置灰的那两个（比如没东西可撤销时的"撤销"）看得出来是按不动的 */
                color: !button.enabled ? "#5c6066"
                                       : (button.active ? "#ffffff"
                                                        : (button.danger ? "#e08a7a"
                                                                         : (buttonHit.containsMouse
                                                                                            ? "#e8e8e8"
                                                                                            : "#b4b8bf")))
            }

            MouseArea {
                id: buttonHit
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: button.clicked()
            }
        }

        component BarGap: Rectangle {
            implicitWidth: 1
            implicitHeight: 16
            color: "#4b4d4f"
        }

        component ColorDot: Rectangle {
            id: dot

            property color dotColor: "#ffffff"
            readonly property bool picked: root.annotColor.toString() === dot.dotColor.toString()

            implicitWidth: 16
            implicitHeight: 16
            radius: 8
            color: dot.dotColor
            border.width: dot.picked ? 2 : 1
            border.color: dot.picked ? "#ffffff" : "#4b4d4f"

            MouseArea {
                anchors.fill: parent
                anchors.margins: -3
                cursorShape: Qt.PointingHandCursor
                onClicked: root.setColor(dot.dotColor)
            }
        }

        ColumnLayout {
            anchors.centerIn: parent
            spacing: 3

            RowLayout {
                id: barRow
                spacing: 4

                Text {
                    text: Math.round(root.sel.width) + " × " + Math.round(root.sel.height)
                    font.pixelSize: 12
                    color: "#8b929e"
                    Layout.leftMargin: 3
                    Layout.rightMargin: 3
                }

                BarGap {}

                /*
                 * 工具组：文字 / 箭头 / 铅笔。再点一下同一个 = 收起工具
                 * （回到框选），右键也是收起。
                 */
                BarButton {
                    label: "文字"
                    active: root.tool === "text"
                    onClicked: root.pickTool("text")
                }
                BarButton {
                    label: "箭头"
                    active: root.tool === "arrow"
                    onClicked: root.pickTool("arrow")
                }
                BarButton {
                    label: "铅笔"
                    active: root.tool === "pencil"
                    onClicked: root.pickTool("pencil")
                }
                BarButton {
                    label: "方框"
                    active: root.tool === "rect"
                    onClicked: root.pickTool("rect")
                }

                BarGap {}

                /* 这两个键跟着工具走：形状工具下是线宽（标签也换成细/粗） */
                BarButton { label: root.sizeDownLabel; onClicked: root.bumpSize(-2) }

                /* 中间那个数字：点一下弹出一格格的候选值，直接挑（不用一直点 +） */
                Rectangle {
                    id: sizePicker

                    implicitWidth: 34
                    implicitHeight: 24
                    radius: 4
                    color: root.menuSize ? root.accent
                                         : (pickerHit.containsMouse ? "#45484c" : "#1e2023")
                    border.color: "#4b4d4f"
                    border.width: 1

                    Text {
                        anchors.centerIn: parent
                        text: root.sizeValue
                        font.pixelSize: 12
                        color: root.menuSize ? "#ffffff" : "#e8e8e8"
                    }

                    MouseArea {
                        id: pickerHit
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.menuColor = false
                            root.menuSize = !root.menuSize
                        }
                    }
                }

                BarButton { label: root.sizeUpLabel; onClicked: root.bumpSize(2) }

                BarGap {}

                ColorDot { dotColor: "#ff3b30" }
                ColorDot { dotColor: "#ffd60a" }
                ColorDot { dotColor: "#30d158" }
                ColorDot { dotColor: "#ffffff" }

                /* 更多颜色：弹一块调色板，最后一行还能开系统取色框 */
                BarButton {
                    id: moreColors
                    label: "更多"
                    active: root.menuColor
                    onClicked: {
                        root.menuSize = false
                        root.menuColor = !root.menuColor
                    }
                }

                BarGap {}

                BarButton {
                    label: "撤销"
                    onClicked: root.undoLast()
                    enabled: root.undoAvailable
                }

                BarGap {}

                BarButton { label: "复制"; onClicked: root.runAction("copy") }
                BarButton { label: "保存…"; onClicked: root.runAction("save") }
                BarButton { label: "固定到桌面"; onClicked: root.runAction("pin") }

                BarGap {}

                BarButton { label: "取消"; danger: true; onClicked: root.runAction("cancel") }
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: "T 文字 / A 箭头 / P 铅笔 / R 方框 · 左下角拖整框 · 左上旋转 · 右下缩放（Shift 整体放大）· Ctrl+Z 撤销 · 双击取消"
                font.pixelSize: 10
                color: "#6f737a"
            }
        }

        /*
         * 数字框弹出来的候选值。
         *
         * 做成"弹出的小面板"而不是下拉框：截图时鼠标就在附近，一格一格点
         * 比在列表里拖快。位置贴着工具条上方（工具条翻到上方时改到下方），
         * 并夹在窗口里。
         */
        Rectangle {
            id: sizeMenu

            visible: root.menuSize
            width: sizeGrid.implicitWidth + 16
            height: sizeGrid.implicitHeight + 16
            x: Math.max(4, Math.min(bar.width - width - 4,
                                    sizePicker.mapToItem(bar, 0, 0).x))
            y: (bar.y - height - 6 >= 4) ? -height - 6 : bar.height + 6
            radius: 6
            color: "#2b2d30"
            border.color: "#4b4d4f"
            border.width: 1

            Grid {
                id: sizeGrid
                anchors.centerIn: parent
                columns: 5
                spacing: 4

                Repeater {
                    model: root.sizePresets

                    delegate: Rectangle {
                        required property var modelData

                        width: 30
                        height: 22
                        radius: 4
                        color: modelData === root.sizeValue
                               ? root.accent
                               : (presetHit.containsMouse ? "#45484c" : "transparent")

                        Text {
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 11
                            color: modelData === root.sizeValue ? "#ffffff" : "#c8ccd1"
                        }

                        MouseArea {
                            id: presetHit
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.setSizeValue(modelData)
                                root.menuSize = false
                            }
                        }
                    }
                }
            }
        }

        /* "更多颜色"那块调色板（8 列），最下面一行是系统取色框 */
        Rectangle {
            id: colorMenu

            visible: root.menuColor
            width: colorGrid.implicitWidth + 16
            height: colorGrid.implicitHeight + customColor.height + 26
            x: Math.max(4, Math.min(bar.width - width - 4,
                                    moreColors.mapToItem(bar, 0, 0).x))
            y: (bar.y - height - 6 >= 4) ? -height - 6 : bar.height + 6
            radius: 6
            color: "#2b2d30"
            border.color: "#4b4d4f"
            border.width: 1

            Grid {
                id: colorGrid
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 8
                columns: 8
                spacing: 4

                Repeater {
                    model: root.colorPalette

                    delegate: Rectangle {
                        required property var modelData

                        width: 18
                        height: 18
                        radius: 4
                        color: modelData
                        border.width: modelData === root.annotColor.toString() ? 2 : 1
                        border.color: modelData === root.annotColor.toString() ? "#ffffff" : "#4b4d4f"

                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -2
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                root.setColor(modelData)
                                root.menuColor = false
                            }
                        }
                    }
                }
            }

            BarButton {
                id: customColor
                label: "自定义…"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 8
                onClicked: root.pickCustomColor()
            }
        }
    }

    /*
     * 一进来（或还没动过手）时的提示，拖一下就收。
     *
     * 贴在屏幕下沿：工具条是跟着选区走的（选区贴底时它会翻到选区上方），
     * 只有这一条固定在下面，两边不会撞在一起。
     */
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.height - height - 28
        width: hintText.implicitWidth + 20
        height: 26
        radius: 4
        color: "#2b2d30"
        border.color: "#4b4d4f"
        visible: root.hinted && root.overlayReady

        Text {
            id: hintText
            anchors.centerIn: parent
            text: "拖动鼠标框选区域，只点一下 = 整屏"
            font.pixelSize: 12
            color: "#b4b8bf"
        }
    }

    /*
     * 快捷键走 Shortcut 而不是 Keys。
     *
     * 文字输入框拿到焦点之后就没有"当前项"了（打字全给 TextEdit），
     * 挂在根元素上的 Keys 收不到 Esc —— Shortcut 是窗口级的，不受焦点影响。
     *
     * 全部挂在 `enabled: root.shortcutsOn` 上（= 没在打字 && 没开着系统取色框）：
     * **打字的时候要让开** —— 回车在文本框里是"换行"（TextEdit 自己处理），
     * Esc 交给编辑框自己的 Keys.onEscapePressed；窗口级 Shortcut 的优先级比
     * 控件的按键处理高，不让开的话回车永远换不了行。开着系统对话框时同理
     * （见 pickingColor 的说明）。
     */
    Shortcut {
        sequence: "Escape"
        enabled: root.shortcutsOn
        onActivated: {
            /* 一层一层往后退：先取消选中 -> 都没有才撤销整个截图 */
            if (root.selected >= 0)
                root.selected = -1
            else
                Shot.cancelCapture()
        }
    }

    Shortcut {
        sequence: "Return"
        enabled: root.shortcutsOn
        onActivated: root.runAction("copy")
    }

    /* 撤销：打字的时候让开（那会儿 Ctrl+Z 归 TextEdit 自己用） */
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: root.shortcutsOn
        onActivated: root.undoLast()
    }

    Shortcut {
        sequence: "Enter"
        enabled: root.shortcutsOn
        onActivated: root.runAction("copy")
    }

    /*
     * 工具快捷键：T 文字 / A 箭头 / P 铅笔 / R 方框
     * （再按一下同一个 = 收起工具）。都要在打字的时候让开 ——
     * 否则在文本框里敲这几个字母会被它们吃掉。
     */
    Shortcut {
        sequence: "T"
        enabled: root.shortcutsOn
        onActivated: root.pickTool("text")
    }

    Shortcut {
        sequence: "A"
        enabled: root.shortcutsOn
        onActivated: root.pickTool("arrow")
    }

    Shortcut {
        sequence: "P"
        enabled: root.shortcutsOn
        onActivated: root.pickTool("pencil")
    }

    Shortcut {
        sequence: "R"
        enabled: root.shortcutsOn
        onActivated: root.pickTool("rect")
    }
}
