#include "WindowHelper.h"

#include <QCoreApplication>
#include <QTimer>

#include <QEvent>
#include <QGuiApplication>
#include <QPainterPath>
#include <QRegion>
#include <QScreen>
#include <QWidget>
#include <QWindow>

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
    return wh->startSystemMove();
}

void WindowHelper::refreshMask()
{
    applyRoundedMask();
}

void WindowHelper::applyRoundedMask()
{
    if (!m_widget)
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

    /* 最大化时给直角：这就是"最大化后左右没有圆角"的正解 */
    const int r = m_maximized ? 0
                              : qRound(qMin(m_cornerRadius, qMin(w / 2, h / 2)) * dpr);

    if (r <= 0) {
        m_widget->clearMask();
        return;
    }

    QPainterPath path;
    path.addRoundedRect(QRectF(0, 0, w, h), r, r);
    const QRegion reg(path.toFillPolygon().toPolygon());
    m_widget->setMask(reg);
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

void WindowHelper::attachWidget(QWidget *widget)
{
    if (m_widget == widget)
        return;

    if (m_widget)
        m_widget->removeEventFilter(this);

    m_widget = widget;
    if (m_widget) {
        m_widget->installEventFilter(this);
        cacheNormalGeometry(m_widget->geometry());
        updateMaximizedFromWindow();
        applyRoundedMask();
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

    if (updateMaximizedFromWindow() || m_maximized)
        return;   // 已经被系统最大化了（比如拖到屏幕顶端吸附）

    /*
     * 先抓住"还原矩形"：取窗口当下的几何。只有常规状态才会走到这里
     * （上面已排除"系统已最大化"），所以这就是用户最后摆出来的那个
     * 位置和尺寸，点还原时原样回去。
     */
    m_restoreAnchor = m_normalRect.isValid() && m_normalRect.width() > 0
                          ? m_normalRect
                          : m_widget->geometry();

    applyState(true);
}

void WindowHelper::restore()
{
    if (!m_widget)
        return;

    if (!m_maximized && !updateMaximizedFromWindow())
        return;   // 本来就没最大化

    applyState(false);
}

/*
 * 一次到位地把窗口切到目标状态，然后通知界面淡入。
 *
 * 这里刻意不插任何中间帧：几何只改一次，所以整棵界面也只需要重排重绘
 * 一次 —— 这就是"内容三千行也不卡"的原因。淡入由 QML 侧做，
 * 纯粹是拿它盖住这次跳变，不参与布局。
 */
void WindowHelper::applyState(bool maximize)
{
    if (!m_widget)
        return;

    if (maximize) {
        m_widget->showMaximized();

        /*
         * 无边框窗口在部分平台上 showMaximized() 回来时几何还没落定，
         * 补一次到屏幕可用区域，保证和任务栏不重叠。
         */
        if (QScreen *screen = screenOf()) {
            const QRect avail = screen->availableGeometry();
            if (m_widget->geometry() != avail)
                m_widget->setGeometry(avail);
        }

        m_normalRect = m_restoreAnchor;
    } else {
        m_widget->showNormal();

        const QRect to = clampToScreen(m_restoreAnchor.isValid() ? m_restoreAnchor
                                                                 : m_normalRect);
        if (to.isValid() && to.width() > 0 && to.height() > 0) {
            m_widget->setGeometry(to);
            m_normalRect = to;
        }
    }

    if (m_maximized != maximize) {
        m_maximized = maximize;
        emit maximizedChanged();
    }
    /* 最大化/还原会改变要不要圆角 */
    applyRoundedMask();

    notifyTransition();
    updateMaximizedFromWindow();
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
    if (watched != m_widget || !m_widget)
        return QObject::eventFilter(watched, event);

    switch (event->type()) {
    case QEvent::Move:
    case QEvent::Resize:
        // 只在"常规状态"下记还原矩形；最大化时的整屏几何不是还原目标
        if (!m_maximized)
            cacheNormalGeometry(m_widget->geometry());
        /* 尺寸变了要按新尺寸重算圆角遮罩 */
        applyRoundedMask();
        break;

    case QEvent::WindowStateChange:
        // 用户拖到屏幕顶端吸附最大化 / 从最大化拖出来：状态跟着窗口走
        updateMaximizedFromWindow();
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}
