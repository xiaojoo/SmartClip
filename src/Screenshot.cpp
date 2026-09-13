#include "Screenshot.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QQmlError>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScreen>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <cmath>

namespace {

/*
 * 固定到桌面的那个小窗（"贴图"）。
 *
 * 就是一块画着图的置顶无边框窗口：拖动移动、滚轮缩放、双击 / Esc 关掉、
 * 右键出菜单（复制 / 另存为 / 原始大小 / 关闭）。
 *
 * 用 QWidget 而不是 QML 的 Window：置顶、无边框、不进任务栏（Qt::Tool）
 * 都是窗口标志位的事，C++ 这边一行一个；而且它要长期挂在桌面上，
 * 没必要为了一张静态图再养一个 QML 窗口和一份引擎上下文。
 *
 * 注意这里没有 Q_OBJECT：它不发信号、不需要 qobject_cast，
 * 认它靠的是 Screenshot::m_pins 这份清单。
 */
class PinWindow final : public QWidget {
public:
    PinWindow(const QImage &image, const QPoint &pos)
        : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
          m_image(image) {
        /* 关掉就删：贴图窗口是"用完即弃"的东西，留一堆隐藏窗口没意义 */
        setAttribute(Qt::WA_DeleteOnClose);
        /* 别把焦点从主窗口抢走：贴上去的图只是看，不参与打字 */
        setAttribute(Qt::WA_ShowWithoutActivating);
        setCursor(Qt::OpenHandCursor);
        setWindowTitle(QStringLiteral("SmartClip 贴图"));

        /*
         * 图的尺寸是设备像素，窗口尺寸要的是逻辑像素 ——
         * 高 DPI 下直接拿 image.size() 会大一倍。
         */
        const qreal dpr = m_image.devicePixelRatio() > 0 ? m_image.devicePixelRatio() : 1.0;
        m_base = QSizeF(m_image.size()) / dpr;
        resize(m_base.toSize());
        move(pos);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        /*
         * 铺满整个窗口：窗口按逻辑尺寸，图是设备尺寸，比例正好是 DPR，
         * 缩放之后一个图像像素对一个物理像素（不糊也不失真）。
         */
        painter.drawImage(rect(), m_image);
        painter.setPen(QColor(0x4b, 0x4d, 0x4f));
        painter.drawRect(rect().adjusted(0, 0, -1, -1));
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::RightButton) {
            showMenu(event->globalPosition().toPoint());
            event->accept();
            return;
        }
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }

        /*
         * 拖动交给窗口管理器（和主窗口顶栏拖动同一个理由：贴边吸附、
         * 多屏 DPI 切换都归它管）。它不支持时退回自己搬。
         */
        m_grabOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
        m_manualDrag = !windowHandle() || !windowHandle()->startSystemMove();
        event->accept();
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (m_manualDrag && (event->buttons() & Qt::LeftButton))
            move(event->globalPosition().toPoint() - m_grabOffset);
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        m_manualDrag = false;
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton)
            close();
    }

    void wheelEvent(QWheelEvent *event) override {
        setZoom(m_scale * (event->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1));
        event->accept();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape)
            close();
        else
            QWidget::keyPressEvent(event);
    }

private:
    void setZoom(qreal scale) {
        m_scale = qBound(0.1, scale, 8.0);
        resize((m_base * m_scale).toSize());
    }

    void showMenu(const QPoint &global) {
        QMenu menu(this);
        QAction *copyAct = menu.addAction(QStringLiteral("复制到剪贴板"));
        QAction *saveAct = menu.addAction(QStringLiteral("另存为…"));
        menu.addSeparator();
        QAction *resetAct = menu.addAction(QStringLiteral("原始大小"));
        menu.addSeparator();
        QAction *closeAct = menu.addAction(QStringLiteral("关闭贴图"));

        QAction *picked = menu.exec(global);
        if (picked == copyAct) {
            QGuiApplication::clipboard()->setImage(m_image);
        } else if (picked == saveAct) {
            saveAs();
        } else if (picked == resetAct) {
            setZoom(1.0);
        } else if (picked == closeAct) {
            close();
        }
    }

    void saveAs() {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
        const QString suggested =
            dir + QDir::separator()
            + QStringLiteral("贴图_%1.png")
                  .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
        const QString path = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存截图"), suggested,
            QStringLiteral("PNG 图片 (*.png);;JPEG 图片 (*.jpg);;所有文件 (*.*)"));
        if (!path.isEmpty() && !m_image.save(path))
            QMessageBox::warning(this, QStringLiteral("截图"),
                                 QStringLiteral("写文件失败：\n") + path);
    }

    QImage m_image;
    QSizeF m_base;
    QPoint m_grabOffset;
    qreal m_scale = 1.0;
    bool m_manualDrag = false;
};

/*
 * image://shot/… 的取图口。
 *
 * QML 那边只有一处用它：选区窗口铺在底下的那张冻结整屏图
 * （qml/screenshot/CaptureOverlay.qml 的 shotImage）。
 *
 * 拿 QPointer 而不是裸指针：这个提供者是引擎持有的（addImageProvider 接管
 * 所有权），引擎比 Screenshot 活得久一点点，理论上存在"图还在、主人没了"
 * 的窗口期，QPointer 自己会变空。
 */
class ShotImageProvider final : public QQuickImageProvider {
public:
    explicit ShotImageProvider(Screenshot *shot)
        : QQuickImageProvider(QQuickImageProvider::Image), m_shot(shot) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override {
        Q_UNUSED(requested);
        const QImage image = m_shot ? m_shot->imageForId(id) : QImage();
        if (size)
            *size = image.size();
        return image;
    }

private:
    QPointer<Screenshot> m_shot;
};

}  // namespace

Screenshot::Screenshot(QObject *parent) : QObject(parent) {}

void Screenshot::setHostWidget(QWidget *host) { m_host = host; }

void Screenshot::setEngine(QQmlEngine *engine) {
    m_engine = engine;
    if (m_engine)
        m_engine->addImageProvider(QStringLiteral("shot"), new ShotImageProvider(this));
}

void Screenshot::beginCapture() {
    if (m_active || m_pending) {
        /* 已经开着（用户又按了一次快捷键）：把选区窗口提到前面就够了 */
        if (m_overlay) {
            m_overlay->raise();
            m_overlay->activateWindow();
        }
        return;
    }
    if (!m_engine) {
        qWarning("截图：还没拿到 QML 引擎，建不出选区窗口");
        return;
    }

    /*
     * 截鼠标所在的那块屏。
     *
     * 不拼"整个虚拟桌面"：多屏混 DPI 时把几块屏拼成一张图，坐标换算会
     * 变成另一个坑（每块屏的 devicePixelRatio 还不一样）。绝大多数截图
     * 就是截当前正在看的那块屏，选它既简单又不会错。
     */
    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;

    /*
     * 主窗口正好压在这块屏上时先把它藏起来。
     *
     * 从菜单 / 左栏图标触发截图时，窗口本来就盖在屏上，不藏的话第一张
     * 图里必有 SmartClip 自己（用户要截的是别人的界面）。藏起来之后
     * 收尾（取消 / 复制 / 保存 / 贴图）再放回来。
     *
     * 只在**就是这块屏**时藏：窗口在另一块屏上时不用动它，
     * 用户多半就是在截那块屏上的别的程序。
     */
    m_hiddenHost = m_host && m_host->isVisible() && m_host->screen() == screen;
    if (m_hiddenHost)
        m_host->hide();

    m_screen = screen;
    m_pending = true;

    /*
     * 藏窗口到桌面真的重画完，中间隔着一次合成。
     *
     * 不延时直接抓的话，抓到的还是"窗口还在上面"的那一帧（实测：菜单刚关
     * 掉、窗口刚 hide 掉时最容易撞上）。150ms 是肉眼看不出来的停顿，
     * 换一张干净的画面很值。
     */
    QTimer::singleShot(m_hiddenHost ? 150 : 0, this, &Screenshot::grabAndShow);
}

void Screenshot::grabAndShow() {
    m_pending = false;

    QScreen *screen = m_screen;
    m_screen = nullptr;
    if (!screen) {
        restoreHost();
        return;
    }

    const QPixmap shot = screen->grabWindow(0);
    if (shot.isNull()) {
        restoreHost();
        QMessageBox::warning(m_host, QStringLiteral("截图"),
                             QStringLiteral("抓屏失败：系统没有给出画面。"));
        return;
    }

    m_shot = shot.toImage();
    m_dpr = shot.devicePixelRatio() > 0 ? shot.devicePixelRatio() : 1.0;
    m_screenRect = screen->geometry();
    ++m_serial;
    m_active = true;
    emit stateChanged();

    showOverlay();
}

void Screenshot::showOverlay() {
    /*
     * 选区窗口：铺满被截的那块屏，无边框 + 置顶 + 拿焦点（Esc / 输入都要）。
     *
     * 内容用 QQuickWidget 装 —— 和主窗口一样，挂的是**同一个引擎**
     * （见 setEngine 的说明），所以 QML 里直接用 Shot 这个单例。
     */
    auto *overlay = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint
                                             | Qt::WindowStaysOnTopHint);
    overlay->setAttribute(Qt::WA_DeleteOnClose);
    overlay->setWindowTitle(QStringLiteral("SmartClip 截图"));
    overlay->setGeometry(m_screenRect);

    auto *view = new QQuickWidget(m_engine, overlay);
    view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    /* 底图自己铺满，这里的清屏色只在图还没解码出来时露一下 */
    view->setClearColor(Qt::black);

    auto *layout = new QVBoxLayout(overlay);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(view);

    view->setSource(QUrl(QStringLiteral("qrc:/qt/qml/SmartClip/qml/screenshot/CaptureOverlay.qml")));
    if (view->status() == QQuickWidget::Error) {
        for (const QQmlError &error : view->errors())
            qWarning("%s", qPrintable(error.toString()));
        overlay->close();
        m_active = false;
        m_shot = QImage();
        restoreHost();
        emit stateChanged();
        return;
    }

    m_overlay = overlay;
    /*
     * 以 this 作为上下文接：Screenshot 先没掉时这条连接自动断，
     * 免得进程退出时窗口被拆还回头调已经析构的对象。
     */
    connect(overlay, &QObject::destroyed, this, [this]() {
        m_overlay = nullptr;
        if (m_active) {
            m_active = false;
            m_shot = QImage();
            restoreHost();
            emit stateChanged();
        }
    });

    overlay->show();
    overlay->raise();
    overlay->activateWindow();
    view->setFocus();
}

void Screenshot::endCapture() {
    /*
     * 取色框 / 保存框开着的时候**不许**关选区窗口。
     *
     * 那两个用的都是模态对话框里的嵌套事件循环，而窗口级快捷键照样能漏进来
     * （实测：取色框开着按 Esc，选区窗口的 Esc Shortcut 先接住了）——
     * 一关就把对话框的父窗口拆了，可对话框还在 exec() 里，于是直接 abort，
     * 弹出"Microsoft Visual C++ Runtime Library"错误框。等对话框自己收尾
     * （用户确认或取消）之后才允许关 —— QML 那侧同时也把快捷键让开。
     */
    if (m_modalOpen)
        return;

    if (m_overlay) {
        QWidget *overlay = m_overlay;
        /* 先断开引用：close() 之后它随时会被删，别的地方不要再伸手 */
        m_overlay = nullptr;
        overlay->close();
    }
    m_active = false;
    m_shot = QImage();
    restoreHost();
    emit stateChanged();
}

void Screenshot::restoreHost() {
    if (!m_hiddenHost)
        return;
    m_hiddenHost = false;
    if (m_host)
        m_host->show();
}

QImage Screenshot::compose(const QRectF &sel, const QVariantList &texts) const {
    if (m_shot.isNull())
        return QImage();

    /* 选区夹回屏幕范围：QML 那边按理不会越界，夹一下免得算出负宽高 */
    const QRectF bounds(QPointF(0, 0), QSizeF(m_screenRect.size()));
    const QRectF clipped = sel.normalized().intersected(bounds);
    if (clipped.width() < 1.0 || clipped.height() < 1.0)
        return QImage();

    /* 屏幕坐标（逻辑）-> 图像坐标（设备像素） */
    QRect device(qRound(clipped.x() * m_dpr), qRound(clipped.y() * m_dpr),
                 qRound(clipped.width() * m_dpr), qRound(clipped.height() * m_dpr));
    device = device.intersected(m_shot.rect());
    if (device.isEmpty())
        return QImage();

    QImage out = m_shot.copy(device);
    /*
     * 画之前把 DPR 抹成 1。
     *
     * 下面每个坐标和字号都自己乘过 dpr 了；QPainter 打在 QImage 上到底会不会
     * 再按 DPR 缩一次，各版本行为不一致（赌错了就是"字和位置都大一倍"），
     * 索性在这里把话说死：合成期间这张图就是"一像素是一像素"的。
     */
    out.setDevicePixelRatio(1.0);

    QPainter painter(&out);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    for (const QVariant &entry : texts) {
        const QVariantMap item = entry.toMap();
        const QString kind = item.value(QStringLiteral("kind")).toString();
        const qreal stroke = qMax(1.0, item.value(QStringLiteral("stroke"), 3.0).toDouble()) * m_dpr;

        /*
         * 箭头 / 铅笔：两种都用"屏幕坐标 -> 裁剪切块坐标"这同一套换算。
         * 线宽、箭头头部尺寸都乘 dpr（和字号一样，高 DPI 下才不会细成一条线）。
         */
        if (kind == QLatin1String("arrow") || kind == QLatin1String("pencil")
            || kind == QLatin1String("rect")) {
            QColor color(item.value(QStringLiteral("color")).toString());
            if (!color.isValid())
                color = QColor(0xff, 0x3b, 0x30);
            QPen pen(color);
            pen.setWidthF(stroke);
            pen.setCapStyle(Qt::RoundCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.setBrush(color);

            auto toLocal = [&](double sx, double sy) {
                return QPointF((sx - clipped.x()) * m_dpr, (sy - clipped.y()) * m_dpr);
            };

            if (kind == QLatin1String("rect")) {
                /*
                 * 方框：只有描边（不填充 —— 填了就把底下的内容挡住了）。
                 * 两个角点可能哪个大哪个小，normalized() 抹平。
                 */
                const QPointF a = toLocal(item.value(QStringLiteral("x1")).toDouble(),
                                          item.value(QStringLiteral("y1")).toDouble());
                const QPointF b = toLocal(item.value(QStringLiteral("x2")).toDouble(),
                                          item.value(QStringLiteral("y2")).toDouble());
                painter.setBrush(Qt::NoBrush);
                painter.drawRect(QRectF(a, b).normalized());
            } else if (kind == QLatin1String("arrow")) {
                const QPointF a = toLocal(item.value(QStringLiteral("x1")).toDouble(),
                                          item.value(QStringLiteral("y1")).toDouble());
                const QPointF b = toLocal(item.value(QStringLiteral("x2")).toDouble(),
                                          item.value(QStringLiteral("y2")).toDouble());
                painter.drawLine(a, b);

                /*
                 * 箭头头：和 QML 画布那边同一套数（张角 ±25.7°、头长
                 * max(8px, 3×线宽)），两边才长得一样。
                 */
                constexpr double kPi = 3.14159265358979323846;
                const double angle = std::atan2(b.y() - a.y(), b.x() - a.x());
                const double head = qMax(8.0 * m_dpr, pen.widthF() * 3.0);
                QPolygonF head3;
                head3 << b
                      << b - QPointF(std::cos(angle - kPi / 7.0), std::sin(angle - kPi / 7.0)) * head
                      << b - QPointF(std::cos(angle + kPi / 7.0), std::sin(angle + kPi / 7.0)) * head;
                painter.drawPolygon(head3);
            } else {
                /* 铅笔：按点连成折线（点存在 [x,y,x,y,…] 的扁平数组里） */
                const QVariantList pts = item.value(QStringLiteral("pts")).toList();
                if (pts.size() >= 4) {
                    QPolygonF poly;
                    for (int i = 0; i + 1 < pts.size(); i += 2)
                        poly << toLocal(pts.at(i).toDouble(), pts.at(i + 1).toDouble());
                    painter.drawPolyline(poly);
                }
            }
            continue;
        }

        const QString text = item.value(QStringLiteral("text")).toString();
        if (text.isEmpty())
            continue;

        QFont font = QApplication::font();
        font.setPixelSize(
            qBound(8, qRound(item.value(QStringLiteral("size"), 16).toDouble() * m_dpr), 400));
        painter.setFont(font);

        QColor color(item.value(QStringLiteral("color")).toString());
        if (!color.isValid())
            color = QColor(0xff, 0x3b, 0x30);
        painter.setPen(color);

        /*
         * 标注是个**能折行、能转的文本框**（见 CaptureOverlay.qml），所以这里
         * 要把 QML 那边算好的框宽 / 框高接过来：
         *
         *   w   折行宽。QML 按字号和内容量出来的宽度，这边用同一个值配
         *       Qt::TextWordWrap，折行位置才和预览一致；没定过宽（w 就是
         *       内容宽）时也不会多折出一行。
         *   h   框高。旋转原点是**框中心**，两边 h 不一样就会转出两个位置 ——
         *       所以必须用 QML 那份（同一种 TextEdit 排版算出来的），
         *       不能在这边用 QFontMetrics 自己估。
         *   rot 顺时针角度，和 QML 的 Rotation 同向（QPainter 默认坐标 y 向下，
         *       rotate() 也是顺时针）。
         */
        const qreal x = (item.value(QStringLiteral("x")).toDouble() - clipped.x()) * m_dpr;
        const qreal y = (item.value(QStringLiteral("y")).toDouble() - clipped.y()) * m_dpr;
        const qreal w = item.value(QStringLiteral("w")).toDouble() * m_dpr;
        const qreal h = item.value(QStringLiteral("h")).toDouble() * m_dpr;
        const qreal rot = item.value(QStringLiteral("rot")).toDouble();

        painter.save();
        painter.translate(x + w / 2.0, y + h / 2.0);
        if (!qFuzzyIsNull(rot))
            painter.rotate(rot);
        painter.translate(-w / 2.0, -h / 2.0);
        painter.drawText(QRectF(0, 0, w, h),
                         Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, text);
        painter.restore();
    }
    painter.end();

    /* 交出去（剪贴板 / 保存 / 贴图）时按逻辑尺寸算 */
    out.setDevicePixelRatio(m_dpr);
    return out;
}

bool Screenshot::copyResult(const QRectF &sel, const QVariantList &texts) const {
    const QImage image = compose(sel, texts);
    if (image.isNull())
        return false;
    QGuiApplication::clipboard()->setImage(image);
    return true;
}

bool Screenshot::saveResult(const QString &path, const QRectF &sel,
                            const QVariantList &texts) const {
    if (path.isEmpty())
        return false;
    const QImage image = compose(sel, texts);
    if (image.isNull())
        return false;
    return image.save(path);
}

QString Screenshot::defaultFileName() {
    return QStringLiteral("截图_%1.png")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
}

bool Screenshot::saveResultAs(const QRectF &sel, const QVariantList &texts) {
    const QImage image = compose(sel, texts);
    if (image.isNull())
        return false;

    QString dir = m_saveDir;
    if (dir.isEmpty() || !QDir(dir).exists())
        dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);

    /* 同样是嵌套事件循环，期间不许关选区窗口 —— 见 endCapture 的说明 */
    m_modalOpen = true;
    const QString path = QFileDialog::getSaveFileName(
        m_overlay ? m_overlay : m_host, QStringLiteral("保存截图"),
        dir + QDir::separator() + defaultFileName(),
        QStringLiteral("PNG 图片 (*.png);;JPEG 图片 (*.jpg);;所有文件 (*.*)"));
    m_modalOpen = false;
    if (path.isEmpty())
        return false;

    if (!image.save(path)) {
        QMessageBox::warning(m_overlay ? m_overlay : m_host, QStringLiteral("截图"),
                             QStringLiteral("写文件失败：\n") + path);
        return false;
    }
    m_saveDir = QFileInfo(path).absolutePath();
    return true;
}

QString Screenshot::pickColor(const QString &current) {
    /*
     * "更多颜色 -> 自定义…"：开系统取色框。
     *
     * 父窗口用选区窗口：它铺满整屏又是置顶的，取色框挂在它上面才不会被盖住
     * （和保存对话框同一个道理，实测那边是好的）。取消返回空串，QML 那边
     * 保持原色不动。
     */
    QColor start(current);
    if (!start.isValid())
        start = QColor(0xff, 0x3b, 0x30);

    /* 嵌套事件循环期间不许关选区窗口，见 endCapture 的说明 */
    m_modalOpen = true;
    const QColor picked = QColorDialog::getColor(start, m_overlay ? m_overlay : m_host,
                                                 QStringLiteral("选择标注颜色"));
    m_modalOpen = false;
    return picked.isValid() ? picked.name() : QString();
}

void Screenshot::pinResult(const QRectF &sel, const QVariantList &texts) {
    const QImage image = compose(sel, texts);
    if (image.isNull())
        return;

    /*
     * 就钉在选区原来的位置上（截图时选的是哪儿，贴出来就在哪儿），
     * 这样"固定桌面"看起来像是把刚框住的那块画面留在了桌面上。
     */
    const QPoint pos(m_screenRect.topLeft() + QPoint(qRound(sel.x()), qRound(sel.y())));

    auto *pin = new PinWindow(image, pos);
    m_pins.append(pin);
    pin->show();
}

int Screenshot::pinnedCount() const {
    int count = 0;
    for (const QPointer<QWidget> &pin : m_pins)
        if (!pin.isNull())
            ++count;
    return count;
}

void Screenshot::closeAllPins() {
    for (const QPointer<QWidget> &pin : m_pins)
        if (!pin.isNull())
            pin->close();
    m_pins.clear();
}

QObject *Screenshot::overlayRoot() const {
    if (!m_overlay)
        return nullptr;
    auto *view = m_overlay->findChild<QQuickWidget *>();
    return view ? view->rootObject() : nullptr;
}

QImage Screenshot::imageForId(const QString &id) const {
    /* "full<serial>"：序号只是用来绕开 QQuickPixmapCache 的，取图时忽略 */
    if (id.startsWith(QLatin1String("full")))
        return m_shot;
    return QImage();
}
