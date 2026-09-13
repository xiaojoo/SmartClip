#pragma once

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVariantList>

class QQmlEngine;
class QScreen;
class QWidget;

/*
 * 截图（选区 -> 加文字 -> 复制 / 保存 / 固定到桌面）。
 *
 * 为什么整件事要绕 C++ 一圈，而不是全写在 QML 里：
 *
 *  1) 抓屏只有 QScreen::grabWindow() 这一条路（Qt 里没有 QML 的抓屏类型），
 *     而且冻结的那张图要能"按设备像素"取一段出来 —— 这是 QImage 的活。
 *  2) 最终产物有三条出口（剪贴板 / png / 贴图窗口），三条必须是**同一张**
 *     合成图：标注在 QML 里是文本项，落到图上要 QPainter 画一遍。
 *     只在 C++ 里有一份 compose()，三处共用。
 *  3) "固定到桌面"就是再开一个**置顶无边框窗口**（见 Screenshot.cpp 里的
 *     PinWindow）。这类窗口用 QWidget 最省事：置顶、无边框、不进任务栏
 *     都是现成的标志位，QML 的 Window 还得自己接一层。
 *
 * 界面（选区框 / 浮动工具条 / 内联编辑文字）在 qml/screenshot/CaptureOverlay.qml，
 * 由这里用 QQuickWidget 装进一个覆盖整块屏的置顶无边框窗口里 —— 和主窗口
 * （见 src/main.cpp：QWidget 承载 QQuickWidget）是同一个套路，好处是窗口
 * 几何、置顶、焦点这些都在 C++ 手里，QML 只画内容。
 *
 * 坐标约定（三套，别混）：
 *   * 屏幕坐标（逻辑像素）：QML 里的 self.x/y、屏幕上鼠标的位置。
 *     (0,0) 是**被截的那块屏**的左上角，因为选区窗口就铺在那块屏上。
 *   * 图像坐标（设备像素）：冻结图上真实的像素，= 屏幕坐标 × devicePixelRatio。
 *   * 窗口全局坐标：贴图窗口摆的位置。
 * 只有 compose() 里那一次换算，别的地方都按屏幕坐标走。
 */
class Screenshot final : public QObject {
    Q_OBJECT

    /* 选区窗口开着没（自检用，也防止重复触发叠出第二个选区窗口） */
    Q_PROPERTY(bool active READ active NOTIFY stateChanged)

    /*
     * 抓屏序号，每抓一次 +1。
     *
     * 冻结图的 URL 是 "image://shot/full<serial>"：QQuickPixmapCache 是
     * **按 URL 缓存**的，序号不变的话第二次截图会拿到上一张的缓存，
     * 选区窗口里显示的还是上一次的画面（实测过）。
     */
    Q_PROPERTY(int serial READ serial NOTIFY stateChanged)

public:
    explicit Screenshot(QObject *parent = nullptr);
    ~Screenshot() override;

    /*
     * 主窗口由 main.cpp 创建，这里只记一个指针：藏 / 恢复它，
     * 以及给文件对话框当父窗口。
     */
    void setHostWidget(QWidget *host);

    /*
     * QML 引擎（QQuickWidget 那一个）。
     *
     * 截图界面要装进第二个 QQuickWidget，而且得**共用同一个引擎** ——
     * 单例（Store / Win / Cmd / Shot）和 QML 模块都是挂在引擎上的，
     * 另起一个引擎就 import 不到了。图片提供者（image://shot/…）也在这里登记。
     */
    void setEngine(QQmlEngine *engine);

    /*
     * 启动时预建选区窗口（藏着）并先渲染一帧，见 Screenshot::prewarm。
     * 不调也能用（第一次截图时现建），只是那一下会慢几百毫秒 ——
     * 主窗口已经藏了、选区窗口还没出来，屏幕上会闪一下桌面。
     */
    void prewarm();

    bool active() const { return m_active; }
    int serial() const { return m_serial; }

    /*
     * 开始截图：先抓屏，再把选区窗口铺满那块屏。
     * 抓屏要等桌面重画完一帧（抓早了会把 SmartClip 自己的窗口也拍进去），
     * 所以这里是异步的 —— 调用完立刻返回，画面准备好了才由 stateChanged()
     * 之外的那条路弹出窗口。
     */
    Q_INVOKABLE void beginCapture();

    /* 关掉选区窗口（取消 / 复制完 / 保存完 / 贴图之后都走它），并恢复主窗口 */
    Q_INVOKABLE void endCapture();

    /*
     * 用户按 Esc / 双击取消这次截图 —— 界面（CaptureOverlay.qml 的 Esc
     * Shortcut、框上双击）走的是这一个，而不是直接 endCapture。
     *
     * 为什么要多这么一个入口：抓屏是**延时**的（beginCapture 里那 30ms /
     * 150ms，见那里的说明），而"取消"经常就落在这段窗口期里 —— 用户按完
     * 快捷键马上按 Esc 就是。那会儿选区窗口还藏得好好的，endCapture 却
     * 什么都关不掉（没有 active 状态可收），于是等延时到点，grabAndShow()
     * 照样把窗口铺出来：屏幕先亮起一整块全屏选区、用户只好再按一次 Esc，
     * 看着就是"取消截图时全屏框闪现了一次才关闭"。
     *
     * 这里把待抓的那次直接掐掉（m_pending 清掉 + 把主窗口放回来），
     * 延时回调一进来就自己收工 —— 窗口从头到尾没被 show() 过，一帧都不闪。
     */
    Q_INVOKABLE void cancelCapture();

    /*
     * 三条出口。sel 是选区（屏幕坐标，逻辑像素），texts 是标注：
     *   [ { x, y, text, size, color } ]，x/y 也是屏幕坐标。
     * 落在选区外的标注自然被裁掉（合成时以选区的图为画布）。
     */
    Q_INVOKABLE bool copyResult(const QRectF &sel, const QVariantList &texts) const;

    /* 存到指定路径（自检用；界面走 saveResultAs 弹对话框选路径） */
    Q_INVOKABLE bool saveResult(const QString &path, const QRectF &sel,
                                const QVariantList &texts) const;

    /* 弹"保存截图"对话框并写盘；用户取消 / 写失败返回 false */
    Q_INVOKABLE bool saveResultAs(const QRectF &sel, const QVariantList &texts);

    /*
     * "更多颜色 -> 自定义…"：开系统取色框，返回 #rrggbb；取消返回空串。
     * current 是打开时停在哪个颜色上。
     */
    Q_INVOKABLE QString pickColor(const QString &current);

    /* 固定到桌面：在选区原来的位置上开一个置顶窗口 */
    Q_INVOKABLE void pinResult(const QRectF &sel, const QVariantList &texts);

    Q_INVOKABLE int pinnedCount() const;
    Q_INVOKABLE void closeAllPins();

    /* ---- 下面两个给自检用（见 src/SelfTest.cpp），界面不走 ---- */

    /* 选区窗口的 QML 根对象；没建起来返回 nullptr */
    QObject *overlayRoot() const;

    /*
     * 选区窗口这会儿是不是**真的**显示着。
     *
     * 别去读 QML 根的 visible 属性：QQuickWidget 里的根项永远是 visible，
     * 窗口藏起来它也不变（自检里踩过）。
     */
    bool overlayVisible() const;

    /*
     * 启动预热时是否已经让选区窗口在幕外映射并画过一帧（见 Screenshot::prewarm）。
     * 这是"第一次抓屏不带空窗帧"的前提，自检拿它钉住。
     */
    bool overlayWarmed() const { return m_overlayWarmed; }

    /* image://shot/<id> 的取图口（id 见下），由 Screenshot.cpp 里的提供者调 */
    QImage imageForId(const QString &id) const;

signals:
    void stateChanged();

private:
    /* 真正去 grabWindow() 那一步（beginCapture 里延时到桌面重画之后调） */
    void grabAndShow();
    /*
     * 掐掉"已经排上队、还没抓"的那次抓屏（见 cancelCapture）。
     * 没有待抓的（没在 pending）返回 false，调用方接着按正常收工走。
     */
    bool cancelPendingCapture();
    /* 建选区窗口（不显示）；窗口是复用的，见 prewarm */
    QWidget *createOverlay();
    void showOverlay();
    void restoreHost();

    /*
     * 把主窗口从屏幕捕获里排除 / 恢复（Windows 的 WDA_EXCLUDEFROMCAPTURE）。
     * 返回系统认不认这个标志位；不认就退回"藏窗口 + 延时"那条路。
     */
    bool setHostCaptureExcluded(bool on);

    /* 选区（屏幕坐标）+ 标注 -> 一张图。三条出口共用这一份 */
    QImage compose(const QRectF &sel, const QVariantList &texts) const;

    static QString defaultFileName();

    QQmlEngine *m_engine = nullptr;
    QWidget *m_host = nullptr;

    /* 选区窗口（已经 close 过就置空；它自己 deleteLater，别的地方不要伸手） */
    QWidget *m_overlay = nullptr;
    /* 待抓的那块屏（beginCapture 和 grabAndShow 之间传一下） */
    QScreen *m_screen = nullptr;

    /* 冻结的整屏图（设备像素），以及它对应的屏幕几何 / 缩放比 */
    QImage m_shot;
    QRect m_screenRect;
    qreal m_dpr = 1.0;

    /* 贴图窗口（置顶无边框小窗，见 Screenshot.cpp 的 PinWindow）；关掉自己变空 */
    QList<QPointer<QWidget>> m_pins;

    /* 上次保存截图的目录（给对话框当起点） */
    QString m_saveDir;

    int m_serial = 0;
    bool m_active = false;
    /*
     * 抓屏是延时的（见 beginCapture）：这段窗口期里 m_active 还是 false，
     * 再按一次快捷键就会排出第二次抓屏、叠出第二个选区窗口 —— 而
     * m_overlay 只记得住最后一个，先那个就再也关不掉了（满屏置顶窗口）。
     * 所以延时期间单独用这个标志挡重复触发。
     */
    bool m_pending = false;
    /*
     * 正开着取色框 / 保存框（都是嵌套事件循环的模态对话框）。
     * 这期间不许关选区窗口 —— 对话框的父窗口被拆掉的话它会 abort，
     * 详见 Screenshot::endCapture 的说明。
     */
    bool m_modalOpen = false;
    /*
     * 启动预热时是不是已经让窗口在幕外"露过脸"（见 Screenshot::prewarm）。
     * 自检用它钉住"第一次抓屏不带空窗帧"这件事的前提条件。
     */
    bool m_overlayWarmed = false;

    /* 抓屏前把主窗口藏起来了，收尾时要放回去 */
    bool m_hiddenHost = false;
    /* 主窗口是"从抓屏里排除"掉的（没藏），收尾时要把标志位摘掉 */
    bool m_excludedHost = false;
};
