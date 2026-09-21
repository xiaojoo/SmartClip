#include "WindowHelper.h"

#include "DialogStyle.h"
#include "EditorViewItem.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QTimer>

#include <QEvent>
#include <QGuiApplication>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QQuickItem>
#include <QQuickWidget>
#include <QRegion>
#include <QScreen>
#include <QStringList>
#include <QWidget>
#include <QWindow>

namespace {

/* --------------------------------------------------------------------------
 * 窗口变化日志（见 WindowHelper.h 里那段说明）
 * ------------------------------------------------------------------------ */

const char *const kWindowTraceFile = "window-trace.log";

/* SWP_* 那几个位：一眼看出这一拍"是移动、是改尺寸、还是只换了层序" */
QString swpText(UINT flags) {
    QStringList bits;
    if (flags & SWP_NOSIZE) bits << QStringLiteral("不改尺寸");
    if (flags & SWP_NOMOVE) bits << QStringLiteral("不移动");
    if (flags & SWP_NOZORDER) bits << QStringLiteral("不动层序");
    if (flags & SWP_NOACTIVATE) bits << QStringLiteral("不激活");
    if (flags & SWP_SHOWWINDOW) bits << QStringLiteral("显示");
    if (flags & SWP_HIDEWINDOW) bits << QStringLiteral("隐藏");
    if (flags & SWP_FRAMECHANGED) bits << QStringLiteral("换边框");
    if (flags & SWP_NOCOPYBITS) bits << QStringLiteral("不拷旧内容");
    if (flags & SWP_NOREDRAW) bits << QStringLiteral("不重画");
    if (flags & SWP_NOOWNERZORDER) bits << QStringLiteral("不动属主层序");
    if (flags & SWP_NOSENDCHANGING) bits << QStringLiteral("不发CHANGING");
    return bits.isEmpty() ? QStringLiteral("0") : bits.join(QLatin1Char('|'));
}

/* 系统发来的命令（WM_SYSCOMMAND）：掩掉低四位，0xF030/0xF032 都算最大化 */
QString sysCommandText(WPARAM cmd) {
    switch (cmd & 0xFFF0) {
    case SC_SIZE:     return QStringLiteral("SC_SIZE");
    case SC_MOVE:     return QStringLiteral("SC_MOVE");
    case SC_MINIMIZE: return QStringLiteral("SC_MINIMIZE");
    case SC_MAXIMIZE: return QStringLiteral("SC_MAXIMIZE");
    case SC_RESTORE:  return QStringLiteral("SC_RESTORE");
    case SC_CLOSE:    return QStringLiteral("SC_CLOSE");
    default:          return QStringLiteral("WM_SYSCOMMAND 其它命令");
    }
}

/*
 * 这个窗口的"系统转场动画"写进去了没有（见 DialogStyle.h 的 disableDwmTransitions）。
 *
 * 只报**写的时候**那个 HRESULT：这个属性反向读不了（DwmGetWindowAttribute 回
 * E_INVALIDARG），所以"设了没有"只能这么记。日志里那一列要是一直"已关"，
 * 说明句柄没被 Qt 换掉；要是看到"**没关住**"，那就是这一条根本没生效。
 */
QString dwmTransitionNote(WId handle, HRESULT hr) {
#if defined(Q_OS_WIN)
    if (!handle)
        return QStringLiteral("还没建");
    return SUCCEEDED(hr) ? QStringLiteral("已关")
                         : QStringLiteral("**没关住** 0x%1")
                               .arg(quint32(hr), 8, 16, QLatin1Char('0'));
#else
    Q_UNUSED(handle);
    Q_UNUSED(hr);
    return QStringLiteral("非 Windows");
#endif
}

/*
 * 界面底色：和 Main.qml 的 contentRoot、宿主的调色板同一个值（#313335）。
 *
 * 窗口"还没画过"的那一块一律擦成它 —— 那块像素直接交给合成器就是纯黑 / 白，
 * 而在这套深色界面上，底色闪一下基本看不出来。现在只有 MessageTrace 的
 * WM_ERASEBKGND 用它（换尺寸那一小段里自己擦，见那段的说明）。
 */
HBRUSH backdropBrush() {
    static HBRUSH brush = ::CreateSolidBrush(RGB(0x31, 0x33, 0x35));
    return brush;
}

/*
 * 系统那边这个窗口的**真实矩形**。
 *
 * 为什么不用 QWidget::geometry()：窗口状态刚变的那一拍（showMaximized / showNormal
 * 回来的当口），Qt 记着的 geometry 还是旧值 —— 拿它跟目标矩形比永远"不相等"，
 * 于是每次都会白补一次 setGeometry()（那一下还会把 Qt 记的最大化状态清掉）。
 * 要判断"系统摆到位了没有"，只能问系统。
 */
QRect nativeFrameRect(QWidget *widget) {
#if defined(Q_OS_WIN)
    if (!widget)
        return QRect();
    const HWND hwnd = reinterpret_cast<HWND>(widget->internalWinId());
    if (!hwnd)
        return QRect();
    RECT rc{};
    if (!::GetWindowRect(hwnd, &rc))
        return QRect();
    return QRect(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
#else
    return widget ? widget->geometry() : QRect();
#endif
}

}  // namespace

/*
 * 设计取舍见 WindowHelper.h 顶部。这里只强调一处实现上的关键点：
 *
 * 窗口切到最大化状态（或切回常规）之后，几何必须"落到窗口管理器认为的
 * 那个矩形"上 —— 展开时用 screen->availableGeometry()（去掉任务栏），
 * 这样和系统自己的最大化位置完全重合，收尾不会看到窗口再挪一下。
 */

WindowHelper::WindowHelper(QObject *parent)
    : QObject(parent)
{
    /* 按住不放超过 4 秒就自己退回卡片（见 prewarmMaximize） */
    m_prewarmLife.setSingleShot(true);
    connect(&m_prewarmLife, &QTimer::timeout, this, &WindowHelper::cancelPrewarm);
}

/*
 * 析构定义在 MessageTrace 之后（见文件中间）：拆那个挂在 qApp 上的原生消息
 * 过滤器要用到它的**完整类型**，光有前置声明在这儿是转不过去的。
 */

/* --------------------------------------------------------------------------
 * 窗口变化日志
 *
 * 格式和 Screenshot 那份 shot-trace.log 一致：左边是"启动至今多少毫秒"，
 * 右边一句话。每次启动把上一份删掉重开，所以文件永远只反映最近这一次运行。
 *
 * **文件只开一次**（一个常驻句柄）：
 * 原来是"一行一次 open/write/flush/close"，一次最大化要写十几行，光这些系统调用
 * 就够吃掉一整帧 —— 而这条日志恰恰是在量"切换那几十毫秒里屏幕上是什么"，
 * 记录本身不能成为主角（实测：改成常驻句柄之后，切换从发出到整窗画完短了一截，
 * 录屏里那两帧"旧内容 + 黑"收成了一帧）。每次写完照样 flush，外面随时能看见。
 * ------------------------------------------------------------------------ */

namespace {

QFile &traceFile()
{
    static QFile file(QString::fromLatin1(kWindowTraceFile));
    return file;
}

}  // namespace

void WindowHelper::trace(const QString &what)
{
    QFile &f = traceFile();

    if (!m_traceStarted) {
        m_traceStarted = true;
        m_traceClock.start();
        f.close();
        QFile::remove(QString::fromLatin1(kWindowTraceFile));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            f.write(QStringLiteral("# SmartClip 窗口变化日志  启动于 %1\n")
                            .arg(QDateTime::currentDateTime().toString(
                                QStringLiteral("HH:mm:ss.zzz")))
                            .toUtf8());
            f.flush();
        }
    }

    if (!f.isOpen())
        return;
    f.write(QStringLiteral("%1  %2\n").arg(m_traceClock.elapsed(), 6).arg(what).toUtf8());
    f.flush();
}

void WindowHelper::traceSnapshot(const QString &tag)
{
    if (!m_widget)
        return;

    const QRect g = m_widget->geometry();
    QString line = QStringLiteral("%1  窗口 %2x%3@%4,%5 最大=%6 可见=%7 露出=%8 前台=%9 "
                                  "遮罩=%10 系统转场=%11")
                       .arg(tag)
                       .arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y())
                       .arg(m_widget->isMaximized() ? 1 : 0)
                       .arg(m_widget->isVisible() ? 1 : 0)
                       .arg(m_widget->windowHandle() && m_widget->windowHandle()->isExposed()
                                ? 1 : 0)
                       .arg(m_widget->isActiveWindow() ? 1 : 0)
                       .arg(m_maskApplied && !m_appliedMask.isEmpty() ? QStringLiteral("圆角")
                                                                     : QStringLiteral("无"))
                       .arg(m_transitionNote);

    /*
     * 子控件此刻的几何也一起记。
     *
     * QML 整块（QQuickWidget）和原生编辑区（QScintilla）都是这个窗口的子控件：
     * 哪一个落后一拍，屏幕上就是"内容没跟上窗口"—— 树和正文之间那条缝在闪，
     * 十有八九就是这两块中间的某一刻不同步。
     */
    const QList<QWidget *> kids =
        m_widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *kid : kids) {
        const QRect kg = kid->geometry();
        line += QStringLiteral("  | %1 %2x%3@%4,%5")
                    .arg(QString::fromLatin1(kid->metaObject()->className()))
                    .arg(kg.width()).arg(kg.height()).arg(kg.x()).arg(kg.y());
    }
    trace(line);
}

void WindowHelper::traceFrames(bool armed)
{
    /* 只在切换前后这一小段里记帧：平时 QML 一动就是几十帧，全记下来会把日志淹掉 */
    m_traceFramesUntil = armed ? m_traceClock.elapsed() + 600 : -1;
}

void WindowHelper::closeWindow()
{
    if (m_widget)
        m_widget->close();
}

/*
 * 真退出（见 WindowHelper.h 里那段说明）。
 *
 * 用的是 `QCoreApplication::exit(0)`，**不是 `quit()`** —— 这两个在这套界面上不等价，
 * 而 `quit()` 在"桌面上摆着便签"时是**完全无效**的：
 *
 *   * `quit()` 只是往 QApplication 自己发一个 QEvent::Quit；
 *   * `QApplication::event()` 收到它先 `closeAllWindows()` —— 便签窗口的
 *     closeEvent 是 `event->ignore()` + 藏起来（关窗口 = 收起那一块，见
 *     StickyNoteWindow::closeEvent），于是"关"完还有窗口露着；
 *   * 接着它逐个看顶层窗口，**只要还有一个露着的就 `e->ignore(); return true;`
 *     —— Quit 事件被吃掉，程序不退出**。
 *
 * 用户看到的就是："有多个便签，点退出会关掉一个便签，程序还是不会退出"
 * （每点一次关掉/收起一块，另一块又被 repairGroup 叫出来，循环往复）。
 *
 * `exit(0)` 直接让事件循环退出（见 Qt 源码 QCoreApplication::exit），不碰
 * 任何窗口 —— 这也正是用户要的："不要关闭便签，程序直接关闭"。便签窗口原样
 * 留在数据里（visible / 位置都没变），main.cpp 收尾时 notes.shutdown() 会把
 * 清单落盘，下次启动那几块便签照旧摆在桌上。
 */
void WindowHelper::quitApp()
{
    QCoreApplication::exit(0);
}

void WindowHelper::askQuit()
{
    /*
     * 只发信号，界面（Main.qml 里的 quitAsk 那块 AskCard）负责把卡片弹出来。
     *
     * 为什么不在这里弹：底下的界面要**原封不动** —— 不压暗、不遮住、不挡鼠标。
     * 这里试过三种做法都不行：加遮罩（哪怕全透明，它铺满整窗就把鼠标吃了，
     * 编辑区点不动）、新建顶层对话框（系统先映射空窗口、内容下一帧才画，
     * 那一帧就是"闪一下"）。最后走的是这个项目里本来就一直在用、从来没闪过
     * 的那类窗口：Popup.Window（和下拉菜单、设置面板同一套）。详见
     * qml/components/AskCard.qml 开头的说明。
     */
    emit quitRequested();
}

void WindowHelper::hideToTray()
{
    /* 藏起来不等于退出：托盘图标还在，托盘右键能截图，全局截图热键也还响 */
    if (m_widget)
        m_widget->hide();
}

void WindowHelper::setCornerRadius(int r)
{
    if (r < 0 || r == m_cornerRadius)
        return;
    m_cornerRadius = r;
    emit cornerRadiusChanged();
    applyRoundedMask();
}

/*
 * 圆角遮罩。
 *
 * 抗锯齿靠 QPainterPath + QRegion(path.toFillPolygon())：直接
 * QRegion(rect, QRegion::Ellipse) 出来的角有台阶，四个角看得见毛刺。
 */
bool WindowHelper::startSystemResize(int edges)
{
    if (!m_widget)
        return false;
    QWindow *wh = m_widget->windowHandle();
    if (!wh)
        return false;
    return wh->startSystemResize(Qt::Edges(edges));
}

bool WindowHelper::startSystemMove()
{
    if (!m_widget)
        return false;
    QWindow *wh = m_widget->windowHandle();
    if (!wh)
        return false;

    /*
     * 最大化状态下拖标题栏 = "拖出来还原"（原生窗口就是这个手感）。
     *
     * 我们自己的最大化只是几何、系统不知道它是最大化（见 applyState 那段：
     * 就是**为了**不让系统放那段转场），系统因此不会替我们还原 —— 这一条
     * 得自己补，不然拖起来是一块铺满屏幕的窗在动。
     *
     * 位置也要自己算：原生行为是"还原之后鼠标还压在标题栏上"，
     * 所以按鼠标在最大化窗口里的**相对横向位置**把还原矩形摆过去，
     * 纵向让鼠标落在标题栏那一条里。
     */
    if (m_maximized) {
        const QPoint cursor = QCursor::pos();
        const QRect from = m_widget->geometry();

        restore();

        if (m_widget->geometry().isValid() && from.width() > 0) {
            const qreal fx = qBound(0.15, qreal(cursor.x() - from.x()) / qreal(from.width()), 0.85);
            const int dy = qBound(0, cursor.y() - from.y(), 16);
            QRect to = m_widget->geometry();
            to.moveLeft(qRound(cursor.x() - fx * to.width()));
            to.moveTop(cursor.y() - dy);
            m_widget->setGeometry(to);
            m_normalRect = to;
            trace(QStringLiteral("startSystemMove：拖出还原，摆到 %1x%2@%3,%4（鼠标还压在标题栏上）")
                      .arg(to.width()).arg(to.height()).arg(to.x()).arg(to.y()));
        }
    }
    return wh->startSystemMove();
}

/*
 * 拖"这一块窗"，不是主窗口。
 *
 * 设置面板原来是在 QML 里自己算增量改 root.x：那是拿 mapToItem(null, ...) 的
 * 场景坐标当参照，而这块窗**就在自己场景里被搬走** —— 窗一动，光标的场景坐标
 * 就反向跟着变，于是下一次增量里含了上一次的量，手感就是"拖一下跳一下、还跑偏"。
 * 交给窗口管理器就没有这笔账要算（和 TopBar 拖主窗口同一条路）。
 */
bool WindowHelper::startSystemMoveFor(QQuickItem *inside)
{
    if (!inside)
        return false;
    QWindow *wh = inside->window();
    if (!wh)
        return false;
    return wh->startSystemMove();
}

bool WindowHelper::attachAsToolWindow(QWindow *window)
{
    if (!window)
        return false;
    /*
     * 宿主取 window()（顶层那块 QWidget）而不是 m_widget 自己：这里要的是"主窗口
     * 的 HWND"，而 m_widget 已经是顶层了 —— 多这一层是防以后有人把宿主套进布局。
     */
    QWidget *top = m_widget ? m_widget->window() : nullptr;
    QWindow *host = top ? top->windowHandle() : nullptr;
    if (!host || host == window)
        return false;   /* 宿主还没建原生窗口：调用方下次打开面板时再试（见 .h） */

    /*
     * 顺序要紧：transientParent 和 flags 都得在**这块窗还没显示之前**定下来。
     * 显示之后再 setFlags 就是销毁重建 HWND —— 上一轮就是那么改的，结果面板
     * 落到最大化主窗口的后面（isVisible 还是真的，但点不到）。
     */
    if (window->transientParent() != host)
        window->setTransientParent(host);

    /*
     * 标志位**只改我们要的那三位**，别整份替换。
     *
     * 原来比的是"flags() 等不等 0x…800a"：QML 的 `Window` 建出来的 flags 里还
     * 带着 Qt::Window 那一位（0x…800b），于是这一比永远不等 → 每次都 setFlags
     * → 每次都销毁重建 HWND。按位改完之后第二次进来就是"已经是对的"，一次都不动。
     */
    const Qt::WindowFlags keep = Qt::WindowType_Mask | Qt::FramelessWindowHint
                               | Qt::NoDropShadowWindowHint;
    Qt::WindowFlags f = window->flags();
    f = (f & ~keep) | Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint;
    if (f != window->flags() && !window->isVisible())
        window->setFlags(f);
    return true;
}

/*
 * 宿主顶层窗在屏幕上的矩形（逻辑像素）—— 见 WindowHelper.h 里那句"QML 拿不到"。
 *
 * 用 geometry() 不用 frameGeometry()：这块窗是 Qt::FramelessWindowHint，两者本来就
 * 一样，而弹窗的 x/y 是相对客户区给的（和 publishHostGeometry 报给菜单的同一个基准）。
 */
QRect WindowHelper::hostScreenGeometry() const
{
    QWidget *top = m_widget ? m_widget->window() : nullptr;
    return top ? top->geometry() : QRect();
}

void WindowHelper::refreshMask()
{
    applyRoundedMask();
}

/*
 * 按住拖动分隔线的那一段，光标由应用级 override 钉住（见 .h 里的说明）。
 *
 * 只压一次、只还一次：MouseArea 在拖动中途可能反复触发按下（比如换方向），
 * 而 restoreOverrideCursor 是弹栈 —— 压两次还一次会把栈留歪，
 * 之后别的地方设的光标都不对了。
 */
void WindowHelper::pushResizeCursor(int shape)
{
    if (m_resizeCursorPushed)
        return;
    m_resizeCursorPushed = true;
    QApplication::setOverrideCursor(
        QCursor(static_cast<Qt::CursorShape>(shape)));
}

void WindowHelper::popResizeCursor()
{
    if (!m_resizeCursorPushed)
        return;
    m_resizeCursorPushed = false;
    QApplication::restoreOverrideCursor();
}

bool WindowHelper::probeEnabled() const
{
    static const bool on = qEnvironmentVariableIsSet("SMARTCLIP_LAYOUT_PROBE");
    return on;
}

void WindowHelper::applyRoundedMask()
{
    if (!m_widget) {
        trace(QStringLiteral("applyRoundedMask：没窗口"));
        return;
    }
    trace(QStringLiteral("applyRoundedMask：进（%1x%2 最大=%3 半径=%4）")
              .arg(m_widget->width()).arg(m_widget->height())
              .arg(m_maximized ? 1 : 0)
              .arg(m_cornerRadius));

    /*
     * setMask() 用的是**设备像素**，而 QWidget::width()/height() 是逻辑像素。
     * 高 DPI 屏上 dpr>1，直接拿逻辑尺寸建遮罩会比窗口小，
     * 表现就是"只有原点附近（左上）被裁到"。
     */
    const qreal dpr = m_widget->devicePixelRatioF();
    const int w = qRound(m_widget->width() * dpr);
    const int h = qRound(m_widget->height() * dpr);
    if (w <= 0 || h <= 0)
        return;

    /*
     * 圆角半径。
     *
     * **不要再"拖边的时候也不给圆角"**：试过（为了少几次 SetWindowRgn），
     * 但摘掉遮罩之后窗口变方角，而四角那几像素从来没画过 —— 拖窗口 / 拉边时
     * 角上会闪白（用户报的"白色背景闪现"）。圆角照旧每拍跟着尺寸算，
     * 多出来的那几次 SetWindowRgn 换来的是"任何时刻都不露没画过的像素"。
     *
     * 最大化要不要圆角：默认**要**（用户 2026-09-17 明确要求"最大化也要圆角"）。
     * 要走回"最大化 = 直角"（和 Windows 原生最大化一致）就设
     * SMARTCLIP_SQUARE_WHEN_MAXIMIZED=1。
     *
     * 注意最大化时这个圆角是**抠掉四个角**：窗口是不透明矩形，圆角靠 SetWindowRgn 裁，
     * 裁掉的那几像素露出的是底下的桌面 / 别的窗口。
     */
    static const bool squareWhenMaximized = qEnvironmentVariableIsSet("SMARTCLIP_SQUARE_WHEN_MAXIMIZED");
    const int r = (m_maximized && squareWhenMaximized)
                      ? 0
                      : qRound(qMin(m_cornerRadius, qMin(w / 2, h / 2)) * dpr);

    QRegion region;
    if (r > 0) {
        QPainterPath path;
        path.addRoundedRect(QRectF(0, 0, w, h), r, r);
        region = QRegion(path.toFillPolygon().toPolygon());
    }

    /*
     * 形状没变就别再 setMask 一遍。
     *
     * setMask 落到 Windows 上是 SetWindowRgn —— 那一下会让整块窗口重画一次。
     * 最大化 / 还原一次要过好几拍 Move / Resize（每拍都调到这里），
     * 每拍都重设一遍就是白白多出几帧闪。实测过：不缓存时一次最大化会重设 2 次
     * （Resize 一拍 + applyState 收尾那次），缓存之后 0~1 次。
     */
    if (m_maskApplied && region == m_appliedMask) {
        trace(QStringLiteral("applyRoundedMask：形状没变，不重设（r=%1 区域=%2 最大=%3）")
                  .arg(r)
                  .arg(region.isEmpty() ? QStringLiteral("空")
                                        : QStringLiteral("%1x%2")
                                              .arg(region.boundingRect().width())
                                              .arg(region.boundingRect().height()))
                  .arg(m_maximized ? 1 : 0));
        return;
    }

    trace(QStringLiteral("applyRoundedMask：r=%1 区域=%2 最大=%3 → %4")
              .arg(r)
              .arg(region.isEmpty() ? QStringLiteral("空")
                                    : QStringLiteral("%1x%2")
                                          .arg(region.boundingRect().width())
                                          .arg(region.boundingRect().height()))
              .arg(m_maximized ? 1 : 0)
              .arg(region.isEmpty() ? QStringLiteral("clearMask()") : QStringLiteral("setMask()")));

    if (region.isEmpty())
        m_widget->clearMask();
    else
        m_widget->setMask(region);

    m_appliedMask = region;
    m_maskApplied = true;
}

/*
 * QWidget 取屏幕：优先用它的原生窗口句柄，其次按窗口中心点找所在屏。
 * 直接 QGuiApplication::screenAt() 在窗口刚创建时可能拿不到。
 */
QScreen *WindowHelper::screenOf() const
{
    if (!m_widget)
        return nullptr;

    if (QWindow *wh = m_widget->windowHandle()) {
        if (QScreen *s = wh->screen())
            return s;
    }
    if (QScreen *s = QGuiApplication::screenAt(m_widget->frameGeometry().center()))
        return s;
    return QGuiApplication::primaryScreen();
}

/*
 * ==========================================================================
 * WndProc 那一层的记录器
 * ==========================================================================
 *
 * 为什么光有 Qt 的 Move / Resize 不够：那是**结果**。用户报的"最大化时窗口
 * 先跑到右边、还在放大"要能查，得看清两件事 ——
 *
 *   1. 这一拍几何是谁下的：系统的 WM_SYSCOMMAND（最大化 / 还原命令）触发的，
 *      还是我们自己在 applyState 里摆的；
 *   2. 系统在这一拍前后要了哪些信息（WM_GETMINMAXINFO 报多大、
 *      WM_WINDOWPOSCHANGING 里的目标矩形和 SWP_* 标志）。
 *
 * 它挂在 qApp 上（QWidget 没有 nativeEvent 可重写的那一层可以用），
 * 所以每条消息都要先认一下"是不是我们这个窗口的"—— 别的窗口（便签 / 卡片 /
 * 编辑区那个原生子窗）的消息量很大，全记下来会把日志淹掉。
 *
 * 顺带在这里拦掉系统的 SC_MAXIMIZE / SC_RESTORE：那两条走系统的默认处理会
 * 带一段缩放动画（见 applyState 里的说明），我们自己办更干净。
 */
class WindowHelper::MessageTrace final : public QAbstractNativeEventFilter {
public:
    explicit MessageTrace(WindowHelper *owner) : m_owner(owner) {}

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override
    {
#if defined(Q_OS_WIN)
        if (eventType != QByteArrayLiteral("windows_generic_MSG"))
            return false;

        auto *msg = static_cast<MSG *>(message);
        WindowHelper *self = m_owner;
        if (!msg || !self || !self->m_widget)
            return false;

        /*
         * 认窗口用 internalWinId()：这里是**消息处理的最里层**，一句 winId()
         * 就可能在"窗口正在创建"的当口再去建一次原生窗口（CreateWindowEx 报
         * 参数错误，窗口一个也建不出来）。详见 dwmTransitionsText 上面那段。
         */
        const HWND ours = reinterpret_cast<HWND>(self->m_widget->internalWinId());
        if (!ours)
            return false;
        const HWND root = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT) : nullptr;
        if (msg->hwnd != ours && root != ours)
            return false;

        switch (msg->message) {
        case WM_WINDOWPOSCHANGING: {
            auto *pos = reinterpret_cast<WINDOWPOS *>(msg->lParam);
            if (pos)
                self->trace(QStringLiteral("WM_WINDOWPOSCHANGING  %1  目标 %2x%3@%4,%5")
                                .arg(swpText(pos->flags))
                                .arg(pos->cx).arg(pos->cy).arg(pos->x).arg(pos->y));
            break;
        }

        case WM_WINDOWPOSCHANGED: {
            auto *pos = reinterpret_cast<WINDOWPOS *>(msg->lParam);
            if (pos)
                self->trace(QStringLiteral("WM_WINDOWPOSCHANGED   %1  落到 %2x%3@%4,%5")
                                .arg(swpText(pos->flags))
                                .arg(pos->cx).arg(pos->cy).arg(pos->x).arg(pos->y));
            self->traceSnapshot(QStringLiteral("  └ 这一拍之后"));
            break;
        }

        case WM_SIZE: {
            const int kind = int(msg->wParam);
            self->trace(QStringLiteral("WM_SIZE  %1  %2x%3")
                            .arg(kind == SIZE_MAXIMIZED ? QStringLiteral("SIZE_MAXIMIZED")
                                 : kind == SIZE_MINIMIZED ? QStringLiteral("SIZE_MINIMIZED")
                                 : kind == SIZE_RESTORED ? QStringLiteral("SIZE_RESTORED")
                                                         : QStringLiteral("其它(%1)").arg(kind))
                            .arg(LOWORD(msg->lParam))
                            .arg(HIWORD(msg->lParam)));
            break;
        }

        case WM_MOVE:
            self->trace(QStringLiteral("WM_MOVE  %1,%2")
                            .arg(int(short(LOWORD(msg->lParam))))
                            .arg(int(short(HIWORD(msg->lParam)))));
            break;

        /*
         * 非客户区压成 0（**只在窗口带标题栏样式时才动手**）。
         *
         * 这一条是留给"让系统那段最大化转场放起来"那条路的：转场只对带标题栏的普通窗口放，
         * 而那样窗口就会长出真的标题栏，所以要把非客户区压掉、客户区 = 整块窗口。
         *
         * 现在这个窗口是 Qt::FramelessWindowHint 出来的 WS_POPUP（实测 style=0x96000000），
         * 本来就没有非客户区 —— 实测四条路都试过：把样式位补成 CAPTION|THICKFRAME、
         * 去掉 WS_POPUP、去掉圆角区域、甚至让窗口**生来就是普通窗口**，DWM **照样不放
         * 那段最大化转场**（素材 build 下 frames-caption2、frames-nopopup、
         * frames-nomask、frames-framed2）；而同时拿画图做对照，
         * 它在这台机器上放得好好的（见 build\frames-paint）。所以"让系统转场盖住空档"
         * 这条路整个搁下（多半是因为这个窗口的"可动画"资格在创建时就定了、且 Qt 给
         * QWidget 建的是那种不参与动画的窗口类）。
         * 这一条留着：以后要是真走"普通窗口 + 压掉非客户区"的做法，还得靠它。
         * 现在走到这里直接让开，不动系统给的矩形。
         */
        case WM_NCCALCSIZE: {
            if (!(::GetWindowLongPtr(ours, GWL_STYLE) & WS_CAPTION))
                break;   /* 没有标题栏样式：不关我们的事，交回系统 / Qt */
            if (msg->wParam) {
                auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
                if (!params)
                    break;
                if (::IsZoomed(ours)) {
                    MONITORINFO mi{};
                    mi.cbSize = sizeof(mi);
                    if (::GetMonitorInfo(::MonitorFromWindow(ours, MONITOR_DEFAULTTONEAREST), &mi)) {
                        params->rgrc[0] = mi.rcWork;
                        self->trace(QStringLiteral("WM_NCCALCSIZE（最大化）客户区按可用区 %1x%2@%3,%4")
                                        .arg(mi.rcWork.right - mi.rcWork.left)
                                        .arg(mi.rcWork.bottom - mi.rcWork.top)
                                        .arg(mi.rcWork.left).arg(mi.rcWork.top));
                    }
                }
            } else {
                auto *rect = reinterpret_cast<RECT *>(msg->lParam);
                if (rect)
                    ::GetWindowRect(ours, rect);
            }
            if (result)
                *result = 0;      /* 0 = 客户区就是上面那个矩形，没有非客户区 */
            return true;
        }

        /*
         * 系统在最大化之前会先问"最大能多大"。
         *
         * 无边框窗口在这里报的数常常是"整块屏"而不是"去掉任务栏的可用区"
         * —— 这正是**不能用系统那套最大化**的一条硬理由：它会一路铺到任务栏底下，
         * 然后还得我们自己补一次 setGeometry(availableGeometry()) 把它拉回来，
         * 那一下就是用户眼里的"又跳了一下"。所以现在最大化只改几何、摆的就是
         * 可用区（见 applyState）。这一条留着是为了下次再有人想改回
         * showMaximized() 时，日志里能直接看到系统心里那个矩形是哪个。
         */
        case WM_GETMINMAXINFO: {
            auto *mmi = reinterpret_cast<MINMAXINFO *>(msg->lParam);
            if (mmi)
                self->trace(QStringLiteral("WM_GETMINMAXINFO  系统要的最大矩形 %1x%2@%3,%4")
                                .arg(mmi->ptMaxSize.x).arg(mmi->ptMaxSize.y)
                                .arg(mmi->ptMaxPosition.x).arg(mmi->ptMaxPosition.y));
            break;
        }

        case WM_SYSCOMMAND: {            const WPARAM cmd = msg->wParam & 0xFFF0;
            const bool iconic = IsIconic(ours) != FALSE;
            self->trace(QStringLiteral("WM_SYSCOMMAND  %1（窗口%2）")
                            .arg(sysCommandText(cmd),
                                 iconic ? QStringLiteral("在最小化状态")
                                        : QStringLiteral("不在最小化状态")));

            /*
             * 最大化 / 还原：**我们自己办**，不让系统走它那套。
             *
             * 系统收到 SC_MAXIMIZE 会做两件事：改状态，然后放一段"把上一张画面
             * 按新矩形缩放过去"的转场（用户看到的就是"窗口先跑到右边、还在放大"）。
             * 后者关不掉（DWMWA_TRANSITIONS_FORCEDISABLED 只管窗口自己的淡入淡出，
             * 管不到这段最大化转场），所以我们干脆不用它那条路：几何自己一步摆过去
             * （见 applyState），然后返回 true 把这条命令吃掉。
             *
             * "在最小化状态"时不拦：那时 SC_RESTORE 的语义是"从最小化恢复"，
             * 交给系统办才对（恢复之后该是最大化就还是最大化）。
             */
            /*
             * 调试开关：SMARTCLIP_LET_SYSTEM_MAXIMIZE=1 —— 不接这两条，交给系统自己办
             * （用来验"系统那套最大化转场到底能不能放起来"：我们一吃掉 SC_MAXIMIZE，
             * 系统就没机会放那段动画了，见下面的说明）。
             */
            static const bool letSystem = qEnvironmentVariableIsSet("SMARTCLIP_LET_SYSTEM_MAXIMIZE");
            if (!letSystem) {
                const bool restoring = (cmd == SC_RESTORE)
                                       && (self->m_maximized || self->updateMaximizedFromWindow());
                if (!iconic && (cmd == SC_MAXIMIZE || restoring)) {
                    self->trace(cmd == SC_MAXIMIZE
                                    ? QStringLiteral("  → 这条我们自己办（不进系统那套最大化转场）")
                                    : QStringLiteral("  → 这条我们自己办（不进系统那套还原转场）"));
                    if (cmd == SC_MAXIMIZE)
                        self->maximize();
                    else
                        self->restore();
                    if (result)
                        *result = 0;
                    return true;
                }
            }
            break;
        }

        case WM_ERASEBKGND: {
            /*
             * 换尺寸那一小段里，背景我们**自己擦成界面底色**。
             *
             * Qt 对这个消息的默认反应是"回 1、什么都不画"（它靠 backing store 画满），
             * 于是"从没画过"的像素（尺寸刚变、区域刚放开、遮罩刚摘掉那一瞬间）
             * 会被原样交给合成器 —— 那是未初始化的显存，白 / 花都有可能。
             * 用户报的"白色背景闪现"最常见的就是它。
             *
             * 擦成 #313335（和界面底色同色）之后，最坏情况也只是"底色闪一下"，
             * 在这套深色界面上看不出来。只在换尺寸那一小段里做（见 m_fastErase），
             * 平时一次都不碰，免得给每次重画都加一遍 GDI 填充。
             */
            if (!self->m_fastErase)
                break;
            HDC dc = reinterpret_cast<HDC>(msg->wParam);
            if (!dc)
                break;
            RECT rc;
            ::GetClientRect(ours, &rc);
            ::FillRect(dc, &rc, backdropBrush());
            if (result)
                *result = 1;
            return true;   /* 已经擦过了，别让系统再擦一遍 */
        }

        case WM_SHOWWINDOW:
            self->trace(QStringLiteral("WM_SHOWWINDOW  %1")
                            .arg(msg->wParam ? QStringLiteral("显示") : QStringLiteral("隐藏")));
            break;

        case WM_ENTERSIZEMOVE:
            self->trace(QStringLiteral("WM_ENTERSIZEMOVE  （开始交互式拖动 / 拉边）"));
            break;

        case WM_EXITSIZEMOVE:
            self->trace(QStringLiteral("WM_EXITSIZEMOVE"));
            self->traceSnapshot(QStringLiteral("  └ 拖动结束"));
            break;

        case WM_DPICHANGED: {
            auto *r = reinterpret_cast<RECT *>(msg->lParam);
            if (r)
                self->trace(QStringLiteral("WM_DPICHANGED  系统建议矩形 %1x%2@%3,%4")
                                .arg(r->right - r->left).arg(r->bottom - r->top)
                                .arg(r->left).arg(r->top));
            break;
        }

        default:
            break;
        }
#endif
        Q_UNUSED(eventType);
        Q_UNUSED(message);
        Q_UNUSED(result);
        return false;
    }

private:
    WindowHelper *m_owner = nullptr;
};

WindowHelper::~WindowHelper()
{
    /*
     * 拆掉挂在窗口上的事件过滤器。
     *
     * Qt 只在**被监听方**销毁时清理过滤器；过滤器自己先没掉的话，那个窗口里
     * 存着的就是个悬空指针 —— 之后任何一次移动 / 缩放 / 关闭事件都会打进一块
     * 已经释放的内存（未定义行为）。主窗口在 main 里是栈对象、比本对象活得久，
     * 所以这条路径一定会走到。
     */
    if (m_widget)
        m_widget->removeEventFilter(this);
    if (m_quickWidget)
        m_quickWidget->removeEventFilter(this);

    /* 挂在 qApp 上的原生消息过滤器同理：不能留着一个悬空的自己 */
    if (m_messageTrace) {
        if (QCoreApplication *app = QCoreApplication::instance())
            app->removeNativeEventFilter(m_messageTrace);
        delete m_messageTrace;
        m_messageTrace = nullptr;
    }
}

void WindowHelper::attachWidget(QWidget *widget)
{
    if (m_widget == widget)
        return;

    if (m_widget)
        m_widget->removeEventFilter(this);

    m_widget = widget;
    if (m_widget) {
        m_widget->installEventFilter(this);

        /*
         * 装整个 QML 界面那块控件也挂上过滤器：它的几何要单独记
         * （屏幕上看到的内容是它画的，不是窗口自己画的）。
         */
        m_quickWidget = m_widget->findChild<QQuickWidget *>();
        if (qobject_cast<QQuickWidget *>(m_quickWidget))
            m_quickWidget->installEventFilter(this);

        /* WndProc 那一层（见 MessageTrace：它同时负责把系统的最大化命令接过来） */
        if (!m_messageTrace) {
            if (QCoreApplication *app = QCoreApplication::instance()) {
                m_messageTrace = new MessageTrace(this);
                app->installNativeEventFilter(m_messageTrace);
            }
        }


        /*
         * 关掉这个窗口的系统转场动画。
         *
         * 最大化 / 还原那一下，系统会把窗口**上一次那张画面**按新矩形缩放一遍再
         * 交出去 —— 看着就是"窗口先跑到右边、还在放大"（用户报的原话），
         * 顺带中间那几帧里树和正文之间那条缝也在跟着缩，像是"间隙在闪"。
         * 这段动画不提供任何信息，所以整窗关掉它（对话框那边早就这么干了，
         * 见 src/DialogStyle.h 里 disableDwmTransitions 的实测说明）。
         *
         * 注意这一条**只管得住窗口自己的淡入淡出**，管不到"最大化转场"
         * —— 后者是系统另走一条路（日志里"系统转场="那一列就是每次现读的
         * 结果，见 dwmTransitionsText）。真正解决最大化那一下靠的是
         * applyState 里"先摆几何、再改状态"的顺序。
         */
#if defined(Q_OS_WIN)
        /*
         * 调试开关：SMARTCLIP_KEEP_DWM_TRANSITION=1 —— 不关系统转场，让 Windows 自己那段
         * "从旧矩形缩放过来"的最大化 / 还原动画放出来（对比"转场盖住空档"这条路用，
         * 见 build\win-verify-switch.ps1）。平时不设，转场是关掉的。
         */
        static const bool keepDwmTransition = qEnvironmentVariableIsSet("SMARTCLIP_KEEP_DWM_TRANSITION");
        const HRESULT dwhr = keepDwmTransition ? S_FALSE : disableDwmTransitions(m_widget);
        m_transitionsDisabled = SUCCEEDED(dwhr);
        m_transitionNote = keepDwmTransition
                               ? QStringLiteral("**没关**（探针要求留着）")
                               : dwmTransitionNote(m_widget->internalWinId(), dwhr);
        trace(QStringLiteral("系统转场动画：%1").arg(m_transitionNote));
#else
        m_transitionNote = QStringLiteral("非 Windows，关不了");
#endif
        cacheNormalGeometry(m_widget->geometry());
        updateMaximizedFromWindow();
        applyRoundedMask();

        /*
         * 宿主几何的兜底轮询（见 publishHostGeometry）。
         *
         * 只比四个数、变了才发信号，开着不心疼；换来的是"窗口挪了、界面没收到"
         * 那条不会因为漏一个 Move 事件而复发（漏一次菜单就留在原地错位）。
         */
        m_hostGeometryPoll.setInterval(120);
        connect(&m_hostGeometryPoll, &QTimer::timeout,
                this, &WindowHelper::publishHostGeometry);
        m_hostGeometryPoll.start();
        publishHostGeometry();


        /* 开头几行：这台机器上窗口和屏幕是怎么摆的（后面所有几何都对着它看） */
        trace(QStringLiteral("==== attach ===="));
        if (QScreen *s = screenOf()) {
            const QRect a = s->availableGeometry();
            const QRect full = s->geometry();
            trace(QStringLiteral("屏幕：可用区 %1x%2@%3,%4 / 整屏 %5x%6  缩放 %7")
                      .arg(a.width()).arg(a.height()).arg(a.x()).arg(a.y())
                      .arg(full.width()).arg(full.height())
                      .arg(s->devicePixelRatio()));
        }
        trace(QStringLiteral("窗口标志 0x%1  原生窗口 0x%2")
                  .arg(quint32(m_widget->windowFlags()), 8, 16, QLatin1Char('0'))
                  .arg(quintptr(m_widget->internalWinId()), 0, 16));
        traceSnapshot(QStringLiteral("attach 之后"));
    }
}

void WindowHelper::minimizeWindow()
{
    if (m_widget)
        m_widget->showMinimized();
}


void WindowHelper::toggleMaximize()
{
    if (m_maximized)
        restore();
    else
        maximize();
}

/* --------------------------------------------------------------------------
 * 预热最大化（见 WindowHelper.h 上 prewarmMaximize 的说明）
 *
 * 一句话：把"内容按 4K 渲染一帧"那 165ms 从"松手之后"挪到"按下和松手之间"。
 * 挪不动它（每帧都要交的面积税，见 applyState ①b 那段拆开的数），只能挪时机。
 * ------------------------------------------------------------------------ */

void WindowHelper::prewarmMaximize()
{
    /* 按下这一发的落点：prewarmRelease() 拿它判"松手时还在这颗按钮上吗" */
    m_prewarmPressPos = QCursor::pos();
    doPrewarm();
}

bool WindowHelper::prewarmRelease()
{
    if (!m_prewarmed)
        return false;    // 没预热成（探针关掉了 / 条件不符）：让 QML 走原来那条 clicked

    m_prewarmLife.stop();

    /*
     * 全局光标还在按下那一点附近 → 这一发就是"点放大"，当场办掉。
     *
     * 判据只能拿全局位置对着按：按下之后布局已经是 4K 那一版了，本 Item 在场景里的
     * 位置都变了，QML 自己那套"松手还在这个 Item 里"的判据在这儿必然不成立。
     */
    if ((QCursor::pos() - m_prewarmPressPos).manhattanLength() <= 8) {
        trace(QStringLiteral("预热：松手命中 → 直接最大化（那 165ms 已经交过）"));
        maximize();
    } else {
        trace(QStringLiteral("预热：松手前指针挪走了 → 这一发当取消"));
        cancelPrewarm();
    }
    return true;
}

void WindowHelper::doPrewarm()
{
    /* 先看条件（"已经最大化了"和"窗口收进托盘了"都从这条路进来） */
    if (m_prewarmed || m_maximized || m_zoomFrame || m_blankBackdrop
        || !m_widget || !m_quickWidget)
        return;
    if (!m_widget->isVisible() || m_widget->isMinimized())
        return;

    QScreen *screen = screenOf();
    const QRect avail = screen ? screen->availableGeometry() : QRect();
    if (!avail.isValid() || avail.isEmpty())
        return;

    /*
     * 先把屏幕上现在这一张贴住。
     *
     * 为什么必须贴：内容控件一摆到 4K，它自己会重画一帧，而那一帧画的是"最大化那一版
     * 布局"—— 卡片那么大一块窗口裁出来的就是它的左上角那一条。真点下去的时候有 ②d
     * 拉伸帧接管，预热这几十到几百 ms 里得自己盖住，走的是同一条通道。
     */
    m_prewarmCover = screen->grabWindow(m_widget->winId());
    if (m_prewarmCover.isNull()) {
        trace(QStringLiteral("预热：抓不到当前画面，放弃"));
        return;
    }

    /*
     * 预热期间把编辑区原生子窗**钉在原坐标**。
     *
     * 不钉的话：QML 那一摆到 4K，编辑区那条 geometryChange 就把原生 QScintilla 窗
     * 跟着撑大，开了换行的文档会当场重排 —— 那是一块 Qt 的 cover 盖不住的窗口（它
     * 自己一块合成表面），屏幕上就是"正文重排了、别的全冻着"。applyState 走 ①c 时
     * 没这个问题，因为它摆完立刻摘窗（②a），只有几 ms。
     */
    EditorViewItem::setGeometryFrozen(true);

    QElapsedTimer clock;
    clock.start();
    placeContent(QRect(QPoint(0, 0), avail.size()));   // ← 那笔 165ms 就在这一行里面
    m_prewarmed = true;
    m_widget->repaint();                               // cover 还挂着：屏幕上没变化
    m_prewarmLife.start(4000);

    /* 这一条是"按下有没有落到这颗按钮上"的探针：没有它，测试脚本分不清
     *  是点击没送到、还是预热条件没过去。 */
    trace(QStringLiteral("预热：按下（光标 %1,%2）→ 4K 渲染提前交掉 %3ms（这一笔原本在点击之后）")
              .arg(m_prewarmPressPos.x()).arg(m_prewarmPressPos.y())
              .arg(clock.elapsed()));
}

void WindowHelper::cancelPrewarm()
{
    m_prewarmLife.stop();

    if (!m_prewarmed)
        return;                         /* 没预热成：只是撤了个看门狗 */
    m_prewarmed = false;

    EditorViewItem::setGeometryFrozen(false);

    QElapsedTimer clock;
    clock.start();
    placeContent(QRect(QPoint(0, 0), m_widget->size()));   // 缩回卡片：实测 8ms
    EditorViewItem::syncAllGeometry();                      // 原生子窗回到卡片那一版坐标
    m_widget->repaint();                                    // cover 还在，这一拍仍是旧画面
    m_prewarmCover = QPixmap();

    trace(QStringLiteral("预热退回（没点到最大化）：%1ms").arg(clock.elapsed()));
}

void WindowHelper::maximize()
{
    if (!m_widget)
        return;

    if (updateMaximizedFromWindow() || m_maximized) {
        trace(QStringLiteral("maximize()：已经是最大化状态，什么都不做"));
        return;   // 已经被系统最大化了（比如拖到屏幕顶端吸附）
    }

    /*
     * 先抓住"还原矩形"：取窗口当下的几何。只有常规状态才会走到这里
     * （上面已排除"系统已最大化"），所以这就是用户最后摆出来的那个
     * 位置和尺寸，点还原时原样回去。
     */
    m_restoreAnchor = m_normalRect.isValid() && m_normalRect.width() > 0
                          ? m_normalRect
                          : m_widget->geometry();

    trace(QStringLiteral("---- maximize()：还原矩形记为 %1x%2@%3,%4 ----")
              .arg(m_restoreAnchor.width()).arg(m_restoreAnchor.height())
              .arg(m_restoreAnchor.x()).arg(m_restoreAnchor.y()));
    applyState(true);
}

void WindowHelper::restore()
{
    if (!m_widget)
        return;

    if (!m_maximized && !updateMaximizedFromWindow()) {
        trace(QStringLiteral("restore()：本来就没最大化，什么都不做"));
        return;   // 本来就没最大化
    }

    trace(QStringLiteral("---- restore()：还原矩形是 %1x%2@%3,%4 ----")
              .arg(m_restoreAnchor.width()).arg(m_restoreAnchor.height())
              .arg(m_restoreAnchor.x()).arg(m_restoreAnchor.y()));
    applyState(false);
}

/*
 * 一次到位地把窗口切到目标状态：**先把内容按目标尺寸画好，再把窗口摆过去**。
 *
 * 为什么是这个顺序（这一版是照录屏改的，素材在 build\frames-*）：
 *
 *   上一版是"showMaximized() 之后等 Qt 自己把内容跟上来"，录屏（60fps，DDA 抓的
 *   合成器输出）里量出来中间有 2~3 帧 —— 窗口**已经**铺满屏幕，而界面内容还是
 *   旧尺寸那一版挂在窗口左上角，其余一大片是没画过的黑 —— 然后才跳成最大化那一版。
 *   用户看到的就是"窗口先跑到角上、再放大"。
 *
 *   那几帧**不是系统那段最大化转场**：转场在 attachWidget 里已经被
 *   DWMWA_TRANSITIONS_FORCEDISABLED 关掉了，录屏里从头到尾没有任何缩放动画
 *   （窗口矩形是"啪"一下到位的）。那几帧是纯粹的"窗口换完尺寸、内容还没按新尺寸
 *   重排重画"。
 *
 * 所以顺序反过来 —— 内容先动，窗口后动（最大化那一路，逐条理由见下面就地注释）：
 *   ① 内容控件（QQuickWidget）先按**目标尺寸**渲染一帧：此刻窗口还是旧尺寸，
 *      多出来的部分被窗口裁掉，屏幕上什么都看不见；而它是离屏渲染的，它的
 *      resizeEvent 里就是 polishItems + sync + render，跟窗口当时多大无关
 *      （见 placeContent 的说明）；
 *   ② 窗口一步摆到目标矩形（setGeometry，不是 showMaximized —— 后者把新尺寸
 *      排队告诉 Qt，紧接着那次合成会按旧尺寸裁）；
 *   ③ 布局归位 + 当场合成内容那一帧（4K 上 ~15ms，省不掉）；
 *   ④ 最后才把最大化状态置上（WS_MAXIMIZE / Qt 的窗口状态，这一步不挪窗口）。
 * ①~④ 在同一个事件循环回合里走完：屏幕上只有"切换前""切换后"两帧。
 *
 * ②→③ 之间有 ~15ms 空档（窗口已撑到 4K、内容还没合成完），DWM 可能把那一帧
 * "表面全屏、内容未就位"放出去。这里**不再主动补一帧底色**去盖它 —— 那等于每次
 * 都必然多呈现一帧裸底色（用户报的"界面一闪"）。空档只靠 WM_ERASEBKGND 兜底
 * （见 m_fastErase）：真空档时那片未初始化显存被擦成界面底色而不是黑；机器空闲时
 * 内容帧赶在前面，屏幕上连这一帧都看不到。
 *
 * 稳态还是普通窗口：最大化就是整块可用区（任务栏 / Alt+Tab / 贴边都照旧），
 * 还原就是用户上次摆出来的那块卡片。
 */
void WindowHelper::applyState(bool maximize)
{
    if (!m_widget)
        return;

    /* 这一小段里每一帧都记进日志（见 traceFrames） */
    traceFrames(true);

    /*
     * 换尺寸这一小段里，WM_ERASEBKGND 由我们擦成界面底色（见 MessageTrace 那个 case）：
     * 从没画过的像素被交给合成器就是"白色背景闪现"。
     */
    m_fastErase = true;


    /* 状态翻转只在这一个地方做 */
    auto setState = [this](bool on) {
        if (m_maximized == on)
            return;
        m_maximized = on;
        emit maximizedChanged();
    };

    if (maximize) {
        setState(true);

        QScreen *screen = screenOf();
        const QRect avail = screen ? screen->availableGeometry() : QRect();

        /*
         * ①a 先把"屏幕上现在这一张"抓下来 —— ②d 那一帧要把它拉伸铺满整块客户区。
         *
         * **必须在这儿抓**，也就是摘原生子窗（②a）、铺底（②b）、摆内容（①b）之前：
         * 抓的是屏幕上现在这一张，晚一步正文那两块就是黑的（实测踩过：拉伸帧里两块黑洞）。
         *
         * 为什么用 QScreen::grabWindow 而不是 QWidget::grab() / render()：后者是让 Qt
         * **重画**一遍，而那时内容控件已经被摆到目标尺寸，抓回来的会是"最大化那一版"
         * 而不是屏幕上现在这一版；而且 Qt 那两条都画不到编辑区那几块原生子窗（它们不在
         * backing store 里）。grabWindow 是"从屏幕上把这块矩形读回来"，一次拷贝、
         * 子窗口齐全（1460x900 实测 11ms）。
         */
        {
            QElapsedTimer snapClock;
            snapClock.start();
            if (screen)
                m_zoomSnapshot = screen->grabWindow(m_widget->winId());
            trace(QStringLiteral("  ①a 抓旧画面：%1x%2，%3ms")
                      .arg(m_zoomSnapshot.width()).arg(m_zoomSnapshot.height())
                      .arg(snapClock.elapsed()));
        }

        /*
         * 预热过（鼠标刚才停在放大按钮上）：从这一拍起"屏幕上贴住旧画面"这件事
         * 交回给 ②d 的拉伸帧，预热那张 cover 没用了；原生子窗也放开，让 ①c 摆到
         * 最大化那一版坐标上去。
         *
         * 下面 ①b 会命中 placeContent 的"尺寸没变"那一支 —— 那 165ms 已经在
         * doPrewarm 里交过了，这一枪是 0ms。
         */
        m_prewarmLife.stop();
        m_prewarmCover = QPixmap();
        if (m_prewarmed) {
            m_prewarmed = false;
            EditorViewItem::setGeometryFrozen(false);
            trace(QStringLiteral("  ①b 之前：预热命中，这一次不用按 4K 重渲染"));
        }

        /*
         * ① 内容先按最大化之后的尺寸渲染一帧（窗口还是小的，多出来的那圈被窗口
         *    裁掉，屏幕上看不出来）。
         *
         * 先 clearMask()：最大化 = 直角，而且**顺手让内容控件那次 4K 渲染走快路径**
         * （带遮罩渲染那一帧要 ~140ms，见 applyRoundedMask 里的说明）。
         *
         * **这一笔不能挪到换几何之后**（试过，实测更糟）：窗口一变大，Qt 的布局就会
         * 把内容控件撑到 4K，那次渲染改在换几何的消息处理里跑完 —— 冷启动量到
         * "换几何 → 拉伸帧上屏"中间空了 **574ms**（原来这一段黑 55ms），而 ①b 自己
         * 反而报 0ms。也就是说渲染的时间一点没省，只是从"屏幕上还看得见卡片"
         * 变成了"屏幕上黑着等它"。
         *
         * 这一笔到底在忙什么（2026-09-19 拆开量过，同一篇文档 / 同一台 4K 屏）：
         *   1460x900 → 3840x2112：**163 / 167 / 171 / 175ms**，来回五次一模一样；
         *   3840x2112 → 1460x900：**8ms**；
         *   **什么文档都不开、场景空着**放大到 4K：还是 **164~175ms**。
         * 也就是说它跟内容无关 —— 不是 Markdown 重排、不是图片解码上传、不是 QML
         * 布局（开着文档的还原方向要重排的行数更多，却只要 8ms）。它只跟**这一帧
         * 有多少像素**走：QQuickWidget 是把场景图渲染到一块离屏 FBO，再把整张
         * 8.1 百万像素读回成 QImage 交给 raster 背衬 —— 这笔读回就是税。场景图自己
         * 报的 render 时间只有 1~9ms（QSG_RENDER_TIMING，RHI 走的是 RTX 4080）。
         * 所以：异步解码图片、懒加载、预热布局**都治不了它**（每帧都要交这份税），
         * 能治的只有两条 —— 要么别在点击这一串里交（提前在别处渲染），要么换掉
         * "离屏 FBO + 读回"这条呈现路径（GPU 直接呈现的顶层窗口 —— 同形状同尺寸的纯
         * QML 窗口在这台机器上量过，换尺寸那一下 0 帧黑，所以真要治就是那一整套工程）。
         */
        m_widget->clearMask();
        if (avail.isValid() && !avail.isEmpty())
            placeContent(QRect(QPoint(0, 0), avail.size()));

        /*
         * ①c 让编辑区那个原生子窗**先按新布局摆好**（见 EditorViewItem::syncAllGeometry）。
         *
         * 它本来是挂在 QML 的 geometryChange 上摆的，而"窗口换尺寸"和"QML 布局落定"
         * 谁先谁后不保证 —— 窗口先变的那一版里，编辑器会带着旧内容挂在旧坐标上飘一帧
         * （144fps 录屏里看得见）。这里在换几何**之前**先摆一次、摆到目标坐标上：
         * 超出旧窗口那部分被宿主裁掉、屏幕上看不见；等窗口一变，它正好在那个位置上。
         */
        EditorViewItem::syncAllGeometry();

        /*
         * ②a 编辑区那几个原生子窗**先摘出屏幕**。
         *
         * 为什么底色盖不住它们：QScintilla 是 createWindowContainer() 出来的独立
         * 原生子窗，自己一块合成表面，不参与宿主的 backing store。144fps 逐帧抓到了
         * 证据（build\uc2\b84.png）：中间帧里整屏已经被 ②b 铺成黑的了，**左上角还亮着
         * 一块旧编辑区**（行号、正文看得清清楚楚）—— 那才是"原始窗口闪到左上角"里
         * 唯一有内容的一块。
         *
         * 摘掉之后那块地方露的是 ②b 铺的黑；放回来的时机在换完几何、整窗合成之前
         * （下面 ②c）。
         */
        EditorViewItem::setAllNativeVisible(false);

        /*
         * ②b 先把界面那一层擦掉（和换几何在同一个回合里）。
         *
         * 擦掉：窗口变大的时候，系统会把"变大前那一张画面"拷到新窗口表面的左上角。
         * 拷过去的是整块界面的话，用户看到的就是"界面跑到左上角、再放大"。
         *
         * 为什么用 RedrawWindow(RDW_UPDATENOW) 而不是 repaint()：repaint() 只是**请求**
         * 重画，机器一忙就推迟到下一轮 —— 那样换几何时拷过去的还是整块界面，
         * 于是这个毛病"不是每次都有"（用户原话）。RDW_UPDATENOW 让系统当场把 WM_PAINT
         * 走完（Qt 那次 backing store 上屏也就跟着走了）。
         */
        auto blankBackdrop = [this]() {
            if (m_quickWidget)
                m_quickWidget->setUpdatesEnabled(false);   /* 这一步里内容控件别画 */
            m_blankBackdrop = true;
            m_widget->repaint();
#if defined(Q_OS_WIN)
            if (HWND hwnd = reinterpret_cast<HWND>(m_widget->internalWinId())) {
                ::RedrawWindow(hwnd, nullptr, nullptr,
                               RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
            }
            /*
             * 再等**合成器真的把这一帧吃掉**，然后才去换几何。
             *
             * 为什么非要等：RedrawWindow(RDW_UPDATENOW) 只保证"应用把这一帧画完了"
             * （WM_PAINT 走完了），**不保证 DWM 已经拿它当这个窗口的最新画面**。
             * 紧接着就 setGeometry() 的话，DWM 手上可能还是上一张 —— 也就是那张
             * **旧尺寸的整块界面**，而窗口变大的时候系统会把它贴到新表面的左上角：
             * 用户看到的就是"界面缩在左上角一小块、再铺满"（就是 ②b 要治的那条，
             * 但 ②b 只擦不"确认上屏"的话治不干净）。
             *
             * DwmFlush() 就是"等到合成器画完当前这一帧"的那个口子。
             * 代价是这里会阻塞一帧（144Hz 上 ~7ms），换的是"拷过去的确定是底色
             * 而不是旧界面" —— 这一段本来就已经在做同步 repaint，不差这一下。
             *
             * 调试开关：SMARTCLIP_NO_DWM_FLUSH=1 —— 退回旧行为，用来对跑。
             */
            static const bool noFlush = qEnvironmentVariableIsSet("SMARTCLIP_NO_DWM_FLUSH");
            if (!noFlush)
                ::DwmFlush();
#endif
            m_blankBackdrop = false;
            if (m_quickWidget)
                m_quickWidget->setUpdatesEnabled(true);
        };

        /*
         * 调试开关：SMARTCLIP_NO_BLANK_BACKDROP=1 —— 不做"先擦成底色"这一步。
         *
         * 用来验一件事：这一擦到底有没有用。关掉之后如果屏幕上变成
         * "界面缩在左上角一小块、再铺满"（用户报的那个形状），就说明这一擦是**必要的**，
         * 只是它在某些机器上没顶到合成器；如果关掉之后反而更顺，那这一擦本身就多余。
         */
        static const bool noBlank = qEnvironmentVariableIsSet("SMARTCLIP_NO_BLANK_BACKDROP");
        if (!noBlank)
            blankBackdrop();

        /* ② 窗口一步摆到可用区 */
        /*
         * 调试开关：SMARTCLIP_MAX_BY_SYSTEM=1 —— 几何也让系统那一下摆（showMaximized），
         * 配合 SMARTCLIP_KEEP_DWM_TRANSITION=1，看"系统转场盖住空档"这条路长什么样
         * （实测：这条路在这个窗口上不放动画，空档一样露，留着是为了以后能再验）。
         */
        static const bool maxBySystem = qEnvironmentVariableIsSet("SMARTCLIP_MAX_BY_SYSTEM");
        trace(QStringLiteral("applyState(最大化)：%1 %2x%3")
                  .arg(maxBySystem ? QStringLiteral("交给系统 showMaximized()")
                                   : QStringLiteral("摆到可用区，再置最大化状态"))
                  .arg(avail.width()).arg(avail.height()));
        if (maxBySystem)
            m_widget->showMaximized();   /* 状态 + 几何都交给系统那一下（转场才有东西可动） */
        else
            m_widget->setGeometry(avail);

        /*
         * ②d 拉伸帧：换完几何之后的**第一次**整窗绘制，只把 ①a 抓的那一张旧画面
         * 拉伸铺满整块客户区（真内容留给下面 ③ 那一帧）。
         *
         * 为什么能治"原始窗口闪到左上角"：GDI 窗口换尺寸时，合成器手上只有上一张
         * 重定向表面，而它是按**窗口内坐标 1:1** 贴到新表面左上角的，新露出来的那一大片
         * 是没画过的黑 —— 屏幕上就是"老界面缩在左上角一小块 + 其余全黑"。这一帧把
         * 那"其余全黑"换成"老界面被放大"，观感就和系统自己那段最大化转场一样了
         * （GPU 呈现的窗口之所以没这个毛病，就是因为合成器对它们是拉伸而不是 1:1 贴）。
         *
         * 为什么这里用 RedrawWindow 而不是 repaint()：要走的是**铺底那一帧同一条通道**
         * —— 实测只有走 Qt 自己的绘制 + backing store 上屏才放得出来（GDI 直接写窗口 DC
         * 那一版实测一帧都上不去，见文件顶部走过的弯路）。不带 RDW_ERASE：带了就先被
         * 底色整窗填一遍，白挨一次 4K 填充。
         */
        if (!m_zoomSnapshot.isNull()) {
            m_zoomFrame = true;
#if defined(Q_OS_WIN)
            if (HWND hwnd = reinterpret_cast<HWND>(m_widget->internalWinId()))
                ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            else
#endif
                m_widget->repaint();
            /*
             * **不清掉 m_zoomFrame**：从换几何到真内容上屏中间还有一截（预热没命中时
             * 那一帧 4K 渲染要 165ms），期间 Qt 还可能自发来重绘（布局激活、子控件
             * 失效…），挂着它 = 那一屏一直是"放大版的旧画面"，不会漏回黑。
             * 收尾在 ②c 之后、flushContent 之前。
             */
        }

        /*
         * ③ 内容那一版：布局归位 + 当场合成一帧（这一步 4K 上要 ~20ms，省不掉）。
         *
         *    这一小段（窗口撑大 → 内容合成上屏）里露出来的那帧是**结构性的**：窗口表面
         *    变大之后、Qt 把内容画出来之前，那块地方是没画过的。四种"盖住它"的办法
         *    都试过、也都用 144Hz 录屏量过（素材在 build\frames-*，判据脚本
         *    build\win-verify-switch.ps1 / win-classify2.ps1）；结论写在这儿免得再走一遍：
         *
         *      * 主动刷一帧界面底色：每次必然多呈现一帧**裸底色**（用户报的"界面一闪"）；
         *      * DWM 隐身（DWMWA_CLOAK；写没写进去用 DWMWA_CLOAKED 读回来验过，
         *        hr=S_OK / cloaked=1，确实生效）：窗口**整个消失** ~40ms（卡片区连续 5 帧
         *        "已经不是卡片"），屏幕上露出背后桌面，比那几帧黑更扎眼；
         *      * "稍微延迟 + 一次性放大"：窗口变大之前先把这一版读回成一张图（延迟这一段
         *        屏幕上还是卡片，是对的），窗口一变就拿它把整块客户区一次画满（纯拷贝
         *        几毫秒）—— 结果**屏幕上的黑帧一帧没少**：我们提前画进窗口表面的那一帧
         *        并没有被放出来，直到 Qt 把放大后的窗口完整合成一次才算数
         *        （日志里"一步放大：贴 cover"到"repaint 之后"隔了 56ms）。
         *        也就是说那几帧不是"我们没画"，是**系统还没把新窗口表面交出去**；
         *      * 让系统那段最大化转场盖（SMARTCLIP_MAX_BY_SYSTEM=1 + KEEP_DWM_TRANSITION=1
         *        + 补 CAPTION/THICKFRAME 样式位 + 去掉 WS_POPUP + 去掉圆角区域 + 让窗口
         *        生来就是普通窗口，六种组合）：**这个窗口上 DWM 压根不放动画**，空档照露
         *        （对照：画图那种普通窗口在这台机器上是放的，见 build\frames-paint）；
         *
         *    所以这一版**不盖**：卡片一直留在屏幕上（不动它），撑大之后那 2~5 帧
         *    （144Hz 上 ~15~40ms）由内容那一帧接上。要把这段再压短，只有把那次整窗合成
         *    从 ~20ms 压到几毫秒（宿主改 GPU 合成），那是另一件工程。
         *    空档里还有 WM_ERASEBKGND 兜底（见 m_fastErase）。
         *
         *    ④ 改状态放在合成**之后**：showMaximized() 实测要 10ms（过一遍系统消息 +
         *    Qt 的窗口状态），放在前面就等于把这 10ms 白算进"屏幕上还没画好"的那段里。
         *
         *    状态这一步**不挪窗口**（矩形已经是可用区了），只把 WS_MAXIMIZE /
         *    Qt 的窗口状态置上 —— 任务栏右键、Win+↓、Alt+Tab 那些语义才是对的。
         *
         *    为什么几何要用 setGeometry() 自己摆、不直接用 showMaximized() 摆：
         *    showMaximized() 那一下，Windows 是**排队**把新尺寸告诉 Qt 的
         *    （日志里 WM_SIZE 到了之后 ~14ms 才有 Qt Resize），而紧接着那次同步合成
         *    会按 Qt 当时记着的**旧尺寸**裁（"帧：主窗口 paint 范围 1460x900@0,0"）——
         *    新露出来的那大半屏就留下一帧黑。
         *    setGeometry() 是 Qt 自己的调用，几何当场就更新，合成覆盖整块窗口。
         */
        /*
         * ②c 编辑区原生子窗放回屏幕（②a 摘掉的那一批）。
         *
         * 为什么放在整窗合成**之前**：放回来的那一下它们自己会重画一帧，紧接着
         * flushContent 把整窗合成一次 —— 两件事落在同一拍里，屏幕上就不会多出一帧
         * "4K 黑底上缺了正文那两块"。
         */
        EditorViewItem::setAllNativeVisible(true);
        trace(QStringLiteral("  ②c 编辑区原生子窗放回屏幕（已在目标坐标）"));

        /* 拉伸帧收尾：从这一拍起允许画真内容（下面 flushContent 就是那一帧） */
        m_zoomFrame = false;
        m_zoomSnapshot = QPixmap();
        flushContent();
        m_widget->showMaximized();

        /*
         * 兜底：万一某个平台/DPI 组合下几何没落定，按**系统那边的真实矩形**
         * （nativeFrameRect）核一次。判据不能用 Qt 的 geometry()：那一拍它记的可能
         * 还是旧值，拿它比会永远"不相等"，于是白补一次 setGeometry()。
         */
        if (avail.isValid() && nativeFrameRect(m_widget) != avail) {
            trace(QStringLiteral("  几何没落定，补一次到可用区"));
            m_widget->setGeometry(avail);
        }

        m_normalRect = m_restoreAnchor;
    } else {
        /*
         * 还原是同一条路反过来走：先把内容按卡片尺寸画好，再把窗口缩回去。
         *
         * 顺序反过来的话，录屏里能看到卡片上先挂 1~2 帧"最大化版左上角那一条"。
         */
        setState(false);

        const QRect to = clampToScreen(m_restoreAnchor.isValid() ? m_restoreAnchor
                                                                 : m_normalRect);
        if (to.isValid() && to.width() > 0 && to.height() > 0)
            placeContent(QRect(QPoint(0, 0), to.size()));

        trace(QStringLiteral("applyState(还原)：showNormal() + 摆到还原矩形 %1x%2@%3,%4")
                  .arg(to.width()).arg(to.height()).arg(to.x()).arg(to.y()));
        m_widget->showNormal();
        if (to.isValid() && to.width() > 0 && to.height() > 0) {
            m_widget->setGeometry(to);
            m_normalRect = to;
        }
        flushContent();
    }

    /*
     * 几何都摆完了，**再把状态对一次**，然后才算圆角。
     *
     * 为什么必须重设：这一路里 showNormal() / showMaximized() 会触发 WindowStateChange，
     * 而那一拍窗口的几何还是"另一半"（还原时还停在整块可用区上），
     * updateMaximizedFromWindow() 按几何就把它又认成"最大化"了 —— 圆角随即被当成
     * "最大化 = 直角"算掉，用户看到的就是"最大化还原之后四个圆角变成直角"（实测复现过：
     * 还原后 GetWindowRgn 回 ERROR）。
     */
    setState(maximize);

    /* 最大化/还原会改变要不要圆角 */
    applyRoundedMask();

    notifyTransition();
    updateMaximizedFromWindow();
    traceSnapshot(maximize ? QStringLiteral("---- applyState(最大化) 完成 ----")
                           : QStringLiteral("---- applyState(还原) 完成 ----"));

    /* 这一摊事唯一的"地面真相"：系统那边窗口现在是什么形状（见 windowRegionText） */
    trace(QStringLiteral("窗口区域（系统答的）= %1").arg(windowRegionText()));

    /* 换尺寸这一段结束：背景擦除回到"Qt 自己管" */
    m_fastErase = false;
}

/*
 * 现在窗口在系统那边到底是什么形状（空 = 整块矩形）。
 *
 * 这一条是**地面真相**：日志里那些"区域/遮罩"的记录都是我们自己记的，
 * 只有这个数是系统答的（用户报的"最大化之后只有一块内容、四周露白"就是
 * 窗口区域没撤干净，而这一点从 Qt 那边看不出来）。
 */
QString WindowHelper::windowRegionText() const
{
#if defined(Q_OS_WIN)
    if (!m_widget)
        return QStringLiteral("?");
    const HWND hwnd = reinterpret_cast<HWND>(m_widget->internalWinId());
    if (!hwnd)
        return QStringLiteral("还没建");
    HRGN rgn = ::CreateRectRgn(0, 0, 0, 0);
    if (!rgn)
        return QStringLiteral("?");
    const int kind = ::GetWindowRgn(hwnd, rgn);
    QString out;
    if (kind == ERROR) {
        /*
         * 按 MSDN：窗口**没有**区域时 GetWindowRgn 返回 ERROR（不是"失败"）。
         * 这正是最大化之后该有的样子 —— 整块矩形、不裁任何东西。
         */
        out = QStringLiteral("无（整块矩形）");
    } else if (kind == NULLREGION) {
        out = QStringLiteral("空");
    } else {
        RECT rc{};
        if (::GetRgnBox(rgn, &rc))
            out = QStringLiteral("%1x%2@%3,%4（%5）")
                      .arg(rc.right - rc.left).arg(rc.bottom - rc.top)
                      .arg(rc.left).arg(rc.top)
                      .arg(kind == SIMPLEREGION ? QStringLiteral("单矩形")
                                                : QStringLiteral("复合"));
        else
            out = QStringLiteral("有区域");
    }
    ::DeleteObject(rgn);
    return out;
#else
    return QStringLiteral("非 Windows");
#endif
}

/*
 * 把内容控件摆到窗口里的某一块位置上，并让它**当场**按这个尺寸渲染一帧。
 *
 * "当场"这三个字是整件事的关键：QQuickWidget 是离屏渲染的，它的 resizeEvent 里
 * 就走 polishItems + sync + render（见 qquickwidget.cc），所以 setGeometry() 返回时
 * 纹理里已经是新尺寸、新布局的内容了 —— 接下来放开区域/合成上屏时，
 * 那一帧就是完整的（不然要等下一次垂直同步，中间又是"旧的还在、新的是空"）。
 *
 * 注意：控件是宿主布局管着的，布局下一次 activate 时会把它摆回"填满宿主"。
 * 每次都恰好是我们想要的（最大化时填满整块、还原后填满卡片），所以这里不需要
 * 跟布局打架 —— 这也是这套做法不用改 QML 的原因。
 */
void WindowHelper::placeContent(const QRect &contentInWindow)
{
    if (!m_quickWidget || !contentInWindow.isValid())
        return;

    if (m_quickWidget->geometry() == contentInWindow) {
        /* 尺寸没变就不会有 resizeEvent，也就不会重画 —— 但内容本来就是对的 */
        trace(QStringLiteral("  ③ 内容控件已经是 %1x%2@%3,%4（不用动）")
                  .arg(contentInWindow.width()).arg(contentInWindow.height())
                  .arg(contentInWindow.x()).arg(contentInWindow.y()));
        return;
    }

    trace(QStringLiteral("  ③ 内容控件摆到 %1x%2@%3,%4 之前")
              .arg(contentInWindow.width()).arg(contentInWindow.height())
              .arg(contentInWindow.x()).arg(contentInWindow.y()));
    m_quickWidget->setGeometry(contentInWindow);
    trace(QStringLiteral("  ③ 内容控件摆好（它的 resizeEvent 里已同步渲染一帧）"));
}

/*
 * 把"窗口刚变完尺寸"这一帧**当场**合成并刷上屏。
 *
 * 为什么需要它（日志 + 屏幕采样量出来的）：窗口几何是一步到位的（2ms 一拍），
 * 内容控件（QQuickWidget）也在 2ms 内被摆到新尺寸、并在它自己的 resizeEvent 里
 * 同步 polishItems + sync + render 了一帧（Qt 的实现：qquickwidget.cc 的
 * QQuickWidget::resizeEvent 末尾就是 d->render(needsSync)）—— 但**这一帧要等
 * Qt 下一轮把整窗合成一次才会出现在屏幕上**。日志里那一截：
 *
 *     WM_WINDOWPOSCHANGED  落到 3840x2112@0,0        ← 窗口 2ms 变完
 *     QQuickWidget Resize  3840x2112@0,0             ← 内容 4ms 也跟上了
 *     帧：界面重画（QQuickWidget paint）             ← 第一次合成，26ms 之后
 *
 * 这一帧要是不主动合成，"区域放开"和"新内容上屏"就错开了 —— 屏幕上会是
 * "整块可用区已经交给窗口了，内容还没画上去"。所以放开区域之后立刻合成一次，
 * 让这两件事落在同一帧里。
 *
 * 这里做两件事，都在同一个事件循环回合里完成：
 *   ① layout()->activate()：让布局**当场**把内容控件摆到新尺寸（不等下一个
 *      LayoutRequest），顺带触发它自己那次同步渲染；
 *   ② 整窗 repaint()：同步走一遍 backing store 并把这一帧刷上屏
 *      （QQuickWidget 是纹理合成的，它自己的 paintEvent 什么都不画，
 *       内容上屏靠的就是这一次合成）。
 *
 * 于是屏幕上只有"变完了"这一帧，没有中间态。
 */
void WindowHelper::flushContent()
{
    if (!m_widget)
        return;

    if (QLayout *lay = m_widget->layout()) {
        lay->activate();
        trace(QStringLiteral("flushContent：布局算完了（内容控件 %1x%2）")
                  .arg(m_quickWidget ? m_quickWidget->width() : 0)
                  .arg(m_quickWidget ? m_quickWidget->height() : 0));
    }

    /* 只有真有那块 QML 控件时才做整窗同步合成（它才是要抢那一帧的东西） */
    if (m_quickWidget) {
        trace(QStringLiteral("flushContent：repaint 之前"));
        m_widget->repaint();
        trace(QStringLiteral("flushContent：repaint 之后（窗口 %1x%2）")
                  .arg(m_widget->width()).arg(m_widget->height()));
    }
}

/*
 * 通知界面"刚才切过了"，让它做一次淡入。
 *
 * 先置 false 再置 true：连续两次切换（比如快速点两下按钮）时，
 * 属性值本身没变就发不出 changed 信号，界面也就不会重新淡入。
 * 这样绕一下保证每次都触发。
 */
void WindowHelper::notifyTransition()
{
    if (m_transitioned) {
        m_transitioned = false;
        emit transitionedChanged();
    }
    m_transitioned = true;
    emit transitionedChanged();
}

bool WindowHelper::updateMaximizedFromWindow()
{
    if (!m_widget)
        return false;

    if (m_widget->isMaximized()) {
        if (!m_maximized) {
            m_maximized = true;
            emit maximizedChanged();
        }
        return true;
    }

    /*
     * 无边框窗口的 visibility 有时停在 Windowed，
     * 但几何其实已经铺满屏幕（拖到屏幕顶端吸附最大化就是这样），
     * 所以再按几何兜一次。
     *
     * 容差按屏幕尺寸取（16px 封顶），不能用固定像素：固定容差在 4K 上
     * 等于"任何窗口都算最大化"，一按放大就会被判定为已最大化、直接跳过。
     */
    if (QScreen *screen = screenOf()) {
        if (!m_widget->isVisible())
            return m_maximized;
        const QRect avail = screen->availableGeometry();
        const QRect g = m_widget->geometry();
        const int slack = qBound(2, avail.width() / 100, 16);
        const QRect loose = avail.adjusted(-slack, -slack, slack, slack);
        if (loose.contains(g)
            && qAbs(g.width() - avail.width()) <= slack
            && qAbs(g.height() - avail.height()) <= slack) {
            if (!m_maximized) {
                m_maximized = true;
                emit maximizedChanged();
            }
            return true;
        }
    }

    if (m_maximized) {
        m_maximized = false;
        emit maximizedChanged();
    }
    return false;
}

void WindowHelper::cacheNormalGeometry(const QRect &geometry)
{
    // 铺满屏幕的几何不能当还原矩形，否则点还原会"还原成最大化"
    if (QScreen *screen = screenOf()) {
        if (geometry.size() == screen->availableGeometry().size())
            return;
    }

    if (geometry.width() <= 0 || geometry.height() <= 0)
        return;

    m_normalRect = geometry;
}

/*
 * 宿主窗口的几何报给界面（只在真变了的时候发一枪）。
 *
 * 为什么要报：下拉菜单是**独立原生窗口**（popupType: Popup.Window），它的屏幕
 * 位置在开出来那一刻就算死了；主窗口后来一挪，这块同级窗口还钉在原来的屏幕位置上
 * （实测：窗口 (400,200) -> (100,116)，244x455 那块菜单还留在 (657,231)）——
 * 屏幕上就是用户报的"整个菜单没挂在「视图」那一栏下面"，偏多少正好等于窗口挪了多少。
 * 界面收到这个信号就把菜单收起来（见 DropdownMenu.qml 里那个 Connections）。
 *
 * 而 QML 那侧的 mapToGlobal / Window.x 和宿主 QWidget 的几何不是同一套坐标系
 * （见 main.cpp 里识别卡片那段说明），所以"窗口现在在哪"只能由这一层给。
 *
 * 取的是 geometry() 而不是 frameGeometry()：弹窗的 x/y 是相对**客户区**算的
 * （QML 内容根就铺在客户区里），两者要同一个基准。无边框窗口下这两个值本来就一样。
 */
void WindowHelper::publishHostGeometry()
{
    if (!m_widget)
        return;

    const QRect g = m_widget->geometry();
    if (g == m_hostGeometry)
        return;

    m_hostGeometry = g;
    trace(QStringLiteral("宿主几何报到界面（菜单据此收起来）：%1x%2@%3,%4")
              .arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y()));
    emit hostGeometryChanged();
}

/*
 * 自检用：把宿主窗口挪一段（见 WindowHelper.h）。
 *
 * 不用 setGeometry() 整个矩形：这里要的就是一次"和用户拖窗口同款"的移动
 * —— 只动位置、尺寸不动，走的是 QWidget::move()，Move 事件和轮询两条路都会响。
 */
bool WindowHelper::moveHostForTest(int dx, int dy)
{
    if (!m_widget)
        return false;

    m_widget->move(m_widget->x() + dx, m_widget->y() + dy);
    /* 自检不等事件循环：当场把新几何报出去，和 Move 事件那条路汇到同一个函数里 */
    publishHostGeometry();
    return true;
}

/*
 * 还原矩形落到已经不存在的显示器上时，先拉回主屏，
 * 否则窗口会还原到看不见的地方。
 */
QRect WindowHelper::clampToScreen(const QRect &rect) const
{
    if (!m_widget)
        return rect;

    const QRect onScreen = screenOf() ? screenOf()->geometry() : QRect();
    if (!onScreen.isEmpty() && onScreen.intersects(rect))
        return rect;

    const QScreen *primary = QGuiApplication::primaryScreen();
    if (!primary)
        return rect;

    const QRect avail = primary->availableGeometry();
    return QRect(avail.topLeft() + QPoint(80, 80), rect.size().boundedTo(avail.size()));
}

bool WindowHelper::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_widget)
        return QObject::eventFilter(watched, event);

    /*
     * 装 QML 那块控件（QQuickWidget）单独记一行。
     *
     * 它的几何和窗口的几何不是一回事：窗口已经变大了、它还没跟上（或者反过来），
     * 屏幕上就是"内容没跟上窗口" —— 树和正文之间那条缝在闪，多半就藏在这一行里。
     */
    if (watched != m_widget) {
        if (watched == m_quickWidget) {
            /*
             * 日志里的名字**现读控件自己的类名**，别写死字面量：曾经有过第二条
             * 内容层实现（GPU 内容层，见 src/main.cpp"GPU 合成的试验结论"），
             * 那时候写死 "QQuickWidget" 会让日志撒谎（实测：明明写着 QQuickWidget，
             * 控件其实是 QOpenGLWidget）。现读一行不多，留着。
             */
            const QString who = QString::fromLatin1(watched->metaObject()->className());
            if (event->type() == QEvent::Resize || event->type() == QEvent::Move) {
                const QRect g = m_quickWidget->geometry();
                trace(QStringLiteral("%1 %2  %3x%4@%5,%6")
                          .arg(who,
                               event->type() == QEvent::Resize ? QStringLiteral("Resize")
                                                              : QStringLiteral("Move"))
                          .arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y()));
            } else if (event->type() == QEvent::Paint) {
                /*
                 * "界面重画了一次" —— 内容跟没跟上窗口，只有这一条说得准。
                 *
                 * 为什么不是 QQuickWindow::frameSwapped：这块界面走的是
                 * QQuickRenderControl 渲染进 FBO，**没有换页那一下**，
                 * frameSwapped 一次都不发（实测：挂上去以后日志里一条都没有）。
                 * 重画则是真的会走到这里的。
                 *
                 * 只在切换后那一小段里记（见 traceFrames）：平时 QML 一动就是一串。
                 */
                if (m_traceFramesUntil >= 0 && m_traceClock.elapsed() <= m_traceFramesUntil)
                    trace(QStringLiteral("帧：界面重画（%1 paint）").arg(who));
            }
        }
        return QObject::eventFilter(watched, event);
    }

    switch (event->type()) {
    case QEvent::Resize:
        traceSnapshot(QStringLiteral("Qt Resize"));
        // 只在"常规状态"下记还原矩形；最大化时的整屏几何不是还原目标
        if (!m_maximized)
            cacheNormalGeometry(m_widget->geometry());
        /* 尺寸变了要按新尺寸重算圆角遮罩 */
        applyRoundedMask();
        /* 界面那边（下拉菜单）要收起来 —— 见 hostGeometryChanged */
        publishHostGeometry();
        break;

    case QEvent::Move:
        traceSnapshot(QStringLiteral("Qt Move"));
        /*
         * 位置变了**不用**重设遮罩：遮罩是窗口自己的形状（相对它自己），
         * 跟它在屏幕上的位置无关。原来这里和 Resize 一起调 applyRoundedMask()，
         * 于是拖窗口 / 最大化那几拍每次都白白 SetWindowRgn 一次 —— 每次都让整块
         * 窗口重画，白挨几帧闪。
         */
        if (!m_maximized)
            cacheNormalGeometry(m_widget->geometry());
        /*
         * 菜单是独立原生窗口，不会自己跟着窗口走：这一枪让界面把它收起来
         * （用户报的"整个菜单没挂在「视图」栏下面"就是漏了这一步）。
         */
        publishHostGeometry();
        break;

    case QEvent::Paint: {
        /*
         * 预热期间（鼠标停在放大按钮上那一小段）：只贴"预热之前那一张"，不合成内容
         * 控件 —— 那时内容控件已经按 4K 摆好，让它自己画就是"最大化那一版的左上角
         * 那一条"缩在卡片里（见 prewarmMaximize）。
         *
         * 目标矩形按逻辑尺寸给：dpr>1 时抓回来的那张是设备像素，直接按像素贴会贴小。
         */
        if (!m_prewarmCover.isNull()) {
            trace(QStringLiteral("  帧：预热贴画（%1x%2）")
                      .arg(m_widget->width()).arg(m_widget->height()));
            QPainter cover(m_widget);
            cover.drawPixmap(QRect(QPoint(0, 0), m_widget->size()), m_prewarmCover);
            return true;
        }
        /*
         * ②d 那一帧：只把旧画面**拉伸铺满**整块客户区，不合成真内容
         * （为什么是这一帧、为什么走这条通道，见 applyState 里 ②d 那段）。
         *
         * 不开 QPainter::SmoothPixmapTransform：4K 目标尺寸下双线性要慢一个量级，
         * 而这一帧本来就只存在几十毫秒，糊一点正好像系统那段转场。
         */
        if (m_zoomFrame && !m_zoomSnapshot.isNull()) {
            trace(QStringLiteral("  帧：拉伸旧画面 → 铺满 %1x%2（源 %3x%4）")
                      .arg(m_widget->width()).arg(m_widget->height())
                      .arg(m_zoomSnapshot.width()).arg(m_zoomSnapshot.height()));
            QPainter zoom(m_widget);
            zoom.drawPixmap(QRect(QPoint(0, 0), m_widget->size()), m_zoomSnapshot);
            return true;
        }
        /*
         * "先把界面擦成底色"这一帧（见 applyState 里 ②b）：只铺一片纯色，不合成内容控件。
         *
         * 为什么要它：窗口变大的时候，合成器会把"上一次呈现的那一张"按窗口内坐标
         * 贴到新表面的左上角。贴过去的是整块界面的话，用户看到的就是"界面跑到左上角、
         * 再放大"（用户报的原话）；先把它铺成一片纯色，贴过去的就只是那片纯色。
         *
         * **为什么这一帧是纯黑、不是界面底色 #313335**（144fps 录屏逐帧取色量的，
         * 素材 build\probe-base2 / probe-fix）：换完几何之后**新露出来的那一大片**，
         * 合成器手里是 (0,0,0) —— 不是底色（WM_ERASEBKGND 里 GDI 擦的那一遍根本到不了
         * 屏幕，Qt 随后按整块脏区上屏，把它盖掉了）。所以：
         *   * 铺黑 → 整段空档是一片均匀的黑，看不出"左上角有一张卡片"；
         *   * 铺 #313335 → 左上角那块变成 (48,50,52)、其余 (0,0,0)，屏幕上显出
         *     一张 1460x900 的**卡片形状** —— 恰好就是用户报的那个形状，更扎眼。
         * 实测两种铺法中间帧数一样（各 4 帧 ≈ 28ms），差的只是"看不看得出形状"。
         */
        if (m_blankBackdrop) {
            trace(QStringLiteral("  帧：铺底色（%1x%2）")
                      .arg(m_widget->width()).arg(m_widget->height()));
            QPainter blank(m_widget);
            blank.fillRect(QRect(QPoint(0, 0), m_widget->size()), QColor(0x00, 0x00, 0x00));
            return true;
        }
        /*
         * 主机窗口自己重画（整窗合成的时机，见 flushContent）。
         *
         * 换尺寸那一小段里把**绘制范围**记下来，用来确认这一拍覆盖的是整块窗口
         * （只覆盖一小块的话，放开窗口时露出来的就是没画过的像素）。
         */
        if (m_fastErase) {
            const QRect r = static_cast<QPaintEvent *>(event)->region().boundingRect();
            trace(QStringLiteral("  帧：主窗口 paint 范围 %1x%2@%3,%4")
                      .arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y()));
        }
        break;
    }

    case QEvent::WindowStateChange:
        traceSnapshot(QStringLiteral("Qt WindowStateChange"));
        // 用户拖到屏幕顶端吸附最大化 / 从最大化拖出来：状态跟着窗口走
        updateMaximizedFromWindow();
        break;

    case QEvent::Show:
        traceSnapshot(QStringLiteral("Qt Show"));
        break;

    case QEvent::Hide:
        traceSnapshot(QStringLiteral("Qt Hide"));
        break;

    case QEvent::WindowActivate:
        traceSnapshot(QStringLiteral("Qt WindowActivate"));
        break;

    case QEvent::WindowDeactivate:
        traceSnapshot(QStringLiteral("Qt WindowDeactivate"));
        break;

    case QEvent::WinIdChange:
        /*
         * Qt 把原生窗口换了一个（某些窗口标志 / 属性变化会让它重建）。
         *
         * 这件事对"系统转场动画关掉了没有"是要紧的：那个开关挂在**句柄**上，
         * 换了句柄就跟着丢了。所以这里补设一遍 —— 日志里"系统转场="那一列
         * 是不是一直"已关"，一眼就能看出来。
         */
        trace(QStringLiteral("Qt WinIdChange：原生窗口换成 0x%1")
                  .arg(quintptr(m_widget->internalWinId()), 0, 16));
#if defined(Q_OS_WIN)
        /* 只在"新句柄已经在了"的时候补设：不然这一句又会绕回创建流程（见上） */
        if (m_widget->internalWinId()) {
            const HRESULT hr = disableDwmTransitions(m_widget);
            m_transitionsDisabled = SUCCEEDED(hr);
            m_transitionNote = dwmTransitionNote(m_widget->internalWinId(), hr);
        }
#endif
        traceSnapshot(QStringLiteral("  └ 换过句柄之后"));
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}
