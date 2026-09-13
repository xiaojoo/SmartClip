#include "Screenshot.h"
#include "LightMenu.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QColorDialog>
#include <QCoreApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
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
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>
#include <cmath>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

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

        /*
         * 白底。全局调色板是深色那套，QMenu 默认跟着走 —— 这个菜单是"贴图小窗"
         * 自己的操作菜单，和托盘那个一样要浅色。样式表和托盘菜单共用一份，
         * 见 src/LightMenu.h。
         */
        applyLightMenuStyle(&menu);

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

Screenshot::~Screenshot() {
    /* 选区窗口是复用的一直留着（见 prewarm），退场时自己收 */
    delete m_overlay;
    m_overlay = nullptr;
}

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
            if (m_pending || (m_active && !overlayVisible())) {
                cancelCapture();
                return true;   /* 吃掉，别让它再下去触发系统提示音 */
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

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
 * 内容用 QQuickWidget 装 —— 和主窗口一样，挂的是**同一个引擎**
 * （见 setEngine 的说明），所以 QML 里直接用 Shot 这个单例。
 *
 * 窗口是**复用**的（见 prewarm）：建好之后一直留着，抓屏时只换图 + show。
 */
QWidget *Screenshot::createOverlay() {
    auto *overlay = new QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint
                                             | Qt::WindowStaysOnTopHint);
    overlay->setWindowTitle(QStringLiteral("SmartClip 截图"));

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
        delete overlay;
        return nullptr;
    }

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
    return overlay;
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
        m_overlay->setAttribute(Qt::WA_ShowWithoutActivating, true);
        m_overlay->setGeometry(QRect(-rect.width() - 10, 0, rect.width(), rect.height()));
        m_overlay->show();
        if (auto *view = m_overlay->findChild<QQuickWidget *>()) {
            view->grabFramebuffer();
            view->repaint();
        }
        m_overlay->hide();
        m_overlay->setAttribute(Qt::WA_ShowWithoutActivating, false);
        m_overlay->setGeometry(rect);   /* 摆回主屏，第一次抓屏直接就用它 */
        m_overlayWarmed = true;
    }
    if (auto *view = m_overlay->findChild<QQuickWidget *>())
        view->grabFramebuffer();
}

void Screenshot::showOverlay() {
    if (!m_overlay)
        m_overlay = createOverlay();
    if (!m_overlay) {
        m_active = false;
        m_shot = QImage();
        restoreHost();
        emit stateChanged();
        return;
    }

    /* 复用同一个窗口：先摆到这块屏上，再把上一次的标注清干净 */
    m_overlay->setGeometry(m_screenRect);
    /*
     * 把屏幕矩形**直接告诉 QML**，别让它按控件尺寸猜 —— setGeometry 之后
     * QQuickWidget 的布局是延迟生效的，复位那一刻它还是预热时的旧尺寸，
     * 于是第一两帧会按一个小方框画（用户看到的"闪一下方框轮廓"）。
     */
    if (QObject *root = overlayRoot())
        QMetaObject::invokeMethod(root, "resetForCapture",
                                  Q_ARG(QVariant, QVariant::fromValue(QRectF(m_screenRect))));

    /*
     * show() 之前要先**强制同步渲染一帧**，这一步不能省。
     *
     * QQuickWidget 的帧是在渲染线程上异步出的：窗口重新显示时，第一帧会先呈现
     * **上一次渲染好的那张** —— 也就是上一轮抓屏时用户框的那块选区，于是"上次的
     * 截图框轮廓"又闪一下（实测：第二次抓屏时旧框左边线会亮 3 帧、约 14ms）。
     * grabFramebuffer() 走一次完整渲染，把这帧换成新的；之后 show() 呈现的
     * 就是干净的全屏选区。
     *
     * 但这次同步渲染有代价，而且代价正好落在"取消"这条路上（真机插桩量出来的，
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
    if (auto *view = m_overlay->findChild<QQuickWidget *>())
        view->grabFramebuffer();

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

    /* 上面那一下要是把它取消了（用户按 Esc 取消），窗口就别露了 */
    if (!m_active) {
        return;
    }

    m_overlay->show();
    m_overlay->raise();
    m_overlay->activateWindow();
    if (auto *view = m_overlay->findChild<QQuickWidget *>())
        view->setFocus();

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
    QTimer::singleShot(60, this, [this]() {
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
        if (auto *view = m_overlay->findChild<QQuickWidget *>()) {
            view->grabFramebuffer();
            view->repaint();
        }
        m_overlay->hide();      /* 只是藏起来，留着下次复用（见 prewarm） */
    }

    m_active = false;
    m_shot = QImage();
    emit stateChanged();
}

void Screenshot::cancelCapture() {
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

bool Screenshot::overlayVisible() const {
    return m_overlay && m_overlay->isVisible();
}

QImage Screenshot::imageForId(const QString &id) const {
    /* "full<serial>"：序号只是用来绕开 QQuickPixmapCache 的，取图时忽略 */
    if (id.startsWith(QLatin1String("full")))
        return m_shot;
    return QImage();
}
