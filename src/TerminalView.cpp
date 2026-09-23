#include "TerminalView.h"

/*
 * 枚举原生子窗口要用 windows.h，但**必须放在 vterm.h 之后**：
 * winerror.h 里有 `#define small 0`，而 libvterm 的 VTermScreenCellAttrs 有个
 * 位域就叫 `small : 1` —— 先引 windows.h 会把那一行编成 `0 : 1`。
 * 同理后补 #undef 掉几个会撞名字段的宏。
 */
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#undef small
#undef large
#undef status
#undef unicode

#include <QClipboard>
#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSGImageNode>
#include <QQuickWindow>
#include <QScreen>
#include <QWindow>
#include <QTextStream>
#include <QTime>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

/*
 * 真程序里的几何记录器（设了 SMARTCLIP_TERM_LOG 才开）。
 *
 * 为什么要它：自检里 62/0 全绿，用户屏幕上照样黑 —— 说明我量到的和他看到的不是
 * 同一件事。那就别再猜了，让**他这一次真实操作**自己把几何链写出来：从最外层宿主
 * 到正文 item 每一级的宽高、引擎以为的行列数、画布实际尺寸。哪一级停在旧数字上，
 * 黑块就在那一级的下面。
 */
QFile *termLogFile()
{
    static QFile *f = [] {
        if (!qEnvironmentVariableIsSet("SMARTCLIP_TERM_LOG"))
            return static_cast<QFile *>(nullptr);
        auto *x = new QFile(QStringLiteral("H:/steward/build/term-geom.log"));
        if (!x->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            delete x;
            return static_cast<QFile *>(nullptr);
        }
        return x;
    }();
    return f;
}

void termLog(const char *tag, const TerminalView *v, const QString &extra = QString())
{
    QFile *f = termLogFile();
    if (!f)
        return;
    static const bool stamped = [&] {
        QTextStream h(f);
        h << "\n==== 进程启动 exe=" << QCoreApplication::applicationFilePath()
          << " 构建=" << __DATE__ << " " << __TIME__ << "\n";
        h.flush();
        return true;
    }();
    Q_UNUSED(stamped)
    QString chain;
    for (const QQuickItem *it = v; it; it = it->parentItem()) {
        chain = QStringLiteral("  [%1 %2x%3 @%4,%5]")
                    .arg(QString::fromLatin1(it->metaObject()->className()).left(22))
                    .arg(it->width()).arg(it->height()).arg(it->x()).arg(it->y())
                    + chain;
    }
    QTextStream s(f);
    s << QStringLiteral("t=%1 %2 视图 %3x%4 格子 %5x%6 回滚 %7 画布 %8x%9%10\n")
             .arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")))
             .arg(QLatin1String(tag))
             .arg(v->width()).arg(v->height())
             .arg(v->columns()).arg(v->rows()).arg(v->historyRows())
             .arg(v->canvasSizeForLog().width()).arg(v->canvasSizeForLog().height())
             .arg(chain.isEmpty() ? QString() : QStringLiteral(" 链:") + chain);
    s << (extra.isEmpty() ? QString() : QStringLiteral("   %1\n").arg(extra));
    s.flush();
}

/*
 * 改尺寸之后 600ms，抓一次**真实屏幕**量视图里的黑段。见文件末尾的实现。
 */

/* 选字时"一个词"的边界：路径、标识符连着选，和 VS Code 终端一致 */
bool isWordChar(QChar ch)
{
    return ch.isLetterOrNumber() || ch == QLatin1Char('_') || ch == QLatin1Char('.')
           || ch == QLatin1Char('-') || ch == QLatin1Char('/') || ch == QLatin1Char('\\')
           || ch == QLatin1Char(':');
}

int modifierBits(Qt::KeyboardModifiers mods)
{
    int m = VTERM_MOD_NONE;
    if (mods & Qt::ShiftModifier)
        m |= VTERM_MOD_SHIFT;
    if (mods & Qt::AltModifier)
        m |= VTERM_MOD_ALT;
    if (mods & Qt::ControlModifier)
        m |= VTERM_MOD_CTRL;
    return m;
}

/* Qt 的键 -> libvterm 的键号；对不上的返回 -1，交给上层按字符发 */
int vtermKeyFor(int qtKey)
{
    switch (qtKey) {
    case Qt::Key_Enter:
    case Qt::Key_Return:
        return VTERM_KEY_ENTER;
    case Qt::Key_Tab:
        return VTERM_KEY_TAB;
    case Qt::Key_Backspace:
        return VTERM_KEY_BACKSPACE;
    case Qt::Key_Escape:
        return VTERM_KEY_ESCAPE;
    case Qt::Key_Up:
        return VTERM_KEY_UP;
    case Qt::Key_Down:
        return VTERM_KEY_DOWN;
    case Qt::Key_Left:
        return VTERM_KEY_LEFT;
    case Qt::Key_Right:
        return VTERM_KEY_RIGHT;
    case Qt::Key_Insert:
        return VTERM_KEY_INS;
    case Qt::Key_Delete:
        return VTERM_KEY_DEL;
    case Qt::Key_Home:
        return VTERM_KEY_HOME;
    case Qt::Key_End:
        return VTERM_KEY_END;
    case Qt::Key_PageUp:
        return VTERM_KEY_PAGEUP;
    case Qt::Key_PageDown:
        return VTERM_KEY_PAGEDOWN;
    default:
        if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F12)
            return VTERM_KEY_FUNCTION_0 + (qtKey - Qt::Key_F1);
        return -1;
    }
}

}  // namespace

TerminalView::TerminalView(QQuickItem *parent) : QQuickItem(parent)
{
    m_engine = new TerminalEngine(this);

    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setActiveFocusOnTab(true);
    /* 输入法要能定位候选窗、中文要能打进 shell */
    setFlag(QQuickItem::ItemAcceptsInputMethod);
    /*
     * 不开 item clip：下面那两个圆角靠 paint() 里的 setClipPath 就够了，item 级裁剪
     * 在这里没有活要干。（2026-09-22 曾怀疑"裁剪矩形停在旧尺寸上，右边像素被丢掉"，
     *  09-23 用记录器量掉了：黑是**格子的底色**，见 updatePaintNode 上面那一段。）
     */
    /*
     * 自己交纹理，不走 QQuickPaintedItem 的缓存：画布和纹理都按当帧尺寸建。
     * 这一刀是 09-22 为了排掉"缓存不可靠"这个变量才换的，换完屏幕照样黑 —— 但它
     * 顺手把"旧纹理贴新矩形"这一类可能性从代码里清出去了，留着。
     */
    setFlag(QQuickItem::ItemHasContents, true);
    /*
     * **不要**开 FastFBOResizing：那个提示的意思是"改尺寸时别重画，把旧内容拉伸一下
     * 交差"，对我们正好是负收益 —— 本 item 每帧自己铺满矩形，拉伸旧图只会把没画过的
     * 那条留成纯黑。
     */

    m_fontFamily = QStringLiteral("Cascadia Mono");
    rebuildFont();

    connect(m_engine, &TerminalEngine::contentsChanged, this, [this] {
        /*
         * 两件事都只能在这一拍做，因为 gridChanged / atBottomChanged 谁都不会
         * 因为"回滚行数变了"而发出来。
         *
         * ① 补发 gridChanged：右侧那条位置条读的是 `view.historyRows > 0`，
         *    而 historyRows 的 NOTIFY 挂的就是 gridChanged。回滚行数在**输出滚动**
         *    时就变（顶出去一行 +1），行列数一点没动 —— 不补这一刀，QML 那个绑定
         *    只在改尺寸时重新求值，条子就是用户报的"时显时隐"。
         * ② 把 scrollUp 夹回新的回滚行数：面板拉高时引擎会 sb_popline 把历史倒回屏上，
         *    historyRows 当场变小，而 m_scrollUp 还停在旧值 → 视图往"屏顶以上 N 行"
         *    去读，读到的是不存在的行（越界那些格子底色是默认构造的 QColor，窄历史行
         *    补出来的是空格）—— 表现同一条黑带，而且看着像"内容被截断"。
         */
        if (m_engine->historyRows() != m_histSeen) {
            m_histSeen = m_engine->historyRows();
            emit gridChanged();
        }
        setScrollUp(m_scrollUp);
        update();
    });
    connect(m_engine, &TerminalEngine::cursorChanged, this, qOverload<>(&QQuickItem::update));
    connect(m_engine, &TerminalEngine::titleChanged, this, &TerminalView::titleChanged);
    connect(m_engine, &TerminalEngine::exited, this, [this](int code) {
        emit runningChanged();
        emit sessionExited(code);
        update();
    });

    m_blink = new QTimer(this);
    m_blink->setInterval(530);
    connect(m_blink, &QTimer::timeout, this, [this] {
        if (!hasActiveFocus() || !m_engine->running())
            return;
        m_cursorOn = !m_cursorOn;
        update();
    });
    m_blink->start();
}

TerminalView::~TerminalView() = default;

void TerminalView::componentComplete()
{
    QQuickItem::componentComplete();
    m_engine->setDefaultColors(m_fg, m_bg);
    relayout();
}

// ------------------------------------------------------------------ 外观

void TerminalView::setFontFamily(const QString &name)
{
    if (m_fontFamily == name)
        return;
    m_fontFamily = name;
    rebuildFont();
    relayout();
}

void TerminalView::setFontSize(qreal pixels)
{
    if (qFuzzyCompare(m_fontSize, pixels) || pixels <= 0)
        return;
    m_fontSize = pixels;
    rebuildFont();
    relayout();
}

void TerminalView::rebuildFont()
{
    m_font = QFont();
    /*
     * 字体族按"逐个字符回退"给：Cascadia / Consolas 没有中日韩字形，把它们排在前面
     * 让 ASCII 拿到等宽字形，中文再回退到雅黑。只写一个族的话中文会被 Qt 随便挑一个
     * 比例字体顶上，宽度就不是两倍格宽了 —— 中文对齐全靠这条。
     */
    m_font.setFamilies({ m_fontFamily, QStringLiteral("Consolas"),
                         QStringLiteral("Courier New"), QStringLiteral("Microsoft YaHei") });
    m_font.setStyleHint(QFont::Monospace);
    m_font.setFixedPitch(true);
    m_font.setPixelSize(qMax(6, int(m_fontSize)));

    const QFontMetricsF fm(m_font);
    /*
     * 格宽按 '0' 取整向上。宁可每格多留一点点，也不要出现"两格挤成 1.9 格"——
     * 累积到第 80 列就是半字的错位，中文尤其明显。
     */
    m_cellW = std::ceil(fm.horizontalAdvance(QLatin1Char('0')));
    if (m_cellW <= 0)
        m_cellW = std::ceil(m_fontSize * 0.6);
    m_cellH = std::ceil(fm.height());
    m_ascent = std::ceil(fm.ascent());
    emit fontChanged();
}

void TerminalView::setForegroundColor(const QColor &c)
{
    if (m_fg == c)
        return;
    m_fg = c;
    m_engine->setDefaultColors(m_fg, m_bg);
    emit themeChanged();
}

void TerminalView::setBackgroundColor(const QColor &c)
{
    if (m_bg == c)
        return;
    m_bg = c;
    m_engine->setDefaultColors(m_fg, m_bg);
    emit themeChanged();
}

void TerminalView::setSelectionColor(const QColor &c)
{
    if (m_selColor == c)
        return;
    m_selColor = c;
    emit themeChanged();
    update();
}

void TerminalView::setCursorColor(const QColor &c)
{
    if (m_cursorColor == c)
        return;
    m_cursorColor = c;
    emit themeChanged();
    update();
}

void TerminalView::setCornerRadius(qreal px)
{
    if (qFuzzyCompare(m_cornerRadius, px))
        return;
    m_cornerRadius = px;
    emit themeChanged();
    update();
}

void TerminalView::setPadding(qreal px)
{
    if (qFuzzyCompare(m_padding, px))
        return;
    m_padding = px;
    emit themeChanged();
    relayout();
}

// ------------------------------------------------------------------ 会话

bool TerminalView::start(const QString &program, const QString &workingDir)
{
    QString err;
    if (!m_engine->start(program, QStringList(), workingDir, &err)) {
        m_errorText = err;
        emit failed(err);
        emit runningChanged();
        return false;
    }
    m_errorText.clear();
    emit runningChanged();
    return true;
}

void TerminalView::closeSession()
{
    m_engine->closeSession();
    emit runningChanged();
    update();
}

void TerminalView::clearBuffer()
{
    /*
     * 清屏这笔**得让 shell 自己清**，不能我们擦屏。
     *
     * 上一版是往我们自己的解析器里灌 ESC[2J + ESC[H：屏是干净了，可 ConPTY 那一侧
     * 的游标还停在它原来那一行 —— 它下一次重画发的是**按它坐标算的绝对定位**，
     * 于是新敲进去的字落在面板中间某一行、还带着缩进（用户 2026-09-23 的图：
     * 清完一片空白，再输 "dir" 出现在第 22 行）。两边各记一个游标，必然分家。
     *
     * 发 Ctrl+L 才是人按的那个"清屏"：PowerShell 的 Clear-Display、cmd 的 cls
     * 会把 conhost 的缓冲清掉、把提示符重画在**第一行**，我们跟着它重画就行。
     * 回滚是我们自己存的那份（libvterm 0.3 没有 scrollback），照旧清我们自己这份。
     */
    m_engine->sendBytes(QByteArray(1, '\x0c'));   // Ctrl+L
    /*
     * 清完开 400ms 的"不进回滚"窗口，而不是过 300ms 再清一遍。
     *
     * 我先试的是再清一遍，那是个地雷：Ctrl+L 到 shell 那边是异步的，
     * 那 300ms 里用户要是灌了几百行，第二刀把它们全砍了
     * —— 自检当场量到"改尺寸之前 602 行、之后只剩 15 行"。
     * 现在只挡住"这一滚被记进回滚"，屏上的字一个不少。
     */
    m_engine->suppressHistoryBriefly();
    setScrollUp(0);
    /* 回滚行数直接归零，引擎那边不会为此发 contentsChanged —— 不补一句，
       QML 那条 `visible: view.historyRows > 0` 停在"还看得见"上（垃圾桶按了条子不藏）。 */
    m_histSeen = 0;
    emit gridChanged();
    update();
}

void TerminalView::scrollToEnd()
{
    setScrollUp(0);
}

void TerminalView::scrollLines(int delta)
{
    /* delta > 0 = 往新内容（贴底）方向滚 */
    setScrollUp(m_scrollUp - delta);
}

void TerminalView::setScrollUp(int rows)
{
    const int clamped = std::clamp(rows, 0, m_engine->historyRows());
    if (clamped == m_scrollUp)
        return;
    m_scrollUp = clamped;
    emit atBottomChanged();
    update();
}

QString TerminalView::lineTextAt(int row) const
{
    return m_engine->lineText(row);
}

QString TerminalView::topVisibleText() const
{
    return m_engine->lineText(docRowOf(0));
}

// ------------------------------------------------------------------ 布局

/*
 * 尺寸一变：要一帧，让 updatePaintNode 按**当帧的 item 尺寸**重建画布。
 *
 * **画布只能由 updatePaintNode 那一侧碰**（新增/释放都不行）。
 *
 * 这一条是踩出来的、而且踩得很响：我原来在这里调了一次 invalidateCanvas()，
 * 想把"旧画布"提前放掉。可 geometryChange 跑在 **GUI 线程**，而 updatePaintNode
 * 跑在 **场景图渲染线程**（Qt 默认 threaded render loop）—— 从 GUI 线程把
 * QImage 放掉（`m_canvas = QImage()`）等于在渲染线程正拿着这块内存画的时候把
 * 它的分配换掉。表现就是用户日志里那一幕：
 *
 *     画布 3797x668      <- 尺寸是对的
 *     fill(#1e1f22)      <- 我们确实铺了底色
 *     但画布右边一片 #000000，而屏幕上也是 #000000
 *
 * 尺寸对、底色铺了、内容却是黑的 —— 那不是"没画到"，是**画到了已经不属于它的
 * 内存上**。同一份内存还会在下一帧被另一个尺寸重新映射，所以左边正常、右边黑，
 * 而且一直黑着。
 *
 * 现在只做两件事：update() + 0ms 补一帧。画布的建/放全交给 updatePaintNode
 * （它本来就有"尺寸不一样就重建"那条判断），单线程渲染和线程渲染都安全。
 */
void TerminalView::geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry)
{
    QQuickItem::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.width() == oldGeometry.width() && newGeometry.height() == oldGeometry.height())
        return;
    /*
     * 尺寸一变就整块要重画，不能只指望 relayout()：它在"行列数没变"时直接 return，
     * 一次 update() 都不发。
     */
    update();
    relayout();
    if (!m_reflowTimer) {
        m_reflowTimer = new QTimer(this);
        m_reflowTimer->setSingleShot(true);
        connect(m_reflowTimer, &QTimer::timeout, this, [this] {
            if (window())
                window()->requestUpdate();
            update();
        });
    }
    /* 0ms：排进本回合之后的第一时间，让"新画布这一帧"当场被呈现 */
    m_reflowTimer->start(0);
    /*
     * 改尺寸 600ms 之后抓**真实屏幕**，把"屏幕上这一针"和"画布里这一针"并排记下来。
     * 只在开了日志时跑；它是给"画布对了、屏幕还是黑"这种情形分层的唯一手段。
     * 读画布像素这一步在 GUI 线程，线程渲染下拿到的是"大概"，够用来分层就行。
     */
    if (termLogFile()) {
        if (!m_blackCheck) {
            m_blackCheck = new QTimer(this);
            m_blackCheck->setSingleShot(true);
            connect(m_blackCheck, &QTimer::timeout, this, &TerminalView::checkScreenForBlack);
        }
        m_blackCheck->start(600);
    }
    termLog("geometryChange", this,
            QStringLiteral(" 旧=%1x%2")
                .arg(oldGeometry.width()).arg(oldGeometry.height()));
}

void TerminalView::relayout()
{
    if (m_cellW <= 0 || m_cellH <= 0 || width() <= 0 || height() <= 0)
        return;
    const int cols = int((width() - 2 * m_padding) / m_cellW);
    const int rows = int(height() / m_cellH);
    if (cols < 20 || rows < 4)
        return;   // 面板收到很窄时不改，免得把 shell 挤成 20 列以下
    if (cols == m_engine->cols() && rows == m_engine->rows())
        return;
    m_engine->setSize(cols, rows);
    emit gridChanged();
    update();
}

// ------------------------------------------------------------------ 绘制

/*
 * 自己把纹理交给场景图：每一帧按**当帧的 item 尺寸**准备画布和纹理，画布尺寸没变时
 * 留着复用（不然每帧 malloc 一张 4K 图）。纹理矩形由画布算、不读 boundingRect() ——
 * "节点几何"和"画布像素"永远是同一个尺寸。
 *
 * ---------------------------------------------------------- 那条硬边黑带（已结案）
 *
 * 现象：主窗口最大化之后，正文从**改尺寸之前**那个宽度往右整片纯黑（量到 2382~2384 px，
 * 左边界钉在 1406/1442），右边顶到窗口缘；滚动条跟着乱；空闲时不自愈，一动就好。
 *
 * 分层量下来的结论（2026-09-23，别再往呈现层找）：
 *   * 画布每一级都跟得上 item（记录器：改完 3ms 就重建到 3797x2017）；
 *   * 黑在**画布里面**（同一帧渲染线程自己读 m_canvas：右边 46499/120175 个采样是黑）；
 *   * 是**我们自己铺上去的**：那一帧 drawRow 铺了 91 块 #ff000000 的单格矩形，
 *     起点正好在"旧列数"那一列上。
 * 根因在引擎：libvterm 补/清空格子用的是当前画笔的颜色（screen.c 的 clearcell /
 * erase_internal），ConPTY 重排整屏时画笔上挂着的就是"背景 = 调色板 0 号 = 纯黑"，
 * 于是新露出来的那些格子底色天生是黑，我们照颜色铺满矩形就铺出一条黑带。
 * 修在 TerminalEngine::setSize()（注一个 ESC[m 把画笔交回默认色）+ blankCell() 的
 * 默认色标志位，判据见 src/SelfTestTerminal.cpp 里那三条。
 */
QSGNode *TerminalView::updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *)
{
    const qreal dpr = window() ? window()->devicePixelRatio() : 1.0;
    const QSize px = (size() * dpr).toSize();
    if (px.isEmpty()) {
        delete oldNode;
        return nullptr;
    }
    if (m_canvas.size() != px) {
        const bool grew = !m_canvas.isNull()
                          && (px.width() > m_canvas.width() || px.height() > m_canvas.height());
        m_canvas = QImage(px, QImage::Format_ARGB32_Premultiplied);
        termLog("画布重建", this, QStringLiteral(" 要=%1x%2").arg(px.width()).arg(px.height()));
        /*
         * 画布换了尺寸 = 屏幕上那一帧很可能还是旧纹理贴出来的。用户机器上实测到：
         * 画布已经 3797x2017、改完 750ms 之后抓屏仍是 3782px 的黑 —— 因为之后
         * 再没有第二次渲染。这里主动再要一帧（离屏窗口重渲染会带着宿主控件一起更新）。
         *
         * 用 0ms 不用 16ms：那一帧越早到越好。等 16ms 的话，`update()` 走的是
         * 脏矩形那一套，而"变大之后新露出来那一条"本来就不在任何人的脏矩形里
         * （见 geometryChange 里那段），所以这一补必须**当场**排进事件循环，
         * 不能等下一拍合成。
         */
        if (!m_reflowTimer) {
            m_reflowTimer = new QTimer(this);
            m_reflowTimer->setSingleShot(true);
            connect(m_reflowTimer, &QTimer::timeout, this, [this] {
                if (window()) {
                    window()->requestUpdate();
                    termLog("补一帧", this);
                }
            });
        }
        if (grew) {
            /* 变大：这一帧之后立刻再补一帧，且**当场**补 */
            m_reflowTimer->start(0);
            update();
        } else {
            m_reflowTimer->start(16);
        }
    }
    m_canvas.setDevicePixelRatio(dpr);
    m_canvas.fill(m_bg);
    m_dbgBgRects = 0;
    m_dbgBlackRects = 0;
    m_dbgBlackFirst.clear();
    /*
     * 诊断：把这块画布的**内存事实**记下来。
     *
     * 为什么不是再读几个像素：用户那次的现象是"尺寸对、fill 也调了，右边却一片
     * #000000"。只看颜色分不出"没画到"和"画到别人的内存上"。这里记的是
     * 缓冲区首地址 / 字节数 / 每行字节数 / 首行末 4 字节 / 首行指针 ——
     * 只要这块内存被别的尺寸重新映射过，这几个数字会立刻露出来。
     */
    if (termLogFile()) {
        const uchar *base = m_canvas.constBits();
        const qsizetype bytes = m_canvas.sizeInBytes();
        const uchar *lastRow = m_canvas.constScanLine(m_canvas.height() - 1);
        const qsizetype rowBytes = m_canvas.bytesPerLine();
        termLog("画布内存", this,
                QStringLiteral(" base=%1 字节=%2 每行=%3 首行指针=%4 末行指针=%5 末行末4字节=%6")
                    .arg(reinterpret_cast<quintptr>(base), 0, 16)
                    .arg(bytes)
                    .arg(rowBytes)
                    .arg(reinterpret_cast<quintptr>(m_canvas.constScanLine(0)), 0, 16)
                    .arg(reinterpret_cast<quintptr>(lastRow), 0, 16)
                    .arg(reinterpret_cast<const QRgb *>(lastRow + rowBytes - 4) != nullptr
                             ? QColor(*reinterpret_cast<const QRgb *>(lastRow + rowBytes - 4))
                                   .name()
                             : QStringLiteral("?")));
    }
    {
        QPainter p(&m_canvas);
        paint(&p);
    }
    ++m_paints;
    ++m_fullPaints;
    /*
     * 全量诊断（SMARTCLIP_TERM_LOG）：把"这一帧到底交了什么"写在日志里。
     *
     * 黑块的判据全在屏幕上，而屏幕上那块像素归**哪一层**管，靠读代码是猜的。
     * 这一行把每一帧的 item 尺寸、画布尺寸、纹理矩形、视口矩形、宿主控件矩形
     * 一起打出来：哪一级的数字和别的不一样，黑块就在那一级下面。
     * 只在设了环境变量时才写，不设就是空函数（termLog 第一行就 return）。
     */
    if (termLogFile()) {
        const QRectF vp = window() ? window()->geometry() : QRect();
        QString host;
#if defined(Q_OS_WIN)
        if (QWidget *hw = window() ? qobject_cast<QWidget *>(window()->parent()) : nullptr) {
            const QRect g = hw->window()->geometry();
            host = QStringLiteral(" 宿主=%1x%2@%3,%4").arg(g.width()).arg(g.height())
                       .arg(g.x()).arg(g.y());
        }
#endif
        /*
         * 画布自己那几针：右边三分之二那一片到底画成了什么色。
         *
         * 黑块量出来是"左边界停在某个 x 上、右边一片纯黑"。这一行是拿来分层的：
         *   * 画布里右边就是黑 -> 黑是**我们自己画出来的**（背景色/画布逻辑的问题）；
         *   * 画布里右边是 #1e1f22、屏幕上却是黑 -> 黑发生在**这一层之上**
         *     （呈现路径，或者有别的窗口盖在上面）。
         * 没有这两针，"画布对了屏幕还是黑"就只能靠猜是哪一层。
         */
        QString probes;
        if (m_canvas.width() >= 64 && m_canvas.height() >= 32) {
            /*
             * 取样点一律**算完再夹**，而且夹的上界必须 >= 下界 ——
             * 画布很窄时 `qBound(0, x, width-1)` 会变成 max<min，Qt 直接断言
             * （qminmax.h:26 "!(max < min)"）。这是我自己踩的，记在这儿。
             */
            const int xLo = 8;
            const int xHi = m_canvas.width() - 8;
            const int y1 = m_canvas.height() / 2;
            const int y2 = m_canvas.height() - 8;
            const int picks[] = { m_canvas.width() / 2, m_canvas.width() * 3 / 4,
                                  m_canvas.width() - 8, 8 };
            for (int x : picks) {
                const int cx = x < xLo ? xLo : (x > xHi ? xHi : x);
                const QRgb a = m_canvas.pixel(cx, y1);
                const QRgb b = m_canvas.pixel(cx, y2);
                probes += QStringLiteral(" (%1,%2)=%3/%4")
                              .arg(cx).arg(y1)
                              .arg(QString::fromLatin1(QColor(a).name().toLatin1()))
                              .arg(QString::fromLatin1(QColor(b).name().toLatin1()));
            }
        }
        termLog("帧", this,
                QStringLiteral(" 画布=%1x%2 纹理矩形=%3,%4 %5x%6 视口=%7x%8%9 画笔=%10 铺底=%11 画布取样:%12")
                    .arg(m_canvas.width()).arg(m_canvas.height())
                    .arg(0).arg(0)
                    .arg(m_canvas.deviceIndependentSize().width())
                    .arg(m_canvas.deviceIndependentSize().height())
                    .arg(vp.width()).arg(vp.height()).arg(host).arg(m_paints)
                    .arg(dbgLastFrameBgStats()).arg(probes));
    }

    auto *node = static_cast<QSGImageNode *>(oldNode);
    if (!node) {
        node = window()->createImageNode();
        node->setFiltering(QSGTexture::Linear);
    }
    /*
     * 贴纹理之前先把这块画布的**身份**记下来：节点指针、画布缓冲区首地址、尺寸、
     * 还有整块缓冲区的内容哈希（FNV-1a）。
     *
     * 用途：把"纹理里到底是哪一帧的像素"这件事变成可比对的数字。
     *   * 哈希每帧都在变（正常，内容在变）—— 但同一帧里 fill 之后和递完纹理之后
     *     再算一次，两次不一样就说明这块缓冲区在我们自己手里被改过；
     *   * 节点指针如果每帧都换，说明场景图在重建节点（意味着"上一帧那张纹理"确实
     *     可能还挂在屏幕上）；
     *   * 缓冲区首地址变化 = 画布被重新分配过。用户机器上那两次"右边一片黑"，
     *     这三个数字是唯一能区分"没画到"和"画到别人内存上"的证据。
     */
    QString frameId;
    if (termLogFile()) {
        quint64 h = 1469598103934665603ULL;
        const uchar *bits = m_canvas.constBits();
        const qsizetype total = m_canvas.sizeInBytes();
        for (qsizetype i = 0; i < total; i += 997)
            h = (h ^ bits[i]) * 1099511628211ULL;
        frameId = QStringLiteral(" node=%1 画布base=%2 尺寸=%3x%4 步进哈希=%5")
                      .arg(reinterpret_cast<quintptr>(node), 0, 16)
                      .arg(reinterpret_cast<quintptr>(bits), 0, 16)
                      .arg(m_canvas.width()).arg(m_canvas.height())
                      .arg(h, 0, 16);
    }
    node->setTexture(window()->createTextureFromImage(m_canvas));
    node->setOwnsTexture(true);
    /*
     * ① 矩形按**画布**算（deviceIndependentSize 已经把 DPR 折回去，单位是逻辑像素，
     *    和 item 的坐标同一套）。它有纹理作保，就一定铺在画出来的那块像素上。
     */
    node->setRect(QRectF(QPointF(0, 0), m_canvas.deviceIndependentSize()));
    if (!frameId.isEmpty())
        termLog("递交", this, frameId);
    return node;
}

/*
 * 画一帧到画布上。**尺寸一律取自 painter 自己的设备**（= 画布的逻辑尺寸），
 * 不读 width()/height()：圆角裁剪路径要是跟 item 的几何走，改尺寸那一拍只要还是旧宽度，
 * 裁剪区就只有旧宽度宽，铺底那一笔会全落在裁剪区外 —— 这条本身要成立。
 *
 * 但它**不是**那条硬边黑带的根因。用户日志里同一帧是这样的：
 *
 *     03:24:58.923 画布内存  每行=15188 末行末4字节=#1e1f22   ← fill 铺满到了右下角
 *     03:24:58.933 帧        画布取样 (1898,115)=#000000 (2847,115)=#000000
 *
 * 铺底铺到了、之后又被涂回黑 —— 涂它的是下面 drawRow() 里那些"非默认底色"的格子矩形
 * （那一帧数到 91 块 #ff000000）。根因和结案过程见 updatePaintNode 上面那一段。
 */
void TerminalView::paint(QPainter *painter)
{
    const QPaintDevice *dev = painter->device();
    const qreal w = dev->devicePixelRatio() > 0
                        ? dev->width() / dev->devicePixelRatio()
                        : dev->width();
    const qreal h = dev->devicePixelRatio() > 0
                        ? dev->height() / dev->devicePixelRatio()
                        : dev->height();
    if (w <= 0 || h <= 0)
        return;
    /*
     * 先按"只圆下面两个角"裁一刀再画：正文的底色是自己铺满矩形的，不裁就会在卡片
     * 下面那两个圆角上戳出一个方角（实测漏出 2px）。半径取 卡片半径 - 让开的那 2px，
     * 两条弧线才重合。上面两角不裁 —— 那一头接标签条，不是卡片的边。
     */
    if (m_cornerRadius > 0) {
        const qreal r = m_cornerRadius;
        QPainterPath clip;
        clip.moveTo(0, 0);
        clip.lineTo(w, 0);
        clip.lineTo(w, h - r);
        clip.quadTo(w, h, w - r, h);
        clip.lineTo(r, h);
        clip.quadTo(0, h, 0, h - r);
        clip.closeSubpath();
        painter->setClipPath(clip);
    }
    if (termLogFile()) {
        const QRectF r = painter->clipBoundingRect();
        termLog("落笔范围", this,
                QStringLiteral(" 画布=%1x%2 裁剪=%3,%4 %5x%6 item=%7x%8")
                    .arg(qRound(w)).arg(qRound(h))
                    .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                    .arg(width()).arg(height()));
    }
    painter->fillRect(QRectF(0, 0, w, h), m_bg);

    painter->setFont(m_font);
    painter->setRenderHint(QPainter::TextAntialiasing);
    painter->setRenderHint(QPainter::Antialiasing, false);

    const int rows = m_engine->rows();
    for (int i = 0; i < rows; ++i)
        drawRow(painter, i, docRowOf(i));

    /*
     * 光标只画"贴底 + 看得见"的那一种情况：滚上去读历史时不该有个东西在闪。
     * 没焦点时画空心框 —— 直接不画会让人以为 shell 挂了。
     */
    int curRow = 0, curCol = 0;
    m_engine->cursorPos(&curRow, &curCol);
    const bool caretHere = m_engine->running() && m_engine->cursorVisible()
                           && m_scrollUp == 0 && curRow >= 0 && curRow < rows
                           && curCol >= 0 && curCol < m_engine->cols();
    if (caretHere) {
        const QRectF box(m_padding + curCol * m_cellW, curRow * m_cellH, m_cellW, m_cellH);
        if (hasActiveFocus() && m_cursorOn) {
            painter->fillRect(box, m_cursorColor);
            /* 块光标盖住的那个字反色再画一遍，否则光标停在字上就看不见那个字 */
            TerminalCell cell;
            if (m_engine->cellAt(curRow, curCol, &cell) && cell.charCount && !cell.conceal) {
                painter->setPen(m_bg);
                painter->drawText(QPointF(box.x(), box.y() + m_ascent),
                                  QString::fromUcs4(cell.chars, cell.charCount));
            }
        } else {
            painter->setPen(QPen(m_cursorColor, 1));
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(box.adjusted(0.5, 0.5, -0.5, -0.5));
        }
    }

    if (!m_engine->running() && !m_errorText.isEmpty()) {
        painter->setPen(QColor(QStringLiteral("#f48771")));
        painter->drawText(QPointF(m_padding, m_ascent), m_errorText);
    }
}

void TerminalView::drawRow(QPainter *painter, int viewRow, int docRow)
{
    const int cols = m_engine->cols();
    if (cols <= 0)
        return;

    m_rowScratch.resize(cols);
    for (int col = 0; col < cols; ++col) {
        if (!m_engine->cellAt(docRow, col, &m_rowScratch[col]))
            m_rowScratch[col] = TerminalCell {};
    }

    int selA = 0, selCa = 0, selB = 0, selCb = 0;
    const bool hasSel = selectionRange(&selA, &selCa, &selB, &selCb);
    auto selected = [&](int row, int col) {
        if (!hasSel || row < selA || row > selB)
            return false;
        if (row > selA && row < selB)
            return true;
        if (row == selA && row == selB)
            return col >= selCa && col <= selCb;
        return row == selA ? col >= selCa : col <= selCb;
    };

    const qreal y = viewRow * m_cellH;

    /* 第一遍：背景。同色的相邻格并成一个矩形（满屏彩色输出时省掉九成矩形） */
    auto bgOf = [&](int col) {
        const TerminalCell &cell = m_rowScratch[col];
        QColor c = cell.reverse ? cell.fg : cell.bg;
        if (selected(docRow, col))
            c = m_selColor;   // 半透明叠上去，字的颜色保持原样（终端的通用做法）
        return c;
    };
    int runStart = 0;
    QColor runColor = bgOf(0);
    for (int col = 1; col <= cols; ++col) {
        const QColor c = col < cols ? bgOf(col) : m_bg;
        if (col == cols || c != runColor) {
            if (runColor != m_bg) {
                const QRectF r(m_padding + runStart * m_cellW, y,
                               qreal(col - runStart) * m_cellW, m_cellH);
                painter->fillRect(r, runColor);
                /*
                 * 记录器：数一数"这一帧我们到底往画布上铺了几块非默认底色"，
                 * 以及里面有没有纯黑的那几块。
                 *
                 * 为什么要数：屏幕上那条硬边黑矩形（自检和用户机器两边都量到 2384 px，
                 * 起点正好停在改尺寸**之前**的宽度上）只有两种可能 ——
                 *   ① 我们在那些格子上主动铺了黑（bgOf 交出来一个非默认的底色）；
                 *   ② 那一片根本没人画，留着的还是新 QImage 的初始黑。
                 * 这两件事的修法完全相反（① 查引擎的颜色解析，② 查铺底为什么没盖住），
                 * 所以必须在这一笔落下的地方分清，不能再到上层猜。
                 */
                ++m_dbgBgRects;
                const bool blackish = runColor.red() < 6 && runColor.green() < 6
                                      && runColor.blue() < 6 && runColor.alpha() > 0;
                if (blackish) {
                    ++m_dbgBlackRects;
                    if (m_dbgBlackFirst.isEmpty())
                        /* 带上这一格的码点和格宽：0xffffffff/宽 0 = 宽字符的尾巴格，
                           0 = 擦出来的空格，别的 = shell 真写了一个黑底的字。 */
                        m_dbgBlackFirst = QStringLiteral("%1@%2,%3 %4x%5 第%6格字=%7 宽=%8")
                                              .arg(runColor.name(QColor::HexArgb))
                                              .arg(qRound(r.x())).arg(qRound(r.y()))
                                              .arg(qRound(r.width())).arg(qRound(r.height()))
                                              .arg(runStart)
                                              .arg(m_rowScratch[runStart].chars[0], 0, 16)
                                              .arg(m_rowScratch[runStart].width);
                }            }
            runStart = col;
            runColor = c;
        }
    }

    /* 第二遍：字。空格子和宽字符的尾巴格跳过（终端里大半格子是空的） */
    int appliedStyle = -1;
    for (int col = 0; col < cols; ++col) {
        const TerminalCell &cell = m_rowScratch[col];
        if (cell.width == 0 || cell.conceal || cell.charCount == 0 || cell.chars[0] == U' ')
            continue;

        const int style = (cell.bold ? 1 : 0) | (cell.italic ? 2 : 0) | (cell.strikeOut ? 4 : 0);
        if (style != appliedStyle) {
            /* 只在样式真的变了时才换 QFont：每格拷一份 QFont 是这一层最贵的动作 */
            QFont f = m_font;
            f.setBold(cell.bold);
            f.setItalic(cell.italic);
            f.setStrikeOut(cell.strikeOut);
            painter->setFont(f);
            appliedStyle = style;
        }
        painter->setPen(cell.reverse ? cell.bg : cell.fg);
        /* 基线是"这一行的顶端 + 上升部"。少加行偏移的话每一行都叠在第一行上（踩过） */
        painter->drawText(QPointF(m_padding + col * m_cellW, y + m_ascent),
                          QString::fromUcs4(cell.chars, cell.charCount));

        if (cell.underline) {
            const qreal uy = m_ascent + std::max<qreal>(1.0, m_cellH * 0.12);
            painter->fillRect(QRectF(m_padding + col * m_cellW, uy, m_cellW,
                                     std::max<qreal>(1.0, m_cellH * 0.06)),
                              cell.reverse ? cell.bg : cell.fg);
        }
    }
}

/*
 * 扫**画布**自己：最长连续纯黑段。和自检里那条"抓屏幕量最长黑段"配成一对，
 * 用来分"黑是我们画出来的"还是"黑在呈现那一层"。见 TerminalView.h 里的声明。
 */
QString TerminalView::dbgCanvasBlackRun()
{
    if (m_canvas.isNull() || m_canvas.width() < 8 || m_canvas.height() < 8)
        return QStringLiteral("画布空 %1x%2").arg(m_canvas.width()).arg(m_canvas.height());
    const int h = m_canvas.height();
    const int w = m_canvas.width();
    int worst = 0, wx = 0, wy = 0, blackSamples = 0, samples = 0;
    const int stepY = qMax(4, int(m_cellH));
    for (int y = h / 4; y < h * 3 / 4; y += stepY) {
        int run = 0;
        for (int x = 0; x < w; ++x) {
            const QRgb p = m_canvas.pixel(x, y);
            if (qRed(p) < 6 && qGreen(p) < 6 && qBlue(p) < 6) {
                if (++run > worst) {
                    worst = run;
                    wx = x - run + 1;
                    wy = y;
                }
            } else {
                run = 0;
            }
        }
    }
    /* 稀疏采一整片的黑比"某一行的一条"更能说明"右边那块从来没铺过底" */
    for (int y = 0; y < h; y += 8) {
        for (int x = 0; x < w; x += 8) {
            const QRgb p = m_canvas.pixel(x, y);
            ++samples;
            if (qRed(p) < 6 && qGreen(p) < 6 && qBlue(p) < 6)
                ++blackSamples;
        }
    }
    m_dbgCanvasRun = worst;
    m_dbgCanvasBlackSamples = blackSamples;
    return QStringLiteral("%1@%2,%3（画布 %4x%5 采样 %6/%7 是黑）")
        .arg(worst).arg(wx).arg(wy).arg(w).arg(h).arg(blackSamples).arg(samples);
}
// ------------------------------------------------------------------ 选区

bool TerminalView::selectionRange(int *rowA, int *colA, int *rowB, int *colB) const
{
    if (!m_selActive)
        return false;
    int a = m_selRowA, b = m_selRowB;
    int ca = m_selColA, cb = m_selColB;
    if (a > b || (a == b && ca > cb)) {
        std::swap(a, b);
        std::swap(ca, cb);
    }
    *rowA = a;
    *colA = ca;
    *rowB = b;
    *colB = cb;
    return true;
}

void TerminalView::notifySelection()
{
    const bool now = m_selActive;
    if (now != m_hasSelNotified) {
        m_hasSelNotified = now;
        emit selectionChanged();
    }
    update();
}

QString TerminalView::textInRange(int rowA, int colA, int rowB, int colB) const
{
    QString out;
    TerminalCell cell;
    for (int row = rowA; row <= rowB; ++row) {
        const int from = row == rowA ? colA : 0;
        const int to = row == rowB ? colB : m_engine->cols();
        QString line;
        for (int col = from; col < to; ++col) {
            if (!m_engine->cellAt(row, col, &cell) || cell.width == 0)
                continue;
            line += cell.charCount ? QString::fromUcs4(cell.chars, cell.charCount)
                                   : QString(QLatin1Char(' '));
        }
        while (!line.isEmpty() && line.at(line.size() - 1) == QLatin1Char(' '))
            line.chop(1);
        out += line;
        if (row != rowB)
            out += QLatin1Char('\n');
    }
    return out;
}

QString TerminalView::selectedText() const
{
    int a, ca, b, cb;
    if (!selectionRange(&a, &ca, &b, &cb))
        return QString();
    return textInRange(a, ca, b, cb + 1);
}

void TerminalView::copySelection()
{
    const QString text = selectedText();
    if (text.isEmpty())
        return;
    QGuiApplication::clipboard()->setText(text);
    clearSelection();
}

void TerminalView::clearSelection()
{
    m_selActive = false;
    notifySelection();
}

void TerminalView::pasteClipboard()
{
    const QString text = QGuiApplication::clipboard()->text();
    if (!text.isEmpty())
        m_engine->pasteText(text);
}

void TerminalView::selectWordAt(int docRow, int col)
{
    TerminalCell cell;
    if (!m_engine->cellAt(docRow, col, &cell) || cell.charCount == 0
        || !isWordChar(QChar(cell.chars[0])))
        return;
    int from = col, to = col;
    while (from > 0 && m_engine->cellAt(docRow, from - 1, &cell) && cell.charCount
           && isWordChar(QChar(cell.chars[0])))
        --from;
    while (to + 1 < m_engine->cols() && m_engine->cellAt(docRow, to + 1, &cell) && cell.charCount
           && isWordChar(QChar(cell.chars[0])))
        ++to;
    m_selActive = true;
    m_selRowA = m_selRowB = docRow;
    m_selColA = from;
    m_selColB = to;
    notifySelection();
}

// ------------------------------------------------------------------ 鼠标

QPoint TerminalView::cellAtPos(const QPointF &pos) const
{
    const int col = std::clamp(int((pos.x() - m_padding) / m_cellW), 0, m_engine->cols() - 1);
    const int viewRow = std::clamp(int(pos.y() / m_cellH), 0, m_engine->rows() - 1);
    return { col, docRowOf(viewRow) };
}

void TerminalView::mousePressEvent(QMouseEvent *event)
{
    forceActiveFocus();
    const QPoint cell = cellAtPos(event->position());

    /*
     * 对面开了鼠标上报（vim 的 :set mouse=a、fzf 的列表）时点击不该选字，
     * 要转成转义序列发过去。按住 Shift 是公认的"我要选字不要转发"逃生口。
     * 只有贴底才转发：滚上去看历史时的点击属于阅读，不该喂给程序。
     */
    if (m_engine->mouseReporting() && !(event->modifiers() & Qt::ShiftModifier)
        && m_scrollUp == 0 && cell.y() >= 0) {
        const int button = event->button() == Qt::RightButton    ? 3
                           : event->button() == Qt::MiddleButton ? 2
                                                                 : 1;
        m_engine->mouseButton(button, true, cell.y(), cell.x(),
                              modifierBits(event->modifiers()));
        m_forwardingMouse = true;
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        pasteClipboard();   // Windows 终端惯例：中键 = 粘贴
        return;
    }
    /*
     * 右键：弹**程序自己那套**自绘菜单（和编辑区、左树、tab 同一份 DropdownMenu，
     * 差别只在条目）。这里不清选区 —— 右键之前多半刚选好字，菜单上的"复制"要用；
     * 而且右键一落就把高亮抹掉，看起来又像"选不中"。
     */
    if (event->button() == Qt::RightButton) {
        emit contextMenuRequested(event->position().x(), event->position().y());
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    m_selecting = true;
    m_selActive = true;
    m_selRowA = m_selRowB = cell.y();
    m_selColA = m_selColB = cell.x();
    notifySelection();
}

void TerminalView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint cell = cellAtPos(event->position());
    if (m_forwardingMouse && m_scrollUp == 0 && cell.y() >= 0) {
        m_engine->mouseMove(cell.y(), cell.x(), modifierBits(event->modifiers()));
        return;
    }
    if (!m_selecting)
        return;
    m_selRowB = cell.y();
    m_selColB = cell.x();
    notifySelection();
}

void TerminalView::mouseReleaseEvent(QMouseEvent *event)
{
    const QPoint cell = cellAtPos(event->position());
    if (m_forwardingMouse) {
        const int button = event->button() == Qt::RightButton    ? 3
                           : event->button() == Qt::MiddleButton ? 2
                                                                 : 1;
        if (m_scrollUp == 0 && cell.y() >= 0)
            m_engine->mouseButton(button, false, cell.y(), cell.x(),
                                  modifierBits(event->modifiers()));
        m_forwardingMouse = false;
        return;
    }
    if (!m_selecting)
        return;
    m_selecting = false;
    /* 选中即复制（Windows Terminal / VS Code 的默认行为），拖一个点不算选 */
    if (m_selRowA != m_selRowB || m_selColA != m_selColB) {
        /*
         * 复制完**留着高亮**。原来这里调的是 copySelection()，它顺手把选区清了 ——
         * 鼠标一松字上的底色就没了，用户报的就是"终端页面不能选中"
         * （2026-09-23）。VS Code / Windows Terminal 松手之后高亮都还在，
         * 要撤是下一次按下、Esc、或菜单里点走。
         * Ctrl+C 那条还是走 copySelection()（复制完清选区，下一次 Ctrl+C 才是打断）。
         */
        const QString text = selectedText();
        if (!text.isEmpty())
            QGuiApplication::clipboard()->setText(text);
        notifySelection();
    } else {
        clearSelection();
    }
}

void TerminalView::selectAll()
{
    /* 从回滚区最老那一行的第 0 列，一直到当前屏最后一行的最后一列 */
    m_selActive = true;
    m_selRowA = -m_engine->historyRows();
    m_selColA = 0;
    m_selRowB = m_engine->rows() - 1;
    m_selColB = qMax(0, m_engine->cols() - 1);
    notifySelection();
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (m_engine->mouseReporting() && !(event->modifiers() & Qt::ShiftModifier))
        return;
    const QPoint cell = cellAtPos(event->position());
    selectWordAt(cell.y(), cell.x());
    copySelection();
}

void TerminalView::wheelEvent(QWheelEvent *event)
{
    const int steps = event->angleDelta().y() / 120;
    if (steps == 0)
        return;
    /*
     * 往前滚（angleDelta > 0）= 看更早的内容 = scrollUp 变大。
     * 原来写的是减号，方向和所有终端相反（用户报的：滚轮往前、滚动条往下）。
     */
    setScrollUp(m_scrollUp + steps * 3);
    event->accept();
}

// ------------------------------------------------------------------ 键盘

void TerminalView::focusInEvent(QFocusEvent *event)
{
    QQuickItem::focusInEvent(event);
    update();
}

void TerminalView::focusOutEvent(QFocusEvent *event)
{
    QQuickItem::focusOutEvent(event);
    m_cursorOn = false;
    update();
}

bool TerminalView::sendCtrlKey(QKeyEvent *event)
{
    const int key = event->key();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) {
        m_engine->sendBytes(QByteArray(1, char(key - Qt::Key_A + 1)));
        return true;
    }
    switch (key) {
    case Qt::Key_Space:
        m_engine->sendBytes(QByteArray(1, '\x00'));
        return true;
    case Qt::Key_BracketLeft:
        m_engine->sendBytes(QByteArray(1, '\x1b'));
        return true;
    case Qt::Key_Backslash:
        m_engine->sendBytes(QByteArray(1, '\x1c'));
        return true;
    case Qt::Key_BracketRight:
        m_engine->sendBytes(QByteArray(1, '\x1d'));
        return true;
    case Qt::Key_AsciiCircum:
        m_engine->sendBytes(QByteArray(1, '\x1e'));
        return true;
    case Qt::Key_Underscore:
        m_engine->sendBytes(QByteArray(1, '\x1f'));
        return true;
    default:
        return false;
    }
}

void TerminalView::keyPressEvent(QKeyEvent *event)
{
    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const bool alt = event->modifiers() & Qt::AltModifier;

    /* Ctrl+Shift+X 这一族是本程序自己的快捷键，不能被 shell 吃掉 */
    if (ctrl && shift) {
        switch (event->key()) {
        case Qt::Key_C:
            copySelection();
            return;
        case Qt::Key_V:
            pasteClipboard();
            return;
        case Qt::Key_K:
            clearBuffer();
            return;
        default:
            break;
        }
    }

    if (ctrl && !shift && !alt && sendCtrlKey(event))
        return;

    /*
     * Ctrl+C 分两种：有选区 = 复制，没选区 = 打断。
     * 这条判断顺序反了就会很难用 —— 想中断一个跑飞的命令，结果因为上一次选的
     * 字还在选区里，复制了一下、命令没停。所以这里"复制完就清选区"（见
     * copySelection），下一次 Ctrl+C 一定是打断。
     */
    if (event->key() == Qt::Key_C && ctrl && !shift && hasSelection()) {
        copySelection();
        return;
    }

    if (event->key() == Qt::Key_Insert && shift) {
        pasteClipboard();
        return;
    }

    const int vk = vtermKeyFor(event->key());
    if (vk >= 0) {
        m_engine->sendKey(vk, modifierBits(event->modifiers()));
        if (m_scrollUp != 0)
            setScrollUp(0);   // 一敲键盘就回到底部（和所有终端一样）
        return;
    }

    const QString text = event->text();
    if (!text.isEmpty() && (alt || text.at(0).unicode() >= 32)) {
        if (alt)
            m_engine->sendBytes("\x1b");
        m_engine->sendText(text);
        if (m_scrollUp != 0)
            setScrollUp(0);
        return;
    }
    event->ignore();
}

void TerminalView::inputMethodEvent(QInputMethodEvent *event)
{
    if (!event->commitString().isEmpty()) {
        m_engine->sendText(event->commitString());
        if (m_scrollUp != 0)
            setScrollUp(0);
    }
    event->accept();
}

QVariant TerminalView::inputMethodQuery(Qt::InputMethodQuery query) const
{
    switch (query) {
    case Qt::ImCursorRectangle: {
        int row = 0, col = 0;
        m_engine->cursorPos(&row, &col);
        return QVariant::fromValue(QRectF(m_padding + col * m_cellW, row * m_cellH, m_cellW,
                                          m_cellH));
    }
    case Qt::ImFont:
        return QVariant::fromValue(m_font);
    case Qt::ImEnabled:
        return true;
    default:
        return QQuickItem::inputMethodQuery(query);
    }
}

/*
 * 改尺寸 600ms 之后抓**真实屏幕**，量视图矩形里的最长黑段。
 *
 * 为什么要它：几何日志已经证明画布每一级都跟上了（改完 3ms 内重建），可你屏幕上
 * 照样是黑的 —— 那黑就发生在纹理之后（离屏回读 / 呈现那一层）。这一条把
 * "画布对但屏幕黑"变成你机器上的一个数字，不用再靠猜是哪一层。
 */
void TerminalView::checkScreenForBlack()
{
    QWindow *w = window();
    QScreen *scr = w ? w->screen() : nullptr;
    if (!w || !scr)
        return;
    const qreal dpr = w->devicePixelRatio();
    /*
     * 全局坐标要从**宿主 QWidget** 算，不能用离屏 QQuickWindow 的 mapToGlobal：
     * 那个窗口没有真实位置，实测返回 (1189,605) 这种值，于是抓屏矩形跑到屏幕外，
     * 屏幕外全是黑 —— 我拿这把坏尺子量出来的"最长黑段 3782"全是假的。
     * SizeRootObjectToView 下场景坐标 == 控件坐标，直接 mapToGlobal 即可。
     */
    QWidget *hostWidget = qobject_cast<QWidget *>(w->parent());
#if defined(Q_OS_WIN)
    if (!hostWidget) {
        /*
         * 离屏窗口的父不一定是 QWidget（QQuickWidget 的离屏窗口就挂在自己的 QObject
         * 层级下）。退一步：在同进程的顶层控件里找**当前活动**的那个，找不到就用
         * 第一个够大的。找不到就照实说，不能静默 return —— 静默会让人以为"没黑块"，
         * 其实是没量成。
         */
        for (QWidget *cand : QApplication::topLevelWidgets()) {
            if (!cand->isVisible() || cand->width() < 600)
                continue;
            if (QApplication::activeWindow() == cand) {
                hostWidget = cand;
                break;
            }
            if (!hostWidget)
                hostWidget = cand;
        }
    }
#endif
    if (!hostWidget) {
        /* 静默 return 会让人以为"没黑块"，其实是没量成。必须留话。 */
        termLog("黑块复查", this, QStringLiteral("量不了：离屏窗口的父控件不是 QWidget（%1）")
                                         .arg(QString::fromLatin1(
                                             w->parent() ? w->parent()->metaObject()->className()
                                                         : "(null)")));
        return;
    }
    const QPoint sceneTopLeft = mapToScene(QPointF(0, 0)).toPoint();
    const QPoint global = hostWidget->mapToGlobal(sceneTopLeft);
    const QPoint grabAt(global.x() * dpr, global.y() * dpr);
    const QSize sz = (size() * dpr).toSize();
    const QImage img = scr->grabWindow(0, grabAt.x(), grabAt.y(), sz.width(), sz.height()).toImage();
    if (img.isNull()) {
        termLog("黑块复查", this, QStringLiteral("抓屏失败 矩形%1x%2 @%3,%4")
                                         .arg(sz.width()).arg(sz.height()).arg(grabAt.x()).arg(grabAt.y()));
        return;
    }
    int worst = 0, wx = 0, wy = 0;
    for (int y = 0; y < img.height(); y += 2) {
        int run = 0;
        for (int x = 0; x < img.width(); ++x) {
            const QRgb p = img.pixel(x, y);
            if (qRed(p) < 8 && qGreen(p) < 8 && qBlue(p) < 8) {
                if (++run > worst) {
                    worst = run;
                    wx = x - run + 1;
                    wy = y;
                }
            } else {
                run = 0;
            }
        }
    }
    /*
     * 屏幕取样 vs 画布取样：这一对数字是拿来**分层**的，不是拿来判有没有黑。
     *
     * 左边是"屏幕上这一针到底是什么色"，右边是"我们自己画布里同一针是什么色"：
     *   * 两个都黑  -> 黑是**我们画出来的**（背景色/画布逻辑）；
     *   * 画布正常、屏幕黑 -> 黑在**这一层之上**：呈现路径，或者有别的窗口盖上来。
     * 没有这一对，"画布对了屏幕还是黑"永远只能猜是哪一层。
     */
    QString cmp;
    if (img.width() >= 64 && img.height() >= 8 && m_canvas.width() >= 1
        && m_canvas.height() >= 1) {
        const qreal sx = qreal(img.width()) / m_canvas.width();
        const qreal sy = qreal(img.height()) / m_canvas.height();
        const int picks[] = { img.width() / 4, img.width() / 2, img.width() * 3 / 4,
                              img.width() - 8 };
        for (int px : picks) {
            /* 同上：夹的上界必须 >= 下界，否则 qBound 直接断言 */
            const int x = px < 0 ? 0 : (px > img.width() - 1 ? img.width() - 1 : px);
            const int y = img.height() / 2;
            const QRgb onScreen = img.pixel(x, y);
            const int cxRaw = int(x / sx);
            const int cyRaw = int(y / sy);
            const int cx = cxRaw < 0 ? 0
                                     : (cxRaw > m_canvas.width() - 1 ? m_canvas.width() - 1
                                                                     : cxRaw);
            const int cy = cyRaw < 0 ? 0
                                     : (cyRaw > m_canvas.height() - 1 ? m_canvas.height() - 1
                                                                      : cyRaw);
            const QRgb inCanvas = m_canvas.isNull() ? qRgb(0, 0, 0) : m_canvas.pixel(cx, cy);
            cmp += QStringLiteral(" [x=%1 屏幕=%2 画布=%3]")
                       .arg(x)
                       .arg(QString::fromLatin1(QColor(onScreen).name().toLatin1()))
                       .arg(QString::fromLatin1(QColor(inCanvas).name().toLatin1()));
        }
    }
    termLog("黑块复查", this, QStringLiteral("抓屏 %1x%2 最长黑段 %3 px @ %4,%5%6")
                                     .arg(img.width()).arg(img.height()).arg(worst).arg(wx).arg(wy)
                                     .arg(cmp));
    /*
     * 把这次抓屏的**坐标来源**也记下来：宿主控件是谁、它相对屏幕在哪、这个视图
     * 在窗口里的位置。这样日志里那个"最长黑段 @ x,y"才能换算回窗口坐标，
     * 和截图里的 x/y 对上号 —— 不然"黑段 @ 1812"到底是窗口哪一块，说不清。
     */
    termLog("黑块复查坐标", this,
            QStringLiteral("宿主=%1 %2x%3@%4,%5 视图窗口内=%6,%7 %8x%9 抓屏原点=%10,%11")
                .arg(QString::fromLatin1(hostWidget->metaObject()->className()))
                .arg(hostWidget->width()).arg(hostWidget->height())
                .arg(hostWidget->x()).arg(hostWidget->y())
                .arg(sceneTopLeft.x()).arg(sceneTopLeft.y())
                .arg(width()).arg(height())
                .arg(grabAt.x()).arg(grabAt.y()));

    /*
     * 同一时刻把宿主窗口下面的**原生子窗口**全列出来。
     * 判据：黑段左边界钉在 1406（= 最大化之前 item 的宽）不动，而我们每次都把纹理
     * 重建到当帧尺寸 —— 纹理要画错会跟着画布走。不跟，就是有别的东西盖在 QML 上面。
     * 编辑区 QScintilla 是 createWindowContainer 出来的原生子窗，正是要找的对象。
     */
    QString kids;
    /*
     * 句柄要从**宿主 QWidget** 拿，不能对 w->winId()：这块 QQuickWindow 是 QQuickWidget
     * 的离屏窗口，对它调 winId() 会逼它建原生句柄，Qt 自己断言挂掉
     * （ASSERT: "!d->offscreenWindow->handle()"，qquickwidget.cpp:1312）。
     */
    HWND host = nullptr;
    if (auto *pw = qobject_cast<QWidget *>(w->parent()))
        host = reinterpret_cast<HWND>(pw->window()->winId());
    if (host) {
        EnumChildWindows(
            host,
            [](HWND h, LPARAM l) {
                auto *out = reinterpret_cast<QString *>(l);
                RECT r {};
                GetWindowRect(h, &r);
                wchar_t cls[128] = L"";
                GetClassNameW(h, cls, 128);
                *out += QStringLiteral(" [%1 %2,%3~%4,%5 vis=%6]")
                            .arg(QString::fromWCharArray(cls))
                            .arg(r.left).arg(r.top).arg(r.right).arg(r.bottom)
                            .arg(IsWindowVisible(h));
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&kids));
    }
    const QPoint origin = hostWidget->mapToGlobal(QPoint(0, 0));
    termLog("原生子窗", this,
            QStringLiteral("宿主原点 %1,%2 视图全局 %3,%4 子窗:%5")
                .arg(origin.x()).arg(origin.y()).arg(global.x()).arg(global.y()).arg(kids));
}
