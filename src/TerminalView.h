#pragma once

#include "TerminalEngine.h"

#include <QImage>
#include <QPoint>
#include <QQuickItem>
#include <QVector>

class QKeyEvent;
class QTimer;

/*
 * 终端面板的正文：一个自绘的字符网格。
 *
 * 为什么是 QQuickPaintedItem 而不是原生子窗口（QScintilla 那种 createWindowContainer
 * 的路子）：本项目已经被原生子窗口咬过好几口 —— 盖住的范围内 QML 收不到鼠标、
 * 下拉菜单和浮层要专门绕、DPI 缩放各算各的（见 qml/components/FindBar.qml 开头那段）。
 * 终端要接住键盘、要画选区、要和上面那条标签条对齐，全在 QML 这一层里最好办。
 *
 * 为什么不用 QML 的 Text 逐行摆：一屏 120x40 = 4800 个格子，每格颜色/粗体各不同，
 * 用 Item 树表达等于每帧重建几千个节点；而终端的输出是**突发**的（一次刷屏几十行），
 * 这条路会在刷屏时直接卡住。自己画一遍网格反而可控：空格子不画、同色背景合并成一条。
 *
 * 尺寸换算：面板拖高拖宽 -> geometryChange -> 按格子宽高算出新的行列数 -> 告诉引擎
 * -> 引擎改伪控制台 -> 对面按新尺寸重排并把重排后的屏幕重新打一遍。也就是说
 * **调整窗口大小这件事不需要我们补画任何东西**，内容会自己跟上。
 */
class TerminalView : public QQuickItem
{
    Q_OBJECT

    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY failed)

    /* 贴不贴底：滚上去看历史时为 false，标签条上那个"回到底部"的提示靠它 */
    Q_PROPERTY(bool atBottom READ atBottom NOTIFY atBottomChanged)
    /* 往上滚了几行（0 = 贴底）；面板右侧那条回滚位置条读它 */
    Q_PROPERTY(int scrollUp READ scrollUp WRITE setScrollUp NOTIFY atBottomChanged)
    Q_PROPERTY(bool hasSelection READ hasSelection NOTIFY selectionChanged)

    /* 字体与配色（QML 侧统一改，引擎那边的"默认前景/背景"跟着走） */
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY fontChanged)
    Q_PROPERTY(qreal fontSize READ fontSize WRITE setFontSize NOTIFY fontChanged)
    Q_PROPERTY(QColor foregroundColor READ foregroundColor WRITE setForegroundColor
                   NOTIFY themeChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor
                   NOTIFY themeChanged)
    Q_PROPERTY(QColor selectionColor READ selectionColor WRITE setSelectionColor
                   NOTIFY themeChanged)
    Q_PROPERTY(QColor cursorColor READ cursorColor WRITE setCursorColor NOTIFY themeChanged)
    Q_PROPERTY(qreal padding READ padding WRITE setPadding NOTIFY themeChanged)
    /*
     * 只圆**下面两个角**的半径。正文自己铺满矩形底色，直角会戳出卡片的圆角；
     * 卡片让开 2px、这里就填 10 - 2 = 8，两条弧线正好重合（圆心都是卡片角那个点）。
     * 上面两个角不圆 —— 那一头接的是标签条，不是卡片的边。
     */
    Q_PROPERTY(qreal cornerRadius READ cornerRadius WRITE setCornerRadius NOTIFY themeChanged)

    /* 量出来的东西：自检和布局都要用（格子尺寸 / 行列数 / 当前屏第一行文字） */
    Q_PROPERTY(qreal cellWidth READ cellWidth NOTIFY fontChanged)
    Q_PROPERTY(qreal cellHeight READ cellHeight NOTIFY fontChanged)
    Q_PROPERTY(int columns READ columns NOTIFY gridChanged)
    Q_PROPERTY(int rows READ rows NOTIFY gridChanged)
    Q_PROPERTY(int historyRows READ historyRows NOTIFY gridChanged)

public:
    explicit TerminalView(QQuickItem *parent = nullptr);
    ~TerminalView() override;

    /* program 传空 = 系统默认 shell；workingDir 传空 = 用户主目录 */
    Q_INVOKABLE bool start(const QString &program, const QString &workingDir);
    Q_INVOKABLE void closeSession();

    Q_INVOKABLE void copySelection();
    Q_INVOKABLE void pasteClipboard();
    Q_INVOKABLE QString selectedText() const;
    Q_INVOKABLE void clearSelection();
    /* 右键菜单里的"全选"：从回滚区最老那一行一直到当前屏最后一行 */
    Q_INVOKABLE void selectAll();
    /* 清屏 + 清回滚（面板上那个垃圾桶） */
    Q_INVOKABLE void clearBuffer();

    Q_INVOKABLE void scrollToEnd();
    Q_INVOKABLE void scrollLines(int delta);

    /* 自检用：读某一行的文字（行号约定见 TerminalEngine.h） */
    Q_INVOKABLE QString lineTextAt(int row) const;
    /*
     * 自检用：**当前视图最上面那一行**的文字。
     * 滚轮方向对不对，只有量这个才说得清 —— 往前滚之后它必须变成更早的那一行。
     */
    Q_INVOKABLE QString topVisibleText() const;

    /*
     * 自检用：paint() 被叫了几次、其中几次 Qt 给的裁剪区盖满了整块。
     * 抓屏幕量不到"拖动途中那一闪"（一次 grabWindow 就 50~100ms，中间帧早没了），
     * 这一层能：改完尺寸要是**一次满幅重绘都没发生**，新露出来的那条必然没人画过。
     */
    Q_INVOKABLE void dbgResetPaintStats() { m_paints = 0; m_fullPaints = 0; }
    Q_INVOKABLE int dbgPaints() const { return m_paints; }
    Q_INVOKABLE int dbgFullPaints() const { return m_fullPaints; }
    /*
     * 自检用：问某一号 ANSI 色**现在**换算成什么 RGB。
     * 判"浅色档那套黄有没有生效"只能这么问 —— 扫屏幕像素的话，得先凑出一段
     * 用 3 号色的输出、还得躲开光标，而调色板本身就是一个数，直接读它最省。
     */
    Q_INVOKABLE QColor ansiColorForTest(int index) const { return m_engine->ansiColor(index); }
    /* 自检/几何记录器用：画布实际多大（黑块判据就是"画布有没有跟上 item"） */
    QSize canvasSizeForLog() const { return m_canvas.size(); }
    /*
     * 自检用：直接扫**我们交给场景图的那张画布**，报里面最长的连续纯黑段。
     *
     * 为什么还要这一把尺子：屏幕上量到的黑有两条可能的来源 —— 我们自己画出来的，
     * 或者呈现那一层没把纹理贴上去。这两件事要分开量才有结论。用户机器上开了
     * SMARTCLIP_TERM_LOG 之后"画布取样"那一行已经给出过一次 #000000（画布自己是黑的），
     * 而同一时刻格子层扫出来的底色**全是默认色**（自检量的），两边对不上，
     * 所以必须在同一个时机并排量这两个数，不能再靠第三层猜。
     * 注意：画布由渲染线程写、这里由 GUI 线程读，拿到的是"大概"这一帧。
     */
    Q_INVOKABLE QString dbgCanvasBlackRun();
    /* 上面那次扫描顺带存下来的两个数，给自检当判据用（-1 = 还没扫过） */
    int dbgCanvasRun() const { return m_dbgCanvasRun; }
    int dbgCanvasBlackSamples() const { return m_dbgCanvasBlackSamples; }

    /*
     * 自检用：上一帧我们**主动**往画布上铺了几块"非默认底色"的矩形，其中几块是纯黑。
     * 和 dbgCanvasBlackRun() 配成一对：画布上那片黑到底是我们自己铺的，还是新位图
     * 初始黑根本没被铺底盖住 —— 这两种的修法相反。
     */
    Q_INVOKABLE QString dbgLastFrameBgStats() const
    {
        return QStringLiteral("非默认底 %1 块 / 其中纯黑 %2 块 首块=%3")
            .arg(m_dbgBgRects).arg(m_dbgBlackRects)
            .arg(m_dbgBlackFirst.isEmpty() ? QStringLiteral("-") : m_dbgBlackFirst);
    }
    /* 自检判据用：上一帧我们主动铺了几块纯黑底格子（修好之后必须是 0） */
    int dbgBlackRects() const { return m_dbgBlackRects; }

    QString title() const { return m_engine->displayTitle(); }
    bool running() const { return m_engine->running(); }
    QString errorText() const { return m_errorText; }
    bool atBottom() const { return m_scrollUp <= 0; }
    int scrollUp() const { return m_scrollUp; }
    bool hasSelection() const { return m_selActive; }

    QString fontFamily() const { return m_fontFamily; }
    void setFontFamily(const QString &name);
    qreal fontSize() const { return m_fontSize; }
    void setFontSize(qreal pixels);

    /*
     * 真正用来画字的那对值（= 基线被方案 font 段盖过之后的结果）。
     * fontFamily / fontSize 那两个属性报的是**基线**（用户在设置里选的），所以自检
     * 要看"方案生效没"必须读这两个，别读那两本账里的另一本。
     */
    Q_INVOKABLE qreal usedFontPixelSizeForTest() const { return m_usedSize; }
    Q_INVOKABLE QString usedFontFamilyForTest() const { return m_usedFamily; }

    QColor foregroundColor() const { return m_fg; }
    void setForegroundColor(const QColor &c);
    QColor backgroundColor() const { return m_bg; }
    void setBackgroundColor(const QColor &c);
    QColor selectionColor() const { return m_selColor; }
    void setSelectionColor(const QColor &c);
    QColor cursorColor() const { return m_cursorColor; }
    void setCursorColor(const QColor &c);

    qreal padding() const { return m_padding; }
    void setPadding(qreal px);

    qreal cornerRadius() const { return m_cornerRadius; }
    void setCornerRadius(qreal px);

    qreal cellWidth() const { return m_cellW; }
    qreal cellHeight() const { return m_cellH; }
    int columns() const { return m_engine->cols(); }
    int rows() const { return m_engine->rows(); }
    int historyRows() const { return m_engine->historyRows(); }

    TerminalEngine *engine() const { return m_engine; }

signals:
    void titleChanged();
    void runningChanged();
    void failed(const QString &reason);
    void atBottomChanged();
    void selectionChanged();
    void fontChanged();
    void themeChanged();
    void gridChanged();
    /*
     * 在正文上按了右键（且对面没开鼠标上报）。x/y 是**本 item 的本地坐标** ——
     * 面板拿它去开那套自绘菜单（DropdownMenu.openAtPoint 收的就是锚点本地坐标，
     * 它自己换算到宿主窗口）。
     */
    void contextMenuRequested(qreal x, qreal y);
    /* shell 自己退出了（用户打了 exit）：面板据此决定标签要不要留 */
    void sessionExited(int exitCode);

protected:
    /*
     * 自己交纹理，不走 QQuickPaintedItem 的缓存：画布和纹理都在 updatePaintNode 里
     * 按**当帧的 item 尺寸**准备，"尺寸变了、纹理还是旧的那张"这种状态从根上不存在。
     * （2026-09-22 那轮换掉基类，是为了排掉"缓存不可靠"这个变量，不是因为它是黑块的根 ——
     *  根因见 updatePaintNode 上面那段，最后落在**格子的底色**上。）
     */
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *data) override;
    /*
     * 画一帧。尺寸从 painter 自己的设备取（= 画布的逻辑尺寸），不读 width()/height()：
     * 圆角裁剪路径要是跟 item 的几何走，改尺寸那一拍只要还是旧宽度，裁剪区就只有旧宽度宽，
     * 铺底那一笔会全落在裁剪区外。这不是当时的根因（根因是格子底色），但这条本身要成立，
     * 画布多大、裁剪区和铺底就多大。
     */
    void paint(QPainter *painter);
    void keyPressEvent(QKeyEvent *event) override;
    void inputMethodEvent(QInputMethodEvent *event) override;
    QVariant inputMethodQuery(Qt::InputMethodQuery query) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void geometryChange(const QRectF &newGeometry, const QRectF &oldGeometry) override;
    void componentComplete() override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;

private:
    void rebuildFont();
    void relayout();
    /* 按当前主题档把 ANSI 16 色推给引擎（浅色一套，深色退回 libvterm 自带） */
    void applyAnsiPalette();
    /*
     * 按当前配色方案的 font 段钉一下终端字体（terminalFamily / terminalSize）。
     * 没钉的那一项回到 QML 给的那个基线值。
     */
    void applySchemeFont();
    void checkScreenForBlack();
    void drawRow(QPainter *painter, int viewRow, int docRow);
    /* 视图里第 i 行 -> 文档行号（负数 = 回滚区） */
    int docRowOf(int viewRow) const { return viewRow - m_scrollUp; }
    /* 选区（已按 flow 顺序归一化，两端都含）；没有选区返回 false */
    bool selectionRange(int *rowA, int *colA, int *rowB, int *colB) const;
    void setScrollUp(int rows);
    void selectWordAt(int docRow, int col);
    QString textInRange(int rowA, int colA, int rowB, int colB) const;
    bool sendCtrlKey(QKeyEvent *event);
    void notifySelection();
    /* 鼠标位置 -> (列, 文档行)；行可能是负数（回滚区） */
    QPoint cellAtPos(const QPointF &pos) const;

    TerminalEngine *m_engine = nullptr;
    QVector<TerminalCell> m_rowScratch;

    /* 基线：设置里那位用户自己选的（由 QML 灌进来） */
    QString m_fontFamily = QStringLiteral("Cascadia Mono");
    qreal m_fontSize = 13;
    /* 实际用来画字的：基线被方案钉住时就是方案那个值（见 applySchemeFont） */
    QString m_usedFamily = QStringLiteral("Cascadia Mono");
    qreal m_usedSize = 13;
    QColor m_fg { QStringLiteral("#d4d4d4") };
    QColor m_bg { QStringLiteral("#1e1f22") };
    /* updatePaintNode 用的画布：尺寸不对就重建，避免每帧 malloc 一张 4K 图 */
    QImage m_canvas;
    QTimer *m_blackCheck = nullptr;   // 记录器用：改尺寸后延迟抓屏
    QTimer *m_reflowTimer = nullptr;  // 画布换尺寸后补要一帧
    QColor m_selColor { 38, 79, 120, 140 };
    QColor m_cursorColor { QStringLiteral("#aeafad") };
    qreal m_padding = 6;

    QFont m_font;
    qreal m_cellW = 8;
    qreal m_cellH = 17;
    qreal m_ascent = 13;

    int m_scrollUp = 0;
    /* 上一次交给 QML 的回滚行数：只在它真的变了时补发一次 gridChanged（见构造函数那条连接） */
    int m_histSeen = -1;
    int m_paints = 0;
    int m_fullPaints = 0;
    /* 上一帧主动铺的非默认底色矩形数 / 其中纯黑的块数 + 第一块的样子（分层量黑块用） */
    int m_dbgBgRects = 0;
    int m_dbgBlackRects = 0;
    QString m_dbgBlackFirst;
    int m_dbgCanvasRun = -1;
    int m_dbgCanvasBlackSamples = -1;

    /* 选区：锚点和当前点，都是文档坐标（行可负） */
    bool m_selActive = false;
    bool m_selecting = false;
    bool m_hasSelNotified = false;
    int m_selRowA = 0, m_selColA = 0;
    int m_selRowB = 0, m_selColB = 0;

    /* 这一轮鼠标事件是转发给程序的（对面开了鼠标上报），不是用来选字 */
    bool m_forwardingMouse = false;

    bool m_cursorOn = true;
    QTimer *m_blink = nullptr;
    QString m_errorText;
    QRectF m_lastCaretRect;
    qreal m_cornerRadius = 0;
};
