#include "WindowHelper.h"

#include "DialogStyle.h"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QFile>
#include <QTimer>

#include <QEvent>
#include <QGuiApplication>
#include <QLayout>
#include <QPainterPath>
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
 * ------------------------------------------------------------------------ */

void WindowHelper::trace(const QString &what)
{
    const QString path = QString::fromLatin1(kWindowTraceFile);

    if (!m_traceStarted) {
        m_traceStarted = true;
        m_traceClock.start();
        QFile::remove(path);
        QFile head(path);
        if (head.open(QIODevice::WriteOnly | QIODevice::Text)) {
            head.write(QStringLiteral("# SmartClip 窗口变化日志  启动于 %1\n")
                           .arg(QDateTime::currentDateTime().toString(
                               QStringLiteral("HH:mm:ss.zzz")))
                           .toUtf8());
            head.close();
        }
    }

    QFile f(path);
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    f.write(QStringLiteral("%1  %2\n").arg(m_traceClock.elapsed(), 6).arg(what).toUtf8());
    f.flush();
    f.close();
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

void WindowHelper::refreshMask()
{
    applyRoundedMask();
}

bool WindowHelper::probeEnabled() const
{
    static const bool on = qEnvironmentVariableIsSet("SMARTCLIP_LAYOUT_PROBE");
    return on;
}

void WindowHelper::applyRoundedMask()
{
    if (!m_widget)
        return;

    /*
     * 换尺寸那一小段里"区域"归幕布管（见 curtainRegion）：这一段里**不许**按窗口
     * 尺寸重算圆角遮罩 —— 那会把幕布顶掉，而 setMask 还会把绘制裁起来，
     * 于是幕布后面那圈画不上、撤幕布时就是白边。
     */
    if (m_curtainOn)
        return;

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
     * 圆角半径：最大化时给直角（见 applyRoundedMask 上面那段）。
     *
     * **不要再"拖边的时候也不给圆角"**：试过（为了少几次 SetWindowRgn），
     * 但摘掉遮罩之后窗口变方角，而四角那几像素从来没画过 —— 拖窗口 / 拉边时
     * 角上会闪白（用户报的"白色背景闪现"）。圆角照旧每拍跟着尺寸算，
     * 多出来的那几次 SetWindowRgn 换来的是"任何时刻都不露没画过的像素"。
     */
    const int r = m_maximized ? 0
                              : qRound(qMin(m_cornerRadius, qMin(w / 2, h / 2)) * dpr);

    QRegion region;
    if (m_curtainOn) {
        /*
         * 换尺寸那一小段：区域钉在"卡片"上（见 curtainRegion）。
         *
         * 这一小段里窗口的几何可能已经是整块可用区了，但屏幕上看起来必须还是
         * 原来那块窗 —— 所以区域**不能**按窗口尺寸算，得用钉住的那一块。
         */
        region = m_curtain;
    } else if (r > 0) {
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
    if (m_maskApplied && region == m_appliedMask)
        return;

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

        case WM_SYSCOMMAND: {
            const WPARAM cmd = msg->wParam & 0xFFF0;
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
            static HBRUSH brush = ::CreateSolidBrush(RGB(0x31, 0x33, 0x35));
            ::FillRect(dc, &rc, brush);
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
        const HRESULT dwhr = disableDwmTransitions(m_widget);
        m_transitionsDisabled = SUCCEEDED(dwhr);
        m_transitionNote = dwmTransitionNote(m_widget->internalWinId(), dwhr);
        trace(QStringLiteral("系统转场动画：%1").arg(m_transitionNote));
#else
        m_transitionNote = QStringLiteral("非 Windows，关不了");
#endif
        cacheNormalGeometry(m_widget->geometry());
        updateMaximizedFromWindow();
        applyRoundedMask();

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
 * 一次到位地把窗口切到目标状态。
 *
 * 这套顺序的目的只有一个：**屏幕上不出现"窗口已经变大、那一块还没画过"的中间态**。
 * 做法是拿窗口区域(SetWindowRgn)当幕布（用户挑的方案 B）：
 *
 *   最大化：
 *     ① 区域钉在"卡片"上（就是现在这块窗的样子）
 *     ② 窗口几何一步长成整块可用区 —— 多出来的那圈被区域裁掉，屏幕上看不出来
 *     ③ 内容控件按最大化尺寸摆好，它的 resizeEvent 里会**同步**渲染一帧
 *     ④ 放开区域（整窗可见）
 *     ⑤ 当场合成上屏
 *   ①②③④⑤ 都在同一个事件循环回合里，所以合成器只会看到"旧卡片"和"最大化界面"
 *   两个状态，中间那些步骤一个都露不出来。
 *
 *   还原是同一条路反过来走：区域先缩成卡片（窗口还是整块）→ 内容缩回卡片并渲染
 *   → 真窗口再缩成卡片（这时区域按新尺寸算，形状和刚才那块完全一样）→ 合成。
 *
 * 为什么这套能成立（实测过，见 build\win-region-proto.ps1）：
 *   * 区域裁掉的部分**不属于这个窗口** —— 桌面照常透出来，那片地方的点击也照常
 *     落到桌面上（WindowFromPoint 验过：卡片外面返回的是桌面/浏览器，卡片里面
 *     才是我们）。所以不需要透明窗口、不需要管 alpha 命中测试；
 *   * 内容控件（QQuickWidget）是离屏渲染的，它自己的 resizeEvent 里就是
 *     polishItems + sync + render，跟窗口当时多大无关 —— 所以"先把内容画好"
 *     这一步能提前，画完再放开区域，那一帧就是完整的。
 *
 * 稳态还是普通窗口：还原状态就是一块 1460x900 的真窗口（任务栏 / Alt+Tab /
 * 贴边 / 拖到别的显示器都照旧），"整块可用区大小"只在切换那几十毫秒里存在。
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
         * ==============================================================
         * 最大化：交给系统（showMaximized），不再自己摆几何 + 幕布
         * ==============================================================
         *
         * 为什么回到这条路（"幕布"那套试过、留着记录免得再走）：
         *   自己摆几何要同时做两件事 —— 把窗口挪到 (0,0)、把"窗口在屏幕上的形状"
         *   从旧卡片换成新卡片。两者是**两个 API 调用**，而区域用的是窗口内坐标：
         *     * 先设区域、后挪窗口 -> 窗口还在旧位置时区域落在客户区外面，
         *       窗口整个不显示（屏幕上就是桌面/浏览器），而且持续整个 4K 渲染过程；
         *     * 先挪窗口、后设区域 -> 中间那一瞬窗口已经在 (0,0)、区域还是旧的，
         *       画面会"跳到左上角"。
         *   实测两种都露出一帧（用户报的"白色边"和"取消最大化也闪"），
         *   这是结构性的，不是调几行能根治的。
         *
         * 而系统自己的最大化是**窗口管理器一手包办**的：几何、表面重建、重画
         * 在一条路径里完成，外加 DWM 那段"从旧矩形缩放过来"的转场把整个过程盖住 ——
         * 所以不可能露白、不可能闪。
         *
         * 这段转场以前看着不对，是因为当时**内容要 160ms 才跟上**（动画里放的是
         * 旧内容、收尾还夹一帧黑）。那 160ms 上一轮查明并消掉了（"带着 setMask
         * 去 resize 内容控件"造成的，现在 ~3ms），所以现在动画里放的就是新内容。
         *
         * 先 clearMask()：最大化 = 直角，而且**顺手让内容控件那次 4K 渲染走快路径**
         * （带遮罩渲染那一帧要 ~140ms，见 applyRoundedMask 里的说明）。
         */
        trace(QStringLiteral("applyState(最大化)：交给系统 showMaximized()（可用区 %1x%2）")
                  .arg(avail.width()).arg(avail.height()));
        m_widget->clearMask();
        m_widget->showMaximized();
        traceSnapshot(QStringLiteral("  已 showMaximized()"));

        /*
         * 无边框窗口在部分平台上 showMaximized() 回来时几何还没落定，
         * 补一次到屏幕可用区域，保证和任务栏不重叠。
         */
        if (avail.isValid() && m_widget->geometry() != avail) {
            trace(QStringLiteral("  几何没落定，补一次到可用区"));
            m_widget->setGeometry(avail);
        }

        m_normalRect = m_restoreAnchor;
    } else {
        /*
         * 还原：同样交给系统退出最大化状态，再把几何对到还原矩形。
         *
         * 顺序和最大化镜像：先 showNormal()（系统自己做"从最大化缩回来"的转场，
         * 一样盖住表面重建），再把窗口摆到用户上次的位置和尺寸。
         */
        setState(false);

        const QRect to = clampToScreen(m_restoreAnchor.isValid() ? m_restoreAnchor
                                                                 : m_normalRect);
        trace(QStringLiteral("applyState(还原)：交给系统 showNormal()（还原矩形 %1x%2@%3,%4）")
                  .arg(to.width()).arg(to.height()).arg(to.x()).arg(to.y()));
        m_widget->showNormal();
        if (to.isValid() && to.width() > 0 && to.height() > 0) {
            m_widget->setGeometry(to);
            m_normalRect = to;
        }
        traceSnapshot(QStringLiteral("  已 showNormal()"));
    }

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
 * 幕布：把窗口在**屏幕上**的形状裁成"卡片"，但**不让 Qt 知道**。
 *
 * 用户报的"点最大化那一下，三条白色间隙"就是这里的错：
 * 之前用的是 QWidget::setMask() —— setMask 除了给窗口设区域，还会让 Qt 把
 * **绘制**也裁在区域里。于是幕布后面那一圈（等下要露出来的地方）**根本画不上**，
 * 幕布一撤，露出来的就是没画过的像素（白 / 花），形状正好是卡片四周的那几条。
 *
 * 走 ::SetWindowRgn 就只有"窗口在屏幕上是这个形状"这一层意思，Qt 照旧把整个窗口
 * 画满 —— 所以只要在撤幕布之前先合成一次，露出来的那圈就已经是画好的内容。
 *
 * 注意 setMask 那套（Qt 自己的圆角遮罩）在这一段里必须让开：见 applyRoundedMask()
 * 开头那句"幕布期间不碰区域"。
 */
void WindowHelper::curtainRegion(const QRect &contentInWindow)
{
    if (!m_widget || !contentInWindow.isValid())
        return;

    const qreal dpr = m_widget->devicePixelRatioF();
    const QRect dev(QPoint(qRound(contentInWindow.x() * dpr), qRound(contentInWindow.y() * dpr)),
                    QSize(qRound(contentInWindow.width() * dpr), qRound(contentInWindow.height() * dpr)));

    QPainterPath path;
    const int r = qRound(qMin<qreal>(m_cornerRadius * dpr, qMin(dev.width(), dev.height()) / 2.0));
    if (r > 0)
        path.addRoundedRect(QRectF(dev), r, r);
    else
        path.addRect(QRectF(dev));

    m_curtain = QRegion(path.toFillPolygon().toPolygon());
    m_curtainOn = true;
    m_maskApplied = false;

    /* Qt 这边先别裁绘制（这一步同时会把窗口区域清掉，紧接着我们设自己的） */
    m_widget->clearMask();

#if defined(Q_OS_WIN)
    if (HWND hwnd = reinterpret_cast<HWND>(m_widget->internalWinId())) {
        HRGN hrgn = m_curtain.toHRGN();          /* 设进去之后由系统接管，不要再 delete */
        if (hrgn)
            ::SetWindowRgn(hwnd, hrgn, TRUE);
    }
#else
    m_widget->setMask(m_curtain);                /* 别的平台没有这层区分，退回 setMask */
#endif
}

/*
 * 撤幕布：把区域交回 Qt 管（按 最大化/常规 算成空区域或圆角卡片）。
 *
 * ⚠ 这里必须**自己**把窗口区域撤掉，不能指望 applyRoundedMask() 里那句
 * clearMask()：curtainRegion() 已经 clearMask() 过一次，Qt 那边记的是"没有遮罩"，
 * 再调一次会被它当成空操作 —— 而幕布是直接 ::SetWindowRgn 设上去的，Qt 不知道，
 * 于是那块区域**永远留着**：窗口在屏幕上一直是"旧卡片那个形状"，
 * 用户看到的就是"最大化之后只有一块内容、四周露桌面"（白色背景）。
 *
 * 调用点都在"**已经把整窗画好之后**"（见 applyState 里那两步）——
 * 所以露出来的那一圈是画好的，不会有白边。
 */
void WindowHelper::dropCurtain()
{
    if (!m_curtainOn)
        return;
    m_curtainOn = false;
    m_curtain = QRegion();

#if defined(Q_OS_WIN)
    if (HWND hwnd = reinterpret_cast<HWND>(m_widget ? m_widget->internalWinId() : 0)) {
        ::SetWindowRgn(hwnd, nullptr, TRUE);   /* nullptr = 恢复成"没有区域"的矩形窗口 */
        trace(QStringLiteral("  幕布撤掉（SetWindowRgn(nullptr)）"));
    }
#endif

    m_maskApplied = false;      /* 让下面这次一定重新落到窗口上 */
    applyRoundedMask();
}

/*
 * 现在窗口在系统那边到底是什么形状（空 = 整块矩形）。
 *
 * 这一条是**地面真相**：幕布是不是真的撤掉了，日志里那几句"板"都是我们自己记的，
 * 只有这个数是系统答的。用户报的"最大化之后只有一块内容、四周露白"就是
 * "幕布没撤"，而这一点以前从日志里看不出来（Qt 那边以为没遮罩）。
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
            if (event->type() == QEvent::Resize || event->type() == QEvent::Move) {
                const QRect g = m_quickWidget->geometry();
                trace(QStringLiteral("%1  %2x%3@%4,%5")
                          .arg(event->type() == QEvent::Resize
                                   ? QStringLiteral("QQuickWidget Resize")
                                   : QStringLiteral("QQuickWidget Move"))
                          .arg(g.width()).arg(g.height()).arg(g.x()).arg(g.y()));
            } else if (event->type() == QEvent::Paint) {
                /*
                 * "界面重画了一次" —— 内容跟没跟上窗口，只有这一条说得准。
                 *
                 * 为什么不是 QQuickWindow::frameSwapped：这块界面是 QQuickWidget
                 * （走 QQuickRenderControl 渲染进 FBO），**没有换页那一下**，
                 * frameSwapped 一次都不发（实测：挂上去以后日志里一条都没有）。
                 * 重画则是真的会走到这里的。
                 *
                 * 只在切换后那一小段里记（见 traceFrames）：平时 QML 一动就是一串。
                 */
                if (m_traceFramesUntil >= 0 && m_traceClock.elapsed() <= m_traceFramesUntil)
                    trace(QStringLiteral("帧：界面重画（QQuickWidget paint）"));
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
        break;

    case QEvent::Paint: {
        /*
         * 主机窗口自己重画（整窗合成的时机，见 flushContent）。
         *
         * 换尺寸那一小段里把**绘制范围**也记下来：幕布挂着的时候这一笔必须覆盖
         * **整块窗口**（幕布只裁屏幕形状，不裁绘制）—— 要是只有卡片那点大，
         * 撤幕布时露出来的那几条就是没画过的像素（用户报的"三条白色间隙"）。
         */
        if (m_fastErase) {
            const QRect r = static_cast<QPaintEvent *>(event)->region().boundingRect();
            trace(QStringLiteral("  帧：主窗口 paint 范围 %1x%2@%3,%4（幕布%5）")
                      .arg(r.width()).arg(r.height()).arg(r.x()).arg(r.y())
                      .arg(m_curtainOn ? QStringLiteral("挂着") : QStringLiteral("已撤")));
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
