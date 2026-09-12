#include "WindowHelper.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPainterPath>
#include <QRegion>
#include <QScreen>
#include <QWidget>
#include <QWindow>
#include <QPainterPath>
#include <QRegion>
#include <QScreen>

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

void WindowHelper::closeWindow()
{
    if (m_widget)
        m_widget->close();
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
