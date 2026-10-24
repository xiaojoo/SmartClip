#include "Screenshot.h"
#include "PinWindow.h"
#include "Translate.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <algorithm>
#include <QFont>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickView>
#include <QQuickWidget>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantMap>
#include <QWheelEvent>
#include <QWidget>
#include <QWindow>
#include <cmath>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace {

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

Screenshot::~Screenshot() {
    /* 选区窗口是复用的一直留着（见 prewarm），退场时自己收 */
    delete m_overlay;
    m_overlay = nullptr;
}

/*
 * === 临时诊断（定位"取消截图时全屏闪一下边框"）===
 *
 * 把截图状态机 + 每一次 Esc 的到达 + 选区窗口的几何变化都带毫秒时间戳记到
 * **当前工作目录**下的 shot-trace.log。复现一次，这个文件就能说明：
 * 那一下 Esc 落在哪一段、当时窗口可见没有、边框画出来没有、窗口被摆到哪儿。
 * 定位完删掉：这个函数 + 所有 shotTrace(...) 调用 + <QFile> 这个 include。
 */
static void shotTrace(const char *what) {
    static QElapsedTimer clock;
    static bool started = false;
    if (!started) {
        clock.start();
        started = true;
        QFile::remove(QStringLiteral("shot-trace.log"));
    }
    QFile f(QStringLiteral("shot-trace.log"));
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(QStringLiteral("%1  %2\n")
                    .arg(clock.elapsed(), 6)
                    .arg(QLatin1String(what))
                    .toUtf8());
        f.flush();
        f.close();
    }
}

static void shotTrace(const QString &what) {
    shotTrace(what.toUtf8().constData());
}

/*
 * === 临时探针（SMARTCLIP_SHOT_PROF=1）：定位"截图时鼠标移动卡顿" ===
 *
 * 数两件事：选区那扇窗每秒**呈现**了多少帧（frameSwapped）、每秒到了多少次
 * MouseMove。"鼠标事件没来"和"画面跟不上鼠标"是两种病，这两个数一分就开。
 *
 * 定位完删掉：这个类 + createOverlay / showOverlay 里各一处调用 + 下面那个函数。
 */
class ShotProfiler final : public QObject {
public:
    explicit ShotProfiler(QWindow *target) : QObject(target) {
        m_clock.start();
        if (auto *win = qobject_cast<QQuickWindow *>(target)) {
            connect(win, &QQuickWindow::frameSwapped, this, [this]() {
                const qint64 now = m_clock.elapsed();
                if (m_lastSwap >= 0) {
                    m_gaps.append(now - m_lastSwap);
                    if (now - m_lastSwap > m_worstGap)
                        m_worstGap = now - m_lastSwap;
                }
                m_lastSwap = now;
                ++m_swaps;
            });
        }
        /* 每秒报一次：QWindow 上没有 QEvent::Paint，报告不能挂在 Paint 里 */
        auto *tick = new QTimer(this);
        tick->setInterval(1000);
        connect(tick, &QTimer::timeout, this, [this, target]() {
            std::sort(m_gaps.begin(), m_gaps.end());
            shotTrace(QStringLiteral("prof t=%1 swapped=%2/s moves=%3/s 帧间隔 中位=%4ms 最差=%5ms 可见=%6")
                          .arg(m_clock.elapsed()).arg(m_swaps).arg(m_moves)
                          .arg(m_gaps.isEmpty() ? 0 : m_gaps[m_gaps.size() / 2])
                          .arg(m_worstGap).arg(target->isVisible() ? 1 : 0));
            m_swaps = 0;
            m_moves = 0;
            m_gaps.clear();
            m_worstGap = 0;      /* 每秒归零：不然"最差"永远是开机到现在的空档 */
        });
        tick->start();
    }

protected:
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) {
            ++m_paints;
        } else if (event->type() == QEvent::MouseMove) {
            ++m_moves;
        }
        return false;
    }

private:
    QElapsedTimer m_clock;
    int m_paints = 0;
    int m_moves = 0;
    int m_swaps = 0;
    qint64 m_lastSwap = -1;
    qint64 m_worstGap = 0;
    QList<qint64> m_gaps;
};

static void shotFrameBenchmark(QQuickWindow *view) {
    if (!view)
        return;
    QElapsedTimer t;
    t.start();
    for (int i = 0; i < 8; ++i) {
        const QImage frame = view->grabWindow();
        shotTrace(QStringLiteral("prof BENCH grabFramebuffer#%1 = %2ms (%3x%4)")
                      .arg(i).arg(t.restart())
                      .arg(frame.width()).arg(frame.height()));
    }
}

/*
 * 几乎整块挪到屏幕外、只在**左边缘留 1 像素**的那份几何（尺寸一模一样）。
 *
 * 幕外这段是给 GPU 窗"先把这一帧画完呈现掉"用的，而**完全**在屏外的窗口
 * DWM 根本不给呈现：实测 presentOneFrame 等满 200ms 超时、frameSwapped 一次没来
 * （留 1px 在屏内就照样呈现，同一次运行里屏内那一次是 93ms swapped=1）。
 * 1 像素压在抓到的画面上，看不见。
 */
static QRect offscreenRect(const QRect &r) {
    return QRect(r.x() - r.width() + 1, r.y(), r.width(), r.height());
}

/*
 * 临时探针（SMARTCLIP_SHOT_OFFCHK=1）：此刻窗口表面里是不是**刚抓到的那张图**。
 *
 * 只回答一个问题：幕外那一帧到底画到位没有 —— 按网格采样，数"整块黑"和
 * "和抓屏一致"各多少个点。同一次调用里会在两个时刻各量一遍：present 之前
 * （该是黑的，这条判据能红 = 这把尺子分得开）和 present 之后（该是抓屏）。
 *
 * 定位完删掉：这个函数 + showOverlay 里那两处调用（和 ShotProfiler 一起清）。
 */
static void shotSurfaceCheck(QQuickWindow *w, const QImage &shot, const QString &when) {
    if (!w)
        return;
    const QImage surface = w->grabWindow().convertToFormat(QImage::Format_RGB32);
    const QImage ref = shot.convertToFormat(QImage::Format_RGB32);
    int n = 0, black = 0, same = 0;
    if (!surface.isNull() && !ref.isNull()) {
        for (int y = 8; y + 8 < surface.height(); y += qMax(1, surface.height() / 36)) {
            for (int x = 8; x + 8 < surface.width(); x += qMax(1, surface.width() / 64)) {
                const QRgb a = surface.pixel(x, y);
                ++n;
                if (qRed(a) <= 8 && qGreen(a) <= 8 && qBlue(a) <= 8)
                    ++black;
                if (x < ref.width() && y < ref.height()) {
                    const QRgb b = ref.pixel(x, y);
                    if (qAbs(qRed(a) - qRed(b)) <= 8 && qAbs(qGreen(a) - qGreen(b)) <= 8
                        && qAbs(qBlue(a) - qBlue(b)) <= 8)
                        ++same;
                }
            }
        }
    }
    shotTrace(QStringLiteral("OFFCHK %1 采样=%2 纯黑=%3 与抓屏一致=%4 表面=%5x%6 抓屏=%7x%8")
                  .arg(when).arg(n).arg(black).arg(same)
                  .arg(surface.width()).arg(surface.height())
                  .arg(ref.width()).arg(ref.height()));
}

/*
 * 让"当前这一帧"真的呈现出去。
 *
 * QWindow 没有 QWidget::repaint() 那种同步刷一次的办法 —— 只能 requestUpdate
 * 之后等 frameSwapped。收工前要用它把窗口表面刷成干净画面（下次 show 时系统
 * 先呈现的就是那张残留表面，见 endCapture 那段）。
 */
static void presentOneFrame(QWindow *w, int timeoutMs = 200) {
    if (!w)
        return;
    /* 临时探针：这一帧到底是等到的，还是超时等不到的 */
    QElapsedTimer probe;
    probe.start();
    bool swapped = false;
    QEventLoop loop;
    QTimer kill;
    kill.setSingleShot(true);
    QObject::connect(&kill, &QTimer::timeout, &loop, &QEventLoop::quit);
    if (auto *win = qobject_cast<QQuickWindow *>(w))
        QObject::connect(win, &QQuickWindow::frameSwapped, &loop, [&loop, &swapped]() {
            swapped = true;
            loop.quit();
        });
    kill.start(timeoutMs);
    w->requestUpdate();
    loop.exec();
    shotTrace(QStringLiteral("presentOneFrame %1ms swapped=%2")
                  .arg(probe.elapsed()).arg(swapped ? 1 : 0));
}

void Screenshot::uiTrace(const QString &what) { shotTrace(what); }

void Screenshot::setHostWidget(QWidget *host) {
    m_host = host;
    /*
     * Esc 的应用级兜底（见 eventFilter 的说明）：必须挂在 qApp 上，
     * 因为那段时间的按键是投给主窗口的，而主窗口没有 Esc 的处理。
     */
    if (qApp)
        qApp->installEventFilter(this);
}

bool Screenshot::eventFilter(QObject *watched, QEvent *event) {
    /*
     * 只在"截图已经排上队 / 正在抓，但选区窗口**还没露脸**"这段里管 Esc。
     *
     * 判据用 overlayVisible() 而不是 m_pending：m_pending 在 grabAndShow() 一进来
     * 就被清了（那时候抓屏已经开始、渲染还没做），而用户按 Esc 恰恰就落在
     * "抓屏 + 渲染"这几十毫秒里 —— 拿 m_pending 当判据这段全漏（自己踩过）。
     * 窗口没出来 = 用户看到的就是"什么都没有"，这会儿的 Esc 只能是在取消。
     *
     * 为什么要在这一层接（真机插桩量出来的）：这段时间主窗口**还是活动窗口**
     * （排除法下它没被藏），用户按的 Esc 会正常投递到主窗口 —— 可主窗口上根本
     * 没有 Esc 的处理（那条 Shortcut 在选区窗口的 QML 里，而窗口这会儿还藏着），
     * 这一下就被丢掉了。等窗口出来再按才轮得到它，用户看到的就是"全屏框闪了
     * 一下才关闭"。
     *
     * 别的时候一律放行：
     *   * 窗口开着时由选区窗口 QML 里那条 Esc Shortcut 管（它还要先"取消
     *     选中"再"撤销整个截图"，那套分级语义在这一层复制一遍只会分家）；
     *   * 没在截图时更不能碰 —— Esc 是编辑器/对话框自己的键。
     */
    Q_UNUSED(watched);
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            shotTrace(QStringLiteral("Esc ARRIVED: pending=%1 active=%2 visible=%3")
                          .arg(m_pending ? 1 : 0).arg(m_active ? 1 : 0)
                          .arg(overlayVisible() ? 1 : 0));
            if (m_pending || (m_active && !overlayVisible())) {
                shotTrace("  -> caught app-level (window not visible) -> cancel");
                cancelCapture();
                return true;   /* 吃掉，别让它再下去触发系统提示音 */
            }
            shotTrace("  -> passed to overlay window (QML Shortcut handles it)");
        }
    }
    return QObject::eventFilter(watched, event);
}

void Screenshot::setEngine(QQmlEngine *engine) {
    m_engine = engine;
    if (!m_engine)
        return;
    m_engine->addImageProvider(QStringLiteral("shot"), new ShotImageProvider(this));
    /*
     * 贴图窗口的底图也走这个引擎（image://pin/<id>）。
     * 提供者在 PinWindow.cpp 里（见 renderPin 的说明）：它自己去找窗口 ——
     * 贴图用完即弃，没法在这里挂一份清单。
     */
    m_engine->addImageProvider(QStringLiteral("pin"), renderPin());
}

void Screenshot::beginCapture() {
    shotTrace("beginCapture");
    if (m_active || m_pending) {
        /* 已经开着（用户又按了一次快捷键）：把选区窗口提到前面就够了 */
        if (m_overlay) {
            m_overlay->raise();
            m_overlay->requestActivate();
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
     * 主窗口正好压在这块屏上时，要让第一张图里**没有 SmartClip 自己**。
     *
     * 两种办法，优先用第一种：
     *
     *  1) WDA_EXCLUDEFROMCAPTURE：让 Windows 把主窗口从屏幕捕获里排除掉
     *     （Win10 2004+）。窗口照常显示给用户看，只是抓屏抓不到它 ——
     *     所以**不用藏、不用等**，抓到选区窗口出现几乎是即时的。
     *     原来那套"藏起来 + 等 150ms 让桌面重画"就是"打开时屏幕闪一下"
     *     的来源：那 150ms（加上抓屏、首帧）里屏幕上露的是桌面。
     *  2) 老办法（兜底）：藏窗口 + 延时。系统不支持上面那个标志位时才用，
     *     见 setHostCaptureExcluded() 的返回值。
     *
     * 只在**就是这块屏**时动手：窗口在另一块屏上时不用管，用户多半就是在
     * 截那块屏上的别的程序。
     */
    const bool onThisScreen = m_host && m_host->isVisible() && m_host->screen() == screen;
    m_excludedHost = onThisScreen && setHostCaptureExcluded(true);
    m_hiddenHost = onThisScreen && !m_excludedHost;
    if (m_hiddenHost)
        m_host->hide();

    m_screen = screen;
    m_pending = true;

    /*
     * 排除法几乎不用等（合成器下一帧就生效），藏窗口那条老路才要等桌面重画完
     * —— 不延时直接抓的话，抓到的还是"窗口还在上面"的那一帧（实测：菜单刚关掉、
     * 窗口刚 hide 掉时最容易撞上）。
     */
    QTimer::singleShot(m_hiddenHost ? 150 : (m_excludedHost ? 30 : 0),
                       this, &Screenshot::grabAndShow);
}

void Screenshot::grabAndShow() {
    shotTrace(m_pending ? "grabAndShow(pending)" : "grabAndShow(CANCELLED)");
    /*
     * 延时这段窗口期里用户取消了（见 cancelCapture）：主窗口已经放回来了，
     * 这里就什么都别做 —— 尤其**不能** show() 选区窗口，否则就是"取消截图时
     * 全屏框闪一下"。
     */
    if (!m_pending) {
        m_screen = nullptr;
        return;
    }

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

/*
 * 建选区窗口（不显示）。
 *
 * 2026-10-24 从"无边框 QWidget + QQuickWidget"换成真正的 GPU 顶层窗 QQuickView。
 * 换的理由是量出来的：QQuickWidget 每一帧要把整块 4K 场景渲到离屏表面、读回来、
 * 再 blit 进控件 —— 一次同步渲染 17~20ms（3840x2160 连测 8 次），框选拖动时界面
 * 只能跑到 ~45 帧/秒，还偶发 56~88ms 的顿；同一台机同一块屏，真顶层窗画同样的
 * 4K 内容每帧 0~6ms（QSG_RENDER_TIMING 量的）。主窗口不能这么换（里面嵌着原生
 * QScintilla 子窗），这扇窗里没有原生子窗，所以换得干净。
 *
 * 引擎还是**共用**那一个（见 setEngine），所以 QML 里照样用 Shot 这个单例。
 * 窗口是**复用**的（见 prewarm）：建好之后一直留着，抓屏时只换图 + show。
 */
QQuickView *Screenshot::createOverlay() {
    auto *view = new QQuickView(m_engine, nullptr);
    view->setFlags(Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    view->setTitle(QStringLiteral("SmartClip 截图"));
    view->setResizeMode(QQuickView::SizeRootObjectToView);
    /* 底图自己铺满，这里的清屏色只在图还没解码出来时露一下 */
    view->setColor(Qt::black);

    view->setSource(QUrl(QStringLiteral("qrc:/qt/qml/SmartClip/qml/screenshot/CaptureOverlay.qml")));
    if (view->status() == QQuickView::Error) {
        for (const QQmlError &error : view->errors())
            qWarning("%s", qPrintable(error.toString()));
        delete view;
        return nullptr;
    }

    /* 临时探针（SMARTCLIP_SHOT_PROF=1，见 ShotProfiler） */
    if (qEnvironmentVariableIsSet("SMARTCLIP_SHOT_PROF")) {
        auto *prof = new ShotProfiler(view);
        view->installEventFilter(prof);
    }

    /*
     * 以 this 作为上下文接：Screenshot 先没掉时这条连接自动断，
     * 免得进程退出时窗口被拆还回头调已经析构的对象。
     */
    connect(view, &QObject::destroyed, this, [this]() {
        m_overlay = nullptr;
        if (m_active) {
            m_active = false;
            m_shot = QImage();
            restoreHost();
            emit stateChanged();
        }
    });
    return view;
}

void Screenshot::prewarm() {
    /*
     * 启动时就把选区窗口建好（藏着），并且先渲染一帧。
     *
     * 为什么：抓屏到选区窗口出现之间，主窗口已经藏了 —— 这段时间屏幕上露的
     * 是桌面。原来这段时间里要现场做的事有：QML 解析、QQuickWidget 的场景图
     * 初始化、4K 底图上传、首帧渲染，加起来几百毫秒，肉眼就是"屏幕闪一下"。
     * 挪到启动时做掉之后，抓屏那一刻只剩"换图 + show"，那一下基本看不见。
     *
     * grabFramebuffer() 是故意的：它强制走一次完整渲染，把着色器编译 /
     * 纹理那些一次性开销也提前付掉。
     */
    if (m_overlay || !m_engine)
        return;
    m_overlay = createOverlay();
    if (!m_overlay) {
        qWarning("截图：选区窗口预建失败（QML 没加载起来）");
        return;
    }
    /*
     * 预热这一帧要**按整块屏来画**，不能就着"还没摆过几何的小窗口"画。
     *
     * 窗口重新显示时，系统会先把上一次保存的那张表面呈现出来（QQuickWidget
     * 内部预热解决不了这个，实测过）。所以这里就把几何摆成主屏大小、把界面复位
     * 成"干净的全屏选区"，再同步画一帧 —— 那么第一次抓屏时先露出来的那张也是
     * 干净的，不会是个小尺寸的框。
     */
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect rect = screen->geometry();
        m_overlay->setGeometry(rect);
        if (QObject *root = overlayRoot())
            QMetaObject::invokeMethod(root, "resetForCapture",
                                      Q_ARG(QVariant, QVariant::fromValue(QRectF(rect))));

        /*
         * 关键一步：**在屏幕外面把它露一次脸**（画好再藏起来）。
         *
         * 选区窗口的原生窗口是第一次 show() 才创建、映射的 —— 那一刻 DWM 手上
         * 没有任何内容，只能先呈现一张空表面，和 Qt 的首帧赛跑：抢在前面就是
         * "整个屏幕闪一下"（用户报的"第一次按快捷键偶发闪屏"；之后几次不闪，
         * 是因为表面里存着上次收工时刷好的干净画面）。
         *
         * 这就是 Win32 那条老配方：窗口**先不给人看地映射一次、把内容画好，再拿去显示**。
         * 挪到屏幕外（负数坐标，多屏环境里合法）就看不见；WA_ShowWithoutActivating
         * 保证不抢用户窗口的焦点。
         */
        m_overlay->setGeometry(offscreenRect(rect));
        m_overlay->show();
        presentOneFrame(m_overlay);       /* 渲一帧并真的呈现出去：着色器、纹理都在这儿预热 */
        /*
         * QWindow 没有 WA_ShowWithoutActivating —— 幕外这一下 show 会抢一次前台。
         * 预热只在启动时做一遍，抢完立刻把焦点还给主窗口，用户看不出差别。
         */
        if (m_host) {
            m_host->raise();
            m_host->activateWindow();
        }

        /*
         * 关键第二步：**摆回主屏之后，趁窗口还可见再画一帧，然后才藏**。
         *
         * 为什么非要有这一步：上面那次渲染是在**屏幕外**的几何上做的，而系统
         * 保存的是"窗口表面最后一次真正呈现出来的内容"。以前是 hide() 之后才
         * setGeometry(rect)，那之后没有任何一帧呈现过 —— 系统手里那张表面就一直是
         * **按屏幕外几何画的**。第一次抓屏 show() 时先呈现的正是它（这段代码开头
         * 那条注释写的就是这个规则），于是屏幕上先亮一帧"位置对不上的整屏选区"，
         * 用户看到的就是"刚按下快捷键、鼠标还没动，屏幕闪一下"。
         *
         * 这里和 endCapture() 收工前那套走法保持一致：grabFramebuffer() 强制同步
         * 渲染，repaint() 把这一帧推到窗口上 —— 差别只是窗口这会儿还在屏幕外露着
         * （WA_ShowWithoutActivating，不抢焦点），用户看不见。
         */
        m_overlay->setGeometry(rect);   /* 先摆回主屏，第一次抓屏直接就用它 */
        presentOneFrame(m_overlay);
        shotTrace("prewarm: 已在主屏几何上重画一帧（窗口表面不再对应屏幕外几何）");
        m_overlay->hide();
        m_overlayWarmed = true;
    }
}

void Screenshot::showOverlay() {
    shotTrace("showOverlay enter");
    if (!m_overlay)
        m_overlay = createOverlay();
    if (!m_overlay) {
        m_active = false;
        m_shot = QImage();
        restoreHost();
        emit stateChanged();
        return;
    }

    /*
     * 复用同一个窗口：先摆到**屏幕外**，把新底图喂进去、画完一帧，再整窗挪进来
     * （挪进来那段在下面，紧接在"排队里的按键抽干净"之后）。
     */
    m_overlay->setGeometry(offscreenRect(m_screenRect));
    m_overlayOnScreen = false;
    shotTrace(QStringLiteral("  window geometry %1,%2 %3x%4")
                  .arg(m_screenRect.x()).arg(m_screenRect.y())
                  .arg(m_screenRect.width()).arg(m_screenRect.height()));
    /*
     * 把屏幕矩形**直接告诉 QML**，别让它按控件尺寸猜 —— setGeometry 之后
     * QQuickWidget 的布局是延迟生效的，复位那一刻它还是预热时的旧尺寸，
     * 于是第一两帧会按一个小方框画（用户看到的"闪一下方框轮廓"）。
     */
    if (QObject *root = overlayRoot())
        QMetaObject::invokeMethod(root, "resetForCapture",
                                  Q_ARG(QVariant, QVariant::fromValue(QRectF(m_screenRect))));

    /*
     * 幕外露面 + 呈现一帧：这一拍要在挪进来之前做完，两件事都靠它。
     *
     * 一、**新底图先画好**。真 GPU 顶层窗重新显示时，DWM 交出来的还是表面里
     * 上一帧（预热那次画的是"底图还没解码"的整屏），新抓的那一张要在 GPU 上
     * 解码 + 上传 + 渲染才换得掉。实测第一次抓屏这段是 55.6ms（144Hz 约 8 帧），
     * 用户看到的就是"按下快捷键、屏幕黑一下"（HEAD 的 QQuickWidget 路径没有
     * 这一笔 —— 那时 show 之前有一次 grabFramebuffer，新画面已经刷进窗口表面）。
     * 这里把同一件事挪到幕外做：画面摆到屏外画完再进来，进来那一刻就是真截图。
     *
     * 二、旧框残留（上一轮用户框的那块选区）同样在这拍被新画面盖掉，
     * 原来那次 grabFramebuffer 承担的就是这个。
     *
     * 但这一拍的代价正好落在"取消"这条路上（真机插桩量出来的，
     * Ctrl+Alt+A 之后 20ms 按 Esc）：
     *
     *    0  beginCapture
     *   31  grabAndShow
     *   96  showOverlay 进来
     *   97  开始 grabFramebuffer（4K 底图的解码 + 上传，把事件循环按住几十毫秒）
     *  145  grabFramebuffer 返回 —— 用户那一下 Esc 的 KeyPress 直到这一刻才被投递进来
     *  148  show()（窗口可见）
     *  196  cancelCapture <- 那一下 Esc 到这儿才被处理
     *
     * 于是窗口先在屏幕上待了约 55ms（3 帧多，肉眼就是一"闪"）才关掉 —— 用户报的
     * "取消截图时全屏框闪现了一次才关闭"。
     *
     * 所以渲染完之后、show() 之前，必须把这段时间里攒下的输入**排空**再做决定：
     * 那一下 Esc 就混在里面，它走 eventFilter -> cancelCapture -> endCapture，
     * 把 m_active 落回 false —— 下面发现已经收工了就**直接返回、不 show()**，
     * 窗口一帧都不露。
     *
     * 为什么不是"调一次 processEvents 就往下走"：插桩显示那会儿键**还没到**
     * （上面 145ms 那一行）。所以这里**看队列**而不是**等固定时间** —— 队列空
     * 就立刻走人（正常截图这条路上队列本来就是空的，几微秒就过，不会平白加上
     * 一截延迟）；队列里有东西就抽出来，抽完再多抽一小会儿（键事件可能分几条
     * 陆续到）。
     */
    /*
     * 幕外把这一帧画完、呈现掉。
     *
     * 原来这一步是 grabFramebuffer()（在 QQuickWidget 上把新画面刷进控件的离屏图）。
     * 换成真窗口后它只剩"从 GPU 读回一张 4K 图"，实测 27~31ms，纯浪费；要的只是
     * "这一帧确实到了窗口表面"，所以改成 requestUpdate + 等 frameSwapped。
     */
    shotTrace("show()幕外 BEFORE");
    m_overlay->show();
    shotTrace("show()幕外 AFTER: 屏外可见");
    /* 临时探针（SMARTCLIP_SHOT_OFFCHK=1）：刚映射完、还没画这一帧时表面是什么 */
    if (qEnvironmentVariableIsSet("SMARTCLIP_SHOT_OFFCHK"))
        shotSurfaceCheck(m_overlay, m_shot, QStringLiteral("幕外 show 之后 / present 之前"));
    presentOneFrame(m_overlay);
    shotTrace("presentOneFrame 回来：新底图已呈现（还在屏外）");
    /* 临时探针（SMARTCLIP_SHOT_OFFCHK=1）：这一帧到位没有 */
    if (qEnvironmentVariableIsSet("SMARTCLIP_SHOT_OFFCHK")) {
        shotSurfaceCheck(m_overlay, m_shot, QStringLiteral("presentOneFrame 之后"));
        cancelCapture();
        return;
    }

    {
        constexpr int kGraceMs = 20;   /* 见好就收：抽到键之后再等这么久就够 */
        QElapsedTimer grace;
        bool sawKey = false;
#if defined(Q_OS_WIN)
        auto queueBusy = []() {
            MSG msg;
            return PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE | PM_QS_INPUT) != FALSE;
        };
#else
        auto queueBusy = []() { return false; };
#endif
        while (m_active) {
            const bool busy = queueBusy();
            if (busy) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                if (!sawKey) {
                    sawKey = true;
                    grace.start();
                }
                continue;
            }
            if (sawKey && grace.elapsed() < kGraceMs) {
                QThread::msleep(1);      /* 让刚抽出来的事件走完 */
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
                continue;
            }
            break;                       /* 队列空了（也没抽到过键）-> 立刻放行 */
        }
    }

    /*
     * 抽到 Esc 那一发的话上面已经收工了（endCapture 把窗口藏回去），这里连幕外
     * 那扇都不该再露着 —— 直接返回。
     */
    if (!m_active) {
        return;
    }

    /* 内容已经在幕外画好了，现在只是把整窗挪到目标屏上 */
    shotTrace("挪进来 BEFORE");
    m_overlay->setGeometry(m_screenRect);
    m_overlayOnScreen = true;
    shotTrace("挪进来 AFTER: 用户可见");
    m_overlay->raise();
    m_overlay->requestActivate();
    if (QQuickItem *ci = m_overlay->contentItem())
        ci->forceActiveFocus();

    /* 临时探针（SMARTCLIP_SHOT_PROF=1，见 ShotProfiler）：整窗 repaint 一次的代价 */
    if (qEnvironmentVariableIsSet("SMARTCLIP_SHOT_PROF")) {
        QTimer::singleShot(300, this, [this]() {
            if (!m_active || !m_overlay)
                return;
            shotFrameBenchmark(m_overlay);
        });
    }

    /*
     * 窗口真的稳住了再让界面把选区边框画出来（见 CaptureOverlay.qml 的 overlayReady）。
     *
     * 为什么不能 show() 之后立刻就画：抓屏 + 4K 底图上传要几十毫秒，这期间窗口可能
     * 已经 show() 出来了，而框选默认是**整屏** —— 屏幕会先亮起一圈"全屏选区"的边框。
     * 用户在窗口刚出来那几十毫秒里按 Esc 取消，看到的就是"全屏闪了一下边框才关掉"。
     *
     * 延迟这一小段再放边框：取消落在这段里的话（用户按完快捷键马上 Esc），这圈框
     * 从头到尾没画过，一帧都不闪；正常截图只是晚一两帧出现边框，用户在框选时
     * 看不出差别。
     */
    /* 临时诊断：settle 之后再等一会儿，看这会儿边框到底可见没有 */
    QTimer::singleShot(250, this, [this]() {
        if (QObject *root = overlayRoot()) {
            QVariant v;
            QMetaObject::invokeMethod(root, "barState", Q_RETURN_ARG(QVariant, v));
            const QVariantMap m = v.toMap();
            shotTrace(QStringLiteral("+250ms: active=%1 visible=%2 ready=%3 borderVisible=%4")
                          .arg(m_active ? 1 : 0)
                          .arg(overlayVisible() ? 1 : 0)
                          .arg(m.value(QStringLiteral("ready")).toInt())
                          .arg(m.value(QStringLiteral("borderVisible")).toInt()));
        }
    });

    QTimer::singleShot(60, this, [this]() {
        shotTrace(QStringLiteral("settle timer: active=%1 visible=%2")
                      .arg(m_active ? 1 : 0).arg(overlayVisible() ? 1 : 0));
        if (m_active && overlayVisible()) {
            if (QObject *root = overlayRoot())
                QMetaObject::invokeMethod(root, "settleOverlay");
        }
    });
}

bool Screenshot::cancelPendingCapture() {
    if (!m_pending)
        return false;
    /*
     * 抓屏还没发生，所以没有图、没有选区窗口：
     * 把等着的那个定时回调变成空操作（清 m_pending，见 grabAndShow 开头），
     * 主窗口原地放回来（排除法下它本来就没被藏过，用户看不见这一下）。
     */
    m_pending = false;
    m_screen = nullptr;
    restoreHost();
    return true;
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

    /*
     * 抓屏还在延时里（beginCapture 到 grabAndShow 之间）就收工：这就是
     * "取消截图"落在快捷键那一下上的情形。那会儿窗口没露过面、也没有图，
     * 该做的只是把这次抓屏掐掉 + 放回主窗口。
     *
     * 少了这一步的后果（用户报的）：Esc 什么都没关掉，延时到点后
     * grabAndShow() 照样铺出选区窗口 —— 屏幕先亮一下全屏框，再按一次 Esc
     * 才关得掉。走 cancelCapture() 那条路也是落到这里。
     */
    if (cancelPendingCapture())
        return;

    /*
     * 顺序要紧：**先把主窗口放回来，再收起选区窗口**。
     *
     * 反过来的话，选区窗口一没、主窗口还没画出来，中间那一两帧露的是桌面 ——
     * 用户看到的就是"退出一闪"。主窗口在置顶的选区窗口底下，先放回来是看不见的，
     * 等选区窗口一收，它已经画好了，接得上。
     */
    restoreHost();
    if (m_overlay) {
        /*
         * 收工前把界面复位成"干净的全屏"并同步画一帧 —— 这样下次显示时，
         * 系统先呈现的那张残留表面也是干净的。
         *
         * 不做这一步的后果实测过：第二轮抓屏时，上一轮用户框的那块选区轮廓
         * 会闪 3 帧（约 14ms）。QQuickWidget 内部预热解决不了它 —— 残留的是
         * **窗口自己的表面**（隐藏前系统保存的那张），所以得在藏之前把它刷成干净的。
         * 此处清标注是安全的：结果早就 compose 完、复制/保存/贴图都做过了。
         */
        if (QObject *root = overlayRoot())
            QMetaObject::invokeMethod(root, "resetForCapture",
                                      Q_ARG(QVariant, QVariant::fromValue(QRectF(m_screenRect))));
        /*
         * 藏起来之前，必须让窗口表面存的是**干净画面**：下次 show() 时系统先呈现的
         * 就是这张表面，否则上一轮框选的轮廓会闪一帧（实测：命中帧截下来看，左侧压暗
         * + x≈700 处的强调蓝框线，正是上一轮的选区）。
         *
         * 两步缺一不可：
         *   grabFramebuffer() —— 强制**同步**渲染这个干净状态（QQuickWidget 平时
         *                        是渲染线程异步出帧，光 repaint 刷进去的还是旧帧）；
         *   repaint()         —— 把刚渲染好的这帧真正推到窗口上。
         * 只做其中任何一个都还剩 1~3 帧残留（都实测过）。
         */
        presentOneFrame(m_overlay);
        shotTrace("endCapture: hide() now");
        m_overlay->hide();      /* 只是藏起来，留着下次复用（见 prewarm） */
        m_overlayOnScreen = false;
        shotTrace("endCapture: hidden");
    }

    m_active = false;
    m_shot = QImage();
    emit stateChanged();
}

void Screenshot::cancelCapture() {
    shotTrace("cancelCapture");
    /*
     * 取消 = 收工。两条路（抓屏还在延时里 / 选区窗口已经开着）都由
     * endCapture 里的 pending 判断分开处理，所以这里直接转过去 ——
     * 界面那边 Esc 和双击都只叫这一个，不用自己分辨当前在哪一段。
     */
    endCapture();
}

void Screenshot::restoreHost() {
    /* 先把"抓屏排除"摘掉：这是加在主窗口上的开关，不能留着 */
    if (m_excludedHost) {
        m_excludedHost = false;
        setHostCaptureExcluded(false);
    }
    if (!m_hiddenHost)
        return;
    m_hiddenHost = false;
    if (m_host)
        m_host->show();
}

/*
 * 把主窗口从"屏幕捕获"里排除 / 恢复（Windows 10 2004+ 的
 * SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)）。
 *
 * 返回值 = 系统认不认这个标志位；不认（老系统 / 非 Windows）就返回 false，
 * 调用方退回"藏窗口 + 延时"那条老路。
 */
bool Screenshot::setHostCaptureExcluded(bool on) {
#if defined(Q_OS_WIN)
#  ifndef WDA_EXCLUDEFROMCAPTURE
#    define WDA_EXCLUDEFROMCAPTURE 0x00000011
#  endif
    if (!m_host)
        return false;
    const HWND hwnd = reinterpret_cast<HWND>(m_host->winId());
    if (!hwnd)
        return false;
    return SetWindowDisplayAffinity(hwnd, on ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE) != FALSE;
#else
    Q_UNUSED(on);
    return false;
#endif
}

QImage Screenshot::cropSelection(const QRectF &sel) const {
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
    return m_shot.copy(device);
}

/*
 * 识别时选区最小要多大（**逻辑像素**）。
 *
 * 为什么单设一道门槛：合成 / 复制那两条路对选区大小无所谓（用户真框了 2×2
 * 也就出 2×2 的图），但识别不一样 —— 那么小一块交给模型纯属浪费一次请求，
 * 回来的一定是"没认出文字"，用户还得等几十秒才知道白点了。
 * 8 是这个界面上"手一抖点一下"的尺度（见 CaptureOverlay 的 selReady）。
 */
constexpr qreal kMinRecognizeSize = 8.0;

bool Screenshot::selectionReady(const QRectF &sel) const {
    const QRectF clipped = sel.normalized();
    if (clipped.width() < kMinRecognizeSize || clipped.height() < kMinRecognizeSize)
        return false;
    /* 光看宽高不够：还没抓到图（m_shot 空）时一样是"没东西可认" */
    return !cropSelection(sel).isNull();
}

QString Screenshot::selectionImage(const QRectF &sel) const {
    if (!selectionReady(sel))
        return QString();
    /* 编码只留一份（见 LlmClient::imageToDataUrl）：选区、贴图、汇总认截图发的是同一种东西 */
    return LlmClient::imageToDataUrl(cropSelection(sel));
}

void Screenshot::copyText(const QString &text) const {
    if (!text.isEmpty())
        QGuiApplication::clipboard()->setText(text);
}

/*
 * 取色 / 放大镜的坐标口径和选区完全一样：QML 那侧的 (x,y) 是"被截那块屏的屏幕
 * 坐标、逻辑像素"（见 CaptureOverlay.qml 文件头），乘 DPR 才是 m_shot 的设备像素。
 * 4K 加 150% 缩放下一个逻辑像素是 1.5 个物理像素，**不乘 DPR 就会偏半个像素**，
 * 读出来的颜色是隔壁那列的。
 */
QString Screenshot::colorAt(qreal x, qreal y) const {
    if (m_shot.isNull())
        return QString();
    const int dx = qRound(x * m_dpr);
    const int dy = qRound(y * m_dpr);
    if (dx < 0 || dy < 0 || dx >= m_shot.width() || dy >= m_shot.height())
        return QString();
    return m_shot.pixelColor(dx, dy).name(QColor::HexRgb).toUpper();
}

void Screenshot::setOverlayCursor(int shape) {
    if (!m_overlay)
        return;
    if (shape < 0)
        m_overlay->unsetCursor();
    else
        m_overlay->setCursor(QCursor(static_cast<Qt::CursorShape>(shape)));
}

QString Screenshot::zoomUrl(qreal x, qreal y, int halfX, int halfY, int scale) const {
    if (m_shot.isNull() || halfX < 1 || halfY < 1 || scale < 1)
        return QString();
    const int dx = qRound(x * m_dpr);
    const int dy = qRound(y * m_dpr);
    const int hx = qRound(halfX * m_dpr);
    const int hy = qRound(halfY * m_dpr);
    /*
     * 贴着屏幕边就整个不给：宁可放大镜闪一下，也不要拿一块错位半格的图当
     * "光标正中间那个像素的颜色"。
     */
    if (dx - hx < 0 || dy - hy < 0
        || dx + hx > m_shot.width() || dy + hy > m_shot.height())
        return QString();
    return QStringLiteral("image://shot/zoom%1-%2-%3-%4-%5-%6")
        .arg(m_serial).arg(dx).arg(dy).arg(hx).arg(hy).arg(scale);
}

QImage Screenshot::compose(const QRectF &sel, const QVariantList &texts) const {
    QImage out = cropSelection(sel);
    if (out.isNull())
        return QImage();

    /* 选区夹回屏幕范围的那一份（标注坐标要按它平移，见 paintAnnotations） */
    const QRectF bounds(QPointF(0, 0), QSizeF(m_screenRect.size()));
    const QRectF clipped = sel.normalized().intersected(bounds);

    /*
     * 画之前把 DPR 抹成 1，再把画笔按 dpr 缩放。
     *
     * 标注的坐标是**屏幕坐标（逻辑像素）**，而这张图是设备像素 —— 两者差一个
     * dpr。逐个坐标乘 dpr 和缩放画笔效果一样，这里选后者：坐标平移交给共用的
     * paintAnnotations，线宽 / 字号由 QPainter 统一放大，不会出现"哪一处忘了乘"。
     *
     * 抹掉 DPR 之后这张图就是"一像素是一像素"的（各 Qt 版本对 QImage 上的 DPR
     * 行为不一致，赌错了就是"字和位置都大一倍"）。
     */
    out.setDevicePixelRatio(1.0);

    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.scale(m_dpr, m_dpr);
    paintAnnotations(painter, texts, clipped);
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
        m_host, QStringLiteral("保存截图"),
        dir + QDir::separator() + defaultFileName(),
        QStringLiteral("PNG 图片 (*.png);;JPEG 图片 (*.jpg);;所有文件 (*.*)"));
    m_modalOpen = false;
    if (path.isEmpty())
        return false;

    if (!image.save(path)) {
        QMessageBox::warning(m_host, QStringLiteral("截图"),
                             QStringLiteral("写文件失败：\n") + path);
        return false;
    }
    m_saveDir = QFileInfo(path).absolutePath();
    return true;
}

void Screenshot::pinResult(const QRectF &sel, const QVariantList &texts) {
    const QImage image = compose(sel, texts);
    if (image.isNull())
        return;

    /*
     * 就钉在选区原来的位置上（截图时选的是哪儿，贴出来就在哪儿），
     * 这样"固定桌面"看起来像是把刚框住的那块画面留在了桌面上。
     *
     * 贴图窗口现在是个**能接着改**的窗口（划重点 / 写字 / 翻译），
     * 所以它要拿主引擎建自己的 QML（见 PinWindow 的说明）——
     * 引擎还没挂上时（理论上不会发生：贴图是用户按出来的，那会儿界面早起来了）
     * 就只贴一张静态图，至少不比原来差。
     */
    const QPoint pos(m_screenRect.topLeft() + QPoint(qRound(sel.x()), qRound(sel.y())));
    auto *pin = new PinWindow(image, pos, m_engine, this);
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

/*
 * 一块贴图自己没了（用户关的、或者窗口被拆）：把它从清单里划掉。
 *
 * 为什么要这一下：m_pins 里存的是 QPointer，本来就会自己变空，但那份清单会
 * 一直长（贴一百次就有一百个空条目）。窗口析构时回头喊一声最省事 ——
 * 反正它手里就有 this。
 */
void Screenshot::forgetPin(QWidget *pin) {
    for (int i = m_pins.size() - 1; i >= 0; --i) {
        if (m_pins.at(i).isNull() || m_pins.at(i).data() == pin)
            m_pins.removeAt(i);
    }
}

QWidget *Screenshot::lastPinned() const {
    for (int i = m_pins.size() - 1; i >= 0; --i) {
        if (!m_pins.at(i).isNull())
            return m_pins.at(i).data();
    }
    return nullptr;
}

QObject *Screenshot::overlayRoot() const {
    if (!m_overlay)
        return nullptr;
    return m_overlay->rootObject();
}

bool Screenshot::overlayVisible() const {
    /*
     * "用户看得见"而不是"窗口映射了"：抓屏时窗口先在**幕外**露面画一帧
     * （见 showOverlay），那一段窗口是可见的但人在屏幕上看不到 —— 而调用方
     * （eventFilter 里那条"窗口还没露脸，这一下 Esc 算取消"）要的就是这个意思。
     */
    return m_overlay && m_overlay->isVisible() && m_overlayOnScreen;
}

QImage Screenshot::imageForId(const QString &id) const {
    /* "full<serial>"：序号只是用来绕开 QQuickPixmapCache 的，取图时忽略 */
    if (id.startsWith(QLatin1String("full")))
        return m_shot;

    /*
     * "zoom<serial>-<设备x>-<设备y>-<半宽>-<半高>-<放大倍数>"：取色放大镜那一小块。
     * 最近邻（FastTransformation）是重点 —— 要的就是像素方块看得清，
     * 平滑插值会把边界糊成一片，反而看不出这格到底是什么色。
     */
    if (id.startsWith(QLatin1String("zoom"))) {
        const QStringList f = id.mid(4).split(QLatin1Char('-'));
        if (f.size() != 6 || m_shot.isNull())
            return QImage();
        bool okX = false, okY = false, okHX = false, okHY = false, okS = false;
        const int dx = f.at(1).toInt(&okX);
        const int dy = f.at(2).toInt(&okY);
        const int hx = f.at(3).toInt(&okHX);
        const int hy = f.at(4).toInt(&okHY);
        const int scale = f.at(5).toInt(&okS);
        if (!okX || !okY || !okHX || !okHY || !okS || hx < 1 || hy < 1 || scale < 1)
            return QImage();
        /* 含中心那一格：横 2*hx+1、纵 2*hy+1 个源像素 */
        const QRect src(dx - hx, dy - hy, hx * 2 + 1, hy * 2 + 1);
        if (!m_shot.rect().contains(src))
            return QImage();
        return m_shot.copy(src)
            .scaled(src.width() * scale, src.height() * scale,
                    Qt::KeepAspectRatioByExpanding, Qt::FastTransformation);
    }
    return QImage();
}
