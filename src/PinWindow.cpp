#include "PinWindow.h"

#include "PinOcr.h"
#include "Screenshot.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QPolygonF>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickImageProvider>
#include <QQuickWidget>
#include <QRunnable>
#include <QScreen>
#include <QStandardPaths>
#include <QThreadPool>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWheelEvent>
#include <QWindow>
#include <cmath>

namespace {

/*
 * 贴图窗口的图一律按"一像素是一像素"处理。
 *
 * 底图是从合成图裁下来的，本来带着屏幕的 devicePixelRatio（2.0 那种）。带着它
 * 画 QPainter 会自己再乘一次，和 QML 那边（Image 铺满 + Canvas 按 zoom 缩放）
 * 就分家了。所以进贴图窗口时把 DPR 抹成 1：**贴图坐标 = 这里的像素坐标**，
 * 两边一套数。
 */
constexpr qreal kPinDpr = 1.0;

constexpr double kPi = 3.14159265358979323846;

/*
 * 一张图 -> png 的 data URL（"data:image/png;base64,…"）。
 *
 * 和选区识别（Screenshot::selectionImage）编出来的**必须是同一种东西** ——
 * 都是发给 LlmClient::recognize 的。太大就返回空串：base64 之后还要再涨三分之一，
 * 几十 MB 的请求体多半会被服务端直接掐掉，报出来还是一句看不懂的 HTTP 错，
 * 界面上回一句人话更省事。
 */
QString imageToDataUrl(const QImage &image) {
    if (image.isNull())
        return QString();
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return QString();
    buffer.close();
    constexpr int kMaxRawBytes = 8 * 1024 * 1024;
    if (png.size() > kMaxRawBytes)
        return QString();
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64());
}

/* 波浪线：沿 a->b 一段段摆过去，用二次曲线连起来（见 paintAnnotation 的说明） */
void paintWavy(QPainter &painter, const QPointF &a, const QPointF &b, qreal amp) {
    const QPointF d = b - a;
    const qreal length = std::hypot(d.x(), d.y());
    if (length < 1.0)
        return;
    const int steps = std::max(2, int(std::lround(length / 8.0)));
    const QPointF u(d.x() / length, d.y() / length);
    const QPointF n(-u.y(), u.x());
    QPainterPath path(a);
    for (int i = 1; i <= steps; ++i) {
        const qreal t = qreal(i) / steps;
        /* 一左一右交替，一个采样点一个波峰 / 波谷 */
        const qreal side = (i % 2 == 0) ? 1.0 : -1.0;
        const QPointF mid = a + d * (t - 0.5 / steps) + n * amp * side;
        const QPointF end = a + d * t + n * amp * side;
        path.quadTo(mid, end);
    }
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(path);
}

}  // namespace

/*
 * ---- 标注的画法（声明在 PinWindow.h 里，要给两个 .cpp 用） ----
 */

/*
 * 一条标注画进"图像坐标"的画布（形状 + 文字）。
 *
 * 这里只有这一份画法：贴图窗口的成品图（PinWindow::composedImage）用它，
 * 截图合成的成品图（Screenshot::compose）也用它 —— 两处都是"标注坐标已经是
 * 图像坐标"，所以不需要再传缩放比。
 *
 * QML 那边的**预览**是另一份实现（PinOverlay.qml 的 Canvas），两边的线宽、
 * 波浪振幅、字号、折行规则必须是同一套数，改这里就得同步改那边：
 *   荧光笔带宽 = max(3, stroke) ×2.4 且 35% 透明，
 *   直线 / 波浪线 / 删除线的线宽 = max(1.2, 0.6×stroke)（细一点，见下），
 *   波浪振幅 = min(4, 1.2×线宽)，波长 8，文字绕**框中心**转。
 */
/*
 * 直线 / 波浪线 / 删除线画多粗：**细一点**（0.6 倍，最细 1.2）。
 * 用户嫌 3px 太粗 —— 下划线、删除线那个位置本来就是一根细线，3px 加上圆头，
 * 看着像拿马克笔划的。荧光笔不在这里：它是"涂一层"，高矮 = stroke×2.4。
 */
qreal annotationLineWidth(qreal stroke) {
    return qMax(1.2, stroke * 0.6);
}

void paintAnnotation(QPainter &painter, const QVariantMap &item) {
    const QString kind = item.value(QStringLiteral("kind")).toString();
    const qreal stroke = qMax(1.0, item.value(QStringLiteral("stroke"), 3.0).toDouble());

    if (kind == QLatin1String("highlight")) {
        /*
         * 荧光笔：沿这条线涂一道粗的、半透明的带子（把线当成矩形的长轴）。
         * 它是"涂一层"，不是画一条线 —— 所以用填充 + 透明度。
         */
        const QPointF a(item.value(QStringLiteral("x1")).toDouble(),
                        item.value(QStringLiteral("y1")).toDouble());
        const QPointF b(item.value(QStringLiteral("x2")).toDouble(),
                        item.value(QStringLiteral("y2")).toDouble());
        const QPointF d = b - a;
        const qreal length = std::hypot(d.x(), d.y());
        if (length < 0.5)
            return;
        QColor color(item.value(QStringLiteral("color")).toString());
        if (!color.isValid())
            color = QColor(0xff, 0xd6, 0x0a);
        color.setAlphaF(0.35);
        painter.save();
        painter.translate(a);
        painter.rotate(std::atan2(d.y(), d.x()) * 180.0 / kPi);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRect(QRectF(0, -stroke * 1.2, length, stroke * 2.4));
        painter.restore();
        return;
    }

    if (kind == QLatin1String("wavy") || kind == QLatin1String("line")
        || kind == QLatin1String("strike")) {
        QColor color(item.value(QStringLiteral("color")).toString());
        if (!color.isValid())
            color = QColor(0xff, 0x3b, 0x30);
        QPen pen(color);
        pen.setWidthF(annotationLineWidth(stroke));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const QPointF a(item.value(QStringLiteral("x1")).toDouble(),
                        item.value(QStringLiteral("y1")).toDouble());
        const QPointF b(item.value(QStringLiteral("x2")).toDouble(),
                        item.value(QStringLiteral("y2")).toDouble());
        if (kind == QLatin1String("wavy"))
            paintWavy(painter, a, b,
                      std::min<qreal>(4.0, 1.2 * annotationLineWidth(stroke)));
        else
            painter.drawLine(a, b);
        return;
    }

    /*
     * 剩下的：箭头 / 铅笔 / 方框（截图工具条那边的）和文字。
     * 贴图窗口现在只用文字，但那三种也一起认 —— Screenshot::compose 要画它们。
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

        if (kind == QLatin1String("rect")) {
            /* 方框：只有描边（填了就把底下的内容挡住了） */
            const QPointF a(item.value(QStringLiteral("x1")).toDouble(),
                            item.value(QStringLiteral("y1")).toDouble());
            const QPointF b(item.value(QStringLiteral("x2")).toDouble(),
                            item.value(QStringLiteral("y2")).toDouble());
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(a, b).normalized());
            return;
        }
        if (kind == QLatin1String("arrow")) {
            const QPointF a(item.value(QStringLiteral("x1")).toDouble(),
                            item.value(QStringLiteral("y1")).toDouble());
            const QPointF b(item.value(QStringLiteral("x2")).toDouble(),
                            item.value(QStringLiteral("y2")).toDouble());
            painter.drawLine(a, b);
            /* 箭头头：张角 ±25.7°、头长 max(8px, 3×线宽)（和 QML 画布同一套数） */
            const double angle = std::atan2(b.y() - a.y(), b.x() - a.x());
            const double head = qMax(8.0, pen.widthF() * 3.0);
            QPolygonF head3;
            head3 << b
                  << b - QPointF(std::cos(angle - kPi / 7.0), std::sin(angle - kPi / 7.0)) * head
                  << b - QPointF(std::cos(angle + kPi / 7.0), std::sin(angle + kPi / 7.0)) * head;
            painter.drawPolygon(head3);
            return;
        }
        /* 铅笔：按点连成折线（点存在 [x,y,x,y,…] 的扁平数组里） */
        const QVariantList pts = item.value(QStringLiteral("pts")).toList();
        if (pts.size() >= 4) {
            QPolygonF poly;
            for (int i = 0; i + 1 < pts.size(); i += 2) {
                poly << QPointF(pts.at(i).toDouble(), pts.at(i + 1).toDouble());
            }
            painter.setBrush(Qt::NoBrush);
            painter.drawPolyline(poly);
        }
        return;
    }

    const QString text = item.value(QStringLiteral("text")).toString();
    if (text.isEmpty())
        return;

    QFont font = QApplication::font();
    font.setPixelSize(qBound(8, qRound(item.value(QStringLiteral("size"), 16).toDouble()), 400));
    painter.setFont(font);

    QColor color(item.value(QStringLiteral("color")).toString());
    if (!color.isValid())
        color = QColor(0xff, 0x3b, 0x30);
    painter.setPen(color);

    /*
     * 标注是个**能折行、能转的文本框**（见 PinOverlay.qml 的文字层），所以要把
     * QML 那边算好的框宽 / 框高接过来：
     *
     *   w   折行宽。QML 按字号和内容量出来的宽度，这边用同一个值配
     *       Qt::TextWordWrap，折行位置才和预览一致；没定过宽（w 就是内容宽）
     *       时也不会多折出一行。
     *   h   框高。旋转原点是**框中心**，两边 h 不一样就会转出两个位置 ——
     *       必须用 QML 那份（同一种 TextEdit 排版算出来的），不能自己估。
     *   rot 顺时针角度，和 QML 的 Rotation 同向（QPainter 的 y 向下，rotate 也顺时针）。
     */
    const qreal x = item.value(QStringLiteral("x")).toDouble();
    const qreal y = item.value(QStringLiteral("y")).toDouble();
    const qreal w = item.value(QStringLiteral("w")).toDouble();
    const qreal h = item.value(QStringLiteral("h")).toDouble();
    const qreal rot = item.value(QStringLiteral("rot")).toDouble();

    painter.save();
    painter.translate(x + w / 2.0, y + h / 2.0);
    if (!qFuzzyIsNull(rot))
        painter.rotate(rot);
    painter.translate(-w / 2.0, -h / 2.0);
    painter.drawText(QRectF(0, 0, w, h), Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, text);
    painter.restore();
}

namespace {

/*
 * 把"屏幕坐标的标注"平移成"选区图像的坐标"。
 *
 * 截图那条路的标注坐标是屏幕坐标（逻辑像素），画之前要减掉选区左上角；
 * 铅笔那串点也得逐个平移。贴图那条路不需要（标注本来就是图像坐标）。
 */
QVariantMap shiftedToImage(const QVariantMap &item, const QRectF &origin) {
    QVariantMap out = item;
    const QString kind = out.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("pencil")) {
        const QVariantList pts = out.value(QStringLiteral("pts")).toList();
        QVariantList moved;
        for (int i = 0; i + 1 < pts.size(); i += 2) {
            moved << (pts.at(i).toDouble() - origin.x())
                  << (pts.at(i + 1).toDouble() - origin.y());
        }
        out.insert(QStringLiteral("pts"), moved);
        return out;
    }
    out.insert(QStringLiteral("x1"), out.value(QStringLiteral("x1")).toDouble() - origin.x());
    out.insert(QStringLiteral("y1"), out.value(QStringLiteral("y1")).toDouble() - origin.y());
    out.insert(QStringLiteral("x2"), out.value(QStringLiteral("x2")).toDouble() - origin.x());
    out.insert(QStringLiteral("y2"), out.value(QStringLiteral("y2")).toDouble() - origin.y());
    if (kind == QLatin1String("text")) {
        out.insert(QStringLiteral("x"), out.value(QStringLiteral("x")).toDouble() - origin.x());
        out.insert(QStringLiteral("y"), out.value(QStringLiteral("y")).toDouble() - origin.y());
    }
    return out;
}

int g_nextPinId = 0;

}  // namespace

/* ===========================================================================
 * 贴图窗口
 * ======================================================================== */

PinWindow::PinWindow(const QImage &image, const QPoint &pos, QQmlEngine *engine,
                     Screenshot *owner)
    : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
      m_base(image),
      m_owner(owner) {
    /* 关掉就删：贴图是"用完即弃"的东西（见 Screenshot::m_pins 那份 QPointer） */
    setAttribute(Qt::WA_DeleteOnClose);
    /*
     * 窗口要**半透明**：现在贴图比底图大一圈，那圈留白（外投影 + 图外工具栏）
     * 在 QML 里是透明的，只有开了 WA_TranslucentBackground 才真透到桌面上、
     * 让投影看着像"浮起来"。不加这一句，透明区会被填成黑色 —— 那圈黑就是
     * 用户报的"黑色边框"（和便签 / 翻译卡片同一个套路，见 StickyNotes / Translate；
     * 注意是加在顶层 QWidget 上，加到 QQuickWidget 上整块会变黑，见 main.cpp 的说明）。
     */
    setAttribute(Qt::WA_TranslucentBackground);
    /*
     * 别把焦点从主窗口抢走：贴上去的图主要是看。但文字框要能打字 —— 用户点进
     * 文字框时 QQuickWidget 会自己把焦点拿过去，这一条只管"刚贴上去那一下别抢"。
     */
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowTitle(QStringLiteral("SmartClip 贴图"));

    /* 底图统一按 DPR = 1（见 kPinDpr 的说明） */
    m_base.setDevicePixelRatio(kPinDpr);
    m_picture = QSizeF(m_base.size());

    /*
     * 起始缩放：默认 100%（用户框多大就贴多大）；大到屏幕放不下时才缩到九成以内
     * —— 不然 4K 上截的一块大图贴出来会顶到屏幕外，连工具条都够不着。
     */
    qreal start = 1.0;
    const QScreen *screen = QGuiApplication::screenAt(pos);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (screen && m_picture.width() > 0 && m_picture.height() > 0) {
        const QRect work = screen->availableGeometry();
        if (m_picture.width() > work.width() * 0.9 || m_picture.height() > work.height() * 0.9) {
            start = qMin(work.width() * 0.9 / m_picture.width(),
                         work.height() * 0.9 / m_picture.height());
        }
    }
    m_zoom = qBound(0.1, start, 8.0);

    m_id = QStringLiteral("pin%1").arg(++g_nextPinId);

    /*
     * 界面用 QQuickWidget 装 —— 和主窗口 / 选区窗口一样挂**同一个引擎**
     * （见 setEngine 里 addImageProvider 那一段）：换一个引擎就 import 不到
     * SmartClip.Globals（Llm 那个单例），翻译那条路直接用不了。
     */
    m_view = new QQuickWidget(engine, this);
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_view->setClearColor(Qt::transparent);
    m_view->setInitialProperties({
        { QStringLiteral("pinWin"), QVariant::fromValue<QObject *>(this) },
        { QStringLiteral("imageId"), m_id },
        { QStringLiteral("viewWidth"), viewWidth() },
        { QStringLiteral("viewHeight"), viewHeight() },
        /* 外边距 / 工具栏条：和 shownSize 用的是同一组常量，QML 照着摆内容 */
        { QStringLiteral("padSide"), kPadSide },
        { QStringLiteral("padBottom"), kPadBottom },
        { QStringLiteral("barGap"), kBarGap },
        { QStringLiteral("barH"), kBarH },
    });
    m_view->setSource(QUrl(QStringLiteral("qrc:/qt/qml/SmartClip/qml/screenshot/PinOverlay.qml")));
    if (m_view->status() == QQuickWidget::Error) {
        for (const QQmlError &error : m_view->errors())
            qWarning("%s", qPrintable(error.toString()));
    }

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view, 1);

    resize(shownSize());
    move(pos);
}

PinWindow::~PinWindow() {
    if (m_owner)
        m_owner->forgetPin(this);
}

QQuickItem *PinWindow::qmlRoot() const {
    return m_view ? m_view->rootObject() : nullptr;
}

QImage PinWindow::imageForId(const QString &id) const {
    if (id.startsWith(m_id))
        return composedImage();
    return QImage();
}

QImage PinWindow::composedImage() const {
    QImage out = m_base;
    if (out.isNull())
        return out;
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (const QVariant &entry : m_annotations)
        paintAnnotation(painter, entry.toMap());
    painter.end();
    return out;
}

QString PinWindow::composedImageUrl() const { return imageToDataUrl(composedImage()); }

void PinWindow::annotationsChanged(const QVariantList &list) {
    m_annotations = list;
}

/* ===========================================================================
 * 图上选字：认一遍图上的字（Windows 自带 OCR，见 src/PinOcr.h）
 * ======================================================================== */

namespace {

/*
 * 认字那一步扔给线程池（一屏几十到几百毫秒，压在 GUI 线程上会卡手；
 * PP-OCR 那条路更慢 —— CPU 上检测一次就要一秒上下，还可能在下模型）。
 *
 * 认完不在这儿干等，而是把结果**投**回 GUI 线程（invokeMethod 排队就行）——
 * 窗口中途被关掉也不会踩空：QPointer 空了就什么都不做。
 */
class OcrTask final : public QRunnable {
public:
    OcrTask(const QPointer<PinWindow> &pin, const QImage &image,
            const QString &engine, const QString &command)
        : m_pin(pin), m_image(image), m_engine(engine), m_command(command) {
        setAutoDelete(true);
    }

    void run() override {
        QString error;
        QVariantList lines;
        if (m_engine == QLatin1String("ppocr"))
            lines = PinOcr::recognizeWithProgram(m_image, m_command, &error);
        else
            lines = PinOcr::recognize(m_image, &error);

        if (m_pin.isNull())
            return;
        const QPointer<PinWindow> pin = m_pin;
        QMetaObject::invokeMethod(pin, [pin, lines, error]() {
            if (pin)
                pin->applyOcrResult(lines, error);
        });
    }

private:
    QPointer<PinWindow> m_pin;
    QImage m_image;      /* QImage 是隐式共享，拷这一下不搬像素 */
    QString m_engine;
    QString m_command;
};

}  // namespace

bool PinWindow::ocrAvailable() const { return PinOcr::available(); }

QString PinWindow::ocrRunnerProblem(const QString &command) const {
    return PinOcr::runnerProblem(command);
}

void PinWindow::startOcr(const QString &engine, const QString &command) {
    if (m_ocrLocked || m_ocrBusy || m_base.isNull())
        return;
    const QString want = engine.trimmed().isEmpty() ? QStringLiteral("windows") : engine.trimmed();

    if (want == QLatin1String("ppocr")) {
        const QString problem = PinOcr::runnerProblem(command);
        if (!problem.isEmpty()) {
            m_ocrLines.clear();
            m_ocrMessage = problem;
            emit ocrChanged();
            return;
        }
    } else if (!PinOcr::available()) {
        m_ocrLines.clear();
        m_ocrMessage = QStringLiteral("这台机器上没有 Windows OCR 语言包，换个引擎试试");
        emit ocrChanged();
        return;
    }

    m_ocrBusy = true;
    m_ocrMessage.clear();
    emit ocrChanged();
    QThreadPool::globalInstance()->start(new OcrTask(this, m_base, want, command));
}

void PinWindow::applyOcrResult(const QVariantList &lines, const QString &error) {
    /* 自检摆进来的行说了算：先出去的那次真认字晚回来了，不许把假的盖掉 */
    if (m_ocrLocked)
        return;
    m_ocrBusy = false;
    m_ocrLines = lines;
    if (!error.isEmpty())
        m_ocrMessage = error;
    else
        m_ocrMessage = lines.isEmpty() ? QStringLiteral("图上没认出字") : QString();
    emit ocrChanged();
}

void PinWindow::setOcrLinesForTest(const QVariantList &lines) {
    m_ocrLocked = true;
    m_ocrBusy = false;
    m_ocrLines = lines;
    m_ocrMessage.clear();
    emit ocrChanged();
}

void PinWindow::copyResult() {
    const QImage image = composedImage();
    if (!image.isNull())
        QGuiApplication::clipboard()->setImage(image);
}

void PinWindow::copyTextToClipboard(const QString &text) {
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

void PinWindow::saveImageAs() {
    const QImage image = composedImage();
    if (image.isNull())
        return;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested =
        dir + QDir::separator()
        + QStringLiteral("贴图_%1.png")
              .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss")));
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("保存贴图"), suggested,
        QStringLiteral("PNG 图片 (*.png);;JPEG 图片 (*.jpg);;所有文件 (*.*)"));
    if (!path.isEmpty() && !image.save(path))
        QMessageBox::warning(this, QStringLiteral("贴图"),
                             QStringLiteral("写文件失败：\n") + path);
}

void PinWindow::zoomBy(qreal factor) { setZoom(m_zoom * factor); }

void PinWindow::closePin() { close(); }

/*
 * 拖动：**自己搬**，不请窗口管理器。
 *
 * 这里是踩了两回的坑，两回都是"看着在拖、窗口不动"：
 *
 *   1) 原来那份静态贴图的代码里只调 startSystemMove()，注释还写着"它不支持时
 *      退回自己搬" —— 退回那段其实没有，我重写时也没补；
 *   2) 补上"自己搬"之后，判据写成 startSystemMove() 的返回值。实测它在这个窗口
 *      上**返回 true 却什么都不做**（Qt::Tool + 无边框 + 置顶 + 150% 缩放），
 *      于是"退回自己搬"永远不触发，用户还是拖不动。
 *
 * 所以现在只有一条路：动作由 QML 一步步喂进来（dragMoveTo，用**逻辑像素**的
 * 全局坐标 —— 不能在这里读 QCursor::pos()，那个是设备像素，和 move() 差一个
 * 缩放比）。代价是没有了窗口管理器的贴边吸附；换来的是"拖动一定动"。
 */
void PinWindow::beginDrag() {
    m_manualDrag = true;
    m_dragAnchor = QPointF();
    m_dragOrigin = pos();
}

void PinWindow::dragMoveTo(qreal globalX, qreal globalY) {
    if (!m_manualDrag)
        return;
    const QPointF at(globalX, globalY);
    /* 第一步只记锚点：位移要从"按下那一点"算起，不然窗口会瞬间跳一下 */
    if (m_dragAnchor.isNull()) {
        m_dragAnchor = at;
        return;
    }
    const QPointF delta = at - m_dragAnchor;
    /* 位移是逻辑像素（QML 给的 mapToGlobal 就是），move() 要的也是 —— 同一套 */
    move(m_dragOrigin + QPoint(qRound(delta.x()), qRound(delta.y())));
}

void PinWindow::endDrag() {
    m_manualDrag = false;
    m_dragAnchor = QPointF();
}

void PinWindow::beginResize() {
    if (windowHandle())
        windowHandle()->startSystemResize(Qt::BottomEdge | Qt::RightEdge);
}

void PinWindow::setZoom(qreal value) {
    const qreal next = qBound(minZoom(), value, 8.0);
    if (qFuzzyCompare(next, m_zoom))
        return;
    m_zoom = next;
    m_settingZoom = true;      /* 见 resizeEvent：这一下 resize 不是用户拖出来的 */
    resize(shownSize());
    m_settingZoom = false;
    emit zoomChanged();
}

qreal PinWindow::minZoom() const {
    if (m_picture.width() <= 0 || m_minBarWidth <= 0)
        return 0.1;
    return qMax(0.1, m_minBarWidth / m_picture.width());
}

void PinWindow::setMinBarWidth(qreal width) {
    if (width <= 0 || qFuzzyCompare(width, m_minBarWidth))
        return;
    m_minBarWidth = width;
    /* 图片当前比工具栏还窄的话，就地抬到刚好等宽（否则那条下限就是摆设） */
    if (m_zoom < minZoom())
        setZoom(minZoom());
}

/*
 * 用户从右下角那个把手拖出来的 resize（startSystemResize）也要认。
 *
 * 不认的话窗口就被拉大了、里面的图还是按原比例铺（Image.Stretch），整块变形 ——
 * 所以这里把"窗口多大"反算成缩放：**图和窗口永远是同一个比例**，标注也就不会
 * 和底图错位。
 */
void PinWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_settingZoom)
        return;                 /* 是我们自己按缩放摆的窗口，别倒过来算一遍 */
    if (m_picture.width() <= 0 || m_picture.height() <= 0)
        return;
    /* 窗口比图片大一圈（外边距 + 底部工具栏条），反算缩放要先扣掉左右边距 */
    const qreal imgW = qreal(width()) - 2.0 * kPadSide;
    const qreal next = qBound(minZoom(), imgW / m_picture.width(), 8.0);
    const bool changed = !qFuzzyCompare(next, m_zoom);
    if (changed)
        m_zoom = next;
    /*
     * 缩放下限（图片最小宽 = 工具栏宽）可能把窗口顶得比"该多大"还小 —— 这时候
     * 光改 zoom 不够，窗口还停在用户拖到的那个小尺寸上（图就会溢出）。按新的 zoom
     * 反算回来，窗口不足 shownSize 就撑回去（m_settingZoom 挡住这一下的递归）。
     */
    const QSize want = shownSize();
    if (width() < want.width() || height() < want.height()) {
        m_settingZoom = true;
        resize(want);
        m_settingZoom = false;
    }
    if (changed)
        emit zoomChanged();
}

QSize PinWindow::shownSize() const {
    /* 底图 × 缩放，再套上左右上下的外边距（下边多一条放常驻工具栏） */
    return QSize(qMax(1, qRound(m_picture.width() * m_zoom + 2.0 * kPadSide)),
                 qMax(1, qRound(m_picture.height() * m_zoom + kPadSide + kPadBottom)));
}

void PinWindow::wheelEvent(QWheelEvent *event) {
    /* 滚轮缩放（和原来那个静态贴图一个手感）；按住 Ctrl 时步长细一点 */
    const qreal step = (event->modifiers() & Qt::ControlModifier) ? 1.02 : 1.1;
    setZoom(m_zoom * (event->angleDelta().y() > 0 ? step : 1.0 / step));
    event->accept();
}

void PinWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape)
        close();
    else
        QWidget::keyPressEvent(event);
}

/* ===========================================================================
 * image://pin/… 的取图口
 * ======================================================================== */

/*
 * QML 那边的底图（PinOverlay.qml 里那个 Image）走这个口。
 *
 * 和截图那个 ShotImageProvider 同一个套路：提供者是**引擎**持有的，没法在这里
 * 挂一份"窗口清单"，所以拿着 id 去顶层窗口里找那块贴图。贴图用完即弃、关掉就
 * 删，找不到（窗口已经没了）就返回空图 —— QML 那边也就跟着没了。
 */
class PinImageProvider final : public QQuickImageProvider {
public:
    PinImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requested) override {
        Q_UNUSED(requested);
        QImage image;
        const QList<QWidget *> widgets = QApplication::topLevelWidgets();
        for (QWidget *widget : widgets) {
            auto *pin = qobject_cast<PinWindow *>(widget);
            if (!pin)
                continue;
            image = pin->imageForId(id);
            if (!image.isNull())
                break;
        }
        if (size)
            *size = image.size();
        return image;
    }
};

/*
 * 把提供者挂到引擎上（Screenshot::setEngine 调）。
 *
 * 单独一个函数是为了让提供者的定义留在本文件里 —— 引擎那边只要知道"有这么个
 * 东西可以挂"，不需要认识这个类。
 */
QQuickImageProvider *renderPin() { return new PinImageProvider(); }

/*
 * 把一批"屏幕坐标的标注"画到图像坐标的画布上（Screenshot::compose 用）。
 *
 * 和贴图窗口共用同一份画法（paintAnnotation），这里只多一步坐标平移 ——
 * 两处各写一份的话，迟早会出现"贴图上划的重点和截图里划的长得不一样"。
 */
void paintAnnotations(QPainter &painter, const QVariantList &items, const QRectF &origin) {
    for (const QVariant &entry : items)
        paintAnnotation(painter, shiftedToImage(entry.toMap(), origin));
}
