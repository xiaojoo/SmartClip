#pragma once

#include <QImage>
#include <QSizeF>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QWidget>

class QQmlEngine;
class QPainter;
class QQuickImageProvider;
class QQuickItem;
class QQuickWidget;
class Screenshot;

/*
 * ---- 标注的画法（形状 + 文字）----
 *
 * 全项目就这一份实现。两个地方要用它，画出来的东西必须是同一个样子：
 *   * 贴图窗口的成品图（PinWindow::composedImage）；
 *   * 截图合成的成品图（Screenshot::compose）。
 *
 * item / items 里每一项就是 QML 那边传过来的一条标注（字段见
 * qml/screenshot/CaptureOverlay.qml 的 annotationsData），坐标必须是
 * **图像坐标** —— 截图那条路传的是屏幕坐标，所以给了 paintAnnotations，
 * 由 origin（选区左上角）负责平移。
 *
 * QML 的预览（Canvas）是另一份实现，改这里的数值要同步改 qml/screenshot/
 * PinOverlay.qml 里 canvas 上面那段（线宽 / 振幅 / 折行规则）。
 */
void paintAnnotation(QPainter &painter, const QVariantMap &item);
/* origin 是这批标注坐标系的左上角（截图那条路是选区左上角，贴图那条路是 0,0） */
void paintAnnotations(QPainter &painter, const QVariantList &items, const QRectF &origin);
/*
 * 挂 image://pin 的取图口（Screenshot::setEngine 调）。
 * 单独开一个口是为了让提供者那个类留在 PinWindow.cpp 里，外面不用认识它。
 */
QQuickImageProvider *renderPin();

/*
 * 固定到桌面的那块图（"贴图"）。
 *
 * 用户要的是**贴上去之后还能改**：能划重点（荧光笔 / 波浪线 / 直线 / 删除线）、
 * 能写字并且写完还能选中改、能把上面的字认出来翻译。所以它不再是"一张静态图 +
 * 右键菜单"，而是一个 **QQuickWidget + qml/screenshot/PinOverlay.qml** 的置顶
 * 无边框窗口（和截图选区窗口同一个套路）。
 *
 * 分工：
 *   * 窗口标志位（置顶 / 无边框 / 不进任务栏）、缩放、存盘、剪贴板在 C++；
 *   * 标注的**预览**在 QML（Canvas 画形状 + TextEdit 写文字、选字、改字）；
 *   * 标注的**合成**在这里（composedImage）：复制 / 保存 / 交给模型识别用的
 *     都是它。两边共用同一套画法（见 PinWindow.cpp 里的 paintAnnotation），
 *     不然会出现"预览里好好的，存出来不一样"。
 *
 * 底图是贴图那一刻的成品图，之后加的都只是"标注"——所以不会越画越糊，
 * 撤销也是真撤销。标注坐标是**底图的像素坐标**（DPR 抹成 1 之后两者同一套），
 * 和窗口逻辑尺寸之间只差一个 zoom。
 *
 * 为什么不写成 QML 的 Window：置顶 / 无边框 / 不进任务栏都是窗口标志位的事，
 * 拖动和多屏 DPI 归窗口管理器管（startSystemMove），这些在 QWidget 上一行一个，
 * 也是这个项目里便签窗口 / 翻译卡片一直用的做法。
 */
class PinWindow final : public QWidget {
    Q_OBJECT

    /* 底图在 image://pin/ 下的 id（QML 那边拼 URL 用） */
    Q_PROPERTY(QString imageId READ imageId CONSTANT)
    /* 逻辑显示尺寸（QML 拿它当"画布"尺寸，也是标注换算的基准） */
    Q_PROPERTY(qreal viewWidth READ viewWidth NOTIFY zoomChanged)
    Q_PROPERTY(qreal viewHeight READ viewHeight NOTIFY zoomChanged)
    /* 显示比例 */
    Q_PROPERTY(qreal zoom READ zoom NOTIFY zoomChanged)

    /* ---- 图上选字（Windows 自带 OCR，见 src/PinOcr.h） ---- */
    /* 这台机器上能不能用（没装 OCR 语言包就是 false，界面把"认字"收起来） */
    Q_PROPERTY(bool ocrAvailable READ ocrAvailable CONSTANT)
    /* 正在认（工具栏那个键转着） */
    Q_PROPERTY(bool ocrBusy READ ocrBusy NOTIFY ocrChanged)
    /* 认出来的行：[{ text, x, y, w, h }]，坐标 0~1（相对底图） */
    Q_PROPERTY(QVariantList ocrLines READ ocrLines NOTIFY ocrChanged)
    /* 不能用 / 认不出字时给界面的一句话（认好了一眼是空串） */
    Q_PROPERTY(QString ocrMessage READ ocrMessage NOTIFY ocrChanged)

public:
    /*
     * image 是贴图那一刻的成品图（底图）；pos 是窗口左上角的全局坐标（就是选区
     * 原来的位置）；engine 必须传 —— 不传 QQuickWidget 会自己 new 一个引擎，
     * PinOverlay.qml 里就 import 不到 SmartClip.Globals（Llm 那些单例）。
     */
    PinWindow(const QImage &image, const QPoint &pos, QQmlEngine *engine, Screenshot *owner);
    ~PinWindow() override;

    QString imageId() const { return m_id; }
    qreal zoom() const { return m_zoom; }
    qreal viewWidth() const { return m_picture.width() * m_zoom; }
    qreal viewHeight() const { return m_picture.height() * m_zoom; }
    bool ocrAvailable() const;
    bool ocrBusy() const { return m_ocrBusy; }
    QVariantList ocrLines() const { return m_ocrLines; }
    QString ocrMessage() const { return m_ocrMessage; }

    /* 底图 + 之后划上去的标注 -> 一张图（复制 / 保存 / 识别都走它） */
    QImage composedImage() const;
    /*
     * 交给模型识别的那份：png 的 data URL（和截图识别同一个格式）。
     *
     * **必须是 Q_INVOKABLE**：QML 那边（PinOverlay 里「翻译」那条路）是在 JS 里
     * 调它的。少了这四个字母，QML 不会说"没这个方法"，而是抛一句
     *     TypeError: Property 'composedImageUrl' of object PinWindow(0x…) is not a function
     * 那句话只进 qWarning（GUI 程序的 stderr 抓不到），界面上看到的就是
     * "点「翻译」什么都没发生"（用户报的）。同文件里别的 Q_INVOKABLE 方法
     * （copyResult / zoomBy / beginDrag…）都是"QML 要调"这个身份，这个漏了。
     */
    Q_INVOKABLE QString composedImageUrl() const;

    /* ---- QML 调的（界面动作） ---- */
    Q_INVOKABLE void copyResult();
    Q_INVOKABLE void copyTextToClipboard(const QString &text);
    Q_INVOKABLE void saveImageAs();
    /* 窗口跟着图一起缩放（图放大窗口也放大，不然图就溢出窗口了） */
    Q_INVOKABLE void zoomBy(qreal factor);
    Q_INVOKABLE void closePin();
    /*
     * 拖动这张贴图。分三步（按下 / 拖 / 松开）调，不是一步 startSystemMove()：
     *
     * 那个 API 在这类窗口上**不可靠** —— 窗口管理器不支持时它返回 false，而界面
     * 那边已经当成"在拖了"，于是怎么拖都不动（这个坑原来那份静态贴图的代码里
     * 就写着"它不支持时退回自己搬"，我重写时把退回那段删了，用户随即报了
     * "还是不能拖动"）。现在两步都留着：先试 startSystemMove，不行就自己搬。
     *
     * 位移**由 QML 把鼠标的全局坐标传进来**（dragMoveTo），不在这里读
     * QCursor::pos()：那个返回的是**设备像素**，而 move() / pos() 用的是**逻辑
     * 像素**，高 DPI 下两者差一个缩放比 —— 照它算位移，窗口要么不动、要么乱跳
     * （实测：150% 缩放下位移算出来是 0）。界面上拿到的是已经对齐好的逻辑坐标。
     */
    Q_INVOKABLE void beginDrag();
    Q_INVOKABLE void dragMoveTo(qreal globalX, qreal globalY);
    Q_INVOKABLE void endDrag();
    /* 改大小交给窗口管理器（右下角那个把手） */
    Q_INVOKABLE void beginResize();
    /* QML 那边改完标注喊一声：复制 / 保存 / 识别要用的那份得跟着重画 */
    Q_INVOKABLE void annotationsChanged(const QVariantList &list);
    /*
     * 认一遍图上的字（"选字"那条路，见 src/PinOcr.h + PinOverlay 的「认字」菜单）。
     *
     * engine：
     *   "windows" —— 本机 Windows 自带 OCR（离线、不用配）
     *   "ppocr"   —— 跑**本机那个程序**（RapidOCR / PaddleOCR），命令行由界面传进来
     *                （command，见 PinOcr::recognizeWithProgram 的约定）
     * 认的是**底图**（m_base），不是成品图：底图才是截图本来的内容，后来划上去的
     * 荧光笔 / 文字框不该改变"图上有哪些字"。认的过程扔在线程池里，界面不卡；
     * 认完 emit ocrChanged，QML 那边（PinOverlay 的文字层）跟着更新。
     */
    Q_INVOKABLE void startOcr(const QString &engine, const QString &command);

    /* 界面问一句：这条 PP-OCR 命令能不能跑（空串 = 能；见 PinOcr::runnerProblem） */
    Q_INVOKABLE QString ocrRunnerProblem(const QString &command) const;

    /*
     * 内部 + 自检用：
     *   * applyOcrResult —— 认字线程认完投回来（见 .cpp 里的 OcrTask），QML 不用管；
     *   * setOcrLinesForTest —— 自检直接把这批行摆进去，并且**锁住不再自动认字**。
     *     为什么要锁：QML 一建出来就会叫一次 startOcr（真跑一遍 OCR），而自检里
     *     "拖鼠标 / 点一下"那几条要的是确定的布局 —— 这台机器认不认得出字、什么时候
     *     回来，都不该影响那些检查。
     */
    void applyOcrResult(const QVariantList &lines, const QString &error = QString());
    void setOcrLinesForTest(const QVariantList &lines);

    /* ---- 自检用（见 src/SelfTest.cpp） ---- */
    QQuickItem *qmlRoot() const;
    int annotationCount() const { return m_annotations.size(); }
    /* 拖动这会儿走的是"自己搬"那条路吗（自检用：量清楚 startSystemMove 接没接手） */
    bool draggingManually() const { return m_manualDrag; }

    /* image://pin/<id> 的取图口（由 PinWindow.cpp 里的提供者调） */
    QImage imageForId(const QString &id) const;

signals:
    void zoomChanged();
    /* 认字那条路的状态变了：ocrBusy / ocrLines / ocrMessage（一起通知，省事） */
    void ocrChanged();

protected:
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setZoom(qreal value);
    /* 窗口该多大：图 × 缩放（图是"一像素是一像素"，见 .cpp 里 kPinDpr 的说明） */
    QSize shownSize() const;

    QString m_id;
    QImage m_base;
    QSizeF m_picture;
    QQuickWidget *m_view = nullptr;
    Screenshot *m_owner = nullptr;
    QVariantList m_annotations;
    qreal m_zoom = 1.0;
    /* 正在按缩放摆窗口（那时候的 resize 事件不该再反过来改缩放，见 resizeEvent） */
    bool m_settingZoom = false;
    /*
     * 自己搬窗口那一路的状态（见 beginDrag）：
     *   m_manualDrag  窗口管理器不接手，改由 dragMoveTo 自己搬
     *   m_dragAnchor  按下那一刻的鼠标全局位置（**逻辑像素**，QML 传进来的）
     *   m_dragOrigin  按下那一刻的窗口左上角（位移加在它上面）
     */
    bool m_manualDrag = false;
    QPointF m_dragAnchor;
    QPoint m_dragOrigin;

    /*
     * ---- 图上选字（Windows OCR）那点状态 ----
     *   m_ocrBusy    正在认（认字线程还没投回来）
     *   m_ocrLines   认出来的行（归一化坐标），QML 的文字层就绑它
     *   m_ocrMessage 不能用 / 认不出字时的一句话
     *   m_ocrLocked  自检摆进来的行，别再自动认（见 setOcrLinesForTest）
     */
    bool m_ocrBusy = false;
    QVariantList m_ocrLines;
    QString m_ocrMessage;
    bool m_ocrLocked = false;
};
