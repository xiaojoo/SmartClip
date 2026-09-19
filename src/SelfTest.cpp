#include "SelfTest.h"

#include "ClipboardStore.h"
#include "EditorViewItem.h"
#include "PinOcr.h"
#include "PinWindow.h"
#include "Screenshot.h"
#include "Speech.h"
#include "Translate.h"
#include <QMessageBox>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QPointingDevice>
#include <QPointer>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QWidget>
#include <QWindow>

#include "WindowHelper.h"

#include "EditorController.h"
#include "TrayIcon.h"

#include <QApplication>
#include <QClipboard>
#include <QAction>
#include <QIcon>
#include <QColor>
#include <QMenu>
#include <QDate>
#include <QDateTime>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QImage>
#include <QPalette>
#include <QPoint>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QMetaObject>
#include <QPainter>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <thread>
#include <QVariant>
#include <QWidget>
#include <QWindow>
#include <cstdio>
#include <functional>

#if defined(Q_OS_WIN)
#  include <windows.h>
/*
 * windows.h（经 rpcndr.h）里 small 是给 MIDL 用的宏（等于 char），
 * 这个文件里有个变量就叫 small —— 不 undef 会报一串莫名其妙的语法错误。
 */
#  undef small
#endif

namespace {

int gPassed = 0;
int gFailed = 0;

QTextStream &out() {
    static QTextStream stream(stdout);
    return stream;
}

void check(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++gPassed;
        out() << "  ok    " << what << Qt::endl;
    } else {
        ++gFailed;
        out() << "  FAIL  " << what;
        if (!detail.isEmpty())
            out() << "   [" << detail << "]";
        out() << Qt::endl;
    }
}

/*
 * Scintilla 的颜色是 0x00BBGGRR（BGR 打包），这里按同样的规则打包 / 还原，
 * 免得断言里写 #1e1f22 这种"看起来对"的值（见 EditorViewItem.cpp 的 scColor）。
 */
int packed(int r, int g, int b) { return (b << 16) | (g << 8) | r; }
int unpacked(int v) { return ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff); }
QString readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray bytes = file.readAll();
    file.close();
    return QString::fromUtf8(bytes);
}

QByteArray readBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    const QByteArray bytes = file.readAll();
    file.close();
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok;
}

/*
 * 往一个 QML 场景里送一次**真的**鼠标左键点击（按下 + 抬起，走 Qt 那条投递链：
 * 命中测试 -> 抢到鼠标的那个 MouseArea -> onClicked）。
 *
 * 为什么要真事件：直接调 QML 函数只能验到"我们自己的逻辑对不对"，验不到
 * **"点到底落没落到那个控件上"** —— 用户报的"菜单里那两行点了没反应"就是后者
 * （上面盖着一层、命中测试没到它、MouseArea 拿不到事件，都会长得一样）。
 *
 * 为什么直接送给 QQuickWindow、而不是那个 QQuickWidget：贴图那个 QQuickWidget 在
 * 自检里没被"暴露"（进程内 qWarning 量到 offscreenWindow 的 isVisible()==0），
 * 送给控件的事件会被它按"窗口不可见"丢掉 —— 谁都点不着，那这个检查就白量了。
 * 送给场景窗口是同一个投递链（命中测试 / 抓取 / onClicked 都在场景这一层），
 * 只少了"操作系统把事件送进 Qt"那一步。
 *
 * 先送一次不带键的移动：命中测试得先知道鼠标在哪儿（真实使用里鼠标总会先划过去）。
 */
void clickScene(QQuickWindow *window, const QPoint &pos) {
    if (!window)
        return;
    const QPointingDevice *device = QPointingDevice::primaryPointingDevice();
    const QPointF local(pos);
    const QPointF global = window->mapToGlobal(pos);
    QMouseEvent move(QEvent::MouseMove, local, local, global, Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier, device);
    QMouseEvent press(QEvent::MouseButtonPress, local, local, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier, device);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local, global, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier, device);
    QCoreApplication::sendEvent(window, &move);
    QCoreApplication::sendEvent(window, &press);
    QCoreApplication::sendEvent(window, &release);
}

/*
 * 往一个 QML 场景里送一次**真的**鼠标悬停（HoverMove）或"离开"（HoverLeave）。
 *
 * 为什么要真事件：工具条现在"鼠标进来才露、一离开就收"，而这套是靠贴图窗口根上的
 * HoverHandler 喂的 —— 直接改 root.hovered 只能验到"我改了它会跟着变"，验不到
 * "鼠标真进来/真离开时它变不变"（一个条件写反，界面上就是"工具条永远不出现"）。
 *
 * 和 clickScene 一样送给场景窗口（QQuickWidget 在自检里没被暴露，事件会被控件
 * 按"窗口不可见"丢掉）。
 */
void hoverScene(QQuickWindow *window, const QPoint &pos, bool inside) {
    if (!window)
        return;
    const QPointF local(pos);
    if (!inside) {
        /*
         * "鼠标走了"要送两下：HoverLeave 是一般的悬停离开，Leave 是窗口级的
         * "指针不在这块上了"。只送前者实测不生效（HoverHandler 还是 hovered=true，
         * 工具条就不收）—— 真实鼠标移出窗口时平台两下都会来。
         */
        QHoverEvent leave(QEvent::HoverLeave, local, window->mapToGlobal(pos), local);
        QCoreApplication::sendEvent(window, &leave);
        QEvent gone(QEvent::Leave);
        QCoreApplication::sendEvent(window, &gone);
        return;
    }
    QHoverEvent move(QEvent::HoverMove, local, window->mapToGlobal(pos), local);
    QCoreApplication::sendEvent(window, &move);
}

/*
 * 只送"按下 + 抬起"，**不带**前面那一下移动。
 *
 * 用来量"这个键这会儿到底吃不吃这一下"：clickScene 会先送一次不带键的移动，而那一下
 * 会把 HoverHandler 的 hovered 变成 true（工具条于是露出来、又变成可点的），
 * 想量"收起来的时候吃不吃"就量不出来了。
 */
void clickSceneNoHover(QQuickWindow *window, const QPoint &pos) {
    if (!window)
        return;
    const QPointingDevice *device = QPointingDevice::primaryPointingDevice();
    const QPointF local(pos);
    const QPointF global = window->mapToGlobal(pos);
    QMouseEvent press(QEvent::MouseButtonPress, local, local, global, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier, device);
    QMouseEvent release(QEvent::MouseButtonRelease, local, local, global, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier, device);
    QCoreApplication::sendEvent(window, &press);
    QCoreApplication::sendEvent(window, &release);
}

/*
 * ======================================================================
 * 弹窗"露出来之后不许再变"的量具
 * ======================================================================
 *
 * 为什么要有它：弹窗"闪一下"的根，几乎都是**显出来之后窗口又被改了一刀** ——
 * 改尺寸 / 改位置 / 改窗口标志。在 Windows 上这三件事都不是原子的：系统会先拿
 * 窗口上**上一次的那张画面**按新样子合成一帧，Qt 下一帧才画新内容。用户看到的
 * 就是"闪一下 / 抖一下 / 像重新出现了一次"。
 *
 * （这个结论不是猜的，是工程里三处实测攒出来的：
 *   * 便签菜单为它拆成了两块窗口 —— qml/notes/NoteMenu.qml 那段"一步挪到位 /
 *     分帧挪都躲不掉"的记录；
 *   * 双击标题栏最大化改成"几何一次到位 + 界面淡入" —— src/WindowHelper.h 开头；
 *   * QtWidgets 那些输入框关掉 DWM 淡入 —— src/DialogStyle.h 的 applyDarkTitleBar。）
 *
 * 所以规矩只有一条：
 *
 *   **它第一次露出来的那一帧，就必须是它最终的样子。**
 *
 * 量法：从"第一次可见"那一帧起，一帧一帧往下采（原生窗口的几何 + 窗口标志），
 * 中途任何一帧和第一帧不一样就是违规，detail 里报"第一帧 -> 变掉那一帧"。
 *
 * 为什么采**原生窗口**而不是 QML 属性：闪的是原生窗口。这个工程里"属性对、
 * 屏幕上不对"栽过不止一次（见 DocCard.qml 里那三条"自检绿了但眼睛一看不对"）。
 *
 * 为什么 windowOf 是个每次现问的函数：弹窗的原生窗口是 open() 那一刻才建/映射的
 * （Screenshot.cpp 的 prewarm 那段分析过为什么），先问一次拿到空指针就白量了。
 */
struct SurfaceShot {
    bool valid = false;
    bool visible = false;
    QRect geo;
    Qt::WindowFlags flags;
};

SurfaceShot shootSurface(QQuickWindow *window) {
    SurfaceShot shot;
    if (!window)
        return shot;
    shot.valid = true;
    shot.visible = window->isVisible();
    shot.geo = window->geometry();
    shot.flags = window->flags();
    return shot;
}

QString surfaceShotText(const SurfaceShot &shot) {
    if (!shot.valid)
        return QStringLiteral("没有原生窗口");
    return QStringLiteral("%1x%2@(%3,%4)")
        .arg(shot.geo.width())
        .arg(shot.geo.height())
        .arg(shot.geo.x())
        .arg(shot.geo.y());
}

/*
 * 从"第一次可见"起连采 samples 帧，要求每一帧都和第一帧一模一样。
 *
 * 返回 true = 稳。detail 给"第一帧（连采 N 帧没变）"或者"第一帧 -> 变掉那帧"。
 * 采不到（一直没露出来 / 露出来得太晚，一帧都没跟上）算失败 —— 那种情况下
 * "没看见它变"没有意义，不能当通过。
 */
bool surfaceStaysPut(const std::function<QQuickWindow *()> &windowOf, int samples,
                     QString *detail) {
    SurfaceShot first;
    int taken = 0;
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 900) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 15);
        QThread::msleep(6);
        const SurfaceShot now = shootSurface(windowOf());
        if (!now.valid)
            continue;                     /* 原生窗口还没建出来 / 还没找到 */
        if (!now.visible)
            continue;                     /* 还没露出来：从"可见"那一帧才算起 */
        if (!first.valid) {
            first = now;                  /* 第一帧：这就是它的"最终样子" */
            continue;
        }
        if (now.geo != first.geo || now.flags != first.flags) {
            if (detail)
                *detail = QStringLiteral("%1 -> %2")
                              .arg(surfaceShotText(first), surfaceShotText(now));
            return false;
        }
        if (++taken >= samples)
            break;
    }
    if (!first.valid) {
        if (detail)
            *detail = QStringLiteral("一直没露出来（或露得太晚，一帧都没跟上）");
        return false;
    }
    if (detail)
        *detail = QStringLiteral("%1（连采 %2 帧没变）")
                      .arg(surfaceShotText(first))
                      .arg(taken + 1);
    return taken >= 1;
}

/*
 * 这个原生窗口的几个关键样式位 —— 白帧查到最后，就靠它判"这块窗到底还是不是
 * 分层（layered）窗口"：分层窗口的"空一帧"是**透出底下的东西**，不透明窗口的
 * "空一帧"只会露自己的底色。分不清这两者，就会一直在"谁画了白色"上打转。
 */
QString nativeStyleText(QWidget *widget) {
#if defined(Q_OS_WIN)
    if (!widget)
        return QString();
    HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    if (!hwnd)
        return QString();
    const LONG_PTR ex = GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    const LONG_PTR st = GetWindowLongPtr(hwnd, GWL_STYLE);
    return QStringLiteral(" 分层=%1 整窗透明=%2 最小化=%3 系统可见=%4 Qt透明属性=%5")
        .arg((ex & WS_EX_LAYERED) ? 1 : 0)
        .arg((ex & WS_EX_TRANSPARENT) ? 1 : 0)
        .arg((st & WS_MINIMIZE) ? 1 : 0)
        .arg(IsWindowVisible(hwnd) ? 1 : 0)
        .arg(widget->testAttribute(Qt::WA_TranslucentBackground) ? 1 : 0);
#else
    Q_UNUSED(widget);
    return QString();
#endif
}

/*
 * 盯着窗口自己的 Move / Resize 事件，把**几何序列**记下来。
 *
 * 为什么要记序列而不是只数次数：用户报的"最大化时窗口会变到右边、还在放大"
 * 就是**两拍几何**的样子 —— 先按新尺寸待在旧位置上（于是界面看着往右挪、还变大），
 * 下一拍才挪到 (0,0)。只数"几拍"看不出这个顺序，记下来一眼就清楚了。
 */
class GeometryTally : public QObject {
public:
    int moves = 0;
    int resizes = 0;
    QStringList sequence;

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Move && event->type() != QEvent::Resize)
            return false;
        if (event->type() == QEvent::Move)
            ++moves;
        else
            ++resizes;
        if (sequence.size() < 12) {
            if (auto *widget = qobject_cast<QWidget *>(watched)) {
                const QRect g = widget->geometry();
                const QString entry = QStringLiteral("%1x%2@%3,%4")
                                          .arg(g.width()).arg(g.height())
                                          .arg(g.x()).arg(g.y());
                /* 同一个几何连着来两下（Move+Resize 一对）只记一次 */
                if (sequence.isEmpty() || sequence.last() != entry)
                    sequence << entry;
            }
        }
        return false;
    }
};

/*
 * 屏幕上一块区域里"近白像素"的个数和平均亮度。
 *
 * 为什么要抓**屏幕**、不抓 grab() 出来的控件图：白是"窗口刚变大、Qt 还没画到
 * 那一块"的时候露出来的 —— 那是**合成层**的事。控件自己 render 出来的图里
 * 根本没有这一帧（它画的时候已经画满了）。这个工程里"属性对、屏幕上不对"栽过
 * 不止一次（见 DocCard.qml 里那三条），所以这里照 Screenshot 那套抓屏。
 */
struct ScreenProbe {
    int white = 0;      /* 近白采样点数 */
    double lum = 0.0;   /* 采样点平均亮度（0~255） */
    double meanR = 0.0, meanG = 0.0, meanB = 0.0;   /* 平均色（看"这一块画的是什么"） */
    int samples = 0;
    QString where;      /* 白点大致在哪儿（给 detail 用） */
    QImage shot;        /* 抓到的这一帧（要存下来看的时候用） */
};

/*
 * 抓屏幕上一块矩形，数近白像素和平均亮度。clip 非空时只统计落在里面的点。
 *
 * step 是采样步长（逻辑像素）。为什么要有它就说明白一件事：
 * **抓 4K 整屏一次要 70~80ms**（实测），采样间隔就等于它 —— 一闪而过的那一两帧
 * 根本抓不着。所以量"闪"的时候抓的是主窗口里一条 800x400 的小条（几毫秒一张），
 * 只有"要存图看看白的是什么"的时候才去抓整屏。
 */
ScreenProbe probeRect(QScreen *screen, const QRect &rect, const QRect &clip, int step) {
    ScreenProbe probe;
    if (!screen || rect.isEmpty())
        return probe;
    const QImage shot =
        screen->grabWindow(0, rect.x(), rect.y(), rect.width(), rect.height()).toImage();
    if (shot.isNull())
        return probe;
    probe.shot = shot;

    /*
     * 抓回来的是**设备像素**（高 DPI 屏上比逻辑像素大一档），
     * 而 rect / clip 是逻辑像素 —— 换算一次，别拿逻辑坐标去索引设备像素。
     */
    const qreal dpr = shot.devicePixelRatio() > 0 ? shot.devicePixelRatio() : 1.0;
    const int px = qMax(1, qRound(step * dpr));

    int minX = 1 << 30, minY = 1 << 30, maxX = -1, maxY = -1;
    double lum = 0.0;
    double sumR = 0.0, sumG = 0.0, sumB = 0.0;
    int count = 0;
    for (int y = 0; y < shot.height(); y += px) {
        for (int x = 0; x < shot.width(); x += px) {
            if (!clip.isEmpty()) {
                const int sx = rect.x() + qRound(x / dpr);
                const int sy = rect.y() + qRound(y / dpr);
                if (!clip.contains(sx, sy))
                    continue;
            }
            const QRgb p = shot.pixel(x, y);
            const int r = qRed(p), g = qGreen(p), b = qBlue(p);
            lum += 0.299 * r + 0.587 * g + 0.114 * b;
            sumR += r;
            sumG += g;
            sumB += b;
            ++count;
            if (r > 200 && g > 200 && b > 200) {
                ++probe.white;
                minX = qMin(minX, x);
                minY = qMin(minY, y);
                maxX = qMax(maxX, x);
                maxY = qMax(maxY, y);
            }
        }
    }
    probe.samples = count;
    probe.lum = count ? lum / count : 0.0;
    probe.meanR = count ? sumR / count : 0.0;
    probe.meanG = count ? sumG / count : 0.0;
    probe.meanB = count ? sumB / count : 0.0;
    if (probe.white > 0)
        probe.where = QStringLiteral("白点范围（相对取样条）%1,%2 %3x%4")
                          .arg(minX).arg(minY)
                          .arg(maxX - minX + 1).arg(maxY - minY + 1);
    return probe;
}

}  // namespace

bool SelfTest::enabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--self-test"))
            return true;
    }
    return false;
}

int SelfTest::run(QObject *qmlRoot, ClipboardStore *store, Screenshot *shot, TrayIcon *tray,
                  EditorController *cmd, StickyNotes *notes, TranslateCards *cards,
                  LlmClient *llm, Speech *speech) {
    EditorViewItem *view = EditorViewItem::instance();

    /*
     * stdout 不缓冲。
     *
     * 自检是"崩了也要知道崩在哪一步"的工具：走默认的全缓冲时，进程一崩，
     * 最后那几行还压在 CRT 缓冲里，日志里什么都看不到（实测过一次 c0000005，
     * 输出文件是空的，只能上调试器）。这里关掉缓冲，每行立刻落盘。
     * 注意：必须留在**第一条输出之前**（setvbuf 要在流被用过之前调用）。
     */
    setvbuf(stdout, nullptr, _IONBF, 0);

    out() << "SmartClip 自检" << Qt::endl;

    if (!view) {
        out() << "  FAIL  编辑器实例不存在（EditorViewItem::instance() 为空）" << Qt::endl;
        return 1;
    }
    if (!qmlRoot) {
        out() << "  FAIL  QML 根对象为空" << Qt::endl;
        return 1;
    }

    /* 通过 QML 的 dispatch 发命令：工具栏 / 菜单 / 快捷键最后都走这条路 */
    auto dispatch = [qmlRoot](const QString &name) {
        QMetaObject::invokeMethod(qmlRoot, "dispatch", Q_ARG(QVariant, name));
    };

    /* 读 QML 侧的绑定状态（见 Main.qml 的 uiState） */
    auto uiState = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "uiState", Q_RETURN_ARG(QVariant, result));
        return result.toMap();
    };

    /*
     * 读 tab 右键菜单的条目清单（见 Main.qml 的 tabMenuActs，和弹出的是同一份构造）。
     *
     * 第二个参数是**哪一栏**（分栏之后有两条标签栏）：不给就传空 —— Main.qml
     * 那边会退回用"当前那一栏"，也就是不分栏时的唯一那一栏。
     */
    auto tabMenuActs = [qmlRoot](int index, QVariant pane = QVariant()) {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "tabMenuActs", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant(index)),
                                  Q_ARG(QVariant, pane));
        return result.toList();
    };

    /* 读编辑区右键菜单的条目清单（见 Main.qml 的 editMenuActs，同上） */
    auto editMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "editMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    /* 读设置菜单的条目清单（见 Main.qml 的 settingsMenuActs，同上） */
    auto settingsMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "settingsMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    /* 读视图菜单的条目清单（见 Main.qml 的 viewMenuActs，同上） */
    auto viewMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "viewMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    /* 读左侧项目树的状态（见 Main.qml 的 treeState） */
    auto treeState = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "treeState", Q_RETURN_ARG(QVariant, result));
        return result.toMap();
    };

    /*
     * 等布局算完。
     *
     * 布局是**下一帧**才做的：dispatch 改完标志位立刻读 folderTree.width，
     * 拿到的还是上一帧那个槽位宽度（实测"收起面板"后读出 300）。
     * 所以量几何之前先把事件跑一轮 —— 只给标志位做断言是量不出这种毛病的。
     */
    auto settle = []() {
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
    };

    /* 读左树"更多"菜单的条目清单（见 Main.qml 的 treeMenuActs，和弹出的是同一份构造） */
    auto treeMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "treeMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    /* 读标题「项目 ∨」那份"看哪一份"菜单的条目清单（同上，见 treeScopeMenuActs） */
    auto treeScopeMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "treeScopeMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    /*
     * 读左树某一类行的**右键**菜单条目（见 Main.qml 的 treeRowMenuActs）。
     * kind 传 "file" / "folder"：找树里第一个这一类行，用它构造菜单。
     */
    auto treeRowMenuActs = [qmlRoot](const QString &kind) {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "treeRowMenuActs", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant(kind)));
        return result.toList();
    };

    /*
     * 读"便签那一格"右键菜单的条目清单（见 Main.qml 的 notesMenuActs）。
     *
     * 这一格以前弹的是 Qt Quick Controls 的 `Menu`（白底、没图标），现在换成
     * 界面里共用那份 DropdownMenu —— 这里钉的就是"换成共用那份之后条目还是
     * 这四条、动作名还是同一批"。
     */
    auto notesMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "notesMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    QDir dir(QDir::tempPath() + QStringLiteral("/smartclip-selftest"));
    dir.removeRecursively();
    dir.mkpath(QStringLiteral("."));

    const QString srcPath = dir.filePath(QStringLiteral("sample.cpp"));
    const QString outPath = dir.filePath(QStringLiteral("out.cpp"));
    const QString bomPath = dir.filePath(QStringLiteral("bom.txt"));

    /* 正文故意带 CRLF 和中文，覆盖换行符检测与 UTF-8 */
    const QByteArray sample =
        "line one: alpha\r\n"
        "line two: NEEDLE here\r\n"
        "line three: alpha NEEDLE\r\n";
    check(writeFile(srcPath, sample), QStringLiteral("准备测试文件"));

    /* ---- 打开文件 ---- */
    const int opened = view->openFile(srcPath);
    check(opened >= 0, QStringLiteral("openFile() 打开文件"), view->lastError());
    check(view->hasDocument(), QStringLiteral("打开后有当前文档"));
    /*
     * 文件末尾是 CRLF，Scintilla 会把它算成一条空行，所以是 4 行 ——
     * 和所有主流编辑器一致（末尾留一个空行可编辑）。
     */
    check(view->lineCount() == 4, QStringLiteral("行数 = 4（末尾换行算一条空行）"),
          QStringLiteral("实际 %1").arg(view->lineCount()));
    check(view->charCount() == sample.size(),
          QStringLiteral("字符数 = 文件字节数（UTF-8）"),
          QStringLiteral("实际 %1 / 期望 %2").arg(view->charCount()).arg(sample.size()));
    check(view->eolMode() == QLatin1String("CRLF"),
          QStringLiteral("换行符识别为 CRLF"), view->eolMode());
    check(view->language() == QLatin1String("cpp"),
          QStringLiteral("按扩展名识别语言为 cpp"), view->language());
    check(!view->modified(), QStringLiteral("刚打开时没有修改标记"));
    check(view->encoding() == QLatin1String("UTF-8"),
          QStringLiteral("编码识别为 UTF-8"), view->encoding());

    /*
     * 装了语法高亮 lexer 之后，样式字号必须还是 uiFont() 那一份（Scintilla 里是
     * "点 ×100"），既不能变成负值，也不能跟正文/行号栏不一致。
     *
     * 踩过：给 lexer 的字体用 QFont::setPixelSize() 造，pointSizeF() 是 -1，
     * QScintilla 会把它 *100 发给 SCI_STYLESETSIZEFRACTIONAL → 字号变成 -100，
     * 表现就是"一切换语言，正文小到看不见"（见 EditorViewItem::uiFont）。
     */
    check(view->styleSize(0) == view->stylePointSize() && view->styleSize(0) > 0,
          QStringLiteral("cpp 高亮下样式 0 的字号跟正文一致"),
          QStringLiteral("实际 %1（应为 %2）").arg(view->styleSize(0)).arg(view->stylePointSize()));
    check(view->styleSize(1) == view->stylePointSize(),
          QStringLiteral("cpp 高亮下样式 1（注释）的字号跟正文一致"),
          QStringLiteral("实际 %1（应为 %2）").arg(view->styleSize(1)).arg(view->stylePointSize()));
    check(view->styleSize(33) == view->stylePointSize(),   // 33 = STYLE_LINENUMBER
          QStringLiteral("行号栏的字号跟正文一致"),
          QStringLiteral("实际 %1（应为 %2）").arg(view->styleSize(33)).arg(view->stylePointSize()));

    /*
     * 行号栏两条（用户报的）：
     *   ① 「1-10、10-100、1000 以上，栏宽会被撑开」→ 宽度按**固定 4 位**算，
     *      不随行数变（见 EditorViewItem::applyMargins）；
     *   ② 「有些文件序号的行高和正文不一致，点进去还会变」「打开后就明显和上一文档
     *      不一样」→ 根因是样式表里"没设过字体"的格子被按**应用字体**量高度，而行高
     *      取整张表最大的 ascent+descent（见 EditorViewItem::unifyStyleFonts）。下面
     *      三条：栏宽度量、同一文档内各行等高、换两轮语言回来行高不变。
     *
     * 这段自己存/还原正文，别把后面几条要用的 sample 换掉。
     */
    {
        const QString savedText = view->currentText();

        const int widthAt12 = [&] {
            view->setText(QStringLiteral("int a = 1;\r\n").repeated(12));
            return view->marginWidth(0);
        }();
        view->setText(QStringLiteral("int a = 1;\r\n").repeated(1200));
        const int widthAt1200 = view->marginWidth(0);
        check(widthAt12 == widthAt1200 && widthAt12 > 0,
              QStringLiteral("行号栏宽度不随行数撑开（12 行 / 1200 行一样宽）"),
              QStringLiteral("%1 px → %2 px").arg(widthAt12).arg(widthAt1200));

        /* 掺进注释、预处理、字符串：让文档里真的出现好几种样式号 */
        QString mixed;
        for (int i = 1; i <= 60; ++i) {
            if (i % 7 == 0)      mixed += QStringLiteral("// 注释一行 %1\r\n").arg(i);
            else if (i % 5 == 0) mixed += QStringLiteral("#include <memory>\r\n");
            else if (i % 3 == 0) mixed += QStringLiteral("const char *s = \"abc%1\";\r\n").arg(i);
            else                 mixed += QStringLiteral("int value%1 = 0;\r\n").arg(i);
        }
        view->setText(mixed);
        check(view->lineHeightSpread() == 0,
              QStringLiteral("混着注释/预处理/字符串的文件里各行行高一致"),
              QStringLiteral("最高与最矮差 %1 px").arg(view->lineHeightSpread()));

        /*
         * 换两轮语言再换回来，行高必须一点没变（真界面实测到的那条）。
         *
         * QScintilla 每次装卸 lexer 都会 SCI_STYLERESETDEFAULT + SCI_STYLECLEARALL
         * 把整张样式表打回内置默认（**字体族为空**），然后只给"有描述的"样式补回字体
         * （qsciscintilla.cpp 的 setLexer）。markdown 用不到 22 号往后 —— 那些格空着
         * 不是"跟随正文"，Qt 平台会拿**应用字体**画（本机 Microsoft YaHei UI 9pt，
         * ascent+descent+1 = 16px，Consolas 12px 是 15px）。而 Scintilla 的行高 =
         * 整张表里最大的 ascent+descent + 额外行距（ViewStyle::FindMaxAscentDescent，
         * 用没用到都算）→ 那一份文档每一行 17px 变 18px，行号栏按 maxAscent 画数字、
         * 正文按自己样式的 ascent 画字，序号和正文错开 1px。
         *
         * 用户报的两条症状都在这上面：「有些文件序号的行高和正文不一致」+「打开后就
         * 明显和上一文档不一样」（表是不是空的取决于上一步走过哪条路）。
         */
        {
            const int baseHeight = view->textLineHeight();
            const QString mdPath = dir.filePath(QStringLiteral("lineheight.md"));
            const QString pyPath = dir.filePath(QStringLiteral("lineheight.py"));
            writeFile(mdPath, QByteArrayLiteral("# 标题\r\n\r\n正文一行\r\n"));
            writeFile(pyPath, QByteArrayLiteral("def f():\r\n    return 1\r\n"));

            const int mdTab = view->openFile(mdPath);
            const int pyTab = view->openFile(pyPath);
            view->activateDocument(mdTab);   // python → markdown：这一换就把表清成空字体

            check(mdTab >= 0 && pyTab >= 0 && view->language() == QLatin1String("markdown"),
                  QStringLiteral("换回 markdown 标签（用于行高一致性检查）"),
                  QStringLiteral("md 标签 %1 / py 标签 %2，当前语言 %3")
                      .arg(mdTab)
                      .arg(pyTab)
                      .arg(view->language()));
            check(view->offFamilyStyleSlots() == 0,
                  QStringLiteral("换过两轮语言后样式表里没有非正文字体的格"),
                  QStringLiteral("还有 %1 格不是正文字体").arg(view->offFamilyStyleSlots()));
            check(view->textLineHeight() == baseHeight,
                  QStringLiteral("换过两轮语言回来，行高和被换走之前一致"),
                  QStringLiteral("%1 px → %2 px").arg(baseHeight).arg(view->textLineHeight()));

            /* 临时标签按从大到小的下标关掉，别把后面几条要用的 sample 挤位 */
            view->closeDocument(pyTab);
            view->closeDocument(mdTab);
            view->activateDocument(view->indexOfPath(srcPath));
        }

        view->setText(savedText);
    }

    /*
     * "设置里写 12"必须就是"12 像素"，不是 12 点。
     *
     * 踩过：直接用 setPointSize(12) 给 QFont，96 DPI 下渲染出来是 16 像素 ——
     * 设置写着 12，字却比界面上别处的 12 大一圈。换算回像素来卡这一条。
     */

    /*
     * 量字号之前先把行高按回 1.0 倍。
     *
     * 行高倍数存在 QSettings 里，自检启动时 Main.qml 会把它恢复出来 ——
     * 用户上次设成 1.5 倍的话，下面"行高跟字号一个量级"那条就会红
     * （12px 的字量出 23px 的行高）。那条要钉的是"字号单位没被当成点"，
     * 得在没有额外行距的前提下量。用户那份设置在本节末尾还原回去。
     */
    const qreal savedLineHeightFactor = view->lineHeightFactor();
    if (!qFuzzyCompare(savedLineHeightFactor, 1.0)) {
        view->setLineHeightFactor(1.0);
        QCoreApplication::processEvents();
    }

    out() << "        （字号换算：设置 " << view->fontPixelSize() << " px → 实际 "
          << view->fontPixelSizeEffective() << " px；行高 " << view->textLineHeight()
          << " px）" << Qt::endl;
    check(qAbs(view->fontPixelSizeEffective() - double(view->fontPixelSize())) < 0.51,
          QStringLiteral("字号单位是像素（设置 12 = 渲染 12px）"),
          QStringLiteral("设置 %1 px / 实际 %2 px")
              .arg(view->fontPixelSize())
              .arg(view->fontPixelSizeEffective()));

    /*
     * 再卡一道不绕圈的：真渲染出来的行高得跟字号是一个量级。
     *
     * 上面那条"换算回像素"用的是同一份换算，等于自己证明自己；这条量的是
     * Scintilla 按当前字体算出来的行高 —— 12pt 的行高在 20px 上下，12px 的
     * 只有 15~17px。单位再被写错（把像素当点）能当场抓住。
     */
    check(view->textLineHeight() > 0 && view->textLineHeight() <= view->fontPixelSize() + 6,
          QStringLiteral("行高跟字号一个量级（没按点当成像素画大）"),
          QStringLiteral("行高 %1 px / 字号 %2 px")
              .arg(view->textLineHeight()).arg(view->fontPixelSize()));

    /*
     * 行高倍数（设置菜单 / 设置面板里的"行高"）。
     *
     * Scintilla 没有"把行高设成 N 像素"的消息，只有给每行加**额外上下空白**
     * （SCI_SETEXTRAASCENT / SCI_SETEXTRADESCENT，见 ViewStyle::Refresh：
     * lineHeight = maxAscent + maxDescent + extraAscent + extraDescent），
     * 所以按"自然行高 × 倍数"算差额再摊到上下两侧。这里量三件事：
     *   * 1.0 倍时实际行高就是自然行高（没偷偷加空）；
     *   * 1.5 倍确实高了一档，且等于自然行高 × 1.5（取整 ±1px）；
     *   * 调回 1.0 倍又回到自然行高（不是只能往松里走）。
     */
    {
        const int natural = view->naturalLineHeight();
        const int plain = view->lineHeight();
        check(natural > 0 && natural == plain,
              QStringLiteral("行高 1.0 倍 = 字体自带的自然行高（没额外加空）"),
              QStringLiteral("自然 %1 px / 实际 %2 px").arg(natural).arg(plain));

        view->setLineHeightFactor(1.5);
        QCoreApplication::processEvents();
        const int wide = view->lineHeight();
        out() << "        （行高：1.0 倍 " << natural << " px → 1.5 倍 " << wide
              << " px）" << Qt::endl;
        check(qAbs(wide - qRound(natural * 1.5)) <= 1,
              QStringLiteral("行高 1.5 倍按自然行高成比例拉开"),
              QStringLiteral("实际 %1 px（应为 %2 px）").arg(wide).arg(qRound(natural * 1.5)));

        view->setLineHeightFactor(1.0);
        QCoreApplication::processEvents();
        check(view->lineHeight() == natural,
              QStringLiteral("行高调回 1.0 倍后回到自然值（不用重开标签）"),
              QStringLiteral("实际 %1 px（应为 %2 px）").arg(view->lineHeight()).arg(natural));

        /*
         * 菜单里那一组"行高"（档位表在 js/EditorMenus.js 的 kLineHeightFactors）：
         * 档位数、档位边界、以及**点下去真的会改行高** —— 三样一起钉。
         * 条目清单由 Main.qml 的 settingsMenuActs 提供，和点"设置"弹出的那份
         * 是同一个调用，所以断言看到的就是菜单里能点的。
         */
        {
            QStringList factors;
            const QVariantList items = settingsMenuActs();
            for (const QVariant &item : items) {
                const QString act = item.toString();
                if (act.startsWith(QStringLiteral("lineHeight:")))
                    factors << act.mid(int(qstrlen("lineHeight:")));
            }
            check(factors.size() == 7,
                  QStringLiteral("设置菜单里有 7 档行高（含\"跟随字体\"）"),
                  QStringLiteral("实际 %1 档：%2")
                      .arg(factors.size()).arg(factors.join(QLatin1Char('/'))));
            check(factors.value(0) == QStringLiteral("1")
                      && factors.value(1) == QStringLiteral("1.15")
                      && factors.contains(QStringLiteral("1.5"))
                      && factors.contains(QStringLiteral("2.5")),
                  QStringLiteral("档位从 1.0 起、最松 2.5 倍（没有比 1.0 更紧的档）"),
                  factors.join(QLatin1Char('/')));

            /* 点 1.15 倍那一档，走的就是菜单条目的 act */
            dispatch(QStringLiteral("lineHeight:1.15"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 1.15) < 0.001,
                  QStringLiteral("dispatch(lineHeight:1.15) 改到编辑器上了"),
                  QStringLiteral("实际 %1（应为 1.15）").arg(view->lineHeightFactor()));
            check(view->lineHeight() > natural,
                  QStringLiteral("这一档确实比自然行高松"),
                  QStringLiteral("实际 %1 px / 自然 %2 px")
                      .arg(view->lineHeight()).arg(natural));

            /*
             * 设置面板上那两个按钮（"行高 − / 行高 +"）：走 lineHeightUp /
             * lineHeightDown，在档位表里前后走一格 —— 1.15 上一格是 1.3，
             * 再下一格又回到 1.15。
             */
            dispatch(QStringLiteral("lineHeightUp"));
            QCoreApplication::processEvents();
            const qreal up = view->lineHeightFactor();
            check(qAbs(up - 1.3) < 0.001,
                  QStringLiteral("行高 + 走到下一档（1.15 → 1.3）"),
                  QStringLiteral("实际 %1").arg(up));
            dispatch(QStringLiteral("lineHeightDown"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 1.15) < 0.001,
                  QStringLiteral("行高 − 走回上一档（1.3 → 1.15）"),
                  QStringLiteral("实际 %1").arg(view->lineHeightFactor()));

            /* 两头要夹住：最松一档再 +、最紧一档再 − 都不许跑出表外 */
            view->setLineHeightFactor(2.5);
            dispatch(QStringLiteral("lineHeightUp"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 2.5) < 0.001,
                  QStringLiteral("最松一档再按\"行高 +\"就停在原地（不越界）"),
                  QStringLiteral("实际 %1").arg(view->lineHeightFactor()));
            view->setLineHeightFactor(1.0);
            dispatch(QStringLiteral("lineHeightDown"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 1.0) < 0.001,
                  QStringLiteral("最紧一档（跟随字体）再按\"行高 −\"也停在原地"),
                  QStringLiteral("实际 %1").arg(view->lineHeightFactor()));
        }

        /* 自检不该把用户调好的行高改掉，最后统一还原 */
        view->setLineHeightFactor(savedLineHeightFactor);
        QCoreApplication::processEvents();
    }

    /*
     * 注释可以单独设字号（设置菜单里的"注释字号"）。
     *
     * 注释样式是 lexer 按 description 里的 "comment" 单独标出来的，所以能给它
     * 一套自己的字号 —— 这里设成 10px，注释样式(1) 应该变成 10px 的点数，
     * 而正文样式(0) 一个像素都不许动。
     */
    {
        view->setCommentFontPixelSize(10);
        QCoreApplication::processEvents();
        const int commentStyle = view->styleSize(1);
        const int bodyStyle = view->styleSize(0);
        const int wantComment = view->styleCommentPointSize();

        out() << "        （注释字号 10px → 样式 " << commentStyle << "（应为 " << wantComment
              << "）；正文样式 " << bodyStyle << "）" << Qt::endl;

        check(commentStyle == wantComment && bodyStyle == view->stylePointSize(),
              QStringLiteral("注释字号能单独设（正文不动）"),
              QStringLiteral("注释 %1 / 正文 %2").arg(commentStyle).arg(bodyStyle));
        check(commentStyle < bodyStyle,
              QStringLiteral("注释字号确实变小了（10px < 12px）"),
              QStringLiteral("注释 %1 / 正文 %2").arg(commentStyle).arg(bodyStyle));

        view->setCommentFontPixelSize(0);
        QCoreApplication::processEvents();
        check(view->styleSize(1) == view->stylePointSize(),
              QStringLiteral("注释字号设回 0 后跟着正文"),
              QStringLiteral("注释 %1 / 正文 %2")
                  .arg(view->styleSize(1)).arg(view->stylePointSize()));
    }

    /* 字体家族能改（"中英文分开设字体"做不到，但能整体换成中英都覆盖的等宽字体） */
    {
        const QString original = view->fontFamily();
        /*
         * 家族名必须用**英文**（NSimSun）：QScintilla 转发给 Scintilla 时走的是
         * QFont::family().toLatin1()，中文名会被压成 "???" —— 这条自检就是按
         * 这个坑写的（正文样式由 lexer 转发，行号栏样式是我们自己发 toUtf8）。
         */
        view->setFontFamily(QStringLiteral("NSimSun"));
        QCoreApplication::processEvents();
        check(view->styleFontName(0) == QStringLiteral("NSimSun")
              && view->styleFontName(33) == QStringLiteral("NSimSun"),   // 33 = STYLE_LINENUMBER
              QStringLiteral("字体家族改得动（正文和行号栏一起换）"),
              QStringLiteral("实际 '%1' / '%2'")
                  .arg(view->styleFontName(0), view->styleFontName(33)));

        view->setFontFamily(original);
        QCoreApplication::processEvents();
        check(view->styleFontName(0) == original,
              QStringLiteral("字体家族改回原样"), view->styleFontName(0));
    }

    /*
     * 行号栏不能变白。
     *
     * SC_MARGIN_NUMBER 的背景取的是 STYLE_LINENUMBER 的 paper，而装 lexer 时
     * QScintilla 的 detachLexer() 会 SCI_STYLECLEARALL 把它刷回默认（白底黑字）——
     * 实测过"一切换语言，行号栏变成一条白带"。所以装完 lexer 必须重刷。
     */
    check(view->styleBack(33) == packed(0x1e, 0x1f, 0x22),   // 33 = STYLE_LINENUMBER
          QStringLiteral("装 lexer 后行号栏底色仍是编辑区底色（不是白的）"),
          QStringLiteral("实际 #%1").arg(unpacked(view->styleBack(33)), 6, 16, QLatin1Char('0')));
    check(view->marginBack(0) == packed(0x1e, 0x1f, 0x22)
          && view->marginBack(1) == packed(0x1e, 0x1f, 0x22),
          QStringLiteral("边距背景也压成编辑区底色（行号栏 / 折叠栏）"),
          QStringLiteral("margin0=#%1 margin1=#%2")
              .arg(view->marginBack(0), 6, 16, QLatin1Char('0'))
              .arg(view->marginBack(1), 6, 16, QLatin1Char('0')));

    /* 折叠边距在（第 1 列有宽度，夹在行号和分隔线之间） */
    check(view->marginWidth(1) > 0,
          QStringLiteral("折叠边距已启用（第 1 列有宽度，在行号右边）"),
          QStringLiteral("实际 %1").arg(view->marginWidth(1)));

    /*
     * 折叠栏宽度 = 箭头宽 + 左右各 5px。
     *
     * 位图标记是画在边距正中的（PlatQt.cpp 的 DrawXPM），所以"左右各 5px"落到
     * 宽度上就是 图标边长 + 10；曾经 14px 的默认宽度（QScintilla 那个
     * defaultFoldMarginWidth）就是在这里被卡住的。
     */
    {
        const int expected = view->foldIconSize() + 10;
        check(qAbs(view->marginWidth(1) - expected) <= 1,
              QStringLiteral("折叠栏宽度 = 尖括号宽 + 左右各 5px"),
              QStringLiteral("图标 %1 → 期望 %2，实际 %3")
                  .arg(view->foldIconSize()).arg(expected).arg(view->marginWidth(1)));
    }

    /*
     * 尖括号的方向：折叠态向右 "›"、展开态向下 "⌄"。
     *
     * 位图标记的形状没法从 Scintilla 那边读回来，所以直接量自己画的那两张图：
     * 向右的竖着比横着长，向下的横着比竖着长。方框标记（原来那套 +/- 方块）
     * 在这里是正方的，这条能当场抓住"又退回方框了"。
     */
    {
        const QVariantList icon = view->foldIconPixelStats();
        const int cw = icon.value(0).toInt(), ch = icon.value(1).toInt();
        const int ow = icon.value(2).toInt(), oh = icon.value(3).toInt();
        out() << "        （折叠箭头墨迹：折叠态 " << cw << "x" << ch
              << "，展开态 " << ow << "x" << oh << "）" << Qt::endl;
        check(cw > 0 && ch > cw,
              QStringLiteral("折叠状态画成向右的尖括号 ›（竖着比横着长）"),
              QStringLiteral("实际 %1x%2").arg(cw).arg(ch));
        check(ow > oh,
              QStringLiteral("展开状态画成向下的尖括号 ⌄（横着比竖着长）"),
              QStringLiteral("实际 %1x%2").arg(ow).arg(oh));
    }

    /*
     * 紧贴正文左边那条分隔竖线（第 2 条边距，也是最后一条）。
     *
     * 宽度 1px、底色是**分隔色**而不是编辑区底色 —— 边距背景整列一次填满，
     * 所以它是一条从顶到底的竖线。放在最后一条边距上，正文左边缘就在它右边
     * （正文左边缘 = 各条边距宽之和 + 左留白），也就是线和正文之间不再有空档。
     */
    check(view->marginWidth(2) == 1 && view->marginBack(2) == packed(0x33, 0x38, 0x40),
          QStringLiteral("正文左边有 1px 分隔竖线（第 2 列，正文紧贴着它）"),
          QStringLiteral("宽 %1 / 底色 #%2")
              .arg(view->marginWidth(2))
              .arg(unpacked(view->marginBack(2)), 6, 16, QLatin1Char('0')));

    /*
     * 这两条竖线的开关和列号都存在 QSettings 里，自检启动时 Main.qml 会把用户
     * 上次设的那份恢复出来（实测：用户把参考线点成 120 字之后，"默认 80"那条
     * 断言就红了 —— 量到的是用户的设置，不是出厂值）。所以下面先把用户那份存
     * 起来，用一组确定的值跑断言，最后原样放回去：自检不改用户的设置。
     */
    const bool savedGutterLine = view->gutterLineVisible();
    const bool savedRulerVisible = view->rulerVisible();
    const int savedRulerColumn = view->rulerColumn();
    view->setGutterLineVisible(true);
    view->setRulerVisible(true);
    view->setRulerColumn(80);
    QCoreApplication::processEvents();

    dispatch(QStringLiteral("toggleGutterLine"));
    check(view->marginWidth(2) == 0,
          QStringLiteral("dispatch(toggleGutterLine) 把分隔线关掉"),
          QStringLiteral("实际宽 %1").arg(view->marginWidth(2)));
    dispatch(QStringLiteral("toggleGutterLine"));
    check(view->marginWidth(2) == 1 && view->gutterLineVisible(),
          QStringLiteral("再切一次分隔线回来"));

    /*
     * 字数参考线（"一行 80 字"那条竖线）。
     *
     * 两件事一起钉：Scintilla 那边的 edge 状态（模式 / 列号 / 颜色），以及
     * **画出来的像素位置** —— 抓图里扫那条线，跟"列号 × 空格宽 + 正文左边缘"
     * 对一下。只看列号的话，"消息发下去了但线没画出来"是查不到的。
     */
    {
        /*
         * 量像素得有文档：一个标签都没开时视图挂的是 scratch 占位文档、编辑区
         * 不显示，抓出来的图是空的。这里自己开一个；后面那些断言用的 before
         * 计数在更靠后的位置取，不受影响。
         */
        if (!view->hasDocument())
            dispatch(QStringLiteral("new"));

        /*
         * 分隔线"真的画出来了"这一条要用像素说话：它占的是第 1 条边距的那 1 像素，
         * 底色是分隔色，边距背景整列填满，所以抓图里应该数得到接近控件高度那么多个
         * 点；关掉开关就一个都不剩。
         */
        {
            const QVariantList on = view->marginPixelStats();
            dispatch(QStringLiteral("toggleGutterLine"));
            const QVariantList off = view->marginPixelStats();
            dispatch(QStringLiteral("toggleGutterLine"));
            out() << "        （分隔竖线像素：开着 " << on.value(3).toInt() << "（第 "
                  << on.value(10).toInt() << " 列起）/ 关掉 " << off.value(3).toInt()
                  << "；三条边距宽 " << on.value(7).toInt() << "+" << on.value(8).toInt()
                  << "+" << on.value(9).toInt()
                  << "；线到正文 " << on.value(11).toInt() << " px）" << Qt::endl;
            check(on.value(3).toInt() > 100 && off.value(3).toInt() == 0,
                  QStringLiteral("那条分隔线真的画出来了（关掉就一个像素都没有）"),
                  QStringLiteral("开着 %1 / 关掉 %2")
                      .arg(on.value(3).toInt()).arg(off.value(3).toInt()));

            /*
             * 线和正文之间不能有空档。
             *
             * 这是用户明确提过的那条：折叠栏（14px）原来夹在分隔线和正文之间，
             * 加上 12px 左留白，线和字之间空了 26px。现在折叠栏在线**左边**、
             * 左留白收到 2px，这个距离应该只剩个位数（字形的左侧留白也要算）。
             * 上界给 12 设备像素：26px 那个版本在 125% 缩放下量出来是 30 以上，
             * 这条能当场抓住"折叠栏又跑回线右边"。
             */
            check(on.value(11).toInt() >= 0 && on.value(11).toInt() <= 12,
                  QStringLiteral("分隔线和正文之间没有空档（折叠栏在线左边）"),
                  QStringLiteral("实测 %1 设备像素（-1 = 没扫到正文墨迹）")
                      .arg(on.value(11).toInt()));
        }

        /*
         * 菜单里得能点到：开关在"视图"菜单，列号档位在"设置"菜单。
         * 断言读的是和弹出来那份同一个构造（Main.qml 的 viewMenuActs /
         * settingsMenuActs），所以"菜单里真的有这一条"是被钉住的。
         */
        {
            QStringList acts;
            for (const QVariant &item : viewMenuActs())
                acts << item.toString();
            check(acts.contains(QStringLiteral("toggleGutterLine"))
                      && acts.contains(QStringLiteral("toggleRuler")),
                  QStringLiteral("视图菜单里有那两条竖线的开关"),
                  acts.join(QLatin1Char('/')));
        }
        {
            QStringList acts, cols;
            for (const QVariant &item : settingsMenuActs()) {
                const QString act = item.toString();
                acts << act;
                if (act.startsWith(QStringLiteral("rulerColumn:")))
                    cols << act.mid(int(qstrlen("rulerColumn:")));
            }
            check(cols.size() == 5 && cols.contains(QStringLiteral("80"))
                      && cols.contains(QStringLiteral("100")),
                  QStringLiteral("设置菜单里有 5 档参考线列号（含 80 与 100）"),
                  cols.join(QLatin1Char('/')));
            check(acts.contains(QStringLiteral("rulerColumnAsk")),
                  QStringLiteral("设置菜单里有\"自定义…\"（弹整数输入框）"));
        }

        check(view->rulerEdgeMode() == 1 && view->rulerEdgeColumn() == 80,
              QStringLiteral("列号设成 80 后 Scintilla 那边就是第 80 列（EDGE_LINE）"),
              QStringLiteral("模式 %1 / 列号 %2")
                  .arg(view->rulerEdgeMode()).arg(view->rulerEdgeColumn()));
        /*
         * 三条竖线一个颜色：分隔线（边距底色）、字数参考线（edge）、缩进参考线
         * （STYLE_INDENTGUIDE 前景色）—— 用户要的"这些竖线跟行号右边那条一样"。
         */
        check(view->rulerEdgeColor() == packed(0x33, 0x38, 0x40)
                  && view->marginBack(2) == packed(0x33, 0x38, 0x40)
                  && view->styleFore(37) == packed(0x33, 0x38, 0x40),   // 37 = STYLE_INDENTGUIDE
              QStringLiteral("分隔线 / 字数参考线 / 缩进参考线是同一个颜色"),
              QStringLiteral("参考线 #%1 / 分隔线 #%2 / 缩进 #%3")
                  .arg(unpacked(view->rulerEdgeColor()), 6, 16, QLatin1Char('0'))
                  .arg(unpacked(view->marginBack(2)), 6, 16, QLatin1Char('0'))
                  .arg(unpacked(view->styleFore(37)), 6, 16, QLatin1Char('0')));

        QVariantList rulerPixels = view->rulerPixelStats();
        out() << "        （参考线：扫到 x=" << rulerPixels.value(0).toInt()
              << "，按 80 字算出来应为 x=" << rulerPixels.value(1).toInt()
              << "，离底边 " << rulerPixels.value(2).toInt() << " px）"
              << Qt::endl;
        check(rulerPixels.value(0).toInt() >= 0
                  && qAbs(rulerPixels.value(0).toInt() - rulerPixels.value(1).toInt()) <= 3,
              QStringLiteral("那条线真的画在第 80 个字的位置上"),
              QStringLiteral("扫到 x=%1 / 应为 x=%2")
                  .arg(rulerPixels.value(0).toInt()).arg(rulerPixels.value(1).toInt()));
        /*
         * 两条竖线都要一直画到控件底边 —— 用户报过"有时候没撑满纵向屏幕"
         * （横条把正文区截短了，线就断在横条上沿）。这一条没有横条，本该是 0。
         */
        check(rulerPixels.value(2).toInt() >= 0 && rulerPixels.value(2).toInt() <= 3,
              QStringLiteral("参考线一直画到编辑区底边"),
              QStringLiteral("离底边 %1 px").arg(rulerPixels.value(2).toInt()));
        {
            const QVariantList m = view->marginPixelStats();
            check(m.value(13).toInt() >= 0 && m.value(13).toInt() <= 3,
                  QStringLiteral("分隔竖线也一直画到编辑区底边"),
                  QStringLiteral("离底边 %1 px").arg(m.value(13).toInt()));
        }

        /* 改列号：菜单里"100 字"那一档，act 就是 "rulerColumn:100" */
        dispatch(QStringLiteral("rulerColumn:100"));
        check(view->rulerColumn() == 100 && view->rulerEdgeColumn() == 100,
              QStringLiteral("dispatch(rulerColumn:100) 改列号"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        rulerPixels = view->rulerPixelStats();
        check(rulerPixels.value(0).toInt() >= 0
                  && qAbs(rulerPixels.value(0).toInt() - rulerPixels.value(1).toInt()) <= 3,
              QStringLiteral("列号改到 100 之后线跟着挪（位置仍然对得上）"),
              QStringLiteral("扫到 x=%1 / 应为 x=%2")
                  .arg(rulerPixels.value(0).toInt()).arg(rulerPixels.value(1).toInt()));

        /*
         * 越界值两道都堵：QML 的 dispatch 直接丢掉不合法的，C++ 的 setter 再夹一道
         * （设置文件是手改得动的，那边不夹的话 0 或者几十万这种值会顶到画面外）。
         */
        dispatch(QStringLiteral("rulerColumn:0"));
        check(view->rulerColumn() == 100,
              QStringLiteral("非法列号（0）被 dispatch 丢掉（要关这条线用开关，不用 0）"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        view->setRulerColumn(0);
        check(view->rulerColumn() == 1, QStringLiteral("C++ 侧把 0 夹到 1"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        view->setRulerColumn(99999);
        check(view->rulerColumn() == 2000, QStringLiteral("C++ 侧把超大值夹到 2000"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        view->setRulerColumn(80);

        /* 关掉之后抓图里要扫不到那条线，开回来又要有 */
        dispatch(QStringLiteral("toggleRuler"));
        check(!view->rulerVisible() && view->rulerEdgeMode() == 0,
              QStringLiteral("dispatch(toggleRuler) 关掉参考线（EDGE_NONE）"),
              QStringLiteral("模式 %1").arg(view->rulerEdgeMode()));
        check(view->rulerPixelStats().value(0).toInt() < 0,
              QStringLiteral("关掉之后抓图里扫不到那条线"),
              QStringLiteral("扫到 x=%1").arg(view->rulerPixelStats().value(0).toInt()));
        dispatch(QStringLiteral("toggleRuler"));
        check(view->rulerVisible() && view->rulerEdgeMode() == 1,
              QStringLiteral("再切一次参考线回来"));

        /* 自检不改用户的设置：把开头存的那份放回去 */
        view->setGutterLineVisible(savedGutterLine);
        view->setRulerVisible(savedRulerVisible);
        view->setRulerColumn(savedRulerColumn);
        QCoreApplication::processEvents();
        out() << "        （用户设置已还原：分隔线 "
              << (savedGutterLine ? "开" : "关") << " / 参考线 "
              << (savedRulerVisible ? "开" : "关") << " / 列号 " << savedRulerColumn
              << "）" << Qt::endl;
    }

    /*
     * 注：这里原本还有"光标行行号高亮"的断言（逐行边距样式 / 强调色样式），
     * 已随功能一起去掉 —— 数字边距不认逐行样式；改用文本边距那条路实测会让
     * QML 侧认不到编辑区对象（改一次字号就"失联"），也没留下。详见
     * EditorViewItem::applyMargins() 里的注释。
     */

    /*
     * 切到"纯文本"之后，正文底色必须还是深色。
     *
     * 踩过（用户报的"选 txt 之后字体全变成白底"）：QScintilla 的 detachLexer()
     * 先发 SCI_STYLERESETDEFAULT —— 那一步把 STYLE_DEFAULT 打回 Scintilla 的内置
     * 默认（白底黑字），紧接着的 SCI_STYLECLEARALL 再把它刷到所有样式号。
     * 装 lexer 的语言不怕：随后 themeLexer() 会把每个样式的底色重设一遍；
     * 纯文本没有 lexer，就没人再压回来 —— 整篇变白底。
     */
    {
        view->setLanguage(QStringLiteral("plain"));
        QCoreApplication::processEvents();
        const int back0 = view->styleBack(0);
        const int back5 = view->styleBack(5);
        const int back17 = view->styleBack(17);
        const int paper = packed(0x1e, 0x1f, 0x22);

        out() << "        （纯文本样式底色 0=#" << QString::number(back0, 16) << " 5=#"
              << QString::number(back5, 16) << " 17=#" << QString::number(back17, 16) << "）"
              << Qt::endl;
        check(back0 == paper && back5 == paper && back17 == paper,
              QStringLiteral("切到纯文本后正文底色仍是深色（不是白底）"),
              QStringLiteral("0=#%1 5=#%2 17=#%3")
                  .arg(back0, 6, 16, QLatin1Char('0'))
                  .arg(back5, 6, 16, QLatin1Char('0'))
                  .arg(back17, 6, 16, QLatin1Char('0')));

        /*
         * 再来一条看画面的：正文区里不许出现纯白像素。
         *
         * 样式表对了不等于画出来就对 —— 用户看到的是"字后面一块白"，所以直接
         * 抓控件图数 #ffffff 附近的像素（正文/底色的混色都在 250 以下）。
         */
        const int whitePx = view->whiteBackgroundPixels();
        out() << "        （纯文本正文区纯白像素 " << whitePx << "）" << Qt::endl;
        check(whitePx == 0, QStringLiteral("切到纯文本后画面里没有白色背景块"),
              QStringLiteral("%1 个纯白像素").arg(whitePx));

        view->setLanguage(QStringLiteral("cpp"));
        QCoreApplication::processEvents();
    }

    /*
     * 再确认一遍"渲染出来"的大小：同一篇正文，装着 lexer 和切成纯文本，行高
     * 得是一个量级。字号被带坏成负值的话，这里会直接塌成 0 或个位数。
     *
     * 允许 ±2px：行高是 Scintilla 按"那一行实际用到的字体"算的，lexer 会给
     * 关键字/预处理行套粗体、注释套斜体，同一字号下这些字形的行高本身就能
     * 差 1px（实测 cpp 15px / 纯文本 16px，属于字体 hinting 的正常抖动）。
     */
    {
        const int withLexer = view->textLineHeight();
        const int guidesWithLexer = view->marginPixelStats().value(12).toInt();
        view->setLanguage(QStringLiteral("plain"));
        QCoreApplication::processEvents();
        const int plain = view->textLineHeight();
        const int guidesPlain = view->marginPixelStats().value(12).toInt();
        view->setLanguage(QStringLiteral("cpp"));
        QCoreApplication::processEvents();

        out() << "        （cpp 高亮行高 " << withLexer << " px / 参考线 " << guidesWithLexer
              << " px ；纯文本行高 " << plain << " px / 参考线 " << guidesPlain
              << " px）" << Qt::endl;

        check(plain > 0 && withLexer > 0 && qAbs(withLexer - plain) <= 2,
              QStringLiteral("装/不装语法高亮时行高一致（没塌掉）"),
              QStringLiteral("%1 px vs %2 px").arg(withLexer).arg(plain));
    }

    /*
     * 光标行的文字必须画在当前行底色**之上**。
     *
     * 这是个纯渲染问题，属性值看着都对，只能抓图数像素：
     * 光标停在第 2 行和停在第 1 行时，整个编辑区的文字像素数应该几乎一样
     * （正文没变，只差一根光标线）。如果第 2 行的文字被当前行底色盖住，
     * 前者会明显少一截。
     *
     * 曾经踩过：SCI_SETCARETLINEBACKALPHA 传了 255。Scintilla 只有在
     * alpha == SC_ALPHA_NOALPHA(256) 时才把当前行底色当背景画在文字下面，
     * 其余值都是"画完文字再叠一层半透明色"，255 就等于把整行文字糊掉。
     */
    {
        /*
         * 当前行底色的 alpha 必须是 256（SC_ALPHA_NOALPHA），不能是 255。
         *
         * 写成 255 时 Scintilla 会把当前行底色叠在文字**上面**，那一行的字就
         * 全糊掉了 —— 表现就是"光标移到哪一行，哪一行的字看不见"。
         * 这个坑名字很像："不透明"是 SC_ALPHA_OPAQUE(255)，而这里要的是
         * "不要 alpha 通道" SC_ALPHA_NOALPHA(256)，两者只差 1。
         *
         * 注意只能这么查：把控件 grab() 成图去数文字像素**看不出**这个问题，
         * 实测抓图里根本没有当前行那层底色（alwaysVisible=1 也一样），
         * 得在真实窗口上截屏才量得到。见 EditorViewItem.cpp 里那段注释。
         */
        check(view->caretLineAlpha() == 256,
              QStringLiteral("当前行底色 alpha = 256（SC_ALPHA_NOALPHA，不是 255）"),
              QStringLiteral("实际 %1").arg(view->caretLineAlpha()));

        /*
         * 当前行底色**只铺到"文本区"右边**：Scintilla 把它当正文段的底色画
         * （EditView::DrawBackground），文本区 = 编辑器宽 - marginRight。
         * 所以右边距必须是 0，否则那层底色会在离卡片右边缘十几像素的地方
         * 断掉 —— 表现就是"当前行有背景，但背景右边空一块"（用户报的）。
         * 这条只能靠属性钉住：控件 grab() 出来的图里根本没有这层底色（见上面）。
         */
        check(view->paddingRight() == 0,
              QStringLiteral("当前行底色一直铺到编辑器右边缘（正文区右边距 = 0）"),
              QStringLiteral("实际 %1").arg(view->paddingRight()));
    }

    /* ---- 查找 ---- */
    const int hitLine = view->find(QStringLiteral("NEEDLE"), true, false, false, true);
    check(hitLine == 2, QStringLiteral("find() 命中第 2 行"),
          QStringLiteral("实际 %1").arg(hitLine));
    check(view->selectionLength() == 6, QStringLiteral("命中后选中 6 个字符"),
          QStringLiteral("实际 %1").arg(view->selectionLength()));

    const int matches =
        view->highlightMatches(QStringLiteral("NEEDLE"), true, false, false);
    check(matches == 2, QStringLiteral("highlightMatches() = 2 处"),
          QStringLiteral("实际 %1").arg(matches));
    view->clearHighlights();

    /*
     * 区分大小写 / 全字匹配那两个开关**真的传下去了**。
     *
     * 这两条是补的：原来 searchFrom() 把 SCFIND_* 标志算出来却忘了传给
     * SCI_SEARCHINTARGET（clang-analyzer 的 dead store 报的就是它），于是
     * "区分大小写""全字匹配""正则"三个开关全是摆设 —— 界面上勾了没反应。
     *
     * 用例文件里第 2、3 行各有一个大写 NEEDLE，**没有**小写 needle，
     * 所以拿小写去搜：不区分大小写时找得到，区分大小写时一个都找不到。
     */
    {
        view->activateDocument(view->indexOfPath(srcPath));
        const int anyCase =
            view->find(QStringLiteral("needle"), false, false, false, true);
        check(anyCase > 0, QStringLiteral("不区分大小写：小写 needle 也搜得到"),
              QStringLiteral("实际 %1").arg(anyCase));

        view->activateDocument(view->indexOfPath(srcPath));
        const int exactCase =
            view->find(QStringLiteral("needle"), true, false, false, true);
        check(exactCase < 0, QStringLiteral("区分大小写：小写 needle 搜不到"),
              QStringLiteral("实际 %1").arg(exactCase));

        view->activateDocument(view->indexOfPath(srcPath));
        const int wholeOk =
            view->find(QStringLiteral("NEEDLE"), true, true, false, true);
        check(wholeOk == 2, QStringLiteral("全字匹配：整词 NEEDLE 还是命中第 2 行"),
              QStringLiteral("实际 %1").arg(wholeOk));

        view->activateDocument(view->indexOfPath(srcPath));
        const int wholeNo =
            view->find(QStringLiteral("NEED"), true, true, false, true);
        check(wholeNo < 0, QStringLiteral("全字匹配：半个词 NEED 不算命中"),
              QStringLiteral("实际 %1").arg(wholeNo));
    }

    /* ---- 全部替换 ---- */
    const int replaced =
        view->replaceAll(QStringLiteral("NEEDLE"), QStringLiteral("PIN"), true, false, false);
    check(replaced == 2, QStringLiteral("replaceAll() 替换 2 处"),
          QStringLiteral("实际 %1").arg(replaced));
    check(view->modified(), QStringLiteral("替换后是已修改状态"));
    check(view->currentText().contains(QStringLiteral("PIN here")),
          QStringLiteral("正文里出现替换结果"));

    /* ---- 撤销 ---- */
    view->undo();
    check(view->currentText().contains(QStringLiteral("NEEDLE here")),
          QStringLiteral("undo() 撤销了全部替换"));

    /* ---- 注释切换 ---- */
    view->selectAll();
    view->toggleComment(QStringLiteral("//"));
    const QString commented = view->currentText();
    check(commented.count(QStringLiteral("//")) == 3,
          QStringLiteral("toggleComment() 给 3 行都加了注释"),
          QStringLiteral("实际 %1 个 //").arg(commented.count(QStringLiteral("//"))));
    view->selectAll();
    view->toggleComment(QStringLiteral("//"));
    check(!view->currentText().contains(QStringLiteral("//")),
          QStringLiteral("再切一次注释被去掉"));

    /* ---- 换行符转换 + 另存为 ---- */
    view->setEolMode(QStringLiteral("LF"));
    check(view->eolMode() == QLatin1String("LF"), QStringLiteral("换行符切到 LF"));
    view->setEncoding(QStringLiteral("UTF-8 BOM"));
    const bool saved = view->saveCurrentAs(outPath);
    check(saved, QStringLiteral("另存为成功"), view->lastError());
    check(!view->modified(), QStringLiteral("保存后修改标记清掉"));

    const QByteArray written = readBytes(outPath);
    check(written.startsWith("\xEF\xBB\xBF"), QStringLiteral("UTF-8 BOM 写进文件"));
    check(!written.contains('\r'), QStringLiteral("保存后没有 CR"));
    check(QString::fromUtf8(written.mid(3)).contains(QStringLiteral("NEEDLE here")),
          QStringLiteral("落盘内容正确"));
    check(view->filePath() == QFileInfo(outPath).absoluteFilePath(),
          QStringLiteral("当前文件路径已更新"));

    /* ---- 编码切换 ---- */
    view->setEncoding(QStringLiteral("UTF-8"));
    check(view->saveCurrent(), QStringLiteral("改回 UTF-8 后直接保存"));
    check(!readBytes(outPath).startsWith("\xEF\xBB\xBF"),
          QStringLiteral("重新保存后 BOM 消失"));

    /* ---- 写入失败要报错，不能静默 ---- */
    view->lastError();
    check(!view->saveCurrentAs(QStringLiteral("Z:/nope/deep/none.txt")),
          QStringLiteral("写入不存在的路径返回 false"));
    check(!view->lastError().isEmpty(), QStringLiteral("失败时 lastError 有内容"));

    /* ---- 多标签 ---- */
    const int before = view->documents().size();
    const int second = view->newDocument();
    check(view->documents().size() == before + 1, QStringLiteral("newDocument() 多出一个标签"));
    check(view->currentIndex() == second, QStringLiteral("新文档成为当前标签"));
    check(!view->modified(), QStringLiteral("新文档没有修改标记"));

    view->activateDocument(0);
    check(view->currentIndex() == 0, QStringLiteral("activateDocument() 切回第一个标签"));
    check(view->filePath() == QFileInfo(outPath).absoluteFilePath(),
          QStringLiteral("切回来文件路径跟着回来"));
    check(view->currentText().contains(QStringLiteral("NEEDLE here")),
          QStringLiteral("切回来正文跟着回来"));

    view->activateNextDocument();
    check(view->currentIndex() == 1, QStringLiteral("activateNextDocument() 到下一个标签"));
    view->closeDocument(1);
    check(view->documents().size() == before, QStringLiteral("closeDocument() 关掉一个标签"));
    check(view->hasDocument(), QStringLiteral("还剩一个标签，不是空状态"));
    check(view->filePath() == QFileInfo(outPath).absoluteFilePath(),
          QStringLiteral("关掉别的标签后当前文件不变"));

    /* ---- QML 命令分发（工具栏 / 菜单走的就是这里）---- */

    /* 界面绑定：菜单栏 / 状态栏有没有真的拿到编辑器 */
    {
        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("hasView")).toBool(),
              QStringLiteral("Main.view 拿到编辑器"));
        check(ui.value(QStringLiteral("topBarHasView")).toBool(),
              QStringLiteral("菜单栏 view 绑定生效"));
        check(ui.value(QStringLiteral("statusHasDoc")).toBool(),
              QStringLiteral("状态栏知道当前有文档"));
    }

    /*
     * 分隔线热区只能占中间那一行的间隙。
     *
     * 之前它写的是 y: 0 / height: 窗口高度，热区从最顶上一直拉到最底下 ——
     * 顶栏菜单和底部状态栏那两条上也压着它：鼠标停在菜单 / 状态文字上光标
     * 会变成 <->，在那里按住也能拖左树宽度（就是"分隔线溢出了上下两条栏"）。
     * 这几条断言把它钉在中间行的上下边界之间。
     */
    {
        const QVariantMap ui = uiState();
        const double top = ui.value(QStringLiteral("splitterTop")).toDouble();
        const double bottom = ui.value(QStringLiteral("splitterBottom")).toDouble();
        const double midTop = ui.value(QStringLiteral("midRowTop")).toDouble();
        const double midBottom = ui.value(QStringLiteral("midRowBottom")).toDouble();
        const double topBar = ui.value(QStringLiteral("topBarHeight")).toDouble();
        const double statusBar = ui.value(QStringLiteral("statusBarHeight")).toDouble();
        const double winHeight = ui.value(QStringLiteral("windowHeight")).toDouble();
        const QString geom = QStringLiteral("热区 %1..%2，中间行 %3..%4，顶栏 %5，底栏 %6，窗口高 %7")
                                 .arg(top).arg(bottom).arg(midTop).arg(midBottom)
                                 .arg(topBar).arg(statusBar).arg(winHeight);

        const double eps = 0.5;
        check(qAbs(top - midTop) < eps,
              QStringLiteral("分隔线热区上边界 = 顶栏下沿（没有盖住顶栏）"), geom);
        check(qAbs(bottom - midBottom) < eps,
              QStringLiteral("分隔线热区下边界 = 底栏上沿（没有盖住底栏）"), geom);
        check(top >= topBar - eps && bottom <= winHeight - statusBar + eps,
              QStringLiteral("分隔线热区整个落在顶栏和底栏之间"), geom);
        check(bottom > top,
              QStringLiteral("分隔线热区有实际高度（能拖得动）"), geom);

        /*
         * 水平方向：抓手必须正好压在树面板的右边缘上。
         *
         * splitterCenterX 是 splitterMouse.x 那条绑定的**当前值**，
         * treeRightLive 是当场 mapToItem 重算的现值。绑定只读 width（没读位置
         * 属性）时，"面板被布局挪走、宽度没变"这一类变化唤不醒它，两个值就
         * 分叉 —— 抓手留在旧坐标上，用户看到的是"编辑区中间一动鼠标就变 <->，
         * 按住还能拖左树宽度"（实测差出 380 px：树右边缘 290，热区 670）。
         */
        const double centerX = ui.value(QStringLiteral("splitterCenterX")).toDouble();
        const double treeRight = ui.value(QStringLiteral("treeRightLive")).toDouble();
        const double leftX = ui.value(QStringLiteral("splitterLeftX")).toDouble();
        const double zoneWidth = ui.value(QStringLiteral("splitterWidth")).toDouble();
        const double gap = ui.value(QStringLiteral("gapWidth")).toDouble();
        const QString horiz = QStringLiteral("热区 %1..%2（宽 %3），树面板右边缘 %4，间隙 %5 px")
                                  .arg(leftX).arg(leftX + zoneWidth).arg(zoneWidth)
                                  .arg(treeRight).arg(gap);
        check(qAbs(centerX - treeRight) < 6.0,
              QStringLiteral("分隔线热区压在树面板右边缘上（没漂到编辑区里）"), horiz);
        /*
         * 左边缘不许越过面板右边缘：滚动条就贴着那条边画
         * （FolderTree 的 ScrollBar.vertical: ThinScrollBar { anchors.right:
         * parent.right }），越过一点就是"鼠标移到滚动条上变 <->、想拖滚动条
         * 反而在改面板宽度"（用户圈着滚动条提的那一条）。
         */
        check(leftX >= treeRight - 0.5,
              QStringLiteral("分隔线热区不压左树的滚动条（左边缘不过面板右边缘）"), horiz);
        check(zoneWidth <= gap + 3.0,
              QStringLiteral("分隔线热区宽度跟着那条缝走（不是一条宽板子）"), horiz);
    }

    dispatch(QStringLiteral("find"));
    check(uiState().value(QStringLiteral("findOpened")).toBool(),
          QStringLiteral("dispatch(find) 打开查找栏"));

    /*
     * 提示框是深底浅字。
     *
     * 分两头：
     *   * main.cpp 设的应用调色板 —— 管**原生（QWidget）**那侧的提示框；
     *   * QML 那侧真正的提示框走 AppToolTip 组件（自己画的 Popup）——
     *     样式 Fusion 那套取的是平台主题的浅色调色板（#FFFFE1），应用调色板
     *     和 QML 调色板都压不住它，所以那边只能自己画。这里量的是组件报出来的
     *     配色，以及"这份调色板确实也到了 QML"。
     */
    {
        const QColor cppBase = QApplication::palette().color(QPalette::ToolTipBase);
        const QColor cppText = QApplication::palette().color(QPalette::ToolTipText);
        const QVariantMap ui = uiState();
        const QColor qmlBase = ui.value(QStringLiteral("toolTipBase")).value<QColor>();
        const QColor qmlText = ui.value(QStringLiteral("toolTipText")).value<QColor>();
        const QColor tipBg = ui.value(QStringLiteral("tipBackground")).value<QColor>();
        const QColor tipFg = ui.value(QStringLiteral("tipTextColor")).value<QColor>();
        const QString tip =
            QStringLiteral("调色板 底 %1 字 %2（QML 看到 %3 / %4）/ 提示框组件 底 %5 字 %6 "
                           "圆角 %7 延迟 %8ms")
                .arg(cppBase.name(), cppText.name(), qmlBase.name(), qmlText.name(),
                     tipBg.name(), tipFg.name())
                .arg(ui.value(QStringLiteral("tipRadius")).toInt())
                .arg(ui.value(QStringLiteral("tipDelay")).toInt());
        out() << "        （提示框：" << tip << "）" << Qt::endl;

        check(cppBase.lightness() < 90 && cppText.lightness() > 150,
              QStringLiteral("原生提示框用的是深底浅字"), tip);
        check(qmlBase == cppBase && qmlText == cppText,
              QStringLiteral("这份调色板也传到了 QML 那侧"), tip);
        check(tipBg.lightness() < 90 && tipFg.lightness() > 150,
              QStringLiteral("QML 提示框（AppToolTip）是深底浅字"), tip);
        check(tipBg != QColor(0xff, 0xff, 0xe1),
              QStringLiteral("QML 提示框不再是系统那种浅黄底（#FFFFE1）"), tip);
        check(ui.value(QStringLiteral("tipRadius")).toInt() >= 3,
              QStringLiteral("提示框是圆角的"), tip);
        check(ui.value(QStringLiteral("tipDelay")).toInt() == 420,
              QStringLiteral("悬停 420ms 才弹（和原来附加属性的 delay 一致）"), tip);
    }

    dispatch(QStringLiteral("replace"));
    check(uiState().value(QStringLiteral("findReplaceVisible")).toBool(),
          QStringLiteral("dispatch(replace) 展开替换行"));

    /*
     * 查找 / 替换栏的外观（照样例改的那三条）：
     *   * 两个输入框一样长；
     *   * 圆角面板，左右各留出间隙（不再是从左铺到右的长条）；
     *   * 面板是圆的。
     * 布局是 QML 算的，所以量的是 FindBar 报上来的实际宽度（见 uiState）。
     */
    {
        /*
         * 先让 QML 重新布局一次：宽度是布局算出来的，dispatch 之后立刻读还是 0
         * （实测第一次读就是 0/0）。跑两轮事件循环，网格布局收敛后再量。
         */
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        const QVariantMap ui = uiState();
        const double findW = ui.value(QStringLiteral("findFieldWidth")).toDouble();
        const double replW = ui.value(QStringLiteral("findReplaceFieldWidth")).toDouble();
        const double gapL = ui.value(QStringLiteral("findPanelLeftGap")).toDouble();
        const double gapR = ui.value(QStringLiteral("findPanelRightGap")).toDouble();
        const double radius = ui.value(QStringLiteral("findPanelRadius")).toDouble();
        const double barH = ui.value(QStringLiteral("findBarHeight")).toDouble();
        const QString geom = QStringLiteral("栏高 %1 / 查找框 %2 / 替换框 %3 / 左 %4 右 %5 / 圆角 %6")
                                 .arg(barH).arg(findW).arg(replW).arg(gapL).arg(gapR).arg(radius);

        out() << "        （查找栏：" << geom << "）" << Qt::endl;
        /* 高度为 0 说明 implicitHeight 那个绑定炸了（踩过：绑到已删掉的 id），
           这时候整个栏什么都不画，但别的断言照样能过，所以单独钉一条 */
        check(barH >= 60, QStringLiteral("展开替换行后查找栏有高度（不是 0）"), geom);
        check(findW > 100 && qAbs(findW - replW) <= 1,
              QStringLiteral("查找框和替换框一样长"), geom);
        check(gapL >= 6 && gapR >= 6 && qAbs(gapL - gapR) <= 1,
              QStringLiteral("面板左右各有间隙（不再顶到卡片两边）"), geom);
        check(radius >= 6, QStringLiteral("面板是圆角的"), geom);
    }

    /*
     * 自检要一个已知的起点：**自动换行是会被界面记住的**
     * （Connections onWrapChanged -> Cmd.remember("wrap", …)）。
     *
     * 用户上一次开着自动换行退出的话，这个开关下次启动就是开的 —— 于是
     * 下面"切一下应该变开 / 再切一下应该关掉"，以及后面横向滚动条那一整组
     * （换行开着时长行会折起来，横条本来就不该有）全都不成立。
     * 实测：只把设置里的 wrap 改成 1，同一个可执行文件这一组 6 条全红，
     * 看着像功能坏了，其实只是起点不一样 —— 所以这里先把起点钉死，
     * 到收尾再还原成用户自己那个值。
     */
    const bool wrapAtStart = view->wrapEnabled();
    view->setWrapEnabled(false);

    dispatch(QStringLiteral("toggleWrap"));
    check(view->wrapEnabled(), QStringLiteral("dispatch(toggleWrap) 生效"));
    dispatch(QStringLiteral("toggleWrap"));
    check(!view->wrapEnabled(), QStringLiteral("再切一次自动换行关掉"));

    dispatch(QStringLiteral("toggleLineNumbers"));
    check(!view->lineNumbersVisible(), QStringLiteral("dispatch(toggleLineNumbers) 生效"));
    dispatch(QStringLiteral("toggleLineNumbers"));
    check(view->lineNumbersVisible(), QStringLiteral("行号切回来"));

    dispatch(QStringLiteral("toggleWhitespace"));
    check(view->whitespaceVisible(), QStringLiteral("dispatch(toggleWhitespace) 生效"));
    dispatch(QStringLiteral("toggleWhitespace"));

    /* 字号：默认 12，可通过“设置”菜单的命令改 */
    dispatch(QStringLiteral("fontSize:18"));
    check(view->fontPixelSize() == 18,
          QStringLiteral("dispatch(fontSize:18) 改编辑器字号"),
          QStringLiteral("实际 %1").arg(view->fontPixelSize()));
    dispatch(QStringLiteral("fontSize:12"));
    check(view->fontPixelSize() == 12,
          QStringLiteral("dispatch(fontSize:12) 回到默认字号"),
          QStringLiteral("实际 %1").arg(view->fontPixelSize()));

    dispatch(QStringLiteral("lang:python"));
    check(view->language() == QLatin1String("python"),
          QStringLiteral("dispatch(lang:python) 切语言"), view->language());

    /* 设置菜单里的"注释字号 / 字体"走的是同一条 dispatch */
    dispatch(QStringLiteral("commentFontSize:10"));
    check(view->commentFontPixelSize() == 10,
          QStringLiteral("dispatch(commentFontSize:10) 生效"),
          QStringLiteral("实际 %1").arg(view->commentFontPixelSize()));
    dispatch(QStringLiteral("commentFontSize:0"));
    check(view->commentFontPixelSize() == 0,
          QStringLiteral("dispatch(commentFontSize:0) 恢复跟随正文"));

    const QString familyBefore = view->fontFamily();
    dispatch(QStringLiteral("font:NSimSun"));
    check(view->fontFamily() == QStringLiteral("NSimSun"),
          QStringLiteral("dispatch(font:NSimSun) 生效"), view->fontFamily());
    dispatch(QStringLiteral("font:") + familyBefore);
    check(view->fontFamily() == familyBefore,
          QStringLiteral("dispatch(font:...) 换回原字体"), view->fontFamily());

    dispatch(QStringLiteral("zoomIn"));
    check(view->zoomPercent() > 100, QStringLiteral("dispatch(zoomIn) 放大"),
          QStringLiteral("实际 %1%").arg(view->zoomPercent()));
    dispatch(QStringLiteral("zoomReset"));
    check(view->zoomPercent() == 100, QStringLiteral("dispatch(zoomReset) 回到 100%"));

    dispatch(QStringLiteral("new"));
    check(view->documents().size() == before + 1,
          QStringLiteral("dispatch(new) 新建标签"));
    /*
     * 点了工具栏"新建"之后要能直接打字：键盘焦点必须落到原生编辑控件上。
     * requestEditorFocus() 是排到下一轮事件循环再落一次的（鼠标点击的收尾
     * 处理会把同步那次抢走），所以这里也等一轮再看。
     *
     * 先把窗口激活：Qt 里"某个子控件有焦点"的前提是**窗口本身是活动窗口**，
     * 自检跑在后台（或者刚才别的窗口抢了前）时，hasFocus() 一律是 false ——
     * 那是环境问题，不是这条断言的意图（实测就因此误报过一次）。
     */
    if (!QApplication::activeWindow()) {
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (w->isVisible() && w->windowTitle().contains(QStringLiteral("SmartClip"))) {
                w->raise();
                w->activateWindow();
                break;
            }
        }
    }
    QCoreApplication::processEvents();
    check(view->hasEditorFocus(),
          QStringLiteral("新建之后键盘焦点在编辑区（可以直接打字）"),
          QApplication::activeWindow() ? QStringLiteral("窗口已激活")
                                       : QStringLiteral("窗口不是活动窗口"));

    dispatch(QStringLiteral("closeTab"));
    check(view->documents().size() == before,
          QStringLiteral("dispatch(closeTab) 关闭标签（新标签没改动，不该弹窗）"));

    /* ---- 只读模式 ---- */
    dispatch(QStringLiteral("toggleReadOnly"));
    check(view->readOnly(), QStringLiteral("dispatch(toggleReadOnly) 生效"));
    dispatch(QStringLiteral("toggleReadOnly"));
    check(!view->readOnly(), QStringLiteral("只读模式切回来"));

    /*
     * ================= 剪贴板内容：落成 md 文件 + 元数据 =================
     *
     * 内容不再进数据库了：复制进来的东西写进
     *     <保存目录>/<日期>/<时分秒>.md
     * 库里只剩元数据（文件清单 + 每条的时间 / 类型 / 标题 / 摘要 / 去重哈希）。
     * 这一段走的就是采集那条链路 —— ClipboardManager 调的 captureText / captureImage。
     *
     * **跑在临时保存目录上**：先把保存位置换到临时目录，全部跑完再换回来
     * （见下面"新建 / 保存 / 改名 / 删除"那一段的收尾）。
     * 不能拿用户真实的目录来跑：采集是"往当天那份 md 里追加"落地的，往用户那份
     * 文件里写一段、再整份删掉，就把用户自己的东西一起删了。
     */
    QString savedRoot;
    if (store) {
        savedRoot = store->rootPath();
        const QString tempRoot = dir.filePath(QStringLiteral("clipstore"));
        check(store->setRootPath(tempRoot), QStringLiteral("保存位置可改（自检用临时目录）"));
        check(store->rootPath() == QDir::cleanPath(tempRoot),
              QStringLiteral("改完读回来还是那个目录"), store->rootPath());

        /* ---- 文本：写进当天的 md ---- */
        const QString marker =
            QStringLiteral("自检内容 %1").arg(QDateTime::currentMSecsSinceEpoch());
        check(store->captureText(marker), QStringLiteral("采集文本：写进了当天的 md"));
        check(!store->captureText(marker),
              QStringLiteral("同一段文本不会写第二遍（按内容去重）"));

        const QString dateKey = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));

        /*
         * ---- 旧布局的清理：只删"看着像我们自己建的日期目录" ----
         *
         * 加「剪贴板」那一层之前，日期目录直接摆在根目录下。用户说历史数据不要了，
         * 所以 open() 里会清掉那些孤儿目录（见 pruneLegacyLayout）。
         *
         * 关键是**不能横扫**：保存根目录可能是用户自己挑的（甚至指向网盘某个夹），
         * 里面放着他别的东西。判据只看目录名严格是 yyyy-MM-dd —— 这里特意在根
         * 目录下摆一个 `我的东西` 和一个 `2026-1-1`（不是零填充的合法日期），
         * 清完之后它们必须还在。
         */
        {
            const QString legacyDay = store->rootPath() + QStringLiteral("/1999-01-02");
            const QString mineFolder = store->rootPath() + QStringLiteral("/我的东西");
            const QString notADate = store->rootPath() + QStringLiteral("/2026-1-1");
            QDir().mkpath(legacyDay);
            QDir().mkpath(mineFolder);
            QDir().mkpath(notADate);
            QFile legacyFile(legacyDay + QStringLiteral("/旧的.md"));
            if (legacyFile.open(QIODevice::WriteOnly))
                legacyFile.write("旧布局的内容");
            legacyFile.close();

            check(store->setRootPath(store->rootPath()),
                  QStringLiteral("重新指一次同一个根目录（触发一次 open/清理）"));

            check(!QDir(legacyDay).exists(),
                  QStringLiteral("旧布局的日期目录被清掉了"), legacyDay);
            check(QDir(mineFolder).exists(),
                  QStringLiteral("用户自己的文件夹**不动**（名字不是日期的都不碰）"),
                  mineFolder);
            check(QDir(notADate).exists(),
                  QStringLiteral("名字长得像日期但不严格的不动（2026-1-1）"), notADate);
            /* 清完把这两块自检造的东西收掉，别留在临时目录里影响后面的检查 */
            QDir(mineFolder).removeRecursively();
            QDir(notADate).removeRecursively();
        }
        /*
         * 日期目录在 **<root>/剪贴板/** 下面 —— 根目录是用户选的保存位置，里面
         * 只放「剪贴板」这一个子目录（用户要的"剪贴板 → 日期 → 文件"三层）。
         * 这里不写死 "剪贴板" 三个字，走 store->contentRoot()：换个名字只改一处。
         */
        const QDir content(store->contentRoot());
        check(content.exists(), QStringLiteral("根目录下建出了「剪贴板」那一层"),
              content.path());
        const QDir day(content.absoluteFilePath(dateKey));
        check(day.exists(), QStringLiteral("日期目录按 2026-09-13 这种名字建在剪贴板下面"),
              day.path());
        check(!QDir(store->rootPath() + QLatin1Char('/') + dateKey).exists(),
              QStringLiteral("日期目录**不再**直接摆在根目录下（旧布局已经不用了）"));

        /*
         * 在目录里**按内容**找文件。
         *
         * 不按文件名找：自检里几条内容往往落在同一秒里，名字会带 -2 / -3 后缀，
         * 而排序上 "073100-2.md" 反而排在 "073100.md" 前面（'-' < '.'），
         * 拿"最后一个"当"最新那个"是不成立的。
         */
        auto fileContaining = [](const QDir &folder, const QString &needle) {
            const QStringList names =
                folder.entryList({ QStringLiteral("*.md") }, QDir::Files, QDir::Name);
            for (const QString &name : names) {
                if (readFile(folder.absoluteFilePath(name)).contains(needle))
                    return folder.absoluteFilePath(name);
            }
            return QString();
        };

        const QString firstFile = fileContaining(day, marker);
        check(!firstFile.isEmpty(), QStringLiteral("当天的目录里有一份 md 装着刚采集的内容"));
        if (!firstFile.isEmpty()) {
            const QString name = QFileInfo(firstFile).fileName();
            check(QRegularExpression(QStringLiteral("^\\d{6}(-\\d+)?\\.md$")).match(name).hasMatch(),
                  QStringLiteral("文件名是时分秒（73100.md 这种）"), name);
            const QString body = readFile(firstFile);
            check(body.contains(QStringLiteral("## ")),
                  QStringLiteral("每条内容前面是 \"## 时分秒\" 的分段行"));
            check(body.startsWith(QStringLiteral("# ") + dateKey),
                  QStringLiteral("文件开头是当天的日期标题"));
        }

        /* ---- 图片：PNG 落到 assets/，md 里写相对引用 ---- */
        QImage shot(24, 24, QImage::Format_ARGB32);
        shot.fill(QColor(0x4c, 0x96, 0xd8));
        check(store->captureImage(shot), QStringLiteral("采集图片：PNG 落到当天目录的 assets/"));
        {
            const QStringList pngs = QDir(day.absoluteFilePath(QStringLiteral("assets")))
                                         .entryList({ QStringLiteral("*.png") }, QDir::Files);
            check(pngs.size() == 1, QStringLiteral("assets/ 里正好一张 PNG"),
                  QStringLiteral("实际 %1 张").arg(pngs.size()));
            const QString imageFile = fileContaining(day, QStringLiteral("](assets/"));
            check(!imageFile.isEmpty()
                      && readFile(imageFile).contains(QStringLiteral("![图片](assets/")),
                  QStringLiteral("md 里用相对路径引用了那张图"), imageFile);
        }

        /* ---- 20K：写满就另起一份 ---- */
        const QString big = QStringLiteral("自检大块内容 ") + QString(21 * 1024, QLatin1Char('y'));
        check(store->captureText(big), QStringLiteral("超过 20K 的正文也能落盘（自己占一份）"));
        const QString afterBig =
            QStringLiteral("自检大块之后的第二条 %1").arg(QDateTime::currentMSecsSinceEpoch());
        check(store->captureText(afterBig), QStringLiteral("紧接着的那一条也写进去了"));

        const QString bigFile = fileContaining(day, big);
        const QString nextFile = fileContaining(day, afterBig);
        check(!bigFile.isEmpty() && !nextFile.isEmpty(),
              QStringLiteral("两份内容都找得到自己的文件"));
        check(bigFile != nextFile,
              QStringLiteral("上一份超过 20K 之后，下一条落在了**新的一份**里"),
              QStringLiteral("%1 / %2").arg(QFileInfo(bigFile).fileName(),
                                            QFileInfo(nextFile).fileName()));
        check(!readFile(bigFile).contains(marker),
              QStringLiteral("超大的那一份是另起的：里面没有更早那条内容"));
        check(QFileInfo(bigFile).size() > 20 * 1024,
              QStringLiteral("单条内容本身就超过 20K 时照写（不截断）"),
              QStringLiteral("%1 字节").arg(QFileInfo(bigFile).size()));

        /* ---- 元数据：有记录，正文不进库 ---- */
        check(store->entryCount() >= 4, QStringLiteral("元数据里有条目记录（只记元数据）"),
              QStringLiteral("%1 条").arg(store->entryCount()));
        check(store->fileCount() >= 3, QStringLiteral("元数据里有文件记录"),
              QStringLiteral("%1 份").arg(store->fileCount()));

        const QVariantList clipTree = store->tree(QString(), true);
        check(!clipTree.isEmpty(), QStringLiteral("左树数据能从元数据建出来"));
        if (!clipTree.isEmpty()) {
            /*
             * 树顶是「剪贴板」，它下面才是日期文件夹：
             *
             *     剪贴板
             *       2026-09-16
             *         011647.md
             *
             * 用户要的层次（和磁盘布局一致，见 ClipboardStore::contentRoot）。
             */
            const QVariantMap clipRoot = clipTree.first().toMap();
            check(clipRoot.value(QStringLiteral("label")).toString() == QStringLiteral("剪贴板"),
                  QStringLiteral("树顶第一层是「剪贴板」"),
                  clipRoot.value(QStringLiteral("label")).toString());
            check(clipRoot.value(QStringLiteral("kind")).toString() == QLatin1String("folder"),
                  QStringLiteral("「剪贴板」是普通文件夹（不是日期那一支）"),
                  clipRoot.value(QStringLiteral("kind")).toString());

            const QVariantList days = clipRoot.value(QStringLiteral("children")).toList();
            check(!days.isEmpty(), QStringLiteral("「剪贴板」下面有日期文件夹"));
            if (!days.isEmpty()) {
                const QVariantMap day = days.first().toMap();
                check(day.value(QStringLiteral("kind")).toString() == QLatin1String("date"),
                      QStringLiteral("日期文件夹在「剪贴板」下面（第二层）"),
                      day.value(QStringLiteral("kind")).toString());
                check(day.value(QStringLiteral("label")).toString() == dateKey,
                      QStringLiteral("日期文件夹的名字就是这一天"),
                      day.value(QStringLiteral("label")).toString());
                check(day.value(QStringLiteral("files")).toInt() >= 3,
                      QStringLiteral("日期文件夹下面挂着那几份 md"),
                      QStringLiteral("%1 份").arg(day.value(QStringLiteral("files")).toInt()));
                check(day.value(QStringLiteral("depth")).toInt() == 1,
                      QStringLiteral("日期文件夹的层级是 1（剪贴板下面）"),
                      QStringLiteral("depth=%1").arg(day.value(QStringLiteral("depth")).toInt()));
            }
        }

        /* 搜索：按条目摘要找得到（库里存的是摘要，不是正文） */
        check(!store->tree(marker, true).isEmpty(),
              QStringLiteral("按内容搜得到：只留命中的文件"));
        check(store->tree(QStringLiteral("绝对不存在的关键词 zzz"), true).isEmpty(),
              QStringLiteral("搜不到的词 -> 空树"));

        /*
         * ---- 程序自己往剪贴板里写的内容不算采集对象 ----
         *
         * 编辑器里 Ctrl+C、菜单里的"复制全文"、左树回填剪贴板……都会触发
         * QClipboard::dataChanged，不挡掉的话刚复制的东西立刻又被采集一遍。
         * 机制就是这一个标记：置上 -> 下一次采集跳过 -> 用完就清。
         */
        store->markOwnCopy();
        check(store->takeSkipNextCapture(),
              QStringLiteral("自己人写的剪贴板变化：下一次采集会跳过"));
        check(!store->takeSkipNextCapture(),
              QStringLiteral("这个标记只生效一次（不会把后面真正的外部复制吃掉）"));
    }

    /* 缩进参考线：默认开、颜色和另外两条竖线一样（不是正文色、不是 Scintilla 默认） */
    check(view->indentGuidesVisible(), QStringLiteral("缩进参考线默认开启"));
    check(view->styleFore(37) == packed(0x33, 0x38, 0x40),   // 37 = STYLE_INDENTGUIDE
          QStringLiteral("缩进参考线是压过的灰，和行号右边那条竖线一个颜色"),
          QStringLiteral("实际 #%1").arg(unpacked(view->styleFore(37)), 6, 16, QLatin1Char('0')));

    /* ---- 代码折叠：单独开一个带大括号的临时文件，别动前面那些断言的行号 ---- */
    {
        const QString foldPath = dir.filePath(QStringLiteral("foldtest.cpp"));
        const QByteArray foldSrc =
            "int main() {\n"
            "    if (1) {\n"
            "        return 0;\n"
            "    }\n"
            "    return 1;\n"
            "}\n";
        check(writeFile(foldPath, foldSrc), QStringLiteral("准备折叠测试文件"));
        view->openFile(foldPath);
        check(view->hasDocument(), QStringLiteral("打开折叠测试文件"));
        check(view->foldingEnabled(), QStringLiteral("代码折叠默认开着"));

        view->unfoldAll();
        check(view->lineVisible(3), QStringLiteral("展开状态下第 3 行可见"));

        view->foldAll();
        check(!view->lineVisible(3),
              QStringLiteral("foldAll() 之后第 3 行被折起来（不可见）"));

        view->unfoldAll();
        check(view->lineVisible(3), QStringLiteral("unfoldAll() 之后第 3 行又可见"));

        /*
         * 边距渲染的像素检查（看的是控件自己渲染出来的图）：
         *   行号栏要有数字、折叠栏要有折叠标记、**不能有纯白像素**
         *   （白带就是 SC_MARGIN_NUMBER 的背景被 lexer 的 STYLECLEARALL
         *     刷回默认白色造成的）。
         */
        view->gotoLine(1);
        QCoreApplication::processEvents();
        const QVariantList stats = view->marginPixelStats();
        const int numberInk = stats.value(0).toInt();
        const int foldInk = stats.value(1).toInt();
        const int whitePx = stats.value(2).toInt();

        out() << "        （行号墨点 " << numberInk << " / 折叠墨点 " << foldInk
              << " / 纯白 " << whitePx
              << " / 边距宽 " << stats.value(7).toInt() << "+" << stats.value(8).toInt()
              << "+" << stats.value(9).toInt() << "）" << Qt::endl;

        check(whitePx == 0, QStringLiteral("边距里没有纯白像素（行号栏不是白底）"),
              QStringLiteral("实际 %1 个白点").arg(whitePx));
        check(numberInk > 0, QStringLiteral("行号栏画出了数字"),
              QStringLiteral("墨点 %1").arg(numberInk));
        check(foldInk > 0, QStringLiteral("折叠栏画出了折叠标记"),
              QStringLiteral("墨点 %1").arg(foldInk));

        /* 缩进参考线：开着能画出来，关掉就一个像素都没有 */
        view->setIndentGuidesVisible(true);
        QCoreApplication::processEvents();
        const int guidesOn = view->marginPixelStats().value(12).toInt();
        view->setIndentGuidesVisible(false);
        QCoreApplication::processEvents();
        const int guidesOff = view->marginPixelStats().value(12).toInt();
        view->setIndentGuidesVisible(true);
        QCoreApplication::processEvents();

        check(guidesOn > 0, QStringLiteral("缩进参考线画出来了"),
              QStringLiteral("像素 %1").arg(guidesOn));
        check(guidesOff == 0, QStringLiteral("关掉缩进参考线后一个像素都没有"),
              QStringLiteral("像素 %1").arg(guidesOff));

        view->closeDocument(view->currentIndex());
    }

    /*
     * ============ 横向滚动条：内容没撑满就不该有 ============
     *
     * 用户报的："内容区只有几个字，但是横向有滚动条"。
     *
     * 根因在 Scintilla 判显隐的口径（third/qscintilla/src/ScintillaQt.cpp
     * 的 ModifyScrollBars）：
     *     hNewPage = GetTextRectangle().Width();          // 一页**文本**宽
     *     hMax     = scrollWidth > hNewPage ? scrollWidth - hNewPage : 0;
     * 横条是 AsNeeded 策略，hMax > 0 就露出来。而 GetTextRectangle() 是 viewport
     * 再扣掉行号/折叠那几条边距和左右留白之后的宽度，**比 viewport 窄几十像素**。
     * updateHorizontalScroll() 原来拿 viewport 宽当"放得下"的界：短内容时
     * scrollWidth = viewport 宽，仍比 hNewPage 大一截 -> hMax 恒 > 0 ->
     * 正文只有几个字也一直挂着横条，还能向右滚那几十像素（正好是边距 + 留白）。
     *
     * 这里钉三件事：
     *   1) 短内容：横条不出现（maximum = 0），scrollWidth 没超过一页文本宽；
     *   2) 长内容：横条出现（maximum > 0），能向右滚到底；
     *   3) 口径一致：自己按公式算的一页宽 == Scintilla 的 pageStep（hNewPage），
     *      免得以后两边又各算一份、慢慢走样。
     */
    {
        const QString shortPath = dir.filePath(QStringLiteral("hscroll-short.txt"));
        const QString longPath = dir.filePath(QStringLiteral("hscroll-long.txt"));

        /* 几个汉字 / 一个字都不换行的长行（400 字符，必然比编辑区宽） */
        check(writeFile(shortPath, QString::fromUtf8("换承载方式\n").toUtf8()),
              QStringLiteral("准备短内容文件"));
        check(writeFile(longPath,
                        QByteArray("LINE ") + QByteArray(390, 'x') + QByteArray("\n")),
              QStringLiteral("准备长行文件"));

        auto hState = [view]() { return view->horizontalScrollState(); };
        auto detail = [](const QVariantMap &s) {
            return QStringLiteral("可见 %1 / maximum %2 / pageStep %3 / 算出来 %4"
                                  " / viewport %5 / scrollWidth %6 / 内容 %7")
                .arg(s.value(QStringLiteral("visible")).toBool())
                .arg(s.value(QStringLiteral("maximum")).toInt())
                .arg(s.value(QStringLiteral("pageStep")).toInt())
                .arg(s.value(QStringLiteral("pageWidthComputed")).toInt())
                .arg(s.value(QStringLiteral("viewportWidth")).toInt())
                .arg(s.value(QStringLiteral("scrollWidth")).toInt())
                .arg(s.value(QStringLiteral("contentWidth")).toInt());
        };

        /* ---- 短内容 ---- */
        check(view->openFile(shortPath) >= 0, QStringLiteral("打开短内容文件"),
              view->lastError());
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap small = hState();
        out() << "        （短内容：" << detail(small) << "）" << Qt::endl;
        check(small.value(QStringLiteral("maximum")).toInt() == 0
              && !small.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("短内容：没有横向滚动条（maximum = 0）"), detail(small));
        check(small.value(QStringLiteral("scrollWidth")).toInt()
              <= small.value(QStringLiteral("pageStep")).toInt(),
              QStringLiteral("短内容：scrollWidth 没超过一页文本宽"), detail(small));
        check(small.value(QStringLiteral("contentWidth")).toInt() > 0,
              QStringLiteral("短内容：量到了内容宽（不是没量）"), detail(small));

        /*
         * 一页宽的两种算法必须一致：公式（viewport - 边距 - 左右留白）对
         * Scintilla 自己写进 pageStep 的那个值。差一点点就说明口径又开始分家了
         * —— 这正是原来那条 bug 的来源。
         */
        check(qAbs(small.value(QStringLiteral("pageStep")).toInt()
                   - small.value(QStringLiteral("pageWidthComputed")).toInt()) <= 1,
              QStringLiteral("一页文本宽：自己算的和 Scintilla 的一致"), detail(small));

        /*
         * ---- 面板被拉窄的**那一拍**不许冒出横条 ----
         *
         * 用户报的："左右拖动分栏分隔线时，底下那条横条一闪一闪，正文根本没超出
         * 屏幕也闪出来。"
         *
         * 根因是 scrollWidth 的取值口径：原来"放得下"这一支把它写成**当时那一页
         * 文本宽**，于是这个数把"设它的那一刻面板有多宽"也记了进去。拖动把它变窄的
         * 那一拍，Scintilla 那边 hMax = scrollWidth - 新的 hNewPage 就成了正数
         * （ScintillaQt.cpp 的 ModifyScrollBars），横条当场冒出来闪一下，
         * 下一拍 updateHorizontalScroll() 才压回去 —— 拖动是一格一格来的，
         * 于是"一闪一闪"。
         *
         * 这里卡的就是那一拍：把控件拉窄之后**不转事件循环**，直接读横条状态。
         * （转一次事件循环，applyGeometry 排的那次重算就把值压回去了，
         * 这一条就永远量不到东西。）
         */
        {
            const QVariantMap narrowed =
                view->horizontalScrollAfterNarrowForTest(-220);
            out() << "        （拉窄那一拍：" << detail(narrowed) << "）" << Qt::endl;
            check(narrowed.value(QStringLiteral("maximum")).toInt() == 0
                  && !narrowed.value(QStringLiteral("visible")).toBool(),
                  QStringLiteral("面板变窄的那一拍：短内容不冒横向滚动条（拖分隔线不闪）"),
                  detail(narrowed));
            /*
             * 换个说法钉同一条：横向范围不许超过**变窄之后**那一页宽。
             * 旧口径（scrollWidth = 当时的一页宽）在这里是 1107 > 887。
             */
            check(narrowed.value(QStringLiteral("scrollWidth")).toInt()
                  <= narrowed.value(QStringLiteral("pageStep")).toInt(),
                  QStringLiteral("变窄之后 scrollWidth 仍不超过一页文本宽"), detail(narrowed));
        }

        /* ---- 长内容 ---- */
        check(view->openFile(longPath) >= 0, QStringLiteral("打开长行文件"),
              view->lastError());
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap big = hState();
        out() << "        （长内容：" << detail(big) << "）" << Qt::endl;
        check(big.value(QStringLiteral("maximum")).toInt() > 0
              && big.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("长行：横向滚动条出现"), detail(big));

        /*
         * 横条出现之后，正文区就短了一截（横条那 12px）。两条竖线必须**补到控件
         * 底边**，不能断在横条上沿 —— 用户报的就是这个："有时候没撑满纵向屏幕"。
         * 补线是编辑控件上那块透明小控件画的（见 EditorViewItem::updateBottomLines）。
         */
        {
            QCoreApplication::processEvents();
            const QVariantList ruler = view->rulerPixelStats();
            const QVariantList margin = view->marginPixelStats();
            out() << "        （有横条时：参考线离底边 " << ruler.value(2).toInt()
                  << " px，分隔线离底边 " << margin.value(13).toInt() << " px）" << Qt::endl;
            check(ruler.value(2).toInt() >= 0 && ruler.value(2).toInt() <= 3,
                  QStringLiteral("有横条时参考线仍然画到编辑区底边"),
                  QStringLiteral("离底边 %1 px").arg(ruler.value(2).toInt()));
            check(margin.value(13).toInt() >= 0 && margin.value(13).toInt() <= 3,
                  QStringLiteral("有横条时分隔竖线也画到编辑区底边"),
                  QStringLiteral("离底边 %1 px").arg(margin.value(13).toInt()));
            /*
             * 补线控件是贴在那一条上的，它自己不能画背景、也不能接鼠标事件
             * （否则等于把滚动条糊住 / 点不动）。滚轮、拖横条都得照常能用。
             */
            const QVariantList bl = view->bottomLinesState();
            check(bl.value(0).toBool() && bl.value(1).toBool() && bl.value(2).toBool()
                      && !bl.value(3).toBool() && bl.value(4).toInt() == 2
                      && bl.value(5).toInt() >= 8,
                  QStringLiteral("补线控件在工作：鼠标穿透、不画背景、补两条线"),
                  QStringLiteral("可见 %1 / 穿透 %2 / 不画背景 %3 / 自动填背景 %4 "
                                 "/ 线数 %5 / 高 %6")
                      .arg(bl.value(0).toBool()).arg(bl.value(1).toBool())
                      .arg(bl.value(2).toBool()).arg(bl.value(3).toBool())
                      .arg(bl.value(4).toInt()).arg(bl.value(5).toInt()));
        }
        check(big.value(QStringLiteral("contentWidth")).toInt()
              > big.value(QStringLiteral("pageStep")).toInt(),
              QStringLiteral("长行：量出来的内容宽确实超过了一页"), detail(big));

        /*
         * 自动换行开着时内容折起来，横条必须收回去；关掉换行又得立刻回来 ——
         * 切"换行"开关会走一趟 updateHorizontalScroll，这里量的是那趟有没有
         * 把 scrollWidth 算拧（换行时 hNewPage 是折行宽度，不是原来那个）。
         */
        view->setWrapEnabled(true);
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const QVariantMap wrapped = hState();
        check(!wrapped.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("自动换行：长行折起来，横条收回去"), detail(wrapped));

        view->setWrapEnabled(false);
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const QVariantMap unwrapped = hState();
        check(unwrapped.value(QStringLiteral("maximum")).toInt() > 0
              && unwrapped.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("关掉自动换行：长行又把横条要回来"), detail(unwrapped));

        /*
         * ---- 滚动条的右键菜单：换成应用自己那套深色菜单 ----
         *
         * Qt 自带的 QScrollBar 右键菜单是浅色底 + 英文条目（"Scroll here /
         * Left edge / Page left / …"），跟界面里其它菜单完全不是一个样子。
         * 现在这条右键在 EditorViewItem::eventFilter 里被**吃掉**，改发信号让
         * Main.qml 弹同一套 DropdownMenu（条目见 js/EditorMenus.js 的 scrollBarMenu）。
         *
         * 钉三件事：
         *   1) 菜单里就是那七条，横向纵向各一组（scroll:h:* / scroll:v:*）；
         *   2) 事件真的被吃掉、弹出来的是我们那套菜单（原生那个没机会出现）；
         *   3) 点下去真的会滚 —— 拿横条的 value 量"右边缘 / 左边缘"。
         */
        {
            auto scrollActs = [qmlRoot](bool horizontal) {
                QVariant result;
                QMetaObject::invokeMethod(qmlRoot, "scrollMenuActs",
                                          Q_RETURN_ARG(QVariant, result),
                                          Q_ARG(QVariant, QVariant(horizontal)));
                return result.toList();
            };

            QStringList hActs, vActs;
            for (const QVariant &a : scrollActs(true))
                hActs << a.toString();
            for (const QVariant &a : scrollActs(false))
                vActs << a.toString();
            const QStringList expectedH = { QStringLiteral("scroll:h:here"),
                                            QStringLiteral("scroll:h:edgeStart"),
                                            QStringLiteral("scroll:h:edgeEnd"),
                                            QStringLiteral("scroll:h:pageBack"),
                                            QStringLiteral("scroll:h:pageForward"),
                                            QStringLiteral("scroll:h:lineBack"),
                                            QStringLiteral("scroll:h:lineForward") };
            check(hActs == expectedH,
                  QStringLiteral("横向滚动条右键 = 七条（和 Qt 原来那套语义一一对应）"),
                  hActs.join(QLatin1Char('/')));
            check(vActs.size() == 7
                      && vActs.contains(QStringLiteral("scroll:v:pageForward"))
                      && vActs.contains(QStringLiteral("scroll:v:edgeEnd")),
                  QStringLiteral("纵向那一组是 v 轴的动作"), vActs.join(QLatin1Char('/')));

            /* 事件被吃掉 + 弹的是我们那套菜单 */
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            /*
             * 返回值用**精确类型**接：QMetaObject::invokeMethod 对不上返回类型
             * 就直接失败、方法根本不会被调用（QVariant 接 bool 就是这样，
             * 踩过一次：表现为"探针一行都没打、handled 一直是 false"）。
             */
            bool handled = false;
            QMetaObject::invokeMethod(view, "triggerScrollBarContextMenu",
                                      Q_RETURN_ARG(bool, handled),
                                      Q_ARG(bool, true), Q_ARG(int, -1));
            settle();
            check(handled,
                  QStringLiteral("滚动条的右键事件被我们吃掉（Qt 那个浅色英文菜单不会弹）"));
            {
                const QVariantMap menu = uiState();
                check(menu.value(QStringLiteral("menuOpened")).toBool(),
                      QStringLiteral("滚动条右键弹出的是应用自己的菜单"));
                check(menu.value(QStringLiteral("menuHasIcons")).toBool(),
                      QStringLiteral("滚动条菜单也带图标（和编辑菜单一套观感）"));
            }
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            settle();

            /* 点下去真的会滚 */
            const QVariantMap atStart = hState();
            check(atStart.value(QStringLiteral("value")).toInt() == 0,
                  QStringLiteral("动作之前横条在最左边"),
                  QStringLiteral("value %1").arg(atStart.value(QStringLiteral("value")).toInt()));

            dispatch(QStringLiteral("scroll:h:edgeEnd"));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            const QVariantMap atEnd = hState();
            check(atEnd.value(QStringLiteral("maximum")).toInt() > 0
                      && atEnd.value(QStringLiteral("value")).toInt()
                             == atEnd.value(QStringLiteral("maximum")).toInt(),
                  QStringLiteral("菜单里\"右边缘\"把横条滚到底"),
                  QStringLiteral("value %1 / maximum %2")
                      .arg(atEnd.value(QStringLiteral("value")).toInt())
                      .arg(atEnd.value(QStringLiteral("maximum")).toInt()));

            dispatch(QStringLiteral("scroll:h:edgeStart"));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            check(hState().value(QStringLiteral("value")).toInt() == 0,
                  QStringLiteral("菜单里\"左边缘\"滚回最左边"));
        }

        /* ---- 再回到短内容：横条要收回去 ---- */
        view->closeDocument(view->currentIndex());
        check(view->filePath() == QFileInfo(shortPath).absoluteFilePath(),
              QStringLiteral("关掉长行文件后回到短内容文件"), view->filePath());
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap again = hState();
        check(again.value(QStringLiteral("maximum")).toInt() == 0
              && !again.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("又切回短内容：横条收回去"), detail(again));

        dispatch(QStringLiteral("closeAllTabs"));
    }

    /*
     * ============ 正文卡片：两条滚动条都要贴着卡片边 ============
     *
     * 用户提的是两条滚动条的位置：
     *   1) 横向滚动条离卡片底边太远（原来编辑器四边都让开 10px，横条下面
     *      就空出一条）；
     *   2) 竖向滚动条离卡片右边太远（同理，右边也让开 10px）。
     *
     * 让开的理由本来只有一个：编辑器是**原生子控件**，自己的矩形角是直角，
     * 贴着卡片的角会把 contentArea（radius: 10）画出来的圆角盖成直角。
     * 但**实测（截图逐像素比对）2px 的余量就够了** —— 编辑器底色和卡片底色
     * 本来就是同一个（paperColor: root.editorBg），角上那一两个像素看不出来，
     * 2px 和 10px 画出来的圆角一模一样。所以右边 / 底边都收到 2px，滚动条跟着
     * 贴到卡片边上。
     *
     * 左边后来也收到 2px：编辑器最左边那一条就是行号栏，"序号贴紧左边"
     * 只能靠这个左边距（Scintilla 的 SCI_SETMARGINLEFT 落在**分隔竖线和正文**
     * 之间，跟行号位置无关）。正文离卡片左边缘的余量由行号栏 + 折叠栏的
     * 宽度顶着，不再靠这里。
     *
     * 这里钉住：三条边都只留一点点（>0 且 ≤4px，给坐标取整留余量）、
     * 编辑器整体不出卡片。改回 10px 或者改成 0 都会在这里红。
     */
    {
        /* 需要编辑器在场（visible）时量，所以先开一个文档 */
        check(view->newDocument() >= 0, QStringLiteral("卡片几何用例：新建文档"));
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap card = uiState().value(QStringLiteral("editorCard")).toMap();
        const double radius = card.value(QStringLiteral("radius")).toDouble();
        const double bottomGap = card.value(QStringLiteral("bottomGap")).toDouble();
        const double leftGap = card.value(QStringLiteral("leftGap")).toDouble();
        const double rightGap = card.value(QStringLiteral("rightGap")).toDouble();
        const double viewBottom = card.value(QStringLiteral("viewBottom")).toDouble();
        const double viewRight = card.value(QStringLiteral("viewRight")).toDouble();
        const double cardH = card.value(QStringLiteral("cardHeight")).toDouble();
        const double cardW = card.value(QStringLiteral("cardWidth")).toDouble();
        const QString geom =
            QStringLiteral("卡片 %1x%2 圆角 %3 / 编辑器 %4,%5 %6x%7 / 下留 %8 左留 %9 右留 %10")
                .arg(cardW).arg(cardH).arg(radius)
                .arg(card.value(QStringLiteral("viewX")).toDouble())
                .arg(card.value(QStringLiteral("viewY")).toDouble())
                .arg(card.value(QStringLiteral("viewWidth")).toDouble())
                .arg(card.value(QStringLiteral("viewHeight")).toDouble())
                .arg(bottomGap).arg(leftGap).arg(rightGap);

        out() << "        （" << geom << "）" << Qt::endl;

        check(bottomGap >= 1.0 && bottomGap <= 4.0,
              QStringLiteral("编辑器贴着卡片底边（横向滚动条不再浮在半空）"), geom);
        check(rightGap >= 1.0 && rightGap <= 4.0,
              QStringLiteral("编辑器贴着卡片右边（竖向滚动条不再浮在中间）"), geom);
        check(leftGap >= 1.0 && leftGap <= 4.0,
              QStringLiteral("编辑器贴着卡片左边（行号栏不再浮在中间）"), geom);
        check(viewBottom <= cardH + 0.5 && viewRight <= cardW + 0.5,
              QStringLiteral("编辑器没有溢出卡片（不压状态栏、不出画布）"), geom);

        /*
         * ============ 分栏（两个对等的编辑组，tab 右键那三条） ============
         *
         * 钉的是用户要的那套 VS Code 语义（见 EditorViewItem.h 里分栏那一段）：
         *   1) 右键菜单里那三条命令**接得上**（dispatch 之后 editor.splitMode
         *      真的变了）—— 菜单是数据、dispatch 是执行，两边名字写错一个字
         *      在界面上就是"点了没反应"；
         *   2) 分出来的是**两个各自独立的编辑组**：各自一组标签（打开一份新
         *      文件只动发出命令的那一栏），各自看自己那一份；
         *   3) 同一份文档在两栏里就是**同一个底层文档**：改一边另一边立刻就是
         *      新内容（这是"信息共用"那一半）。
         *
         * 注意用 view->newDocument() 而不是 dispatch("new")：后者会弹系统
         * 文件框（未命名文档要问路径），自检里没有用户，会卡在那儿。
         */
        {
            /*
             * 用**真文件**分栏：上面那个是 newDocument() 出来的空白标签，
             * 两边都是 0 字，比出来"相等"也说明不了什么。
             */
            check(view->openFile(srcPath) >= 0,
                  QStringLiteral("分栏用例：先打开一份有内容的文件"));

            dispatch(QStringLiteral("splitRight"));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();

            const QString mode = uiState().value(QStringLiteral("splitMode")).toString();
            check(mode == QLatin1String("right"),
                  QStringLiteral("dispatch(splitRight) 之后真的在左右分栏"), mode);

            const QString mirrorText =
                uiState().value(QStringLiteral("mirrorText")).toString();
            const QString mainText = view->currentText();
            check(!mirrorText.isEmpty() && mirrorText == mainText,
                  QStringLiteral("分出来的第二栏显示的是同一份正文"),
                  QStringLiteral("主 %1 字 / 第二栏 %2 字")
                      .arg(mainText.size()).arg(mirrorText.size()));

            /* 两栏各有自己的一组标签，而且当前显示的是同一份文档 */
            const int mainTabs = uiState().value(QStringLiteral("mainTabCount")).toInt();
            const int mirrorTabs = uiState().value(QStringLiteral("mirrorTabCount")).toInt();
            check(mainTabs >= 1 && mirrorTabs == 1,
                  QStringLiteral("第二栏只有一条标签（分栏时把当前这一份挪过去，"
                                 "不是把主栏那一堆全抄一份）"),
                  QStringLiteral("主 %1 条 / 第二栏 %2 条；C++ view 看到 %3 条 / "
                                 "池子 %4 份")
                      .arg(mainTabs).arg(mirrorTabs).arg(view->documents().size())
                      .arg(view->poolCount()));
            check(uiState().value(QStringLiteral("mainDocId")).toInt() >= 0
                      && uiState().value(QStringLiteral("mainDocId")).toInt()
                             == uiState().value(QStringLiteral("mirrorDocId")).toInt(),
                  QStringLiteral("两栏当前看的是同一份文档（同一份，不是拷贝）"));

            /*
             * 独立：在主栏再开一份文档 —— 主栏多一条标签并切过去，
             * 第二栏保持原样（还看着原来那一份、标签也不多不少）。
             */
            const int beforeMainTabs =
                uiState().value(QStringLiteral("mainTabCount")).toInt();            const int beforeMirrorDoc =
                uiState().value(QStringLiteral("mirrorDocId")).toInt();
            const QString beforeMirrorText =
                uiState().value(QStringLiteral("mirrorText")).toString();
            const int second = view->newDocument();
            QCoreApplication::processEvents();
            check(second >= 0
                      && uiState().value(QStringLiteral("mainTabCount")).toInt()
                             == beforeMainTabs + 1
                      && uiState().value(QStringLiteral("mirrorTabCount")).toInt() == 1,
                  QStringLiteral("主栏新开一份：只有主栏多出标签，第二栏不动"),
                  QStringLiteral("主 %1 条 / 第二栏 %2 条")
                      .arg(uiState().value(QStringLiteral("mainTabCount")).toInt())
                      .arg(uiState().value(QStringLiteral("mirrorTabCount")).toInt()));
            check(uiState().value(QStringLiteral("mirrorDocId")).toInt() == beforeMirrorDoc
                      && uiState().value(QStringLiteral("mirrorText")).toString()
                             == beforeMirrorText,
                  QStringLiteral("主栏切到别的文档之后，第二栏还看着原来那一份"));

            /* 切回文件那一份，好接着验"同一份文档改一边两边都变" */
            const int backToFile = view->indexOfPath(srcPath);
            if (backToFile >= 0)
                view->activateDocument(backToFile);
            QCoreApplication::processEvents();
            check(backToFile >= 0 && view->filePath() == QFileInfo(srcPath).absoluteFilePath(),
                  QStringLiteral("分栏用例：切回文件那一份"));

            /*
             * 共用：两栏现在看的是同一份文档，在主栏把正文改掉，
             * 第二栏读到的必须是新内容。改完把修改标记清掉（下面还要
             * closeAllTabs，留着"已修改"会弹「保存 / 不保存」卡片等回答）。
             */
            const QString edited = mainText + QStringLiteral("// edited in main\n");
            view->setText(edited);
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            check(uiState().value(QStringLiteral("mirrorText")).toString() == edited,
                  QStringLiteral("同一份文档：主栏改了，第二栏立刻就是新内容"));
            view->setModified(false);
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();

            /*
             * 分栏之后正文**真的分成两块**（用户报的："上下分栏没有展开"）。
             *
             * 只验 splitMode / 标签条数是查不出这个的：那两条标签栏确实分了，
             * 但布局里 mainPane / mirrorPaneHolder 的锚点过约束（四个边都锚着、
             * 又给了 width / height），QML 里锚点赢、显式尺寸被丢掉 → 两块都铺满
             * 整块正文区叠在一起，屏幕上看着就是"没分开"。
             *
             * 量的是**场景坐标**：两栏的父壳不是同一个（mainPane / mirrorPaneHolder），
             * 直接比 y() 没意义。
             */
            {
                auto *mainView = qobject_cast<EditorViewItem *>(
                    uiState().value(QStringLiteral("mainPane")).value<QObject *>());
                auto *mirrorView = qobject_cast<EditorViewItem *>(
                    uiState().value(QStringLiteral("mirrorPane")).value<QObject *>());
                check(mainView != nullptr && mirrorView != nullptr,
                      QStringLiteral("分栏几何：拿得到两栏的编辑器对象"));

                if (mainView && mirrorView) {
                    auto sceneAt = [](QQuickItem *it) {
                        return it->mapToItem(nullptr, QPointF(0, 0));
                    };
                    auto detailOf = [&](const QPointF &mainAt, const QPointF &mirrorAt) {
                        return QStringLiteral("主 %1,%2 %3x%4 / 第二栏 %5,%6 %7x%8")
                            .arg(mainAt.x()).arg(mainAt.y())
                            .arg(mainView->width()).arg(mainView->height())
                            .arg(mirrorAt.x()).arg(mirrorAt.y())
                            .arg(mirrorView->width()).arg(mirrorView->height());
                    };

                    /* 左右分栏（上面 dispatch 的就是它） */
                    const QPointF mainRight = sceneAt(mainView);
                    const QPointF mirrorRight = sceneAt(mirrorView);
                    const QString rightDetail = detailOf(mainRight, mirrorRight);
                    check(mirrorRight.x() >= mainRight.x() + mainView->width() - 2,
                          QStringLiteral("左右分栏：第二栏在主栏右边（不是叠在一起）"),
                          rightDetail);
                    check(qAbs(mainView->width() - mirrorView->width()) <= 4,
                          QStringLiteral("左右分栏：两栏一样宽（各占一半）"), rightDetail);

                    /*
                     * 两条标签栏要**贴在一起**：交界处只有那条 1px 的分界线，
                     * 不许留缝（用户报的"两栏 tab 之间不要有间隙"）。
                     */
                    {
                        const QVariantMap lay =
                            uiState().value(QStringLiteral("splitLayout")).toMap();
                        const QVariantMap mt =
                            lay.value(QStringLiteral("mainTab")).toMap();
                        const QVariantMap rt =
                            lay.value(QStringLiteral("mirrorTab")).toMap();
                        const QString tabDetail =
                            QStringLiteral("上栏标签 %1..%2 / 下栏标签 %3..%4（y %5 / %6）")
                                .arg(mt.value(QStringLiteral("x")).toInt())
                                .arg(mt.value(QStringLiteral("x")).toInt()
                                     + mt.value(QStringLiteral("w")).toInt())
                                .arg(rt.value(QStringLiteral("x")).toInt())
                                .arg(rt.value(QStringLiteral("x")).toInt()
                                     + rt.value(QStringLiteral("w")).toInt())
                                .arg(mt.value(QStringLiteral("y")).toInt())
                                .arg(rt.value(QStringLiteral("y")).toInt());
                        check(qAbs((mt.value(QStringLiteral("x")).toInt()
                                    + mt.value(QStringLiteral("w")).toInt())
                                   - rt.value(QStringLiteral("x")).toInt()) <= 1
                              && mt.value(QStringLiteral("y")).toInt()
                                 == rt.value(QStringLiteral("y")).toInt(),
                              QStringLiteral("左右分栏：两条标签栏贴在一起（中间只有 1px 分界线）"),
                              tabDetail);
                    }

                    /* 上下分栏：第二栏要落到**下面**，两块上下拼满整块正文区 */
                    dispatch(QStringLiteral("splitDown"));
                    for (int i = 0; i < 3; ++i)
                        QCoreApplication::processEvents();
                    const QPointF mainDown = sceneAt(mainView);
                    const QPointF mirrorDown = sceneAt(mirrorView);
                    const QString downDetail = detailOf(mainDown, mirrorDown);
                    check(mirrorDown.y() >= mainDown.y() + mainView->height() - 2,
                          QStringLiteral("上下分栏：第二栏在主栏下面（不是叠在一起）"),
                          downDetail);

                    /*
                     * 下面那一栏的标签栏必须**紧贴在它自己那一栏正文上面**
                     * （用户报的："这个 tab 没有显示在下面一栏紧贴"）——
                     * 原来两条标签栏都摞在最顶上，下面那一栏的 tab 离它那一栏
                     * 有大半屏远。
                     */
                    {
                        const QVariantMap lay =
                            uiState().value(QStringLiteral("splitLayout")).toMap();
                        const QVariantMap mt =
                            lay.value(QStringLiteral("mainTab")).toMap();
                        const QVariantMap rt =
                            lay.value(QStringLiteral("mirrorTab")).toMap();
                        const QVariantMap mv =
                            lay.value(QStringLiteral("mirrorView")).toMap();
                        const QString tabDetail =
                            QStringLiteral("下栏标签 y %1..%2 / 下栏正文 y %3 / 上栏标签 y %4..%5")
                                .arg(rt.value(QStringLiteral("y")).toInt())
                                .arg(rt.value(QStringLiteral("y")).toInt()
                                     + rt.value(QStringLiteral("h")).toInt())
                                .arg(mv.value(QStringLiteral("y")).toInt())
                                .arg(mt.value(QStringLiteral("y")).toInt())
                                .arg(mt.value(QStringLiteral("y")).toInt()
                                     + mt.value(QStringLiteral("h")).toInt());
                        check(qAbs((rt.value(QStringLiteral("y")).toInt()
                                    + rt.value(QStringLiteral("h")).toInt())
                                   - mv.value(QStringLiteral("y")).toInt()) <= 2,
                              QStringLiteral("上下分栏：下面那条标签栏紧贴在下栏正文上方"),
                              tabDetail);
                        check(rt.value(QStringLiteral("y")).toInt()
                                  > mt.value(QStringLiteral("y")).toInt()
                                    + mt.value(QStringLiteral("h")).toInt() + 100,
                              QStringLiteral("上下分栏：下面那条标签栏跟着它那一栏到了中间"),
                              tabDetail);
                    }

                    /*
                     * 两块正文都落在正文区里；下面那一栏短出来的正好是它自己
                     * 那条标签栏（35px）+ 中间那条缝。
                     */
                    check(mainView->height() - mirrorView->height() >= 30
                          && mainView->height() - mirrorView->height() <= 56,
                          QStringLiteral("上下分栏：下栏比上栏矮一条标签栏（它自己的那条）"),
                          downDetail);
                    check(qAbs(mainView->width() - mirrorView->width()) <= 4,
                          QStringLiteral("上下分栏：两条都是整幅宽"), downDetail);

                    /*
                     * 光 item 对上还不够：**原生控件**也得跟着新几何摆
                     * （它是子窗口，位置/尺寸由 applyGeometry 从 item 算出来）。
                     * 控件停在旧尺寸上时会盖住分隔线，屏幕上就是"没分开"。
                     */
                    auto widgetDetail = [](const char *who, EditorViewItem *v) {
                        const QVariantMap g = v->paneGeometryForTest();
                        return QStringLiteral("%1 item %2x%3 @%4,%5 / 控件 %6x%7")
                            .arg(QString::fromLatin1(who))
                            .arg(g.value(QStringLiteral("itemW")).toInt())
                            .arg(g.value(QStringLiteral("itemH")).toInt())
                            .arg(g.value(QStringLiteral("sceneX")).toInt())
                            .arg(g.value(QStringLiteral("sceneY")).toInt())
                            .arg(g.value(QStringLiteral("widgetW")).toInt())
                            .arg(g.value(QStringLiteral("widgetH")).toInt());
                    };
                    const QVariantMap mg = mainView->paneGeometryForTest();
                    const QVariantMap rg = mirrorView->paneGeometryForTest();
                    check(qAbs(mg.value(QStringLiteral("itemW")).toInt()
                               - mg.value(QStringLiteral("widgetW")).toInt()) <= 2
                          && qAbs(mg.value(QStringLiteral("itemH")).toInt()
                                  - mg.value(QStringLiteral("widgetH")).toInt()) <= 2,
                          QStringLiteral("上下分栏：主栏的原生控件跟着 item 摆（没停在旧高度）"),
                          widgetDetail("主栏", mainView));
                    check(qAbs(rg.value(QStringLiteral("itemW")).toInt()
                               - rg.value(QStringLiteral("widgetW")).toInt()) <= 2
                          && qAbs(rg.value(QStringLiteral("itemH")).toInt()
                                  - rg.value(QStringLiteral("widgetH")).toInt()) <= 2,
                          QStringLiteral("上下分栏：第二栏的原生控件也跟着 item 摆"),
                          widgetDetail("第二栏", mirrorView));
                }
            }

            dispatch(QStringLiteral("splitNone"));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            check(uiState().value(QStringLiteral("splitMode")).toString().isEmpty(),
                  QStringLiteral("dispatch(splitNone) 之后回到单栏"));
            check(uiState().value(QStringLiteral("mirrorTabCount")).toInt() == 0,
                  QStringLiteral("取消分栏之后第二栏把标签都放回去了"));
            /*
             * 取消分栏只把**第二栏**关掉，主栏自己的标签一条都不能少 ——
             * 分栏时在主栏多开的那一份也还在。
             */
            check(view->documents().size() == beforeMainTabs + 1,
                  QStringLiteral("取消分栏不影响主栏自己的标签"),
                  QStringLiteral("剩 %1 条（分栏前 %2 条 + 分栏期间新开的 1 条）")
                      .arg(view->documents().size()).arg(beforeMainTabs));

            /*
             * 第二栏最后一条标签关掉 = 整条分栏收起。
             *
             * 用户报的"右边那条标签上的小叉点了没反应"就是这一步：小叉本身
             * 是好的（closeDocument 之后那一栏确实空了），坏在**紧接着又自动
             * 补了一份主栏当前文档回去**（原来那条 syncSplitWithDocuments），
             * 标签当场长回来，看着就是点了没用。所以这里钉两件事：
             *   * 走和小叉**同一条路**（closeTab(index, 第二栏那个对象)）；
             *   * 关完之后第二栏空着、分栏也收起（不是补一条回来）。
             */
            dispatch(QStringLiteral("splitRight"));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            const QVariant mirrorPane =
                uiState().value(QStringLiteral("mirrorPane"));
            check(uiState().value(QStringLiteral("mirrorTabCount")).toInt() == 1
                      && mirrorPane.value<QObject *>() != nullptr,
                  QStringLiteral("复核：重新分栏之后拿得到第二栏那个对象、里面有一条标签"));

            QVariant closeRet;
            QMetaObject::invokeMethod(qmlRoot, "closeTab", Q_RETURN_ARG(QVariant, closeRet),
                                      Q_ARG(QVariant, QVariant(0)),
                                      Q_ARG(QVariant, mirrorPane));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            check(uiState().value(QStringLiteral("mirrorTabCount")).toInt() == 0,
                  QStringLiteral("第二栏最后一条标签关掉了（小叉那条路真的关得掉）"));
            check(uiState().value(QStringLiteral("splitMode")).toString().isEmpty(),
                  QStringLiteral("第二栏关空之后分栏整条收起（不再自动补一份回来）"),
                  QStringLiteral("splitMode='%1' 第二栏标签 %2 条")
                      .arg(uiState().value(QStringLiteral("splitMode")).toString())
                      .arg(uiState().value(QStringLiteral("mirrorTabCount")).toInt()));

            /*
             * ---- 上下分栏 + md 预览：那个"源码 / 预览"开关必须还在 ----
             *
             * 用户报的："上下分栏后，md 文档预览，不能切换了，按钮不见了。"
             *
             * 开关在界面上只画一次，而上下分栏时下面那一栏的标签栏是挂在
             * 正文块（paneHolder）里的 —— 预览一开，正文块整块收起来，
             * 开关要是画在那条上就跟着没了，于是切不回源码。
             * 所以它必须落在**预览时也看得见**的那条标签栏上（上面那条）。
             */
            {
                const QString mdPath = dir.filePath(QStringLiteral("split-preview.md"));
                check(writeFile(mdPath, QByteArray("## title\n\nbody\n")),
                      QStringLiteral("准备一份 md（验分栏下的预览开关）"));
                view->openFile(mdPath);
                for (int i = 0; i < 3; ++i)
                    QCoreApplication::processEvents();
                check(uiState().value(QStringLiteral("canPreviewMarkdown")).toBool(),
                      QStringLiteral("分栏预览：这份 md 可以预览"));

                dispatch(QStringLiteral("splitDown"));
                for (int i = 0; i < 3; ++i)
                    QCoreApplication::processEvents();

                /*
                 * 先把预览归零再开：这个偏好是上次退出时记下来的（设置里那一项），
                 * 不一定就是关着的 —— 不归零的话下面"切上去 / 切回来"两次判断
                 * 会正好反过来。
                 */
                if (uiState().value(QStringLiteral("markdownPreview")).toBool()) {
                    dispatch(QStringLiteral("toggleMarkdownPreview"));
                    for (int i = 0; i < 3; ++i)
                        QCoreApplication::processEvents();
                }
                check(!uiState().value(QStringLiteral("markdownPreview")).toBool(),
                      QStringLiteral("分栏预览：起点是源码模式"));

                dispatch(QStringLiteral("toggleMarkdownPreview"));
                for (int i = 0; i < 3; ++i)
                    QCoreApplication::processEvents();
                const QString previewWhy =
                    QStringLiteral("预览 %1 / 能预览 %2 / 当前栏文件 %3 / splitMode %4")
                        .arg(uiState().value(QStringLiteral("markdownPreview")).toBool() ? 1 : 0)
                        .arg(uiState().value(QStringLiteral("canPreviewMarkdown")).toBool() ? 1 : 0)
                        .arg(uiState().value(QStringLiteral("previewFile")).toString())
                        .arg(uiState().value(QStringLiteral("splitMode")).toString());
                check(uiState().value(QStringLiteral("markdownPreview")).toBool(),
                      QStringLiteral("分栏预览：上下分栏下预览开得起来"), previewWhy);

                const QVariantMap lay =
                    uiState().value(QStringLiteral("splitLayout")).toMap();
                const bool mainActs =
                    lay.value(QStringLiteral("mainTabActions")).toBool();
                const bool mirrorActs =
                    lay.value(QStringLiteral("mirrorTabActions")).toBool();
                const bool mainShown =
                    lay.value(QStringLiteral("mainTabShown")).toBool();
                const QString previewDetail =
                    QStringLiteral("开关：上栏 %1 / 下栏 %2；上栏那条看得见 %3")
                        .arg(mainActs ? 1 : 0).arg(mirrorActs ? 1 : 0)
                        .arg(mainShown ? 1 : 0);
                check(mainActs && !mirrorActs && mainShown,
                      QStringLiteral("上下分栏开着预览时，「源码 / 预览」开关还在看得见的那条上"),
                      previewDetail);

                /* 切得回去才算有用（按钮在但点不动一样是坏的） */
                dispatch(QStringLiteral("toggleMarkdownPreview"));
                for (int i = 0; i < 3; ++i)
                    QCoreApplication::processEvents();
                check(!uiState().value(QStringLiteral("markdownPreview")).toBool(),
                      QStringLiteral("上下分栏开着预览时切得回源码"));

                dispatch(QStringLiteral("splitNone"));
                for (int i = 0; i < 3; ++i)
                    QCoreApplication::processEvents();
                /* 收尾：把这份 md 关掉，别影响后面的检查 */
                const int mdTab = view->indexOfPath(mdPath);
                if (mdTab >= 0)
                    view->closeDocument(mdTab);
                for (int i = 0; i < 3; ++i)
                    QCoreApplication::processEvents();
            }
        }

        dispatch(QStringLiteral("closeAllTabs"));
    }

    /* ---- 收尾 ---- */
    dispatch(QStringLiteral("closeAllTabs"));
    check(view->documents().isEmpty(), QStringLiteral("closeAllTabs 之后没有标签"));
    check(!view->hasDocument(), QStringLiteral("空状态：hasDocument = false"));

    /*
     * ============ tab 撑满容器：顶上那条横向滚动条 ============
     *
     * 标签多到装不下时，标签栏顶部要出现一条横向滚动条（见 EditorArea 的
     * ScrollBar.horizontal），而且：
     *
     *   * 没撑满时不出现；
     *   * **出现时也不许把标签栏撑高**（不占高度）：标签栏始终 35px、标签始终
     *     29px 高、标签上沿始终在 y=3 —— 那条横条是浮在标签原有的 3px 上边距
     *     里的，标签和下面的编辑区都不该挪一下；
     *   * 横条左右要让开容器圆角半径那么多，否则会把圆角啃成直角；
     *   * 横条整个在标签上沿之上，不盖住标签。
     *
     * 标签宽 132（短标题取最小值）、间距 3：9 个就撑满（1104px 的标签区）。
     */
    {
        /* 先开两个：没撑满，应该没有横条 */
        dispatch(QStringLiteral("new"));
        dispatch(QStringLiteral("new"));
        const QVariantMap small = uiState().value(QStringLiteral("tabBar")).toMap();
        check(!small.value(QStringLiteral("scrollShown")).toBool(),
              QStringLiteral("两个标签：没撑满，顶部没有滚动条"),
              QStringLiteral("%1 个标签 / 高 %2")
                  .arg(small.value(QStringLiteral("tabs")).toInt())
                  .arg(small.value(QStringLiteral("height")).toDouble()));
        check(qAbs(small.value(QStringLiteral("height")).toDouble() - 35.0) < 0.5,
              QStringLiteral("标签栏是 35px"),
              QStringLiteral("实际 %1").arg(small.value(QStringLiteral("height")).toDouble()));
        check(qAbs(small.value(QStringLiteral("stripHeight")).toDouble() - 29.0) < 0.5,
              QStringLiteral("两个标签时标签高度 29px"),
              QStringLiteral("实际 %1").arg(small.value(QStringLiteral("stripHeight")).toDouble()));
        const double smallStripTop = small.value(QStringLiteral("stripTop")).toDouble();

        /* 再开到 12 个：撑满了，横条出现 */
        for (int i = 0; i < 10; ++i)
            dispatch(QStringLiteral("new"));

        /*
         * 等 QML 把标签重新摆一遍再量。
         *
         * 上面那串 dispatch 是同步返回的：文档已经加进去了，但标签的宽度
         * （Row 的宽度 -> contentWidth）要等这一轮布局跑完才是新的，
         * 滚动条的比例也是跟着 visibleArea 才更新的。不等的话量到的是
         * 上一次布局的旧值（实测量到 278，是只有两个标签时的宽度）。
         */
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap big = uiState().value(QStringLiteral("tabBar")).toMap();
        const double h = big.value(QStringLiteral("height")).toDouble();
        const double w = big.value(QStringLiteral("width")).toDouble();
        const double radius = big.value(QStringLiteral("cornerRadius")).toDouble();
        const double stripTop = big.value(QStringLiteral("stripTop")).toDouble();
        const double stripH = big.value(QStringLiteral("stripHeight")).toDouble();
        const double left = big.value(QStringLiteral("scrollLeft")).toDouble();
        const double right = big.value(QStringLiteral("scrollRight")).toDouble();
        const double barTop = big.value(QStringLiteral("scrollTop")).toDouble();
        const double barBottom = big.value(QStringLiteral("scrollBottom")).toDouble();
        const QString geom = QStringLiteral("栏 %1x%2 圆角 %3 / 横条 x %4..%5 y %6..%7 / 标签 %8..%9"
                                            " / size %10 pos %11 可见 %12 / Flickable %13 内容 %14")
                                 .arg(w).arg(h).arg(radius).arg(left).arg(right)
                                 .arg(barTop).arg(barBottom).arg(stripTop)
                                 .arg(stripTop + stripH)
                                 .arg(big.value(QStringLiteral("scrollSize")).toDouble())
                                 .arg(big.value(QStringLiteral("scrollPosition")).toDouble())
                                 .arg(big.value(QStringLiteral("scrollVisible")).toBool())
                                 .arg(big.value(QStringLiteral("flickWidth")).toDouble())
                                 .arg(big.value(QStringLiteral("flickContent")).toDouble());

        check(big.value(QStringLiteral("scrollShown")).toBool(),
              QStringLiteral("12 个标签：撑满了，顶部出现横向滚动条"),
              QStringLiteral("%1 个标签").arg(big.value(QStringLiteral("tabs")).toInt()));
        /*
         * 不占高度这条是重点：撑满之后标签栏高度、标签高度、标签上沿
         * 都必须和没撑满时一模一样。
         */
        check(qAbs(h - small.value(QStringLiteral("height")).toDouble()) < 0.5,
              QStringLiteral("横条出现时标签栏没被撑高（还是 35px）"), geom);
        check(qAbs(stripTop - smallStripTop) < 0.5,
              QStringLiteral("标签上沿没挪（横条浮在它上面那条 3px 里）"), geom);
        check(qAbs(stripH - 29.0) < 0.5,
              QStringLiteral("标签本身还是 29px 高"), geom);
        check(qAbs((barBottom - barTop) - 3.0) < 0.5,
              QStringLiteral("横条高 3px"), geom);
        check(left >= radius - 0.5 && (w - right) >= radius - 0.5,
              QStringLiteral("横条左右各让开容器圆角，没压在圆角上"), geom);
        check(barTop >= -0.5 && barBottom <= stripTop + 0.5,
              QStringLiteral("横条贴在容器顶边上、整个在标签上沿之上（不盖标签）"), geom);
        check(right > left, QStringLiteral("横条有实际宽度"), geom);
        check(barBottom <= h - 0.5 && right <= w - 0.5,
              QStringLiteral("横条整个在标签栏里面"), geom);

        /* 关掉多余的，回到一个：横条应该收回去（同样要等这一轮布局） */
        dispatch(QStringLiteral("closeAllTabs"));
        dispatch(QStringLiteral("new"));
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const QVariantMap again = uiState().value(QStringLiteral("tabBar")).toMap();
        check(!again.value(QStringLiteral("scrollShown")).toBool(),
              QStringLiteral("标签又少了：滚动条收回去，标签栏还是 35px"),
              QStringLiteral("高 %1").arg(again.value(QStringLiteral("height")).toDouble()));
        dispatch(QStringLiteral("closeAllTabs"));
    }

    /*
     * ================= 内容区 tab 的右键菜单 =================
     *
     * 两件事要钉住：
     *
     *  1) 菜单里的动作是按**被右键的那个标签**来的，不是当前激活的那个。
     *     右键一个没激活的标签时两者不是同一个，"关闭其他"要留下点中的那个、
     *     关掉其余的全部；动错标签就是这个功能最典型的 bug。
     *     这里用"文件标签 + 空白标签"两种标签来分辨：文件标签有 filePath、
     *     空白标签没有，关错了从 filePath 上一眼就能看出来。
     *
     *  2) 菜单左上角紧贴鼠标右键那一点（DropdownMenu.openAtPoint）。
     *     这条只能量：菜单 x/y 必须正好等于传进去的坐标。
     */
    check(view->openFile(srcPath) >= 0, QStringLiteral("tab 菜单用例：打开文件标签"));
    dispatch(QStringLiteral("new"));
    check(view->documents().size() == 2, QStringLiteral("tab 菜单用例：文件标签 + 空白标签"),
          QStringLiteral("实际 %1 个").arg(view->documents().size()));

    /* closeTab:<i>：关的是下标 0 那个（文件），当前标签是 1（空白） */
    dispatch(QStringLiteral("closeTab:0"));
    check(view->documents().size() == 1, QStringLiteral("closeTab:0 只关掉一个标签"),
          QStringLiteral("剩 %1 个").arg(view->documents().size()));
    check(view->filePath().isEmpty(),
          QStringLiteral("关掉的是下标 0 那个文件标签，活下来的是空白标签"),
          view->filePath());

    /* closeOthers:<i>：留下的是下标 1（文件），当前标签是下标 2 */
    check(view->openFile(srcPath) >= 0, QStringLiteral("tab 菜单用例：再打开文件标签"));
    dispatch(QStringLiteral("new"));
    check(view->documents().size() == 3 && view->currentIndex() == 2,
          QStringLiteral("tab 菜单用例：三个标签，当前的不是要留下的那个"),
          QStringLiteral("共 %1 个 / 当前下标 %2")
              .arg(view->documents().size()).arg(view->currentIndex()));

    dispatch(QStringLiteral("closeOthers:1"));
    check(view->documents().size() == 1, QStringLiteral("closeOthers:1 只留一个标签"),
          QStringLiteral("剩 %1 个").arg(view->documents().size()));
    check(view->filePath() == QFileInfo(srcPath).absoluteFilePath(),
          QStringLiteral("留下的是下标 1 那个文件标签（不是当前标签）"), view->filePath());

    /*
     * 菜单条目：关闭那一组 + 分栏那一组 + 与此文件对比。
     *
     * 下标跟着"被右键的那个标签"走（这里问的是下标 1）—— 关闭/关闭其他那两条
     * 必须带 :1，分栏和对比是"对当前编辑器 / 当前文档"的动作，不带下标。
     */
    {
        QString acts;
        const QVariantList list = tabMenuActs(1);
        for (const QVariant &a : list)
            acts += (acts.isEmpty() ? QString() : QStringLiteral(" | ")) + a.toString();
        check(acts == QStringLiteral("closeTab:1 | closeOthers:1 | closeAllTabs"
                                     " | separator | splitRight | splitDown | splitNone"
                                     " | separator | compareTab:1"),
              QStringLiteral("tab 菜单 = 关闭那一组 + 分栏那一组 + 与此文件对比，下标是点中的那个"),
              acts);
    }

    /*
     * 左上角就落在鼠标那一点上。
     *
     * anchor 传 null：坐标直接按宿主窗口内容区算，自检不用真的去点某个标签
     * （标签当锚点时走的是同一行 mapToItem，和菜单栏那套 openFor 共用）。
     * 菜单开着不动它：和下面那组长菜单检查一样，进程随后就退出了。
     */
    const double probeX = 300.0;
    const double probeY = 120.0;
    QMetaObject::invokeMethod(qmlRoot, "openTabMenu",
                              Q_ARG(QVariant, QVariant()),      /* 哪一栏：给空 = 当前栏 */
                              Q_ARG(QVariant, QVariant(0)),
                              Q_ARG(QVariant, QVariant()),
                              Q_ARG(QVariant, QVariant(probeX)),
                              Q_ARG(QVariant, QVariant(probeY)));
    {
        const QVariantMap ui = uiState();
        const double mx = ui.value(QStringLiteral("menuX")).toDouble();
        const double my = ui.value(QStringLiteral("menuY")).toDouble();
        const double contentH = ui.value(QStringLiteral("menuContentHeight")).toDouble();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("openTabMenu 弹出菜单（tab 右键那条路）"));
        check(qAbs(mx - probeX) < 0.5 && qAbs(my - probeY) < 0.5,
              QStringLiteral("菜单左上角紧贴鼠标点（300,120）"),
              QStringLiteral("实际 (%1, %2)").arg(mx).arg(my));
        /*
         * 内容高度 = 条目数 × 行高 + 分隔线 + 内边距。
         *
         * 不写死 92：tab 菜单现在多了分栏和"与此文件对比"那几条（见
         * js/EditorMenus.js 的 tabMenu）。这里按**实际条目**算一遍 ——
         * 钉的是"菜单确实按每项 28px 排出来的"，条目增删不用改这一行。
         */
        const int tabEntryCount = tabMenuActs(0).size();
        int tabSeparators = 0;
        for (const QVariant &a : tabMenuActs(0)) {
            if (a.toString() == QLatin1String("separator"))
                ++tabSeparators;
        }
        const double expectH = (tabEntryCount - tabSeparators) * 28.0 + tabSeparators * 9.0 + 8.0;
        check(qAbs(contentH - expectH) < 0.5,
              QStringLiteral("菜单按每项 28px 排出来（条目数对得上）"),
              QStringLiteral("内容高 %1（期望 %2，条目 %3 + 分隔线 %4）")
                  .arg(contentH).arg(expectH).arg(tabEntryCount - tabSeparators)
                  .arg(tabSeparators));
    }

    /*
     * ================= 左侧项目树标题栏那排按钮 =================
     *
     * 标题栏现在摆着七件事：新建 md / 刷新 / 定位 / 全部折叠 / 全部展开 / 更多 /
     * 收起面板。这里钉三件事：
     *
     *  1) 七个按钮真的摆在标题栏里（个数是从标题栏那排 RowLayout 里数出来的，
     *     不是写死的常量，见 FolderTree.toolbarButtonCount）；
     *  2) 全部折叠 / 全部展开真的把日期文件夹收拢 / 铺开（量的是 treeRows
     *     的行数，不是只看那个布尔量）；
     *  3) 收起面板把**布局槽位**收成 0，再点一次原样回来 ——
     *     只改标志位、宽度没跟着走，从界面上是一眼能看出来的。
     *
     * 菜单那几条走 treeMenuActs()（和"更多"弹出的是同一份构造）。
     */
    {
        QString acts;
        for (const QVariant &a : treeMenuActs())
            acts += (acts.isEmpty() ? QString() : QStringLiteral(" | ")) + a.toString();
        check(acts == QStringLiteral("treeNew | refresh | treeLocate | treeExpandAll"
                                     " | treeCollapseAll | treeSortNewest | treeSortOldest"
                                     " | treeImportFolder | treeOpenRoot | treeChooseRoot"
                                     " | treeHide"),
              QStringLiteral("左树\"更多\"菜单 = 新建 / 刷新 / 定位 / 全展开 / 全折叠 / "
                             "排序 / 导入文件夹 / 保存位置 / 收起面板"),
              acts);

        /*
         * 标题「项目 ∨」那份是**看哪一份**（和 PyCharm 一样三条），和 ⋯ 那份分开：
         * 标题管视图、工具按钮管动作（用户报的"项目弹框改成图里那三项"）。
         */
        {
            QString scopeActs;
            for (const QVariant &a : treeScopeMenuActs())
                scopeActs += (scopeActs.isEmpty() ? QString() : QStringLiteral(" | "))
                             + a.toString();
            check(scopeActs == QStringLiteral("treeScope:project | treeScope:projectFiles"
                                              " | treeScope:openFiles"),
                  QStringLiteral("标题「项目」菜单 = 项目 / 项目文件 / 打开的文件"),
                  scopeActs);

            /*
             * 三条都真的切得动：平铺那两条一个文件夹行都不该有，
             * 切回"项目"还得是原来那棵树。
             */
            const int treeFolderRows = treeState().value(QStringLiteral("folderRows")).toInt();
            check(treeFolderRows > 0, QStringLiteral("默认那份（项目）里有文件夹行"),
                  QStringLiteral("实际 %1 行").arg(treeFolderRows));

            dispatch(QStringLiteral("treeScope:projectFiles"));
            settle();
            {
                const QVariantMap s = treeState();
                check(s.value(QStringLiteral("scope")).toString() == QLatin1String("projectFiles")
                          && s.value(QStringLiteral("folderRows")).toInt() == 0
                          && s.value(QStringLiteral("rows")).toInt() > 0,
                      QStringLiteral("切到「项目文件」：所有文件平铺，一行文件夹都没有"),
                      QStringLiteral("scope=%1 行 %2 / 文件夹行 %3")
                          .arg(s.value(QStringLiteral("scope")).toString())
                          .arg(s.value(QStringLiteral("rows")).toInt())
                          .arg(s.value(QStringLiteral("folderRows")).toInt()));
            }

            dispatch(QStringLiteral("treeScope:project"));
            settle();
            check(treeState().value(QStringLiteral("scope")).toString() == QLatin1String("project")
                      && treeState().value(QStringLiteral("folderRows")).toInt() > 0,
                  QStringLiteral("切回「项目」：树还是原来那棵（文件夹行回来了）"));
        }

        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("treeToolbarButtons")).toInt() == 7,
              QStringLiteral("标题栏摆着七个工具按钮（新建 / 刷新 / 定位 / 全折 / 全展 / 更多 / 收起）"),
              QStringLiteral("实际 %1 个")
                  .arg(ui.value(QStringLiteral("treeToolbarButtons")).toInt()));

        const QVariantMap before = treeState();
        const int folders = before.value(QStringLiteral("folderCount")).toInt();
        /*
         * 日期文件夹的个数由**磁盘上有哪些日期目录**决定（自检跑在临时保存
         * 目录上，所以只有今天一个），导入的目录算在同一个计数里。
         */
        check(folders >= 1, QStringLiteral("左树上至少有一个日期文件夹"),
              QStringLiteral("实际 %1 个").arg(folders));

        dispatch(QStringLiteral("treeCollapseAll"));
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("openFolders")).toInt() == 0,
                  QStringLiteral("全部折叠：日期文件夹都收起来了"));
            /*
             * 折叠干净之后，剩下的就是"最外层那几行"。
             *
             * 这里不能拿 folderCount 比：导入的目录能往下套（chat/frontend），
             * 那些子目录折叠时本来就不占行 —— 用户设置里挂着导入目录时，
             * 老写法会数出"行 2 / 文件夹 3"这种假红。
             */
            check(s.value(QStringLiteral("rows")).toInt()
                      == before.value(QStringLiteral("topLevelRows")).toInt(),
                  QStringLiteral("折叠后树里只剩最外层那几行"),
                  QStringLiteral("实际 %1 行 / 最外层 %2 行")
                      .arg(s.value(QStringLiteral("rows")).toInt())
                      .arg(before.value(QStringLiteral("topLevelRows")).toInt()));
        }

        dispatch(QStringLiteral("treeExpandAll"));
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("openFolders")).toInt() == folders,
                  QStringLiteral("全部展开：日期文件夹都开了"));
            check(s.value(QStringLiteral("rows")).toInt() >= folders,
                  QStringLiteral("展开后行数不少于文件夹数"),
                  QStringLiteral("实际 %1 行").arg(s.value(QStringLiteral("rows")).toInt()));
            /* 展开之后，当天的那些 md 应该真的作为文件行出现在树里 */
            check(s.value(QStringLiteral("rows")).toInt() > folders,
                  QStringLiteral("展开后有文件行（日期文件夹下面挂着 md）"),
                  QStringLiteral("%1 行 / %2 个文件夹")
                      .arg(s.value(QStringLiteral("rows")).toInt()).arg(folders));
        }

        /*
         * 左树右键菜单：文件一条路、文件夹一条路（见 js/EditorMenus.js 的
         * fileContextMenu / folderContextMenu）。这里钉"菜单里有这几条"
         * 以及"每条都带着那个文件 / 文件夹的路径" —— 动作名是
         * fileOpen:<路径> 这种前缀形式，dispatch 按前缀切。
         */
        {
            QStringList fileActs;
            for (const QVariant &a : treeRowMenuActs(QStringLiteral("file")))
                fileActs << a.toString();
            check(fileActs.size() == 4,
                  QStringLiteral("文件右键菜单四条（打开 / 重命名 / 删除 / 在文件夹中显示）"),
                  QStringLiteral("实际 %1 条").arg(fileActs.size()));
            check(fileActs.value(0).startsWith(QStringLiteral("fileOpen:"))
                      && fileActs.value(1).startsWith(QStringLiteral("fileRename:"))
                      && fileActs.value(2).startsWith(QStringLiteral("fileDelete:"))
                      && fileActs.value(3).startsWith(QStringLiteral("fileReveal:")),
                  QStringLiteral("四条各带自己的动作前缀"),
                  fileActs.join(QLatin1Char('/')));
            {
                /* 菜单里带的那条路径得是磁盘上真有的那份 md */
                const QString acted = fileActs.value(0).mid(int(qstrlen("fileOpen:")));
                check(!acted.isEmpty() && QFileInfo::exists(acted),
                      QStringLiteral("菜单里带的就是磁盘上那份文件"), acted);
            }

            QStringList folderActs;
            for (const QVariant &a : treeRowMenuActs(QStringLiteral("folder")))
                folderActs << a.toString();
            check(folderActs.size() == 3
                      && folderActs.value(0) == QLatin1String("treeNew")
                      && folderActs.value(1) == QLatin1String("refresh")
                      && folderActs.value(2).startsWith(QStringLiteral("folderReveal:")),
                  QStringLiteral("文件夹右键菜单三条（新建 / 刷新 / 在文件夹中显示）"),
                  folderActs.join(QLatin1Char('/')));

            /*
             * "便签"那一格（左侧那排工具格里的便签图标）的右键菜单。
             *
             * 这一格以前弹的是 Qt Quick Controls 的 `Menu` —— 白底、没图标，
             * 和界面里其它菜单长相不一致（用户截图报的就是这个）。现在走共用那份
             * DropdownMenu（深色 + 图标 + 右边快捷键），条目在 js/EditorMenus.js
             * 的 notesMenu 里，act 用的是"文件"菜单里同一批（dispatch 落到
             * Notes 那四条命令上，和托盘菜单是同一份实现）。
             */
            {
                QStringList noteActs;
                for (const QVariant &a : notesMenuActs())
                    noteActs << a.toString();
                check(noteActs.size() == 4
                          && noteActs.value(0) == QLatin1String("note")
                          && noteActs.value(1) == QLatin1String("notesArrange")
                          && noteActs.value(2) == QLatin1String("notesShowAll")
                          && noteActs.value(3) == QLatin1String("notesHideAll"),
                      QStringLiteral("便签那格右键菜单四条（新建 / 排列 / 显示全部 / 收起全部）"),
                      noteActs.join(QLatin1Char('/')));
            }

            /*
             * 右键菜单指着的那一行要有灰黑底。
             *
             * 蓝底（rowHighlight）是"这份文件开在编辑器里"，和"菜单要动哪一行"
             * 是两件事：右键一个没打开的文件时，光看菜单看不出动的是谁，所以
             * 那一行单独画一层灰黑底。这里走的就是界面上那条路
             * （openTreeRowMenuFor -> openTreeRowMenu），量的也是委托自己
             * 报上来的 rowContext。
             */
            QVariant ctxPath;
            QMetaObject::invokeMethod(qmlRoot, "openTreeRowMenuFor", Q_RETURN_ARG(QVariant, ctxPath),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("file"))));
            settle();
            {
                const QVariantMap s = treeState();
                const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
                check(!ctxPath.toString().isEmpty()
                          && s.value(QStringLiteral("contextPath")).toString() == ctxPath.toString(),
                      QStringLiteral("右键：菜单指着的那一行记下来了"),
                      QStringLiteral("菜单行 %1 / 树上报 %2")
                          .arg(ctxPath.toString())
                          .arg(s.value(QStringLiteral("contextPath")).toString()));
                check(hl.value(QStringLiteral("context")).toInt() == 1,
                      QStringLiteral("右键：那一行画上了灰黑底"),
                      QStringLiteral("带着灰黑底的行 %1")
                          .arg(hl.value(QStringLiteral("context")).toInt()));
            }

            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            settle();
            {
                const QVariantMap s = treeState();
                const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
                check(s.value(QStringLiteral("contextPath")).toString().isEmpty()
                          && hl.value(QStringLiteral("context")).toInt() == 0,
                      QStringLiteral("右键：菜单收起后灰黑底也撤掉"),
                      QStringLiteral("菜单行 %1 / 灰黑底 %2")
                          .arg(s.value(QStringLiteral("contextPath")).toString())
                          .arg(hl.value(QStringLiteral("context")).toInt()));
            }

            /*
             * 一级（日期文件夹）和二级（文件）的图标要落在同一列上。
             *
             * 文件夹行比文件行多一格展开箭头，那一格文件行不占宽 —— 少补
             * 16-14=2px 的话两级的图标就是歪的（用户报的"没对齐"）。量的是
             * 委托自己报出来的坐标（TreeDelegate.iconCellX），不是把缩进公式
             * 在 C++ 这侧再算一遍。
             */
            {
                const QVariantMap cols =
                    treeState().value(QStringLiteral("iconColumns")).toMap();
                const double folderX = cols.value(QStringLiteral("folder")).toDouble();
                const double fileX = cols.value(QStringLiteral("file")).toDouble();
                check(folderX > 0 && fileX > 0 && qAbs(folderX - fileX) < 0.5,
                      QStringLiteral("左树：一级 / 二级图标左边对齐"),
                      QStringLiteral("文件夹图标 x=%1 / 文件图标 x=%2").arg(folderX).arg(fileX));
            }

            /*
             * 标题「项目」的左边要和一级行的**展开箭头**对齐（用户报的"树往左靠、
             * 标题别动"，观感照 PyCharm：项目那一行正好在标题下面）。
             *
             * 量的是**文字**的左边缘（FolderTree.titleTextX），不是那个胶囊 ——
             * 胶囊左右各留 5px，看着没对齐的正是字。
             *
             * 比的是**面板坐标**里的两个数：列表能横向滚（leftMargin 那几 px 就在
             * contentX 上），滚过之后箭头/图标的场景坐标会挪，而标题在列表外面不动
             * —— 直接比场景坐标会随滚动飘。
             */
            {
                const QVariantMap s = treeState();
                const double panelX = s.value(QStringLiteral("panelX")).toDouble();
                const double titlePanelX =
                    s.value(QStringLiteral("titleTextX")).toDouble() - panelX;
                const double chevronPanelX =
                    s.value(QStringLiteral("firstChevronPanelX")).toDouble();
                check(chevronPanelX > 0 && qAbs(titlePanelX - chevronPanelX) <= 1.0,
                      QStringLiteral("左树：标题「项目」和一级行的展开箭头左边对齐"),
                      QStringLiteral("标题 x=%1 / 箭头 x=%2（面板坐标，1px 是取整误差）")
                          .arg(titlePanelX)
                          .arg(chevronPanelX));
            }

            /*
             * 导入的文件夹要**原样**列出来：什么后缀都收（src 里的 .cpp/.h）、
             * 隐藏目录（.idea）要进去、一个文件都没有的空目录也得有一行。
             *
             * 用户报的就是这个：导入 H:\test 之后，TetrisGame\src 整块不见了
             * （上一版只收 md / markdown / txt），.idea 那个空目录也没有节点
             * （树是按文件拼的，没文件的目录出不来）。这里照那个形状造一份：
             *
             *     imported-project/
             *         README.md
             *         src/main.cpp
             *         .idea/            <- 空目录
             *         shot.png          <- 二进制，不许当文本解析
             */
            {
                const QString projectDir = dir.filePath(QStringLiteral("imported-project"));
                QDir().mkpath(projectDir + QStringLiteral("/src"));
                QDir().mkpath(projectDir + QStringLiteral("/.idea"));
                /* 依赖目录：只该留一行，里面的东西不进去扫（见 ClipboardStore::scanFolder） */
                QDir().mkpath(projectDir + QStringLiteral("/node_modules/dep"));
                auto writeFile = [](const QString &path, const QByteArray &bytes) {
                    QFile f(path);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        f.write(bytes);
                };
                writeFile(projectDir + QStringLiteral("/README.md"), "## 07:31:00\nhello\n");
                writeFile(projectDir + QStringLiteral("/src/main.cpp"), "int main() {}\n");
                writeFile(projectDir + QStringLiteral("/node_modules/dep/index.js"),
                          "module.exports = 1;\n");
                /* 带 NUL 的假图片：looksLikeText 该把它当二进制 */
                writeFile(projectDir + QStringLiteral("/shot.png"),
                          QByteArray("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));

                check(store->addImportedFolder(projectDir),
                      QStringLiteral("导入用例：整个项目目录挂到左树上"));
                settle();

                /* 直接在"树"那份数据上找 —— QML 画的左树就是它 */
                std::function<bool(const QVariantList &, const QString &)> hasLabel =
                    [&](const QVariantList &list, const QString &label) -> bool {
                    for (const QVariant &v : std::as_const(list)) {
                        const QVariantMap m = v.toMap();
                        if (m.value(QStringLiteral("label")).toString() == label)
                            return true;
                        if (hasLabel(m.value(QStringLiteral("children")).toList(), label))
                            return true;
                    }
                    return false;
                };
                const QVariantList nodes = store->tree(QString(), true);
                check(hasLabel(nodes, QStringLiteral("src")),
                      QStringLiteral("导入：子目录 src 在树上（里面的 .cpp 也算数）"));
                check(hasLabel(nodes, QStringLiteral("main.cpp")),
                      QStringLiteral("导入：src 里的 .cpp 文件在树上"));
                check(hasLabel(nodes, QStringLiteral(".idea")),
                      QStringLiteral("导入：空目录 .idea 也在树上"));
                check(hasLabel(nodes, QStringLiteral("shot.png")),
                      QStringLiteral("导入：png 这类二进制也在树上"));

                /*
                 * 依赖目录（node_modules…）：**留一行，但不进去扫**。
                 *
                 * 这是"导入大文件夹卡死"的正解：H:\chat 三万四千个文件，三万三千
                 * 个在 node_modules 里，全过一遍库就是十几秒的卡死（实测）。主流
                 * 编辑器也是这么办的（排除依赖 / 构建目录）。那一行标成"未索引"，
                 * 不冒充"0 个文件"。
                 */
                {
                    std::function<QVariantMap(const QVariantList &, const QString &)> findLabel =
                        [&](const QVariantList &list, const QString &label) -> QVariantMap {
                        for (const QVariant &v : std::as_const(list)) {
                            const QVariantMap m = v.toMap();
                            if (m.value(QStringLiteral("label")).toString() == label)
                                return m;
                            const QVariantMap nested =
                                findLabel(m.value(QStringLiteral("children")).toList(), label);
                            if (!nested.isEmpty())
                                return nested;
                        }
                        return {};
                    };
                    const QVariantMap deps = findLabel(nodes, QStringLiteral("node_modules"));
                    check(!deps.isEmpty(),
                          QStringLiteral("导入：依赖目录 node_modules 留了一行"));
                    check(deps.value(QStringLiteral("skipped")).toBool(),
                          QStringLiteral("导入：那一行标着「未索引」（没进去扫）"),
                          QStringLiteral("skipped=%1")
                              .arg(deps.value(QStringLiteral("skipped")).toString()));
                    /*
                     * 判据只看这一棵子树：用户设置里可能还挂着别的导入目录，
                     * 树里别处出现同名的 index.js 不算数（第一版就这么误报了）。
                     */
                    check(deps.value(QStringLiteral("children")).toList().isEmpty()
                              && deps.value(QStringLiteral("files")).toInt() == 0,
                          QStringLiteral("导入：依赖目录里的文件一个都没列（不扫进去）"),
                          QStringLiteral("子节点 %1 / 文件 %2")
                              .arg(deps.value(QStringLiteral("children")).toList().size())
                              .arg(deps.value(QStringLiteral("files")).toInt()));
                }

                /* 二进制不当文本读：条数必须是 0，不能从乱码里数出几段来 */
                {
                    QVariant entries = -1;
                    for (const QVariant &v : std::as_const(nodes)) {
                        const QVariantMap root0 = v.toMap();
                        if (root0.value(QStringLiteral("label")).toString()
                            != QLatin1String("imported-project"))
                            continue;
                        for (const QVariant &c : root0.value(QStringLiteral("children")).toList()) {
                            const QVariantMap m = c.toMap();
                            if (m.value(QStringLiteral("label")).toString()
                                == QLatin1String("shot.png"))
                                entries = m.value(QStringLiteral("entries"));
                        }
                    }
                    check(entries.toInt() == 0,
                          QStringLiteral("导入：png 按二进制处理（不解析内容，条数 0）"),
                          QStringLiteral("条数 %1").arg(entries.toInt()));
                }

                /* 编辑器也不许把二进制当文本打开（灌进去就是乱码，存回去就毁了） */
                check(view->openFile(projectDir + QStringLiteral("/shot.png")) < 0,
                      QStringLiteral("二进制文件编辑器不开（拒绝而不是灌乱码）"),
                      view->lastError());

                /* 收尾：别把用户自己的导入列表改了 */
                check(store->removeImportedFolder(projectDir),
                      QStringLiteral("导入用例：收尾把目录移除"));
                settle();
                QDir(projectDir).removeRecursively();
            }
        }

        /*
         * 设置面板的"存储"栏。
         *
         * 那一栏里全是绑定（保存位置 / 文件数 / 内容条数 / 导入的文件夹），
         * 打开它等于把这些绑定真算一遍 —— QML 侧的绑定错误只有算过才暴露
         * （"Sequence length out of range" 就是这么抓出来的）。
         */
        dispatch(QStringLiteral("storage"));
        settle();
        {
            const QVariantMap panel = uiState();
            check(panel.value(QStringLiteral("settingsOpened")).toBool()
                      && panel.value(QStringLiteral("settingsSection")).toString()
                             == QLatin1String("storage"),
                  QStringLiteral("dispatch(storage) 打开设置面板的\"存储\"栏"),
                  panel.value(QStringLiteral("settingsSection")).toString());
            check(panel.value(QStringLiteral("storageRoot")).toString() == store->rootPath(),
                  QStringLiteral("\"存储\"栏显示的就是当前保存位置"),
                  panel.value(QStringLiteral("storageRoot")).toString());
            /*
             * 内容目录 = 保存位置下面的「剪贴板」那一层。
             *
             * 界面上单独列了这一行（不然用户只知道"保存位置"，找不到文件到底在哪），
             * 这里钉住它确实是 rootPath 的子目录、而且真的存在。
             */
            check(panel.value(QStringLiteral("storageContentRoot")).toString()
                      == store->contentRoot()
                      && store->contentRoot().startsWith(store->rootPath()),
                  QStringLiteral("\"存储\"栏显示的内容目录在保存位置下面"),
                  panel.value(QStringLiteral("storageContentRoot")).toString());
            check(panel.value(QStringLiteral("storageEntries")).toInt() == store->entryCount(),
                  QStringLiteral("\"存储\"栏的内容条数和元数据一致"),
                  QStringLiteral("面板 %1 / 元数据 %2")
                      .arg(panel.value(QStringLiteral("storageEntries")).toInt())
                      .arg(store->entryCount()));
        }
        QMetaObject::invokeMethod(qmlRoot, "closeSettings");
        settle();

        /*
         * 「帮助」那一栏：**点一下直接开"关于 SmartClip"**，不弹下拉菜单。
         *
         * 走 Main.qml 的 activateMenuTab（它转给 TopBar::activateTab）—— 和鼠标点
         * 那一栏是**同一个函数**。用户要的就是这一条：原来它弹两项（快捷键一览 /
         * 关于 SmartClip），现在一步到位，快捷键一览在设置面板里本来就有。
         */
        {
            QMetaObject::invokeMethod(qmlRoot, "activateMenuTab",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("帮助"))));
            settle();
            const QVariantMap panel = uiState();
            check(panel.value(QStringLiteral("settingsOpened")).toBool()
                      && panel.value(QStringLiteral("settingsSection")).toString()
                             == QLatin1String("about"),
                  QStringLiteral("点「帮助」直接开\"关于 SmartClip\"（不弹菜单）"),
                  QStringLiteral("打开=%1 栏目=%2")
                      .arg(panel.value(QStringLiteral("settingsOpened")).toBool())
                      .arg(panel.value(QStringLiteral("settingsSection")).toString()));
            /* 菜单不该跟着弹出来 */
            check(!uiState().value(QStringLiteral("menuOpened")).toBool(),
                  QStringLiteral("点「帮助」不会弹出下拉菜单"));
            QMetaObject::invokeMethod(qmlRoot, "closeSettings");
            settle();
        }

        /*
         * 菜单挂在**被点的那一栏下方**，不是窗口最左边。
         *
         * 这里真出过问题：TopBar::activateTab 的锚点写死成 appBadge（左边那个应用
         * 图标），于是点「设置」菜单从最左边弹出来，整条偏移到应用图标底下去了
         * （用户报的"全部偏移菜单了"）。
         *
         * 判据：菜单实际画在窗口里的左上角 x，要落在「设置」那一栏的宽度范围内 ——
         * 菜单是独立原生窗口，锚点对不对只有把两边的坐标都拿出来比才知道。
         */
        {
            QVariant tabLeft;
            QMetaObject::invokeMethod(qmlRoot, "topBarTabLeft", Q_RETURN_ARG(QVariant, tabLeft),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("设置"))));
            QMetaObject::invokeMethod(qmlRoot, "clickMenuTab",
                                      Q_ARG(QVariant, QVariant(QStringLiteral("设置"))));
            settle();
            const QVariantMap ui = uiState();
            const double menuLeft = ui.value(QStringLiteral("menuX")).toDouble();
            const double barLeft = tabLeft.toDouble();
            const double barWidth = ui.value(QStringLiteral("menuAnchorWidth")).toDouble();
            check(ui.value(QStringLiteral("menuOpened")).toBool(),
                  QStringLiteral("点「设置」打开了下拉菜单"));
            check(barLeft >= 0 && menuLeft >= barLeft && menuLeft <= barLeft + barWidth + 1,
                  QStringLiteral("菜单挂在被点的那一栏下方（不是窗口最左边）"),
                  QStringLiteral("菜单 x=%1，「设置」那一栏 %2..%3")
                      .arg(menuLeft).arg(barLeft).arg(barLeft + barWidth));

            /*
             * 主窗口挪了，菜单要**收起来**（不能留在原地错位）。
             *
             * 这是用户报的那条"整个菜单没挂在「视图」那一栏下面"：菜单是**独立原生
             * 窗口**（popupType: Popup.Window），屏幕位置在开出来那一刻就算死了 ——
             * 主窗口后来一挪（拖窗口 / 最大化还原 / 系统贴边吸附 / 换显示器），
             * 弹窗还钉在原来的屏幕位置上。实测偏过 700 多像素
             * （见 build\probe-submenu*.ps1 那几套探针）。
             *
             * 判据：宿主窗口一移动，菜单必须已经收起来 —— 只要还开着，它就在
             * 原来的屏幕位置上（离被点的那一栏越来越远），那正是用户看到的那一幕。
             * 挪完再挪回去，后面的检查都按窗口原来的几何算。
             */
            {
                QVariant moved;
                const int dx = 137, dy = 61;
                QMetaObject::invokeMethod(qmlRoot, "moveHostForTest",
                                          Q_RETURN_ARG(QVariant, moved),
                                          Q_ARG(QVariant, QVariant(dx)),
                                          Q_ARG(QVariant, QVariant(dy)));
                settle();

                check(moved.toBool(), QStringLiteral("自检：宿主窗口挪了一段"));
                check(!uiState().value(QStringLiteral("menuOpened")).toBool(),
                      QStringLiteral("宿主窗口一移动，菜单就收起来（不会留在原地错位）"),
                      QStringLiteral("菜单还开着（x=%1）")
                          .arg(uiState().value(QStringLiteral("menuX")).toDouble()));

                /* 挪回去 */
                QMetaObject::invokeMethod(qmlRoot, "moveHostForTest",
                                          Q_RETURN_ARG(QVariant, moved),
                                          Q_ARG(QVariant, QVariant(-dx)),
                                          Q_ARG(QVariant, QVariant(-dy)));
                settle();
            }

            /*
             * 菜单在上一步（挪窗口）里已经自己收起来了 —— 原来这里是"再点一下
             * 同一条 = 收起来"，菜单已经关着的话那一发反而会把它重新打开，
             * 所以改成明确收尾：这一段结束时菜单必须是关着的。
             */
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            settle();
        }

        /*
         * 设置面板这块窗口本身的两条要求。
         *
         *  1) 点面板外面的空白处不许自己收起来 —— 面板里那几个按钮弹的是
         *     **系统**文件对话框，用户去点那个对话框，按"点外面就收"的老规矩
         *     面板会先一步没掉（而它本该一直开着）。关它只能靠标题栏的 ✕ / Esc。
         *  2) 会话框出现时面板得让开一条路：这块窗口是 Qt 按 Popup.Window 建的，
         *     flags 里带着 WindowStaysOnTopHint（下面报出来那一行），Windows 上
         *     非置顶窗口永远盖不住置顶窗口，所以对话框只能出现在面板下面 ——
         *     见 Main.qml 的 withSettingsPanelAway。
         */
        dispatch(QStringLiteral("storage"));
        settle();
        {
            QWindow *panelWin = nullptr;
            for (QWindow *w : QGuiApplication::topLevelWindows()) {
                if (w->isVisible() && w->width() > 700 && w->width() < 900 && w->height() > 400)
                    panelWin = w;
            }
            check(panelWin != nullptr, QStringLiteral("设置面板：那块窗口开出来了"));
            if (panelWin) {
                /* 只报一声不断言：这是 Qt 建窗口的规矩，不是我们的设定 */
                out() << "        （设置面板窗口 flags = 0x"
                      << QString::number(int(panelWin->flags()), 16) << "）" << Qt::endl;
            }
            check(!uiState().value(QStringLiteral("settingsClosesOnOutside")).toBool(),
                  QStringLiteral("设置面板：点面板外面的空白处不会自己收起来"));

            /*
             * 面板里那几个按钮要弹系统文件夹选择框 —— 弹之前面板必须先让开，
             * 弹完还要原样回来（栏目、位置都不变）。
             */
            QVariant away;
            QMetaObject::invokeMethod(qmlRoot, "probeSettingsAway", Q_RETURN_ARG(QVariant, away));
            settle();
            check(away.toString() == QLatin1String("away"),
                  QStringLiteral("设置面板：弹系统对话框之前自己让开了"),
                  away.toString());
            check(uiState().value(QStringLiteral("settingsOpened")).toBool()
                      && uiState().value(QStringLiteral("settingsSection")).toString()
                             == QLatin1String("storage"),
                  QStringLiteral("设置面板：对话框关掉之后原栏目放回来"),
                  uiState().value(QStringLiteral("settingsSection")).toString());
        }
        QMetaObject::invokeMethod(qmlRoot, "closeSettings");
        settle();

        /*
         * 先把面板**归一化成"摆着"**再开始量。
         *
         * 收起 / 展开这个状态是**记在设置里**的（Cmd.remember("treeHidden")，重启
         * 会照上次的样子回来，见 Main.qml 的 toggleFolderTree）。所以上一次自检
         * 如果正好停在"收起来"，下一次进来它一开始就是收着的 —— 这里再 dispatch
         * 一次反而把它展开了，三条断言全反着来（读出 hidden=false、宽度 276）。
         * 自检不该依赖上一次跑剩什么，先摆正。
         */
        if (treeState().value(QStringLiteral("hidden")).toBool()) {
            dispatch(QStringLiteral("treeHide"));
            settle();
        }

        dispatch(QStringLiteral("treeHide"));
        settle();
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("hidden")).toBool(),
                  QStringLiteral("收起面板：标志位置上了"));
            check(s.value(QStringLiteral("panelWidth")).toDouble() < 0.5,
                  QStringLiteral("收起面板：布局里的槽位宽度归 0"),
                  QStringLiteral("实际 %1").arg(s.value(QStringLiteral("panelWidth")).toDouble()));
        }

        dispatch(QStringLiteral("treeHide"));
        settle();
        {
            const QVariantMap s = treeState();
            check(!s.value(QStringLiteral("hidden")).toBool()
                  && s.value(QStringLiteral("panelWidth")).toDouble() > 100.0,
                  QStringLiteral("再点一次面板回来（宽度还是收起前那个）"),
                  QStringLiteral("实际 %1").arg(s.value(QStringLiteral("panelWidth")).toDouble()));
        }
    }

    /*
     * ================= 新建 md / Ctrl+S 写回文件 / 改名 / 删除 =================
     *
     * 走路：Store.createFile（左侧树 "+" 那条路）
     *       -> 编辑器标签 -> 改一笔 -> saveCurrent() -> 磁盘上那份 md 跟着变；
     *       再走一遍重命名 / 删除（左树右键菜单那两条）。
     *
     * 同样跑在临时保存目录上（保存位置在上一段换成临时目录的），收尾时换回来。
     */
    if (store) {
        const QString path = store->createFile(QStringLiteral("自检新建的内容"));
        check(!path.isEmpty(), QStringLiteral("新建文件：在今天的目录里建出一份 md"), path);
        check(QFileInfo::exists(path), QStringLiteral("新建的文件真的在磁盘上"));
        check(QFileInfo(path).fileName().contains(QRegularExpression(QStringLiteral("^\\d{6}"))),
              QStringLiteral("新建的文件名也是时分秒"), QFileInfo(path).fileName());

        view->openFile(path);
        check(view->hasDocument(), QStringLiteral("新建的 md 能打开成标签"));
        check(QFileInfo(view->filePath()).absoluteFilePath() == QFileInfo(path).absoluteFilePath(),
              QStringLiteral("标签认得这份文件的路径"), view->filePath());
        check(view->currentText().contains(QStringLiteral("自检新建的内容")),
              QStringLiteral("新建时给的那段内容是文件正文的一部分"));

        /* 改一笔：复制一行（新建出来的文件第一行是 "# 日期" 标题，复制的是它） */
        const QString beforeEdit = readFile(path);
        view->duplicateLine();
        check(view->modified(), QStringLiteral("改一笔 -> 已修改状态"));

        check(view->saveCurrent(), QStringLiteral("md 标签 Ctrl+S 写回文件"), view->lastError());
        check(!view->modified(), QStringLiteral("写回之后修改标记清掉"));
        {
            /*
             * 判据是"磁盘上那份 = 编辑器里的正文"，不是"某段文字出现几次"：
             * 文件格式（日期标题 + "## 时分秒" 分段）以后可能变，这条断言不该跟着变。
             * Scintilla 会把 CRLF 归一，比较时先把 \r 抹平。
             */
            auto straight = [](QString s) {
                s.remove(QLatin1Char('\r'));
                return s;
            };
            const QString disk = readFile(path);
            check(disk.length() > beforeEdit.length(),
                  QStringLiteral("改的那一笔真的落到了磁盘上（文件变长了）"),
                  QStringLiteral("%1 -> %2 字符").arg(beforeEdit.length()).arg(disk.length()));
            check(straight(disk) == straight(view->currentText()),
                  QStringLiteral("磁盘上那份文件 = 编辑器里的正文"),
                  QStringLiteral("文件 %1 字符 / 编辑器 %2 字符")
                      .arg(disk.size()).arg(view->currentText().size()));
            check(disk.contains(QStringLiteral("自检新建的内容")),
                  QStringLiteral("新建时给的那段内容还在文件里"));
        }

        /* ---- 左树定位当前文件 ---- */
        dispatch(QStringLiteral("treeCollapseAll"));
        check(treeState().value(QStringLiteral("openFolders")).toInt() == 0,
              QStringLiteral("定位用例：先全部折叠，看它会不会自己展开"));

        dispatch(QStringLiteral("treeLocate"));
        settle();
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("currentPath")).toString()
                      == QFileInfo(path).absoluteFilePath(),
                  QStringLiteral("定位用例：当前标签认得出是哪一份文件（按路径认）"),
                  s.value(QStringLiteral("currentPath")).toString());
            check(s.value(QStringLiteral("selectedPath")).toString()
                      == QFileInfo(path).absoluteFilePath(),
                  QStringLiteral("定位用例：左树里选中的就是当前标签那一份"),
                  s.value(QStringLiteral("selectedPath")).toString());
            check(s.value(QStringLiteral("openFolders")).toInt() >= 1,
                  QStringLiteral("定位用例：它所在的那个日期目录被展开了"));

            /*
             * 蓝底只给选中的文件：日期文件夹那一级不亮。
             *
             * 量的是委托自己报的 rowHighlight（见 FolderTree.highlightCounts），
             * 不是把 QML 里那个表达式在 C++ 这侧再算一遍。
             */
            const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
            check(hl.value(QStringLiteral("folders")).toInt() == 0,
                  QStringLiteral("日期文件夹不亮蓝底"),
                  QStringLiteral("亮着的文件夹行 %1")
                      .arg(hl.value(QStringLiteral("folders")).toInt()));
            check(hl.value(QStringLiteral("files")).toInt() == 1,
                  QStringLiteral("蓝底只留在选中的那一份文件上"),
                  QStringLiteral("亮着的文件行 %1")
                      .arg(hl.value(QStringLiteral("files")).toInt()));
        }

        /*
         * 关掉标签之后，左树那一行的蓝底也要跟着撤掉。
         *
         * 用户报的：右边标签关光了、编辑区回到欢迎页，左边还蓝着一行，看着
         * 像那份文件还开着。判据取两处：selectedPath 清空 + 委托自己报的亮行
         * 数归零（不把 QML 里那个表达式在 C++ 这侧再算一遍）。
         */
        dispatch(QStringLiteral("closeTab"));
        settle();
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("selectedPath")).toString().isEmpty(),
                  QStringLiteral("关掉标签后左树不再选中它"),
                  QStringLiteral("还选着 %1").arg(s.value(QStringLiteral("selectedPath")).toString()));
            const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
            check(hl.value(QStringLiteral("files")).toInt() == 0,
                  QStringLiteral("关掉标签后那一行的蓝底也撤掉"),
                  QStringLiteral("亮着的文件行 %1")
                      .arg(hl.value(QStringLiteral("files")).toInt()));
        }

        /* 下面那条用例要在标签开着的前提下改名，所以重新打开它 */
        check(view->openFile(path) >= 0, QStringLiteral("重新打开它，接着测重命名"));
        settle();

        /* ---- 重命名：磁盘上的文件和开着的标签一起改 ---- */
        const QString newName = QStringLiteral("selfcheck-renamed.md");
        const QString renamed = QFileInfo(path).absolutePath() + QLatin1Char('/') + newName;
        check(store->renameFile(path, newName), QStringLiteral("重命名：文件改名成功"));
        check(QFileInfo::exists(renamed), QStringLiteral("改完名字的文件在磁盘上"), renamed);
        check(!QFileInfo::exists(path), QStringLiteral("老名字那份已经不在了"));
        check(view->updateDocumentPath(path, renamed),
              QStringLiteral("打开着的标签跟着换路径（不换的话下一次 Ctrl+S 会写回老名字）"));
        check(QFileInfo(view->filePath()).absoluteFilePath() == QFileInfo(renamed).absoluteFilePath(),
              QStringLiteral("标签上的路径就是新名字"), view->filePath());
        check(!store->renameFile(QStringLiteral("这个文件不存在.md"), QStringLiteral("x")),
              QStringLiteral("重命名不存在的文件会失败（不会悄悄新建一份）"));

        /* ---- 删除：文件从磁盘上消失，元数据也跟着清 ---- */
        const int entriesBefore = store->entryCount();
        view->closeDocument(view->indexOfPath(renamed));
        check(store->deleteFile(renamed), QStringLiteral("删除：文件真的被删掉了"));
        check(!QFileInfo::exists(renamed), QStringLiteral("磁盘上那份已经不在了"));
        check(store->entryCount() < entriesBefore,
              QStringLiteral("删掉文件之后它的条目元数据也清了"),
              QStringLiteral("%1 -> %2").arg(entriesBefore).arg(store->entryCount()));
        check(!store->deleteFile(renamed),
              QStringLiteral("再删一次会失败（文件已经不在了）"));

        /*
         * 不在左树管得着的范围里的标签（未命名空白文档）没什么可定位的：
         * 准星按钮该是灰的。
         */
        dispatch(QStringLiteral("new"));
        {
            const QVariantMap s = treeState();
            check(!s.value(QStringLiteral("locateEnabled")).toBool()
                      && s.value(QStringLiteral("currentPath")).toString().isEmpty(),
                  QStringLiteral("定位按钮：未命名空白标签时置灰（没有可定位的文件）"));
        }
        dispatch(QStringLiteral("closeTab"));

        /*
         * 收尾：保存位置换回用户原来那个。
         *
         * setRootPath 会顺带重扫一遍，所以元数据也跟着回到真实目录上
         * （临时目录那些记录会在"扫不到的文件"那一步被清掉）。
         */
        check(store->setRootPath(savedRoot), QStringLiteral("收尾：保存位置换回原目录"));
        check(store->rootPath() == QDir::cleanPath(savedRoot),
              QStringLiteral("收尾：读回来就是用户原来那个目录"), store->rootPath());
    }

    /*
     * ================= 子菜单（视图 -> 语言 / 编码 / 换行符） =================
     *
     * 这三条要在主菜单**右边**再展开一栏（条目就在 js/EditorMenus.js 里挂着
     * submenu: true + items 的那几条）。要钉三件事：
     *
     *  1) 真的多出一栏，而且那一栏在主栏右边（量位置，不是量"展开了"这个标志）；
     *  2) 长列表（语言 27 项）照样限高 + 可滚动；
     *  3) 两条入口都能通：菜单栏那条（dispatch("menu:视图") 开主菜单）
     *     和条目自己的 items。
     *
     * 悬停那一下 C++ 点不出来，所以走 Main.openSubmenuFor —— 它和界面用的是
     * 同一份 Menus.menuItems("视图") 构造、同一个 ddMenu.openSubmenu()。
     *
     * 先开一个空白标签：语言 / 编码 两张表是按**当前文档**算出来的
     * （没文档时 languageItems() 直接返回空表，子菜单也就没什么可展开的）。
     */
    dispatch(QStringLiteral("new"));

    /* 工具栏取消后，图标改由菜单条目承担：图标在左、快捷键在右 */
    dispatch(QStringLiteral("menu:文件"));
    {
        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("dispatch(menu:文件) 打开下拉菜单"));
        check(ui.value(QStringLiteral("menuHasIcons")).toBool(),
              QStringLiteral("文件菜单条目带图标（图标在左、快捷键在右）"));
    }

    /*
     * "文件"菜单里既要能打开文件，也要能打开文件夹（= 把目录挂到左树上，
     * 就是本程序里"打开一个项目"的意思）—— 上一版只有"打开…"，想开目录
     * 得绕到左边树的"更多"里去找。这里核对的就是界面上那份菜单本身。
     */
    {
        QVariant acts;
        QMetaObject::invokeMethod(qmlRoot, "topMenuActs", Q_RETURN_ARG(QVariant, acts),
                                  Q_ARG(QVariant, QVariant(QStringLiteral("文件"))));
        QStringList names;
        for (const QVariant &a : acts.toList())
            names << a.toString();
        check(names.contains(QStringLiteral("open"))
                  && names.contains(QStringLiteral("treeImportFolder")),
              QStringLiteral("\"文件\"菜单里既能打开文件、也能打开文件夹"),
              names.join(QLatin1Char('/')));
    }

    dispatch(QStringLiteral("menu:视图"));
    /*
     * 等一帧：菜单条目的 y 是 Column（positioner）算的，下一帧才摆到位 ——
     * 同一轮事件里读，每条都还是 y = 0，子菜单就会"对齐到第一行"。
     */
    settle();
    {
        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("dispatch(menu:视图) 打开\"视图\"菜单"));
    }

    /*
     * 子菜单那一栏"刚打开时是空的"—— 用"再 openFor 一次，它要归零"来验。
     *
     * 原来直接读一句 `!submenuOpened` 就完事，但那个会**偶发飘红**：自检跑的时候
     * 真实鼠标指针可能恰好压在菜单上（"视图"里带子菜单的那几条），hover 一触就
     * 把右边那块展开了。那和"openFor 复位了没有"是两件事。
     *
     * 重新 openFor 一次之后再读就稳定了：openFor 的头几行就是
     * `subEntries = []`，不管之前有没有被子菜单占着，这一下必须归零。
     */
    dispatch(QStringLiteral("menu:视图"));
    settle();
    {
        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("menuOpened")).toBool()
                  && !ui.value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("重新打开时右边没有子菜单那一栏（openFor 复位了）"));
    }

    {
        QVariant opened;
        QMetaObject::invokeMethod(qmlRoot, "openSubmenuFor", Q_RETURN_ARG(QVariant, opened),
                                  Q_ARG(QVariant, QVariant(QStringLiteral("menu:语言"))));
        check(opened.toBool(), QStringLiteral("openSubmenuFor(menu:语言) 展开了子菜单"));

        const QVariantMap ui = uiState();
        const double subH = ui.value(QStringLiteral("submenuHeight")).toDouble();
        const double subContentH = ui.value(QStringLiteral("submenuContentHeight")).toDouble();
        const double paneW = ui.value(QStringLiteral("menuPaneWidth")).toDouble();
        const double inset = ui.value(QStringLiteral("submenuInset")).toDouble();
        const double totalW = ui.value(QStringLiteral("menuTotalWidth")).toDouble();
        const double totalH = ui.value(QStringLiteral("menuTotalHeight")).toDouble();
        const double mainH = ui.value(QStringLiteral("menuHeight")).toDouble();
        const double subTop = ui.value(QStringLiteral("submenuTop")).toDouble();
        const double subRowY = ui.value(QStringLiteral("submenuRowY")).toDouble();
        const double gap = ui.value(QStringLiteral("menuPaneGap")).toDouble();

        check(ui.value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("语言子菜单那一块真的画出来了"));
        check(qAbs(inset - (paneW + gap)) < 0.5,
              QStringLiteral("子菜单在主菜单右边，中间留着一条缝"),
              QStringLiteral("子栏 x %1 / 主栏宽 %2 / 缝 %3").arg(inset).arg(paneW).arg(gap));
        check(gap >= 3.0, QStringLiteral("那条缝够看得见两块面板各自的圆角"),
              QStringLiteral("缝 %1").arg(gap));
        check(qAbs(totalW - (paneW * 2 + gap)) < 0.5,
              QStringLiteral("弹窗宽度 = 两块面板 + 中间那条缝"),
              QStringLiteral("弹窗宽 %1").arg(totalW));
        /*
         * 顶边对齐"父级那一条所在的行"，不是贴到菜单顶上 ——
         * "语言"在"视图"菜单里靠下（y 远大于 0），所以这两条一量就能分清。
         */
        check(qAbs(subTop - subRowY) < 0.5,
              QStringLiteral("子菜单顶边 = 父级那一条所在的行（没被夹到菜单顶上）"),
              QStringLiteral("子栏顶边 %1 / 那一行 %2").arg(subTop).arg(subRowY));
        check(subRowY > 100.0, QStringLiteral("对的是菜单靠下的那一条（不是第一行）"),
              QStringLiteral("行 y %1").arg(subRowY));
        check(totalH > mainH + 0.5,
              QStringLiteral("弹窗往下长高，把从中间那一行伸出来的子栏装下"),
              QStringLiteral("弹窗高 %1 / 主栏高 %2").arg(totalH).arg(mainH));
        check(subContentH > 700, QStringLiteral("语言子菜单条目总高 > 700px（27 项）"),
              QStringLiteral("实际 %1").arg(subContentH));
        check(subH < subContentH, QStringLiteral("长子菜单被限高，不再整块铺下去"),
              QStringLiteral("画出来 %1 / 内容 %2").arg(subH).arg(subContentH));
        check(subH <= 461, QStringLiteral("子菜单高度夹在 maxMenuHeight 以内"),
              QStringLiteral("实际 %1").arg(subH));
        check(ui.value(QStringLiteral("submenuScrollable")).toBool(),
              QStringLiteral("长子菜单标记为可滚动"));

        /*
         * 鼠标往右挪进子菜单：进的是子菜单里第一条，**不能**把子菜单收掉。
         *
         * 这一步原来错了 —— 委托把"普通条目"的收子菜单逻辑也用在子菜单自己的
         * 条目上，于是鼠标刚移进去面板就没了（用户报的"还没移上去就消失了"）。
         * 这里走的是 DropdownMenu.hoverEntry()，和真实悬停同一份判断。
         */
        QVariant stillOpen;
        QMetaObject::invokeMethod(qmlRoot, "hoverSubmenuEntry", Q_RETURN_ARG(QVariant, stillOpen),
                                  Q_ARG(QVariant, QVariant(0)));
        check(stillOpen.toBool() && uiState().value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("鼠标移进子菜单里第一条，子菜单不会自己收掉"));
    }

    /* 子菜单里的条目照样能点：选一门语言，菜单自己也收掉 */
    dispatch(QStringLiteral("lang:python"));
    check(view->language() == QLatin1String("python"),
          QStringLiteral("子菜单里的条目（lang:python）能执行"));

    /*
     * ================= 编辑区的右键菜单 =================
     *
     * 编辑器里按右键弹的必须是 **QML 那套菜单**（和菜单栏"编辑"同一份构造），
     * 不是 Scintilla 自带的 QtWidgets 菜单 —— 后者英文、观感也和界面不搭。
     *
     * C++ 侧点不出右键，所以走 Main.openEditorContextMenu（编辑器那个
     * contextMenuRequested 信号最终就是调它）。要钉两件事：
     *   1) 菜单真的弹在了右键那一点上；
     *   2) 弹出的是"编辑"菜单那一份（条目总高对得上）。
     */
    const double editProbeX = 620.0;
    const double editProbeY = 260.0;
    QMetaObject::invokeMethod(qmlRoot, "openEditorContextMenu",
                              Q_ARG(QVariant, QVariant(editProbeX)),
                              Q_ARG(QVariant, QVariant(editProbeY)));
    {
        const QVariantMap ui = uiState();
        const double mx = ui.value(QStringLiteral("menuX")).toDouble();
        const double my = ui.value(QStringLiteral("menuY")).toDouble();
        const double contentH = ui.value(QStringLiteral("menuContentHeight")).toDouble();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("编辑区右键弹出 QML 菜单"));
        check(qAbs(mx - editProbeX) < 0.5 && qAbs(my - editProbeY) < 0.5,
              QStringLiteral("右键菜单左上角紧贴鼠标点（620,260）"),
              QStringLiteral("实际 (%1, %2)").arg(mx).arg(my));
        /*
         * 高度按**实际条目**算（12 条命令 + 4 条分隔线，现在又多了格式化 /
         * 校验 / 预览那几条，见 js/EditorMenus.js 的 editMenu）。
         * 每项 28px、分隔线 9px、上下各 4px 内缩 —— 钉的是"菜单按这套尺寸排
         * 出来了、条目数和菜单栏那份一致"，增删条目不用改这一行。
         */
        const QVariantList editActs = editMenuActs();
        int editItems = 0;
        int editSeps = 0;
        for (const QVariant &a : editActs) {
            if (a.toString() == QLatin1String("separator"))
                ++editSeps;
            else
                ++editItems;
        }
        const double expectEditH = editItems * 28.0 + editSeps * 9.0 + 8.0;
        check(qAbs(contentH - expectEditH) < 0.5,
              QStringLiteral("弹的就是\"编辑\"菜单那一份（条目数和总高对得上）"),
              QStringLiteral("内容高 %1（期望 %2，条目 %3 + 分隔线 %4）")
                  .arg(contentH).arg(expectEditH).arg(editItems).arg(editSeps));
        check(ui.value(QStringLiteral("menuHasIcons")).toBool(),
              QStringLiteral("右键菜单条目也带图标（和菜单栏一致）"));
    }
    QMetaObject::invokeMethod(qmlRoot, "closeMenu");

    /*
     * 长下拉菜单必须限高 + 可滚动（老路子：直接把语言列表当整个菜单弹出来）。
     *
     * dispatch("menu:语言") 这条仍然可用 —— 它不管子菜单那套，直接把列表
     * 当主菜单摆出来。放在最后做：菜单开着直接退出进程。
     */
    dispatch(QStringLiteral("menu:语言"));
    {
        const QVariantMap ui = uiState();
        const double menuH = ui.value(QStringLiteral("menuHeight")).toDouble();
        const double contentH = ui.value(QStringLiteral("menuContentHeight")).toDouble();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("dispatch(menu:语言) 打开下拉菜单"));
        check(contentH > 700, QStringLiteral("语言菜单条目总高 > 700px（27 项）"),
              QStringLiteral("实际 %1").arg(contentH));
        check(menuH < contentH, QStringLiteral("长菜单被限高，不再整块铺下去"),
              QStringLiteral("画出来 %1 / 内容 %2").arg(menuH).arg(contentH));
        check(menuH <= 461, QStringLiteral("菜单高度夹在 maxMenuHeight 以内"),
              QStringLiteral("实际 %1").arg(menuH));
        check(ui.value(QStringLiteral("menuScrollable")).toBool(),
              QStringLiteral("长菜单标记为可滚动"));
        check(!ui.value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("当主菜单弹出来时右边不留上一个子菜单"));
    }

    /*
     * 收尾：把菜单关掉再退出。
     *
     * 弹窗是独立原生窗口（见 DropdownMenu.qml 开头），留着它在进程退出时拆，
     * 偶尔会踩到拆除顺序的竞态 —— 实测有过一次 0xC0000005（访问冲突），
     * 报出来的却是"自检崩了"，而检查项其实一条没挂。
     * 在事件循环还活着的时候正常 close()，这个假故障就没了。
     */
    QMetaObject::invokeMethod(qmlRoot, "closeMenu");

    /* 还原用户自己的自动换行设置（起点在"dispatch(toggleWrap)"那一处钉过） */
    view->setWrapEnabled(wrapAtStart);

    /*
     * ================= 校验的波浪线（见 EditorViewItem::setCheckIssues） =================
     *
     * 校验那边给的是**行列**（第几行、这一行第几个**字**），Scintilla 要的是
     * **字节**位置 —— 中文一个字三字节，这块换算错了波浪线就画在旁边的字上。
     * 顺带钉住"正文一改就清掉"（改了之后行列全不作数了）。
     */
    {
        const int doc = view->newDocument();
        view->setText(QStringLiteral("第一行,没问题\n第二行也没事\n"));

        QVariantMap issue;
        issue.insert(QStringLiteral("row"), 1);
        issue.insert(QStringLiteral("col"), 3);        /* 「,」前面是三个汉字 */
        issue.insert(QStringLiteral("endRow"), 1);
        issue.insert(QStringLiteral("endCol"), 4);
        issue.insert(QStringLiteral("severity"), QStringLiteral("warn"));
        issue.insert(QStringLiteral("message"), QStringLiteral("中文句子里用了半角「,」"));
        issue.insert(QStringLiteral("snippet"), QStringLiteral(","));
        issue.insert(QStringLiteral("source"), QStringLiteral("local"));
        issue.insert(QStringLiteral("suggestion"), QStringLiteral("，"));
        view->setCheckIssues(QVariantList{ QVariant(issue) });

        const QVariantList ranges = view->checkIssueRanges();
        /* 半角逗号是第 4 个字符（0 基的 3）、第 10 个字节：前面三个汉字各 3 字节 */
        check(ranges.size() == 2 && ranges.at(0).toInt() == 9 && ranges.at(1).toInt() == 10,
              QStringLiteral("波浪线画在那个半角逗号上（第 3 个字 = 第 9..10 个字节）"),
              QStringLiteral("实际 %1").arg(ranges.size() == 2
                                               ? QStringLiteral("%1..%2")
                                                     .arg(ranges.at(0).toInt())
                                                     .arg(ranges.at(1).toInt())
                                               : QStringLiteral("%1 段").arg(ranges.size() / 2)));

        /*
         * 真的画出来了没有：正文区里数"波浪线那个色"的像素。
         *
         * 区间算得再对，指示器没配好 / 没刷出来屏幕上还是一条线都没有 ——
         * 这条就是钉那个的（见 checkWavePixelStats）。
         */
        const QVariantList wave = view->checkWavePixelStats();
        check(wave.value(1).toInt() > 0,
              QStringLiteral("那条波浪线真的画在了正文里（警告色像素 > 0）"),
              QStringLiteral("错误色 %1 / 警告色 %2").arg(wave.value(0).toInt())
                  .arg(wave.value(1).toInt()));

        /*
         * 鼠标停在那条波浪线上 -> 弹出来的就是这条问题的说明（见 checkTipAtPoint，
         * 悬浮那条路走的是同一个函数）。取的是上面数出来的那个像素点。
         */
        const int hitX = wave.value(2).toInt(), hitY = wave.value(3).toInt();
        const QString tip = view->checkTipAtPoint(hitX, hitY);
        check(tip.contains(QStringLiteral("半角")) && tip.contains(QStringLiteral("第 1 行")),
              QStringLiteral("停在那条波浪线上：说明框里就是这条问题"),
              tip.isEmpty() ? QStringLiteral("（那儿什么都没有）") : tip);

        view->setText(QStringLiteral("第一行，没问题\n第二行也没事\n"));
        check(view->checkIssueRanges().isEmpty()
                  && view->checkTipAtPoint(hitX, hitY).isEmpty(),
              QStringLiteral("正文一改，波浪线自动清掉（行列不再作数）"));

        view->closeDocument(doc);
    }

    /*
     * ============ 截图（抓屏 -> 选区 -> 加文字 -> 合成 / 贴图） ============
     *
     * 为什么走选区窗口 QML 上那几个 test* 函数，而不是合成键鼠去点：
     * 选区窗口是**独立的原生置顶窗口**，和编辑区那个 QScintilla 一样，
     * 系统级的合成鼠标事件进不到里面（见文件头）。所以这里调的是
     * CaptureOverlay.qml 里那几个函数 —— 它们和界面上的操作是**同一批**：
     *
     *   testSelect -> 拖框那一步（同一个 sel）
     *   testAddText -> 文字工具点一下（同一个 addText）
     *   textsData -> 工具条上"复制 / 保存 / 贴图"要传的那份数据
     *
     * 要钉的几件事：真的抓到屏了、选区窗口铺满整块屏、框出来的选区尺寸对得上、
     * 文字真的画进了最终图（不是只有预览里有）、三条出口都能出图。
     */
    if (shot) {
        /* 自检会往剪贴板里放图，先记着原来的文字，收尾放回去 */
        const QString oldClipboard = QGuiApplication::clipboard()->text();

        /*
         * 预热必须**真的把窗口在幕外映射并画过**（见 Screenshot::prewarm）。
         * 否则第一次 show() 时 DWM 手上是空的，会和 Qt 的首帧赛跑 ——
         * 抢在前面就是"第一次按快捷键偶发闪一下整屏"。
         */
        check(shot->overlayWarmed(),
              QStringLiteral("截图：选区窗口启动时已在幕外画过一帧（第一次抓屏不带空窗帧）"));

        /*
         * ============ 复现：启动后**第一次**抓屏，马上按 Esc 取消 ============
         *
         * 用户报的就是这一下：程序起来之后第一次按截图快捷键，屏幕上先亮起
         * 一整块全屏选区（要再按一次 Esc 才关得掉）。关键是"第一次" —— 所以
         * 这段必须放在**任何别的抓屏之前**：上面那些预热检查没有动过窗口，
         * 此刻的选区窗口正是启动预热留下的那份状态（见 Screenshot::prewarm），
         * 和用户冷启动后第一次按键时一模一样。
         *
         * 走 QML 里和 Esc Shortcut **一字不差**的代码（captureCancelDuringPending），
         * 同时从 C++ 这边每 3ms 采一次窗口可见性 —— "闪一下"这种一闪而过的
         * 残留只有这么采样才抓得住。取消时刻试 0/5/15/25ms（都在延时里）和
         * 60ms（延时之后，窗口本来就该开着，取消就该关掉它）。
         */
        {
            const int cancelAt[] = { 0, 5, 15, 25, 60 };
            for (int at : cancelAt) {
                QObject *root = shot->overlayRoot();
                if (!root)
                    break;
                QVariant probe;
                QMetaObject::invokeMethod(root, "captureCancelDuringPending",
                                          Q_RETURN_ARG(QVariant, probe),
                                          Q_ARG(QVariant, QVariant(at)));
                const QVariantMap probeMap = probe.toMap();

                int visibleFrames = 0;
                long long lastVisibleMs = -1;
                QElapsedTimer watch;
                watch.start();
                while (watch.elapsed() < 1200) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 3);
                    QThread::msleep(3);
                    if (shot->overlayVisible()) {
                        ++visibleFrames;
                        lastVisibleMs = watch.elapsed();
                    }
                }
                const int qmlFrames = probeMap.value(QStringLiteral("frames")).toInt();

                /*
                 * 延时里取消（at < 30）：窗口一帧都不该露。
                 * 延时之后取消（at >= 30）：窗口先开出来了，取消必须把它关掉。
                 */
                if (at < 30) {
                    check(visibleFrames == 0 && qmlFrames == 0,
                          QStringLiteral("截图：第一次抓屏、%1ms 时按 Esc 取消 -> 窗口一帧都没露")
                              .arg(at),
                          QStringLiteral("C++ 数到 %1 帧 / QML 数到 %2 帧")
                              .arg(visibleFrames).arg(qmlFrames));
                } else {
                    check(!shot->overlayVisible(),
                          QStringLiteral("截图：第一次抓屏、%1ms 时按 Esc（窗口已开）-> 收工后是关着的")
                              .arg(at),
                          QStringLiteral("最后可见于 %1ms").arg(lastVisibleMs));
                }

                /* 复位，别把上一次的窗口状态带进下一次 */
                shot->cancelCapture();
                for (int i = 0; i < 20; ++i) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(5);
                }
            }
        }

        shot->beginCapture();
        /*
         * 抓屏是**延时**的（要先等主窗口藏起来那一帧重画完，见
         * Screenshot::beginCapture），所以这里等它真正进入截图状态。
         *
         * 判据用 active()、不是 overlayRoot()：选区窗口现在是预建复用的
         * （Screenshot::prewarm），窗口对象一开始就在，拿它当"出来了吗"
         * 会立刻返回、后面全成时序赌运气。
         */
        for (int i = 0; i < 60 && !shot->active(); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(25);
        }

        QObject *overlay = shot->overlayRoot();
        check(overlay != nullptr && shot->overlayVisible(),
              QStringLiteral("截图：抓屏之后选区窗口显示出来了"));
        check(shot->active(), QStringLiteral("截图：处于截图状态（active = true）"));

        if (overlay) {
            settle();

            /*
             * "撤销"按钮的可用状态。
             *
             * 钉这个是因为踩过：history 是 JS 数组，原地 push 不触发绑定，
             * 按钮的 enabled 一直停在启动时那次求值上 —— 画了再多标注，
             * "撤销"也一直是灰的、点不动。所以"还没有标注 -> 不可用，
             * 落一条之后 -> 可用"这两头都得量一下。
             */
            check(!overlay->property("undoAvailable").toBool(),
                  QStringLiteral("截图：刚开出来（还没标注）时「撤销」是置灰的"));

            const double overlayW = overlay->property("width").toDouble();
            const double overlayH = overlay->property("height").toDouble();
            check(overlayW > 640 && overlayH > 480,
                  QStringLiteral("截图：选区窗口铺满整块屏（不是一个小窗）"),
                  QStringLiteral("窗口 %1x%2").arg(overlayW).arg(overlayH));

            const QImage frozen =
                shot->imageForId(QStringLiteral("full%1").arg(shot->serial()));
            const double dpr = frozen.devicePixelRatio() > 0 ? frozen.devicePixelRatio() : 1.0;
            check(!frozen.isNull()
                      && frozen.width() >= qRound(overlayW * dpr) - 1
                      && frozen.height() >= qRound(overlayH * dpr) - 1,
                  QStringLiteral("截图：冻结图是按设备像素抓下来的（不小于窗口尺寸 × DPR）"),
                  QStringLiteral("图 %1x%2（dpr %3）/ 窗口 %4x%5")
                      .arg(frozen.width()).arg(frozen.height()).arg(dpr)
                      .arg(overlayW).arg(overlayH));

            /*
             * 复位时选区必须**按调用方给的矩形**，不能按控件尺寸猜。
             *
             * 这是"闪一下方框轮廓"的根：选区窗口预建复用（Screenshot::prewarm），
             * setGeometry 之后 QQuickWidget 的布局是延迟生效的 —— 复位那一刻读
             * width/height 拿到的还是预热时的旧尺寸（640x480 那种），于是头一两帧
             * 按一个小方框画出来，等 resize 事件到了才变成整屏。所以改成由 C++
             * 把屏幕矩形直接传进来（见 CaptureOverlay::resetForCapture 的说明）。
             */
            {
                QMetaObject::invokeMethod(overlay, "resetForCapture",
                                          Q_ARG(QVariant, QVariant::fromValue(QRectF(11, 22, 333, 155))));
                /* 等布局落定再读（setGeometry 之后 QQuickWidget 的布局是延迟生效的） */
                settle();
                const QRectF got = overlay->property("sel").toRectF();
                check(qAbs(got.x() - 11) < 1.5 && qAbs(got.y() - 22) < 1.5
                          && qAbs(got.width() - 333) < 1.5 && qAbs(got.height() - 155) < 1.5,
                      QStringLiteral("截图：复位时选区按调用方给的矩形（不按控件尺寸猜）"),
                      QStringLiteral("得到 %1×%2 @%3,%4")
                          .arg(got.width()).arg(got.height()).arg(got.x()).arg(got.y()));
            }

            /* 框一块 + 在框里落一条文字（走界面上那同一批函数） */
            const double selX = 120.0, selY = 90.0, selW = 420.0, selH = 260.0;

            /*
             * 浮动工具条的位置：还没框选（选区是整屏）时在**右上角**，框选之后
             * 贴着选区下沿。
             *
             * 这两条都量，是因为"右上角"这个位置被来回改过：曾经为了消灭"一进
             * 截图先闪在右上角"改成过"贴鼠标"，用户明确要求改回右上角，所以这里
             * 把**两边**都钉住，以后谁再动它都会红。
             *
             * 期望值按产品里同一套夹取算：工具条比选区宽时右边顶出屏幕，x 会被
             * 夹回窗口里（bar 的 x = max(8, min(ctrlW - bw - 8, 选区右下 - bw))）。
             */
            {
                QMetaObject::invokeMethod(
                    overlay, "resetForCapture",
                    Q_ARG(QVariant, QVariant::fromValue(QRectF(0, 0, 1500, 900))));
                settle();

                /* 工具条是可视项（QQuickItem），C++ 的 findChild 够不着 —— 让根对象报 */
                QVariant barStateVar;
                QMetaObject::invokeMethod(overlay, "barState",
                                          Q_RETURN_ARG(QVariant, barStateVar));
                const QVariantMap bs = barStateVar.toMap();
                const double bw = bs.value(QStringLiteral("width")).toDouble();
                const double bh = bs.value(QStringLiteral("height")).toDouble();
                check(!bs.isEmpty() && bh > 20 && bs.contains(QStringLiteral("atScreensRight")),
                      QStringLiteral("截图：读得到浮动工具条的状态（barState）"),
                      QStringLiteral("工具条 %1,%2（%3×%4）")
                          .arg(bs.value(QStringLiteral("x")).toDouble())
                          .arg(bs.value(QStringLiteral("y")).toDouble())
                          .arg(bw).arg(bh));
                check(bs.value(QStringLiteral("atScreensRight")).toBool(),
                      QStringLiteral("截图：还没框选时工具条按整屏选区定位（右上角）"));
                /*
                 * 工具条里只剩按钮那一行：底下那句"T 文字 / A 箭头 / …"的快捷键
                 * 提示撤了（用户要求）。这条量的是"板块数"—— 谁再往 barRow 里塞
                 * 一行（那句提示、或者别的说明文字）就红。
                 */
                check(bs.value(QStringLiteral("rows")).toInt() == 1,
                      QStringLiteral("截图：工具条里就按钮那一行（底下那句提示撤了）"),
                      QStringLiteral("板块数 %1（期望 1）")
                          .arg(bs.value(QStringLiteral("rows")).toInt()));

                /*
                 * 复位之后**不许**马上画选区边框（见 CaptureOverlay.qml 的
                 * overlayReady）。
                 *
                 * 用户报的："取消截图后全屏出现了一下绿色边框"。抓屏 + 4K 底图上传
                 * 要几十毫秒，这期间窗口可能已经 show() 出来了，而框选默认是整屏
                 * —— 那圈"全屏选区"的边框就会先亮一下。所以复位时先关掉，等窗口
                 * 稳住了由 settleOverlay() 打开。
                 */
                check(!bs.value(QStringLiteral("ready")).toBool()
                          && !bs.value(QStringLiteral("borderVisible")).toBool(),
                      QStringLiteral("截图：刚复位时先把选区边框压住（取消时不会闪一下全屏边框）"));

                QMetaObject::invokeMethod(overlay, "settleOverlay");
                settle();
                QMetaObject::invokeMethod(overlay, "barState",
                                          Q_RETURN_ARG(QVariant, barStateVar));
                const QVariantMap bsReady = barStateVar.toMap();
                check(bsReady.value(QStringLiteral("ready")).toBool()
                          && bsReady.value(QStringLiteral("borderVisible")).toBool(),
                      QStringLiteral("截图：窗口稳住之后（settleOverlay）选区边框才出来"));

                const double ctrlW = overlay->property("width").toDouble();
                const double wantRightX = qMax(8.0, qMin(ctrlW - bw - 8.0, 1500.0 - bw));
                check(qAbs(bs.value(QStringLiteral("x")).toDouble() - wantRightX) < 8,
                      QStringLiteral("截图：一进截图工具条在右沿（剪掉夹取那部分）"),
                      QStringLiteral("工具条 x=%1 / 期望 %2（控件宽 %3，条宽 %4）")
                          .arg(bs.value(QStringLiteral("x")).toDouble())
                          .arg(wantRightX).arg(ctrlW).arg(bw));

                /* 框选之后贴到选区下沿 */
                QMetaObject::invokeMethod(overlay, "testSelect", Q_ARG(QVariant, selX),
                                          Q_ARG(QVariant, selY), Q_ARG(QVariant, selW),
                                          Q_ARG(QVariant, selH));
                settle();
                QMetaObject::invokeMethod(overlay, "barState",
                                          Q_RETURN_ARG(QVariant, barStateVar));
                const QVariantMap bs2 = barStateVar.toMap();
                const double wantX = qMax(8.0, qMin(ctrlW - bw - 8.0, selX + selW - bw));
                check(qAbs(bs2.value(QStringLiteral("x")).toDouble() - wantX) < 8
                          && qAbs(bs2.value(QStringLiteral("y")).toDouble()
                                  - (selY + selH + 10)) < 8,
                      QStringLiteral("截图：框选之后工具条贴着选区下沿"),
                      QStringLiteral("工具条 %1,%2 / 期望 %3,%4")
                          .arg(bs2.value(QStringLiteral("x")).toDouble())
                          .arg(bs2.value(QStringLiteral("y")).toDouble())
                          .arg(wantX).arg(selY + selH + 10));

                /* 复位回整屏、清掉这次试出来的选区，别带进后面的检查 */
                QMetaObject::invokeMethod(
                    overlay, "resetForCapture",
                    Q_ARG(QVariant, QVariant::fromValue(QRectF(0, 0, 1500, 900))));
                settle();
            }

            QMetaObject::invokeMethod(overlay, "testSelect", Q_ARG(QVariant, selX),
                                      Q_ARG(QVariant, selY), Q_ARG(QVariant, selW),
                                      Q_ARG(QVariant, selH));
            QMetaObject::invokeMethod(overlay, "testTextTool", Q_ARG(QVariant, true));

            QVariant added;
            QMetaObject::invokeMethod(overlay, "testAddText", Q_RETURN_ARG(QVariant, added),
                                      Q_ARG(QVariant, selX + 40), Q_ARG(QVariant, selY + 60),
                                      Q_ARG(QVariant, QStringLiteral("自检文字")));
            check(added.toBool(), QStringLiteral("截图：文字工具在选区里落下一段文字"));
            /* 落了一条之后"撤销"必须变成可点的（不能一直是灰的） */
            check(overlay->property("undoAvailable").toBool(),
                  QStringLiteral("截图：落一条标注之后「撤销」变成可点（不再是灰的）"));

            QVariant textsVar;
            QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, textsVar));
            const QVariantList texts = textsVar.toList();
            check(texts.size() == 1
                      && texts.first().toMap().value(QStringLiteral("text")).toString()
                             == QStringLiteral("自检文字"),
                  QStringLiteral("截图：标注数据（x/y/文字/字号/颜色）齐了"),
                  QStringLiteral("条目数 %1").arg(texts.size()));

            /*
             * QML 与 C++ 之间那份数据的**字段契约**：合成要用到框宽 / 框高 /
             * 旋转角（见 Screenshot::compose）。哪天 QML 那边改了字段名而
             * C++ 没跟着改，合成出来的位置就是错的，而且不报错 —— 钉一下。
             */
            {
                const QVariantMap first = texts.isEmpty() ? QVariantMap()
                                                          : texts.first().toMap();
                check(first.contains(QStringLiteral("w"))
                          && first.value(QStringLiteral("w")).toDouble() > 0
                          && first.contains(QStringLiteral("h"))
                          && first.value(QStringLiteral("h")).toDouble() > 0
                          && first.contains(QStringLiteral("rot")),
                      QStringLiteral("截图：标注带着框宽 / 框高 / 旋转角（合成的三个输入）"),
                      QStringLiteral("w=%1 h=%2 rot=%3")
                          .arg(first.value(QStringLiteral("w")).toDouble())
                          .arg(first.value(QStringLiteral("h")).toDouble())
                          .arg(first.value(QStringLiteral("rot")).toDouble()));
            }

            /*
             * 文字工具是"按住左键拖出文本框"：拖出 200 × 90，框就该是这个尺寸
             * （走的是界面上同一个 resizeBox），反向拖落出来的矩形要一样。
             */
            {
                QVariant boxed;
                QMetaObject::invokeMethod(overlay, "testBoxDrag", Q_RETURN_ARG(QVariant, boxed),
                                          Q_ARG(QVariant, selX + 200), Q_ARG(QVariant, selY + 120),
                                          Q_ARG(QVariant, selX + 400), Q_ARG(QVariant, selY + 210));
                QVariant boxTexts;
                QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, boxTexts));
                const QVariantMap drawn = boxTexts.toList().isEmpty()
                                              ? QVariantMap()
                                              : boxTexts.toList().last().toMap();
                check(boxed.toBool()
                          && qAbs(drawn.value(QStringLiteral("w")).toDouble() - 200.0) < 1.5
                          && qAbs(drawn.value(QStringLiteral("h")).toDouble() - 90.0) < 1.5,
                      QStringLiteral("截图：文字工具拖出来的框就是拖的那个尺寸（200 × 90）"),
                      QStringLiteral("w=%1 h=%2")
                          .arg(drawn.value(QStringLiteral("w")).toDouble())
                          .arg(drawn.value(QStringLiteral("h")).toDouble()));

                QVariant back;
                QMetaObject::invokeMethod(overlay, "testBoxDrag", Q_RETURN_ARG(QVariant, back),
                                          Q_ARG(QVariant, selX + 400), Q_ARG(QVariant, selY + 210),
                                          Q_ARG(QVariant, selX + 200), Q_ARG(QVariant, selY + 120));
                QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, boxTexts));
                const QVariantMap backDrawn = boxTexts.toList().isEmpty()
                                                  ? QVariantMap()
                                                  : boxTexts.toList().last().toMap();
                check(back.toBool()
                          && qAbs(backDrawn.value(QStringLiteral("x")).toDouble() - (selX + 200)) < 1.5
                          && qAbs(backDrawn.value(QStringLiteral("y")).toDouble() - (selY + 120)) < 1.5
                          && qAbs(backDrawn.value(QStringLiteral("w")).toDouble() - 200.0) < 1.5,
                      QStringLiteral("截图：反向拖（右下往左上）落出来的框位置一样"),
                      QStringLiteral("x=%1 y=%2 w=%3")
                          .arg(backDrawn.value(QStringLiteral("x")).toDouble())
                          .arg(backDrawn.value(QStringLiteral("y")).toDouble())
                          .arg(backDrawn.value(QStringLiteral("w")).toDouble()));
            }

            /*
             * 三条边手柄（左 / 右 / 下）：走的是界面上同一个 dragEdge。
             * 靶子就是上面拖出来的那个 200 × 90 的空框。
             */
            {
                QVariant all;
                QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, all));
                const int boxIndex = all.toList().size() - 1;
                auto boxState = [&]() {
                    QVariant now;
                    QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, now));
                    const QVariantList list = now.toList();
                    return (boxIndex >= 0 && boxIndex < list.size()) ? list.at(boxIndex).toMap()
                                                                     : QVariantMap();
                };
                auto dragEdge = [&](const QString &edge, double dx, double dy) {
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testEdgeDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex), Q_ARG(QVariant, edge),
                                              Q_ARG(QVariant, dx), Q_ARG(QVariant, dy));
                    return ok.toBool();
                };
                auto num = [](const QVariantMap &m, const char *key) {
                    return m.value(QString::fromLatin1(key)).toDouble();
                };

                dragEdge(QStringLiteral("right"), 100.0, 0.0);
                QVariantMap st = boxState();
                check(qAbs(num(st, "w") - 300.0) < 1.5
                          && qAbs(num(st, "x") - (selX + 200)) < 1.5,
                      QStringLiteral("截图：拖右边 → 框变宽、左边不动"),
                      QStringLiteral("x=%1 w=%2").arg(num(st, "x")).arg(num(st, "w")));

                dragEdge(QStringLiteral("left"), -80.0, 0.0);
                st = boxState();
                check(qAbs(num(st, "w") - 380.0) < 1.5
                          && qAbs(num(st, "x") - (selX + 120)) < 1.5,
                      QStringLiteral("截图：拖左边 → 框变宽、左上角跟着往左移"),
                      QStringLiteral("x=%1 w=%2").arg(num(st, "x")).arg(num(st, "w")));

                dragEdge(QStringLiteral("bottom"), 0.0, 60.0);
                st = boxState();
                check(qAbs(num(st, "h") - 150.0) < 1.5
                          && qAbs(num(st, "y") - (selY + 120)) < 1.5,
                      QStringLiteral("截图：拖下边 → 框变高、上边不动"),
                      QStringLiteral("y=%1 h=%2").arg(num(st, "y")).arg(num(st, "h")));

                /* 第四条边：上边往下拖 60 → 变矮，且上边界跟着往下走 */
                dragEdge(QStringLiteral("top"), 0.0, 60.0);
                st = boxState();
                check(qAbs(num(st, "h") - 90.0) < 1.5
                          && qAbs(num(st, "y") - (selY + 180)) < 1.5,
                      QStringLiteral("截图：拖上边 → 框变矮、上边界跟着走（下边不动）"),
                      QStringLiteral("y=%1 h=%2").arg(num(st, "y")).arg(num(st, "h")));

                /* 左下角：整体放大 —— 字号和框一起按比例长 */
                {
                    const double wBefore = num(boxState(), "w");
                    const double hBefore = num(boxState(), "h");
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testScaleDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex),
                                              Q_ARG(QVariant, -60.0), Q_ARG(QVariant, 60.0));
                    st = boxState();
                    check(ok.toBool() && num(st, "size") > 16.0
                              && num(st, "w") > wBefore + 100.0
                              && num(st, "h") > hBefore + 20.0,
                          QStringLiteral("截图：拖左下角 → 整体放大（字号和框一起长）"),
                          QStringLiteral("size %1 / w %2→%3 / h %4→%5")
                              .arg(num(st, "size")).arg(wBefore).arg(num(st, "w"))
                              .arg(hBefore).arg(num(st, "h")));
                }

                /* 左下角（现在的语义）：拖动整个框 —— tx/ty 各挪 dx/dy，宽高不变 */
                {
                    const QVariantMap before = boxState();
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testMoveDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex),
                                              Q_ARG(QVariant, 70.0), Q_ARG(QVariant, 40.0));
                    st = boxState();
                    check(ok.toBool()
                              && qAbs(num(st, "x") - (num(before, "x") + 70.0)) < 1.5
                              && qAbs(num(st, "y") - (num(before, "y") + 40.0)) < 1.5
                              && qAbs(num(st, "w") - num(before, "w")) < 1.5
                              && qAbs(num(st, "h") - num(before, "h")) < 1.5,
                          QStringLiteral("截图：左下角手柄拖动整个框（位置走、宽高不动）"),
                          QStringLiteral("(%1,%2)→(%3,%4) w %5→%6")
                              .arg(num(before, "x")).arg(num(before, "y"))
                              .arg(num(st, "x")).arg(num(st, "y"))
                              .arg(num(before, "w")).arg(num(st, "w")));
                }

                /* 右下角：宽高分开拖 */
                {
                    const double wBefore = num(boxState(), "w");
                    const double hBefore = num(boxState(), "h");
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testFreeDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex),
                                              Q_ARG(QVariant, 50.0), Q_ARG(QVariant, 30.0));
                    st = boxState();
                    check(ok.toBool() && qAbs(num(st, "w") - (wBefore + 50.0)) < 1.5
                              && qAbs(num(st, "h") - (hBefore + 30.0)) < 1.5,
                          QStringLiteral("截图：拖右下角 → 宽高分别 +50 / +30"),
                          QStringLiteral("w %1→%2 / h %3→%4")
                              .arg(wBefore).arg(num(st, "w"))
                              .arg(hBefore).arg(num(st, "h")));
                }

                /* 宽度拖到比最小还窄：夹在 minBoxW，不会拖成一条线 */
                dragEdge(QStringLiteral("right"), -5000.0, 0.0);
                st = boxState();
                check(num(st, "w") >= 39.0 && num(st, "w") <= 41.0,
                      QStringLiteral("截图：宽度拖过头 → 夹在最小框宽（40）"),
                      QStringLiteral("w=%1").arg(num(st, "w")));
            }

            const QRectF sel = overlay->property("sel").toRectF();
            check(qAbs(sel.width() - selW) < 1.5 && qAbs(sel.height() - selH) < 1.5,
                  QStringLiteral("截图：框出来的选区就是刚才那一块（420 × 260）"),
                  QStringLiteral("实际 %1 × %2").arg(sel.width()).arg(sel.height()));

            /*
             * 第一条出口：存 png。存两张 —— 带标注的和不带标注的 ——
             * 比出"文字真的画进图里了"，而不是只有预览里有。
             */
            const QString plainPath = dir.filePath(QStringLiteral("shot-plain.png"));
            const QString markPath = dir.filePath(QStringLiteral("shot-marked.png"));
            check(shot->saveResult(plainPath, sel, QVariantList()),
                  QStringLiteral("截图：合成并保存 png（不带标注）"));
            check(shot->saveResult(markPath, sel, texts),
                  QStringLiteral("截图：合成并保存 png（带标注）"));

            const QImage plain(plainPath);
            const QImage marked(markPath);
            const int wantW = qRound(selW * dpr);
            const int wantH = qRound(selH * dpr);
            check(!plain.isNull() && qAbs(plain.width() - wantW) <= 1
                      && qAbs(plain.height() - wantH) <= 1,
                  QStringLiteral("截图：存出来的图正好是选区那一块（设备像素）"),
                  QStringLiteral("图 %1x%2 / 期望 %3x%4")
                      .arg(plain.width()).arg(plain.height()).arg(wantW).arg(wantH));

            /*
             * 数标注色（#ff3b30）的像素：带标注的那张必须明显多出来。
             * 只看文本落点那一小块，桌面背景里正好有一片红色的概率很低，
             * 而且这里比的是同一个位置"有无标注"的差，稳。
             */
            auto reddish = [](const QImage &img) {
                int count = 0;
                for (int y = 0; y < img.height(); ++y) {
                    for (int x = 0; x < img.width(); ++x) {
                        const QColor c = img.pixelColor(x, y);
                        if (c.red() > 170 && c.green() < 120 && c.blue() < 120)
                            ++count;
                    }
                }
                return count;
            };
            const int plainRed = reddish(plain);
            const int markedRed = reddish(marked);
            check(markedRed > plainRed + 30,
                  QStringLiteral("截图：文字真的画进了最终图（标注色像素明显多出来）"),
                  QStringLiteral("带标注 %1 / 不带 %2").arg(markedRed).arg(plainRed));

            /*
             * 旋转那条路也要能出图：把同一条标注转 30° 再合成一张，
             * 尺寸不变、内容必须和没转的那张不一样（转了要是还一样，
             * 说明 compose() 根本没理会 rot）。
             */
            {
                QVariantList rotated = texts;
                QVariantMap first = rotated.first().toMap();
                first.insert(QStringLiteral("rot"), 30.0);
                rotated[0] = first;

                const QString rotPath = dir.filePath(QStringLiteral("shot-rotated.png"));
                check(shot->saveResult(rotPath, sel, rotated),
                      QStringLiteral("截图：带旋转角的标注也能合成出图"));
                const QImage rotatedImage(rotPath);
                check(rotatedImage.size() == marked.size()
                          && rotatedImage != marked,
                      QStringLiteral("截图：转 30° 之后成品图跟着变了（旋转真的生效）"),
                      QStringLiteral("尺寸 %1x%2 / 原图 %3x%4")
                          .arg(rotatedImage.width()).arg(rotatedImage.height())
                          .arg(marked.width()).arg(marked.height()));
            }

            /*
             * 箭头 / 铅笔：走界面上那套"起笔 -> 落笔"（testAddShape 调的
             * 就是 commitShape），要钉三件事：
             *   1) 数据里有它、起终点 / 点列对得上；
             *   2) 两种工具各画一笔都在；
             *   3) 合成出来的成品图**跟着变**（不是只有预览里画了）。
             */
            {
                QVariant ok;
                QMetaObject::invokeMethod(overlay, "testAddShape", Q_RETURN_ARG(QVariant, ok),
                                          Q_ARG(QVariant, QStringLiteral("arrow")),
                                          Q_ARG(QVariant, selX + 40), Q_ARG(QVariant, selY + 180),
                                          Q_ARG(QVariant, selX + 200), Q_ARG(QVariant, selY + 230));

                QVariant shapesVar;
                QMetaObject::invokeMethod(overlay, "shapesData", Q_RETURN_ARG(QVariant, shapesVar));
                const QVariantList arrows = shapesVar.toList();
                const QVariantMap arrow = arrows.isEmpty() ? QVariantMap()
                                                           : arrows.first().toMap();
                check(ok.toBool() && arrows.size() == 1
                          && arrow.value(QStringLiteral("kind")).toString() == QLatin1String("arrow")
                          && qAbs(arrow.value(QStringLiteral("x1")).toDouble() - (selX + 40)) < 1.5
                          && qAbs(arrow.value(QStringLiteral("y2")).toDouble() - (selY + 230)) < 1.5,
                      QStringLiteral("截图：拖出来的箭头带着起终点（kind=arrow）"),
                      QStringLiteral("条目 %1 / 终点 (%2,%3)")
                          .arg(arrows.size())
                          .arg(arrow.value(QStringLiteral("x2")).toDouble())
                          .arg(arrow.value(QStringLiteral("y2")).toDouble()));

                QMetaObject::invokeMethod(overlay, "testAddShape", Q_RETURN_ARG(QVariant, ok),
                                          Q_ARG(QVariant, QStringLiteral("pencil")),
                                          Q_ARG(QVariant, selX + 60), Q_ARG(QVariant, selY + 200),
                                          Q_ARG(QVariant, selX + 240), Q_ARG(QVariant, selY + 160));
                QMetaObject::invokeMethod(overlay, "shapesData", Q_RETURN_ARG(QVariant, shapesVar));
                const QVariantList shapes = shapesVar.toList();
                const QVariantMap pencil = shapes.isEmpty() ? QVariantMap()
                                                            : shapes.last().toMap();
                check(ok.toBool() && shapes.size() == 2
                          && pencil.value(QStringLiteral("kind")).toString()
                                 == QLatin1String("pencil")
                          && pencil.value(QStringLiteral("pts")).toList().size() == 6,
                      QStringLiteral("截图：铅笔那一笔带着点列（x,y 成对）"),
                      QStringLiteral("条目 %1 / 点数 %2")
                          .arg(shapes.size())
                          .arg(pencil.value(QStringLiteral("pts")).toList().size()));

                /*
                 * 三个工具都要能从"按下"开始整条走通（不只是 commitShape）——
                 * 加"方框"时就是这里漏了：数据 / 合成都支持 rect，可按下时的
                 * 分派还写着 arrow||pencil，于是方框工具下拖出来的是"改选区"。
                 */
                {
                    const double bx = selX + 20;
                    const double by = selY + 200;
                    QVariant drew;
                    QMetaObject::invokeMethod(overlay, "testDrawWith", Q_RETURN_ARG(QVariant, drew),
                                              Q_ARG(QVariant, QStringLiteral("rect")),
                                              Q_ARG(QVariant, bx), Q_ARG(QVariant, by),
                                              Q_ARG(QVariant, bx + 60), Q_ARG(QVariant, by + 40));
                    check(drew.toBool(), QStringLiteral("截图：方框工具从按下到松手整条路走得通"));

                    /* 而且**不该**顺手把选区改掉（那正是漏掉分派时的症状） */
                    const QRectF afterDraw = overlay->property("sel").toRectF();
                    check(qAbs(afterDraw.width() - selW) < 1.5
                              && qAbs(afterDraw.height() - selH) < 1.5,
                          QStringLiteral("截图：用形状工具画一笔不会改掉选区"),
                          QStringLiteral("选区 %1 × %2").arg(afterDraw.width())
                              .arg(afterDraw.height()));
                }

                QVariant all;
                QMetaObject::invokeMethod(overlay, "annotationsData", Q_RETURN_ARG(QVariant, all));

                /* 方框：和箭头同一套"按下 + 松开"，只有描边 */
                QMetaObject::invokeMethod(overlay, "testAddShape", Q_RETURN_ARG(QVariant, ok),
                                          Q_ARG(QVariant, QStringLiteral("rect")),
                                          Q_ARG(QVariant, selX + 260), Q_ARG(QVariant, selY + 40),
                                          Q_ARG(QVariant, selX + 380), Q_ARG(QVariant, selY + 150));
                QMetaObject::invokeMethod(overlay, "shapesData", Q_RETURN_ARG(QVariant, shapesVar));
                const QVariantList withRect = shapesVar.toList();
                /* 按 kind + 角点找那一条（列表里这会儿不止一个形状） */
                QVariantMap rect;
                for (const QVariant &v : withRect) {
                    const QVariantMap m = v.toMap();
                    if (m.value(QStringLiteral("kind")).toString() == QLatin1String("rect")
                        && qAbs(m.value(QStringLiteral("x1")).toDouble() - (selX + 260)) < 1.5)
                        rect = m;
                }
                check(ok.toBool() && !rect.isEmpty()
                          && qAbs(rect.value(QStringLiteral("y2")).toDouble() - (selY + 150)) < 1.5,
                      QStringLiteral("截图：方框带着两个角点（kind=rect）"),
                      QStringLiteral("条目 %1 / 角点 (%2,%3)-(%4,%5)")
                          .arg(withRect.size())
                          .arg(rect.value(QStringLiteral("x1")).toDouble())
                          .arg(rect.value(QStringLiteral("y1")).toDouble())
                          .arg(rect.value(QStringLiteral("x2")).toDouble())
                          .arg(rect.value(QStringLiteral("y2")).toDouble()));

                QMetaObject::invokeMethod(overlay, "annotationsData", Q_RETURN_ARG(QVariant, all));
                const QString shapePath = dir.filePath(QStringLiteral("shot-shapes.png"));
                check(shot->saveResult(shapePath, sel, all.toList()),
                      QStringLiteral("截图：箭头 + 铅笔 + 方框 + 文字一起合成出图"));
                const QImage withShapes(shapePath);
                check(withShapes.size() == marked.size() && withShapes != marked,
                      QStringLiteral("截图：箭头 / 铅笔 / 方框真的画进了成品图（不是只在预览里）"));
            }

            /* 第二条出口：剪贴板 */
            check(shot->copyResult(sel, texts), QStringLiteral("截图：复制到剪贴板"));
            const QImage clip = QGuiApplication::clipboard()->image();
            check(!clip.isNull() && clip.size() == marked.size(),
                  QStringLiteral("截图：剪贴板里那张图和存出来的是同一张"),
                  QStringLiteral("剪贴板 %1x%2 / 文件 %3x%4")
                      .arg(clip.width()).arg(clip.height())
                      .arg(marked.width()).arg(marked.height()));

            /* 第三条出口：固定到桌面（贴图窗口） */
            QWidget *overlayWidget = nullptr;
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (w->windowTitle() == QStringLiteral("SmartClip 截图"))
                    overlayWidget = w;
            }
            shot->pinResult(sel, texts);
            QWidget *pin = nullptr;
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (w->windowTitle() == QStringLiteral("SmartClip 贴图"))
                    pin = w;
            }
            check(shot->pinnedCount() == 1 && pin != nullptr,
                  QStringLiteral("截图：固定到桌面开出了一个贴图窗口"));
            if (pin) {
                check(qAbs(pin->width() - qRound(selW)) <= 1
                          && qAbs(pin->height() - qRound(selH)) <= 1,
                      QStringLiteral("截图：贴图窗口就是选区那么大"),
                      QStringLiteral("窗口 %1x%2").arg(pin->width()).arg(pin->height()));
                check(pin->windowFlags().testFlag(Qt::WindowStaysOnTopHint)
                          && pin->windowFlags().testFlag(Qt::FramelessWindowHint),
                      QStringLiteral("截图：贴图窗口是置顶 + 无边框（钉在桌面上）"));
                /* 就钉在选区原来那块地方（截图时框的是哪儿，贴出来在哪儿） */
                if (overlayWidget) {
                    const QPoint want = overlayWidget->pos() + QPoint(qRound(selX), qRound(selY));
                    check((pin->pos() - want).manhattanLength() <= 2,
                          QStringLiteral("截图：贴图窗口钉在选区原来的位置上"),
                          QStringLiteral("实际 %1,%2 / 期望 %3,%4")
                              .arg(pin->pos().x()).arg(pin->pos().y())
                              .arg(want.x()).arg(want.y()));
                }
            }
            shot->closeAllPins();
            check(shot->pinnedCount() == 0, QStringLiteral("截图：贴图窗口关得掉"));
        }

        /*
         * ============ 贴图窗口：贴上去之后还能接着改 ============
         *
         * 用户要的是"固定到桌面那块图能划重点、能写字、能把上面的字翻出来"。
         * 所以这一节验的是：贴图窗口换成了 QML 那套界面（工具条在、标注层在）、
         * 划一笔真的进了数据、文字真的能编辑、撤销真的是撤销、**成品图**里
         * 真的画进去了（复制 / 保存 / 识别交出去的都靠它）。
         */
        {
            /* 选区和缩放比：这一块在 if (overlay) 外面，那边那两个变量在里面，
               所以从根对象 / 冻结图上读（值就是那边同一份） */
            const QRectF pinSel = shot->overlayRoot()
                                      ? shot->overlayRoot()->property("sel").toRectF()
                                      : QRectF();
            const QImage pinFrozen =
                shot->imageForId(QStringLiteral("full%1").arg(shot->serial()));
            const double pinDpr = pinFrozen.devicePixelRatio() > 0
                                      ? pinFrozen.devicePixelRatio() : 1.0;

            shot->pinResult(pinSel, QVariantList());
            QWidget *pinWidget = shot->lastPinned();
            auto *pin = qobject_cast<PinWindow *>(pinWidget);
            check(pin != nullptr, QStringLiteral("贴图：贴上去的那块是新的可编辑窗口"));
            if (pin) {
                /*
                 * 先把"认字"这条锁定成空 —— 贴图一建出来 QML 就会自动认一次字
                 * （真的跑一遍 Windows OCR），而认出来的行会决定"按在字上拖 = 选字
                 * 还是挪窗口"。这块屏上认不认得出字、什么时候回来，不该影响下面
                 * "拖动 / 点一下"那些检查（见 PinWindow::setOcrLinesForTest）。
                 * 选字那几条自己会摆假的行进去。
                 */
                pin->setOcrLinesForTest(QVariantList());
                settle();
                settle();
                QQuickItem *pinRoot = pin->qmlRoot();
                check(pinRoot != nullptr, QStringLiteral("贴图：界面（PinOverlay.qml）加载起来了"));
                if (pinRoot) {
                /* 用户点的那几个键 + 选中，都要真的在（「文字」那个撤了，见下一条） */
                const QStringList wantedKeys{ QStringLiteral("pinCopy"),
                                              QStringLiteral("pinToolHighlight"),
                                              QStringLiteral("pinToolWavy"),
                                              QStringLiteral("pinToolLine"),
                                              QStringLiteral("pinToolStrike"),
                                              QStringLiteral("pinToolSelect"),
                                              QStringLiteral("pinTranslate"),
                                              QStringLiteral("pinClose") };
                int found = 0;
                for (const QString &key : wantedKeys) {
                    if (pinRoot->findChild<QObject *>(key))
                        ++found;
                }
                check(found == wantedKeys.size(),
                      QStringLiteral("贴图：工具条上那几个键都在（复制 / 荧光笔 / 波浪线 / 直线 / 删除线 / 选中 / 翻译 / 关闭）"),
                      QStringLiteral("%1 / %2").arg(found).arg(wantedKeys.size()));
                /*
                 * 「文字」那个键**撤了**（用户说不要这个功能），别哪天又被加回来：
                 * 文字标注本身还认（识别卡片「加到图上」落下来的就是它），所以只量
                 * "工具条上没这个键"，不量"文字那套代码没了"（见下面 testAddText 那条）。
                 */
                check(pinRoot->findChild<QObject *>(QStringLiteral("pinToolText")) == nullptr,
                      QStringLiteral("贴图：工具条上不再有「文字」那个键（这个功能撤了）"));

                /*
                 * 提示条上那句"用法说明"（图上拖 = 选字 / Ctrl+拖 = 挪贴图 …）撤了
                 * （用户要求）：这块现在只写"认字那条路的话"（ocrStatus）。
                 * 量两件事：写的字和 ocrStatus 一字不差，且里头不再有那句的特征词
                 * —— 这么量不挑时机（那一刻 ocrStatus 是空的还是报错都成立）。
                 */
                {
                    auto hintState = [pinRoot]() {
                        QVariant value;
                        QMetaObject::invokeMethod(pinRoot, "testState",
                                                  Q_RETURN_ARG(QVariant, value));
                        return value.toMap();
                    };
                    const QVariantMap hintMap = hintState();
                    const QString hintText = hintMap.value(QStringLiteral("hintText")).toString();
                    const QString status =
                        hintMap.value(QStringLiteral("ocrStatus")).toString();
                    check(hintText == status && !hintText.contains(QStringLiteral("挪贴图"))
                              && !hintText.contains(QStringLiteral("Ctrl+拖")),
                          QStringLiteral("贴图：提示条上不再有那句用法说明（只写认字的话）"),
                          QStringLiteral("提示条「%1」/ ocrStatus「%2」").arg(hintText, status));
                }

                /* 底图取得到（QML 那个 Image 走的是 image://pin/…） */
                const QImage base = pin->imageForId(pin->imageId());
                check(base.width() == qRound(pinSel.width() * pinDpr)
                          && base.height() == qRound(pinSel.height() * pinDpr),
                      QStringLiteral("贴图：底图取得到（image://pin/<id>），就是选区那块"),
                      QStringLiteral("%1x%2 / 期望 %3x%4")
                          .arg(base.width()).arg(base.height())
                          .arg(qRound(pinSel.width() * pinDpr)).arg(qRound(pinSel.height() * pinDpr)));
                check(pin->imageForId(QStringLiteral("不是这个号")).isNull(),
                      QStringLiteral("贴图：别的 id 取不到图（不会串到别的贴图上）"));

                /*
                 * 划一笔荧光笔：走 QML 里和界面同一个 testDraw（pointerDown ->
                 * pointerMove -> pointerUp 那条路），然后看两件事 ——
                 * 数据进没进、**成品图**里那一道真的画上去了没有。
                 */
                const QImage beforeDraw = pin->composedImage();

                /*
                 * 鼠标**只是从图上划过**（没按任何键）不该动 —— 用户报的
                 * "鼠标在固定的图上移动，图就胡乱移动"。
                 *
                 * 这条走的就是 hand 那个 MouseArea 在 hoverEnabled 下发出来的
                 * 那串 positionChanged：以前 pointerMove 只看"挪了几像素"，鼠标
                 * 一移动就被当成"在拖整张贴图"。现在它只认"左键按着"。
                 */
                {
                    const QPoint beforeHover = pin->pos();
                    QVariant hoverPanned;
                    QMetaObject::invokeMethod(pinRoot, "testHoverMove",
                                              Q_RETURN_ARG(QVariant, hoverPanned),
                                              Q_ARG(QVariant, QVariant(60.0)),
                                              Q_ARG(QVariant, QVariant(30.0)));
                    settle();
                    const QPoint hoverMoved = pin->pos() - beforeHover;
                    check(!hoverPanned.toBool() && hoverMoved.isNull(),
                          QStringLiteral("贴图：鼠标从图上划过（没按左键）什么也不动，默认不跟着鼠标跑"),
                          QStringLiteral("当成拖动=%1 / 窗口挪了 %2,%3")
                              .arg(hoverPanned.toBool() ? 1 : 0)
                              .arg(hoverMoved.x()).arg(hoverMoved.y()));
                }

                /*
                 * 固定的图片**能拖动**（用户报的"固定的图片不能拖动"）。
                 *
                 * 走的是界面上那条路：没拿工具时按住拖 -> pointerMove 里认定
                 * "这是拖窗口" -> 叫 PinWindow::beginDrag（交给窗口管理器搬）。
                 * 这里量的是判定本身；真搬窗口那一下归系统，进程内量不到。
                 */
                QVariant tap;
                QMetaObject::invokeMethod(pinRoot, "testTap", Q_RETURN_ARG(QVariant, tap),
                                          Q_ARG(QVariant, QVariant(40.0)),
                                          Q_ARG(QVariant, QVariant(25.0)));
                check(tap.toString() == QStringLiteral("pan"),
                      QStringLiteral("贴图：在图上按住拖 = 拖整张贴图（固定的图片能拖动）"),
                      tap.toString());

                /*
                 * 光"判定是在拖"不够 —— 得看**窗口真的挪了没有**。
                 *
                 * 这条是用户报回来的："还是不能拖动"。当时 pointerMove 里确实认定了
                 * "在拖"，也叫了 PinWindow::beginDrag()，可那边只有一句
                 * startSystemMove()，它在这个环境里不接手，窗口就一直不动。
                 * 所以这里喂一遍按下 -> 拖 -> 松开，然后量窗口的位置。
                 */
                {
                    const QPoint beforePos = pin->pos();
                    /*
                     * 先把"自己搬窗口"那一步单独量了：这条路是自检唯一量得到的
                     * （窗口管理器接手那条归系统，进程内看不见）。它要是好的，
                     * 说明 C++ 那头没问题，剩下的就只是"什么时候叫它"。
                     */
                    QMetaObject::invokeMethod(pinRoot, "testManualDragOnly",
                                              Q_ARG(QVariant, QVariant(30.0)),
                                              Q_ARG(QVariant, QVariant(20.0)));
                    settle();
                    const QPoint moved = pin->pos() - beforePos;
                    check(qAbs(moved.x() - 30) <= 6 && qAbs(moved.y() - 20) <= 6,
                          QStringLiteral("贴图：鼠标挪多少窗口就挪多少（自己搬窗口那条路）"),
                          QStringLiteral("移动了 %1,%2 / 期望 30,20（自己搬=%3）")
                              .arg(moved.x()).arg(moved.y())
                              .arg(pin->draggingManually() ? 1 : 0));
                    /* 量完摆回去，免得影响后面的检查 */
                    pin->move(beforePos);
                    settle();
                }

                /*
                 * 工具条真的铺得开。
                 *
                 * 这条是**实测踩出来的**：bar.width 绑 barFlow.implicitWidth、而
                 * Flow.width 又绑 bar.width，两边成环 —— 量出来是 59×492（所有键
                 * 竖成一列、整条工具条还跑到窗口外，"关闭"根本点不到）。光看"键在
                 * 不在"（上面那条）是看不出来的，必须量几何。
                 */
                QVariant barState;
                QMetaObject::invokeMethod(pinRoot, "barState", Q_RETURN_ARG(QVariant, barState));
                const QVariantMap barMap = barState.toMap();
                const double barW = barMap.value(QStringLiteral("width")).toDouble();
                const double barH = barMap.value(QStringLiteral("height")).toDouble();
                const double barY = barMap.value(QStringLiteral("y")).toDouble();
                check(barW > 300 && barH < 150 && barY + barH <= pin->height() + 1,
                      QStringLiteral("贴图：工具条铺得开、摆在窗口里（不是竖成一列 / 跑出窗口）"),
                      QStringLiteral("%1x%2 @y=%3 / 窗口 %4x%5")
                          .arg(barW).arg(barH).arg(barY).arg(pin->width()).arg(pin->height()));

                /*
                 * 工具条宽度**跟着内容走**：窗口够宽时贴着键收窄，不再一路撑满
                 * （贴图宽到一千多时，两边各留一大片空，键挤在中间一小段里）。
                 *
                 * 上面那个窗口（462）本来就摆不下这一串键，只能撑满 + 折行，量不出
                 * "收窄"，所以这里先把窗口拉宽量一次，量完立刻摆回去。
                 */
                {
                    const QSize keepSize = pin->size();
                    pin->resize(1000, 700);
                    settle();
                    QVariant wideState;
                    QMetaObject::invokeMethod(pinRoot, "barState",
                                              Q_RETURN_ARG(QVariant, wideState));
                    const QVariantMap wideMap = wideState.toMap();
                    const double wideW = wideMap.value(QStringLiteral("width")).toDouble();
                    const double wantW = wideMap.value(QStringLiteral("singleRowWidth")).toDouble();
                    check(wideW < pin->width() - 40 && wantW > 300
                              && qAbs(wideW - (wantW + 18)) <= 2.0,
                          QStringLiteral("贴图：工具条宽度跟着内容走（窗口宽的时候不撑满）"),
                          QStringLiteral("条宽 %1 / 键串 %2 / 窗口 %3")
                              .arg(wideW, 0, 'f', 1).arg(wantW, 0, 'f', 1)
                              .arg(pin->width()));
                    /* 摆不下的时候（窄窗口）：撑满 + 折行，不能把键挤到窗口外 */
                    QVariant narrowState;
                    pin->resize(keepSize);
                    settle();
                    QMetaObject::invokeMethod(pinRoot, "barState",
                                              Q_RETURN_ARG(QVariant, narrowState));
                    const QVariantMap narrowMap = narrowState.toMap();
                    const double narrowW = narrowMap.value(QStringLiteral("width")).toDouble();
                    const double narrowFlowW =
                        narrowMap.value(QStringLiteral("flowWidth")).toDouble();
                    check(narrowW <= pin->width() - 15
                              && narrowFlowW <= narrowW - 15
                              && narrowMap.value(QStringLiteral("singleRowWidth")).toDouble()
                                     > narrowW,
                          QStringLiteral("贴图：窄窗口时工具条撑满并折行（键串比条宽还长）"),
                          QStringLiteral("条宽 %1 / Flow %2 / 键串 %3 / 窗口 %4")
                              .arg(narrowW, 0, 'f', 1).arg(narrowFlowW, 0, 'f', 1)
                              .arg(narrowMap.value(QStringLiteral("singleRowWidth")).toDouble(),
                                   0, 'f', 1)
                              .arg(pin->width()));
                }

                /*
                 * 工具条"鼠标进来才露、一离开就收"（用户要求）。
                 *
                 * 走真鼠标事件（HoverLeave / HoverMove / 真点击），量三件事：
                 *   ① 鼠标离开 -> 收起来（透明度归零，**命中也一并关掉**）
                 *   ② 这时点在它原来的位置上不能误按到那一排键
                 *   ③ 鼠标再进来 -> 又露出来，而且点得动了
                 *
                 * ②挑「认字」来点，不挑「关闭」：判据是同一条（父项 enabled=false
                 * 会把子项那些 MouseArea 一起关掉），而「关闭」真被点到就把这块贴图
                 * 关了 —— 万一哪天判据坏了，这条要红得能看懂，不该把后面全带崩。
                 */
                {
                    auto barNow = [pinRoot]() {
                        QVariant value;
                        QMetaObject::invokeMethod(pinRoot, "barState",
                                                  Q_RETURN_ARG(QVariant, value));
                        return value.toMap();
                    };
                    /*
                     * 露 / 收都带 150ms 淡入淡出，而 settle() 一轮差不多就 150ms ——
                     * 只等一轮会量到"淡到一半"（实测 0.72），得等它淡完再读数。
                     */
                    auto settleOpacity = [&](bool shown) {
                        double o = barNow().value(QStringLiteral("opacity")).toDouble();
                        for (int i = 0; i < 12; ++i) {
                            if (shown ? (o >= 0.99) : (o <= 0.01))
                                break;
                            settle();
                            o = barNow().value(QStringLiteral("opacity")).toDouble();
                        }
                        return o;
                    };
                    QQuickWindow *scene = pinRoot->window();
                    /* 鼠标离开：收起来 */
                    hoverScene(scene, QPoint(qRound(pin->width() / 2.0),
                                             qRound(pin->height() / 2.0)), false);
                    settle();
                    const double hiddenOpacity = settleOpacity(false);
                    const QVariantMap hidden = barNow();
                    check(!hidden.value(QStringLiteral("hovered")).toBool()
                              && !hidden.value(QStringLiteral("shown")).toBool()
                              && hiddenOpacity <= 0.01
                              && !hidden.value(QStringLiteral("enabled")).toBool(),
                          QStringLiteral("贴图：鼠标一离开就把工具条收起来（连命中一起关）"),
                          QStringLiteral("hovered=%1 shown=%2 透明度=%3 enabled=%4")
                              .arg(hidden.value(QStringLiteral("hovered")).toBool())
                              .arg(hidden.value(QStringLiteral("shown")).toBool())
                              .arg(hiddenOpacity, 0, 'f', 2)
                              .arg(hidden.value(QStringLiteral("enabled")).toBool()));

                    /*
                     * ② 收起来的时候这一下**吃不吃**：拿同一个键、同一种事件
                     * （按下+抬起，不带那一下移动）在两种状态下各点一次 ——
                     * 收着的时候不该有反应，露出来的时候必须有反应。一正一反
                     * 才算量到，不然"事件根本没送到"会假装通过。
                     *
                     * 挑「认字」不挑「关闭」：判据是同一条（父项 enabled=false 把
                     * 子项那些 MouseArea 一起关掉），而「关闭」真被点到就把这块贴图
                     * 关了 —— 万一哪天判据坏了，这条要红得能看懂，不该把后面全带崩。
                     */
                    QQuickItem *ocrButton = pinRoot->findChild<QQuickItem *>(QStringLiteral("pinOcr"));
                    check(ocrButton != nullptr,
                          QStringLiteral("贴图：收起来也要问得出「认字」在哪儿（自检按它点）"));
                    if (ocrButton) {
                        QMetaObject::invokeMethod(pinRoot, "testOcrMenu",
                                                  Q_ARG(QVariant, QVariant(false)));
                        settle();
                        const QPoint at = ocrButton
                                              ->mapToScene(QPointF(ocrButton->width() / 2.0,
                                                                   ocrButton->height() / 2.0))
                                              .toPoint();
                        clickSceneNoHover(scene, at);      /* 收着 */
                        settle();
                        check(!pinRoot->property("ocrMenuOpen").toBool(),
                              QStringLiteral("贴图：收起来的时候那个键不吃点击"),
                              QStringLiteral("点在 %1,%2（「认字」的位置）/ 菜单 %3")
                                  .arg(at.x()).arg(at.y())
                                  .arg(pinRoot->property("ocrMenuOpen").toBool()));
                    }

                    /* ③ 鼠标再进来：露出来、点得动（同一下点击这回必须有反应） */
                    hoverScene(scene, QPoint(qRound(pin->width() / 2.0),
                                             qRound(pin->height() / 2.0)), true);
                    settle();
                    const double shownOpacity = settleOpacity(true);
                    const QVariantMap shown = barNow();
                    check(shown.value(QStringLiteral("hovered")).toBool()
                              && shown.value(QStringLiteral("shown")).toBool()
                              && shownOpacity >= 0.99
                              && shown.value(QStringLiteral("enabled")).toBool(),
                          QStringLiteral("贴图：鼠标一进来工具条又露出来（还是可点的）"),
                          QStringLiteral("hovered=%1 shown=%2 透明度=%3 enabled=%4")
                              .arg(shown.value(QStringLiteral("hovered")).toBool())
                              .arg(shown.value(QStringLiteral("shown")).toBool())
                              .arg(shownOpacity, 0, 'f', 2)
                              .arg(shown.value(QStringLiteral("enabled")).toBool()));
                    if (ocrButton) {
                        const QPoint at = ocrButton
                                              ->mapToScene(QPointF(ocrButton->width() / 2.0,
                                                                   ocrButton->height() / 2.0))
                                              .toPoint();
                        clickSceneNoHover(scene, at);      /* 露着 */
                        settle();
                        check(pinRoot->property("ocrMenuOpen").toBool(),
                              QStringLiteral("贴图：露出来之后同一个键就吃了（菜单叫得出来）"),
                              QStringLiteral("点在 %1,%2 / 菜单 %3")
                                  .arg(at.x()).arg(at.y())
                                  .arg(pinRoot->property("ocrMenuOpen").toBool()));
                        QMetaObject::invokeMethod(pinRoot, "testOcrMenu",
                                                  Q_ARG(QVariant, QVariant(false)));
                        settle();
                    }
                }

                QVariant drawn;
                QMetaObject::invokeMethod(pinRoot, "testDraw", Q_RETURN_ARG(QVariant, drawn),
                                          Q_ARG(QVariant, QVariant(QStringLiteral("highlight"))),
                                          Q_ARG(QVariant, QVariant(20.0)),
                                          Q_ARG(QVariant, QVariant(20.0)),
                                          Q_ARG(QVariant, QVariant(140.0)),
                                          Q_ARG(QVariant, QVariant(24.0)));
                settle();
                check(drawn.toBool() && pin->annotationCount() == 1,
                      QStringLiteral("贴图：在图上划一下就多一条标注（荧光笔）"),
                      QStringLiteral("标注 %1 条").arg(pin->annotationCount()));

                const QImage afterDraw = pin->composedImage();
                check(afterDraw.size() == beforeDraw.size() && afterDraw != beforeDraw,
                      QStringLiteral("贴图：划的那一道真的进了成品图（不是只有预览里有）"));

                /* 文字：落一条 -> 改它的字（"内容可以编辑"） -> 成品图跟着变 */
                QVariant added;
                QMetaObject::invokeMethod(pinRoot, "testAddText", Q_RETURN_ARG(QVariant, added),
                                          Q_ARG(QVariant, QVariant(30.0)),
                                          Q_ARG(QVariant, QVariant(60.0)),
                                          Q_ARG(QVariant, QVariant(QStringLiteral("第一版"))));
                settle();
                const int textIndex = added.toInt();
                QVariant edited;
                QMetaObject::invokeMethod(pinRoot, "testEditText", Q_RETURN_ARG(QVariant, edited),
                                          Q_ARG(QVariant, QVariant(textIndex)),
                                          Q_ARG(QVariant, QVariant(QStringLiteral("改过的字"))));
                settle();
                QVariant state;
                QMetaObject::invokeMethod(pinRoot, "testState", Q_RETURN_ARG(QVariant, state));
                check(state.toMap().value(QStringLiteral("text0")).toString()
                          == QStringLiteral("改过的字"),
                      QStringLiteral("贴图：文字标注的内容改得动（内容可以编辑）"),
                      state.toMap().value(QStringLiteral("text0")).toString());

                /* 撤销：把刚加的那条退掉 */
                QMetaObject::invokeMethod(pinRoot, "undo");
                settle();
                check(pin->annotationCount() == 1,
                      QStringLiteral("贴图：撤销退掉最后那条标注"),
                      QStringLiteral("还剩 %1 条").arg(pin->annotationCount()));

                /* 缩放：窗口跟着图一起变（不然图会溢出窗口） */
                const qreal zoom0 = pin->zoom();
                pin->zoomBy(1.1);
                settle();
                check(pin->zoom() > zoom0
                          && qAbs(pin->width() - qRound(pinSel.width() * pin->zoom())) <= 2,
                      QStringLiteral("贴图：放大之后窗口跟着图一起变大"),
                      QStringLiteral("zoom %1 -> %2 / 窗口 %3")
                          .arg(zoom0).arg(pin->zoom()).arg(pin->width()));

                /* 识别那条路要的那份图：和截图识别一样是 png 的 data URL */
                check(pin->composedImageUrl().startsWith(QStringLiteral("data:image/png;base64,")),
                      QStringLiteral("贴图：交给模型识别的图编得出来（png 的 data URL）"),
                      pin->composedImageUrl().left(32));

                /* 复制：进剪贴板的是**成品图**（带上刚划的那道） */
                const QImage composedNow = pin->composedImage();
                pin->copyResult();
                const QImage clip = QGuiApplication::clipboard()->image();
                check(!clip.isNull() && clip.size() == composedNow.size(),
                      QStringLiteral("贴图：复制到剪贴板的是合成之后那张图"),
                      QStringLiteral("剪贴板 %1x%2 / 成品 %3x%4")
                          .arg(clip.width()).arg(clip.height())
                          .arg(composedNow.width()).arg(composedNow.height()));

                /*
                 * ============ 图上选字（两个引擎） ============
                 *
                 * 分三截量，**故意分开**：
                 *   ① 引擎这一层：菜单里那两项在不在、能不能用怎么判、点一行真的
                 *      能不能换（含一次真鼠标点击）、PP-OCR 那条命令的"能不能跑"怎么判；
                 *   ② PP-OCR 的**进程 + 解析**：用一个假的 runner（批处理把一份写好
                 *      的 JSON 拷到结果路径），不依赖这台机器装没装 Python ——
                 *      顺带把"结果 JSON 解析得够宽容"也量了（两种坐标 / 套层 / 代码块）。
                 *   ③ 界面那一层：摆一批**假的行**进去，量"按在字上拖能不能选出
                 *      那一段、跨行会不会连着选、空白处拖动有没有被吃掉"。
                 *
                 * 真认字那条路（Windows 自带 OCR）在②后面单独量。
                 */
                {
                    /* ---- ① 菜单里两个选项 + 可用性判据 ---- */
                    QVariant engineList;
                    QMetaObject::invokeMethod(pinRoot, "testOcrEngines",
                                              Q_RETURN_ARG(QVariant, engineList));
                    const QVariantList engineItems = engineList.toList();
                    QString engineText;
                    bool keysOk = engineItems.size() == 2;
                    for (int i = 0; i < engineItems.size(); ++i) {
                        const QVariantMap item = engineItems.at(i).toMap();
                        const QString key = item.value(QStringLiteral("key")).toString();
                        if (key != QStringLiteral("windows") && key != QStringLiteral("ppocr"))
                            keysOk = false;
                        engineText += QStringLiteral("%1(%2)%3 ")
                                          .arg(key, item.value(QStringLiteral("label")).toString(),
                                               item.value(QStringLiteral("problem")).toString());
                    }
                    check(keysOk,
                          QStringLiteral("选字：认字菜单里两个引擎都在（Windows 自带 / PP-OCRv6）"),
                          engineText);

                    QVariant menuRows;
                    QMetaObject::invokeMethod(pinRoot, "testOcrMenuRows",
                                              Q_RETURN_ARG(QVariant, menuRows));
                    check(menuRows.toInt() >= 3,
                          QStringLiteral("选字：菜单里真把两行画出来了（标题 + 两项）"),
                          QStringLiteral("子项 %1 / 期望 ≥3").arg(menuRows.toInt()));

                    /*
                     * **真点一下**：先点工具条上的「认字」（对照），再点菜单里那一行。
                     *
                     * 为什么非要真事件：直接调 QML 函数只能验到"我们自己的逻辑对不对"，
                     * 验不到"点到底落没落到那个控件上"——用户报的"菜单里那两行点了没反应"
                     * 正是后者，而它长得和"逻辑错了"一模一样。这里往窗口里送真的
                     * 按下 + 抬起（走 QQuickWidget 那条投递链：命中测试 -> MouseArea
                     * 抢到 -> onClicked），先拿一个**已知点得着**的按钮当对照：
                     * 对照要是也点不动，那就是自检这套送事件的办法有问题，不是界面的事。
                     */
                    if (pin) {
                        QQuickWindow *pinScene = pinRoot->window();
                        QQuickItem *ocrButton = pinRoot->findChild<QQuickItem *>(QStringLiteral("pinOcr"));
                        check(ocrButton != nullptr,
                              QStringLiteral("选字：工具条上那个「认字」按钮找得到（对照用）"));
                        QMetaObject::invokeMethod(pinRoot, "testOcrMenu",
                                                  Q_ARG(QVariant, QVariant(false)));
                        settle();
                        if (ocrButton) {
                            const QPoint buttonPos =
                                ocrButton->mapToScene(QPointF(ocrButton->width() / 2.0,
                                                              ocrButton->height() / 2.0))
                                    .toPoint();
                            clickScene(pinScene, buttonPos);
                            settle();
                            check(pinRoot->property("ocrMenuOpen").toBool(),
                                  QStringLiteral("选字：真拿鼠标点「认字」能把菜单叫出来（对照）"),
                                  QStringLiteral("点在 %1,%2 / 现在菜单 %3")
                                      .arg(buttonPos.x()).arg(buttonPos.y())
                                      .arg(pinRoot->property("ocrMenuOpen").toBool()));
                        }
                        /* 对完照把菜单打开，接着点菜单里那一行 */
                        QMetaObject::invokeMethod(pinRoot, "testOcrMenu",
                                                  Q_ARG(QVariant, QVariant(true)));
                        settle();
                    }

                    /*
                     * **真点一下菜单里那一行**。
                     *
                     * 这一段和下面那段是两件事：下面那段直接调 QML 函数（量"选择记没记住"），
                     * 这一段往「PP-OCRv6」那一行的正中送一次**真的按下 + 抬起**（走
                     * QQuickWidget 那条投递链：命中测试 -> MouseArea 抢到 -> onClicked）——
                     * 量的是"点到底落没落到那一行上"。用户报的"这两行点了没反应"就是这一层，
                     * 直接调函数永远量不到它。
                     *
                     * 点之前把命令换成一个跑不通的程序：PP-OCR 那条路会立刻报错返回，
                     * 不会真去跑十几秒的 OCR（这一条量的是"点到了、换了引擎"）。
                     */
                    if (llm && pin) {
                        /* 那一行在哪儿：问 QML（Repeater 自己那份数据） */
                        QVariant centerInfo;
                        QMetaObject::invokeMethod(pinRoot, "testOcrRowCenter",
                                                  Q_RETURN_ARG(QVariant, centerInfo),
                                                  Q_ARG(QVariant,
                                                        QVariant(QStringLiteral("ppocr"))));
                        const QVariantMap center = centerInfo.toMap();
                        check(center.value(QStringLiteral("found")).toBool()
                                  && center.value(QStringLiteral("menuOpen")).toBool(),
                              QStringLiteral("选字：菜单里那一行的位置问得出来（自检要按它点）"),
                              QStringLiteral("found=%1 menuOpen=%2 %3x%4")
                                  .arg(center.value(QStringLiteral("found")).toBool())
                                  .arg(center.value(QStringLiteral("menuOpen")).toBool())
                                  .arg(center.value(QStringLiteral("w")).toInt())
                                  .arg(center.value(QStringLiteral("h")).toInt()));
                        const QString savedEngine = llm->pinOcrEngine();
                        const QString savedRunner = llm->pinOcrRunner();
                        /*
                         * 要能点得动，那一行得是**可用**的（命令跑不通时它是灰的，
                         * 点了当然没反应 —— 那是设计如此，不是毛病）。
                         * 命令指到 `cmd.exe /c exit`：程序在（所以可用）、立刻返回、
                         * 不写结果文件 —— 既验了"点得到"，又不用真跑十几秒的 PP-OCR。
                         */
                        const QString fastRunner = QStandardPaths::findExecutable(
                                                       QStringLiteral("cmd"))
                                                   + QStringLiteral(" /c exit");
                        llm->setPinOcrRunner(fastRunner);
                        llm->setPinOcrEngine(QStringLiteral("windows"));
                        settle();
                        QVariant rowProblem;
                        QMetaObject::invokeMethod(pinRoot, "testEngineProblem",
                                                  Q_RETURN_ARG(QVariant, rowProblem),
                                                  Q_ARG(QVariant, QVariant(QStringLiteral("ppocr"))));
                        check(rowProblem.toString().isEmpty(),
                              QStringLiteral("选字：命令能跑时 PP-OCR 那一行是可点的（不是灰的）"),
                              QStringLiteral("判据「%1」/ 命令 %2")
                                  .arg(rowProblem.toString(), fastRunner));
                        if (center.value(QStringLiteral("found")).toBool()) {
                            /* 场景坐标就是点击坐标（QQuickWidget 里根对象的原点 = 控件原点） */
                            const QPoint rowPos(center.value(QStringLiteral("x")).toDouble(),
                                                center.value(QStringLiteral("y")).toDouble());
                            clickScene(pinRoot->window(), rowPos);
                            settle();
                            check(llm->pinOcrEngine() == QStringLiteral("ppocr"),
                                  QStringLiteral("选字：真拿鼠标点菜单里那一行 = 换成它（点得到、也换了）"),
                                  QStringLiteral("点在 %1,%2 -> 现在记下的是 %3")
                                      .arg(rowPos.x()).arg(rowPos.y())
                                      .arg(llm->pinOcrEngine()));
                        }
                        llm->setPinOcrEngine(savedEngine);
                        llm->setPinOcrRunner(savedRunner);
                        QMetaObject::invokeMethod(pinRoot, "testOcrMenu",
                                                  Q_ARG(QVariant, QVariant(false)));
                        settle();
                    }

                    /*
                     * 换完的选择要**记下来**（不只是拿它跑一遍）。
                     *
                     * 踩过：runOcr 只 startOcr(新引擎) 却不写 Llm.pinOcrEngine ——
                     * 勾号不动、下次开贴图又回老引擎。
                     */
                    if (llm) {
                        const QString savedEngine = llm->pinOcrEngine();
                        const QString savedRunner = llm->pinOcrRunner();
                        /* 命令指到一个跑不通的程序：这条路会立刻报错、不真起进程 */
                        llm->setPinOcrRunner(QStringLiteral("绝对不存在的程序.exe --x"));
                        llm->setPinOcrEngine(QStringLiteral("windows"));
                        QMetaObject::invokeMethod(pinRoot, "runOcr",
                                                  Q_ARG(QVariant, QVariant(QStringLiteral("ppocr"))));
                        settle();
                        check(llm->pinOcrEngine() == QStringLiteral("ppocr"),
                              QStringLiteral("选字：换过的引擎记下来了（勾号跟着动）"),
                              QStringLiteral("换完记下的是 %1").arg(llm->pinOcrEngine()));
                        QMetaObject::invokeMethod(pinRoot, "runOcr",
                                                  Q_ARG(QVariant, QVariant(QStringLiteral("windows"))));
                        settle();
                        check(llm->pinOcrEngine() == QStringLiteral("windows"),
                              QStringLiteral("选字：两行能来回换（换回 Windows 自带）"),
                              QStringLiteral("换回来记下的是 %1").arg(llm->pinOcrEngine()));
                        llm->setPinOcrEngine(savedEngine);
                        llm->setPinOcrRunner(savedRunner);
                    }

                    /* "为什么不能用"要说得出来：临时把命令换成一个跑不通的，问一次 */
                    if (llm) {
                        const QString savedRunner = llm->pinOcrRunner();
                        llm->setPinOcrRunner(QStringLiteral("绝对不存在的程序.exe --x"));
                        QVariant badProblem;
                        QMetaObject::invokeMethod(pinRoot, "testEngineProblem",
                                                  Q_RETURN_ARG(QVariant, badProblem),
                                                  Q_ARG(QVariant, QVariant(QStringLiteral("ppocr"))));
                        check(!badProblem.toString().isEmpty(),
                              QStringLiteral("选字：PP-OCR 命令跑不通时，菜单里说得出为什么"),
                              badProblem.toString());
                        llm->setPinOcrRunner(savedRunner);
                    }
                    check(!PinOcr::runnerProblem(QStringLiteral("绝对不存在的程序.exe --x")).isEmpty(),
                          QStringLiteral("选字：找不到程序时说得出「找不到」"));
                    check(PinOcr::defaultRunnerCommand().contains(QStringLiteral("ppocr_runner.py")),
                          QStringLiteral("选字：默认那条命令指的是随包脚本"),
                          PinOcr::defaultRunnerCommand());

                    /* ---- ② PP-OCR 的进程 + 解析（假 runner，不依赖 Python）---- */
                    QTemporaryDir runnerDir;
                    check(runnerDir.isValid(), QStringLiteral("选字：临时目录建得出来"));
                    if (runnerDir.isValid()) {
                        /* 写一份"runner 应该吐出来"的结果（像素坐标 + 中文） */
                        const QByteArray preparedJson = QStringLiteral(
                                                            "[{\"text\":\"第一行 RUNNER\",\"box\":[10,20,200,30]},"
                                                            "{\"text\":\"第二行 中文\",\"box\":[12,60,160,28]}]")
                                                            .toUtf8();
                        const QString prepared = runnerDir.filePath(QStringLiteral("prepared.json"));
                        {
                            /*
                             * open / write 都是 [[nodiscard]]：接住返回值再断言。
                             * 不接的话 MSVC 会报 C4834（"放弃具有 nodiscard 属性的
                             * 函数的返回值"），而且这里本来就该知道写没写成功 ——
                             * 这份文件是下面假 runner 要去拷的源，写歪了后面全在测空气。
                             */
                            QFile file(prepared);
                            const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
                            check(opened, QStringLiteral("选字：假 runner 的源文件写得出来"),
                                  prepared);
                            if (opened) {
                                const qint64 wrote = file.write(preparedJson);
                                check(wrote == preparedJson.size(),
                                      QStringLiteral("选字：假 runner 的源文件写得完整"),
                                      QStringLiteral("写了 %1 / 共 %2 字节")
                                          .arg(wrote)
                                          .arg(preparedJson.size()));
                            }
                        }
                        /* 假 runner：把准备好的那份拷到"结果路径"（argv[2]）。
                           路径都写成反斜杠：cmd 不认 C:/… 那种（会报"找不到文件"） */
                        const QByteArray fakeBat =
                            QStringLiteral("@echo off\r\ncopy /y \"%1\" \"%~2\" >nul\r\n")
                                .arg(QDir::toNativeSeparators(prepared))
                                .toUtf8();
                        const QString fake = QDir::toNativeSeparators(
                            runnerDir.filePath(QStringLiteral("fake_runner.bat")));
                        {
                            /* 同上：接住 + 断言，别让"批处理没写出来"伪装成"PP-OCR 跑不通" */
                            QFile file(fake);
                            const bool opened = file.open(QIODevice::WriteOnly | QIODevice::Truncate);
                            check(opened, QStringLiteral("选字：假 runner 的批处理写得出来"), fake);
                            if (opened) {
                                const qint64 wrote = file.write(fakeBat);
                                check(wrote == fakeBat.size(),
                                      QStringLiteral("选字：假 runner 的批处理写得完整"),
                                      QStringLiteral("写了 %1 / 共 %2 字节")
                                          .arg(wrote)
                                          .arg(fakeBat.size()));
                            }
                        }
                        /* 命令按"程序 + 参数"的写法给（和设置里那栏一样） */
                        QString command = QStringLiteral("cmd /c ") + fake;

                        QImage paper(640, 360, QImage::Format_ARGB32);
                        paper.fill(Qt::white);
                        QString runnerError;
                        const QVariantList runnerLines =
                            PinOcr::recognizeWithProgram(paper, command, &runnerError);
                        check(runnerLines.size() == 2
                                  && runnerLines.first().toMap().value(QStringLiteral("text"))
                                         .toString() == QStringLiteral("第一行 RUNNER"),
                              QStringLiteral("选字：PP-OCR 那条命令跑得通，结果解析得出两行"),
                              QStringLiteral("%1 行 / err=%2 / 目录 %3 / bat %4 / 结果 %5")
                                  .arg(runnerLines.size()).arg(runnerError)
                                  .arg(runnerDir.path())
                                  .arg(QFileInfo::exists(fake) ? 1 : 0)
                                  .arg(QFileInfo::exists(prepared) ? 1 : 0));
                        if (runnerLines.size() == 2) {
                            /* 像素坐标要按图片尺寸归一化（10/640=0.0156，30/360=0.0833） */
                            const QVariantMap first = runnerLines.first().toMap();
                            const double x = first.value(QStringLiteral("x")).toDouble();
                            const double h = first.value(QStringLiteral("h")).toDouble();
                            check(qAbs(x - 10.0 / 640.0) < 0.002 && qAbs(h - 30.0 / 360.0) < 0.002,
                                  QStringLiteral("选字：结果里的像素坐标按图片尺寸归一化了（0~1）"),
                                  QStringLiteral("x=%1（期望 %2） h=%3（期望 %4）")
                                      .arg(x, 0, 'f', 4).arg(10.0 / 640.0, 0, 'f', 4)
                                      .arg(h, 0, 'f', 4).arg(30.0 / 360.0, 0, 'f', 4));
                        }

                        /* 解析够宽容：外面裹着 ```json、还套了一层 lines、坐标两种写法 */
                        const QString modelReply = QStringLiteral(
                            "```json\n{\"lines\":[{\"text\":\"第一行\",\"box\":[0.1,0.2,0.3,0.1]},"
                            "{\"text\":\"第二行\",\"x\":0.1,\"y\":0.4,\"w\":0.5,\"h\":0.1}]}\n```");
                        QString modelError;
                        const QVariantList modelLines =
                            PinOcr::parseLinesJson(modelReply, QSize(640, 360), &modelError);
                        check(modelLines.size() == 2
                                  && modelLines.first().toMap().value(QStringLiteral("x")).toDouble()
                                         == 0.1,
                              QStringLiteral("选字：结果 JSON 解析得够宽容（```json 代码块 / lines 套层 / 两种坐标）"),
                              QStringLiteral("%1 行 / err=%2").arg(modelLines.size()).arg(modelError));

                        /*
                         * 认字这条路出错时，界面上要说得出话（不是"什么都没发生"）：
                         * 跑一条跑不通的命令，错误文本得写明白 —— 界面把这句话显示在
                         * 提示条上（PinOverlay 的 ocrStatus 绑的就是它）。
                         * 这里不走 startOcr：自检早把认字锁住了（为了下面拖鼠标那几条
                         * 的确定性，见 setOcrLinesForTest）。
                         */
                        QString badError;
                        PinOcr::recognizeWithProgram(paper,
                                                     QStringLiteral("绝对不存在的程序.exe --x"),
                                                     &badError);
                        check(!badError.isEmpty(),
                              QStringLiteral("选字：认字出错时给得出原因（界面显示在提示条上）"),
                              badError);
                    }
                }

                /*
                 * ============ 图上选字：界面那一层 ============
                 *
                 * 摆一批**假的行**进去，量"按在字上拖能不能选出那一段、跨行会不会
                 * 连着选、空白处拖动有没有被吃掉"。这截不依赖这台机器装没装 OCR。
                 */
                {
                    const double vw = double(pin->width());
                    const double vh = double(pin->height());
                    const QString line1 = QStringLiteral("第一行 HELLO WORLD");
                    const QString line2 = QStringLiteral("第二行 你好世界");

                    QVariantList fake;
                    fake.append(QVariantMap{ { QStringLiteral("text"), line1 },
                                             { QStringLiteral("x"), 0.05 },
                                             { QStringLiteral("y"), 0.10 },
                                             { QStringLiteral("w"), 0.80 },
                                             { QStringLiteral("h"), 0.12 } });
                    fake.append(QVariantMap{ { QStringLiteral("text"), line2 },
                                             { QStringLiteral("x"), 0.05 },
                                             { QStringLiteral("y"), 0.30 },
                                             { QStringLiteral("w"), 0.60 },
                                             { QStringLiteral("h"), 0.12 } });
                    pin->setOcrLinesForTest(fake);
                    settle();

                    auto uiState = [pinRoot]() {
                        QVariant value;
                        QMetaObject::invokeMethod(pinRoot, "testState",
                                                  Q_RETURN_ARG(QVariant, value));
                        return value.toMap();
                    };
                    auto dragTo = [pinRoot](double x1, double y1, double x2, double y2) {
                        QVariant value;
                        QMetaObject::invokeMethod(pinRoot, "testOcrDrag",
                                                  Q_RETURN_ARG(QVariant, value),
                                                  Q_ARG(QVariant, QVariant(x1)),
                                                  Q_ARG(QVariant, QVariant(y1)),
                                                  Q_ARG(QVariant, QVariant(x2)),
                                                  Q_ARG(QVariant, QVariant(y2)));
                        return value.toString();
                    };

                    const QVariantMap withLines = uiState();
                    check(withLines.value(QStringLiteral("ocrLines")).toInt() == 2,
                          QStringLiteral("选字：认出来的行摆进了文字层（两条）"),
                          QStringLiteral("行数 %1")
                              .arg(withLines.value(QStringLiteral("ocrLines")).toInt()));

                    /* ① 在第一行上从左往右拖半行：选出来的该是这行的**前半段** */
                    const QPoint posBefore = pin->pos();
                    const QString half = dragTo(0.05 * vw, 0.16 * vh, 0.45 * vw, 0.16 * vh);
                    const QPoint posAfter = pin->pos();
                    check(!half.isEmpty() && line1.startsWith(half) && half.length() < line1.length(),
                          QStringLiteral("选字：在字上拖 = 选中那一段（前半行）"),
                          QStringLiteral("选到「%1」").arg(half));
                    check(posAfter == posBefore,
                          QStringLiteral("选字：在字上拖不挪窗口（选字优先于拖窗口）"),
                          QStringLiteral("窗口 %1,%2 -> %3,%4")
                              .arg(posBefore.x()).arg(posBefore.y())
                              .arg(posAfter.x()).arg(posAfter.y()));

                    /* 选中的就是「翻这段」/ Ctrl+C 用的那段（两路合一个口子） */
                    const QVariantMap picked = uiState();
                    check(picked.value(QStringLiteral("picked")).toString() == half,
                          QStringLiteral("选字：选中的那段就是「翻这段」/ Ctrl+C 要用的那段"),
                          QStringLiteral("picked=%1")
                              .arg(picked.value(QStringLiteral("picked")).toString()));

                    /* 高亮：选中那段该画出来的矩形（根坐标），一行一个 */
                    QVariant ranges;
                    QMetaObject::invokeMethod(pinRoot, "testOcrRanges",
                                              Q_RETURN_ARG(QVariant, ranges));
                    const QVariantList rangeList = ranges.toList();
                    QString rangeText;
                    for (const QVariant &entry : rangeList) {
                        const QVariantMap r = entry.toMap();
                        rangeText += QStringLiteral("[%1,%2 %3x%4]")
                                         .arg(r.value(QStringLiteral("x0")).toDouble(), 0, 'f', 1)
                                         .arg(r.value(QStringLiteral("y0")).toDouble(), 0, 'f', 1)
                                         .arg(r.value(QStringLiteral("x1")).toDouble(), 0, 'f', 1)
                                         .arg(r.value(QStringLiteral("y1")).toDouble(), 0, 'f', 1);
                    }
                    check(rangeList.size() == 1,
                          QStringLiteral("选字：拖完之后高亮矩形算得出来（一行一个）"),
                          QStringLiteral("%1 个：%2").arg(rangeList.size()).arg(rangeText));

                    /* ② 从第一行拖到第二行：两行连着选，中间一个换行 */
                    const QString both = dragTo(0.06 * vw, 0.16 * vh, 0.35 * vw, 0.36 * vh);
                    QString bothShown = both;
                    bothShown.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
                    check(both.contains(QLatin1Char('\n')) && both.contains(QStringLiteral("第二行")),
                          QStringLiteral("选字：跨行拖 = 两行连着选（行与行之间是换行）"),
                          QStringLiteral("选到「%1」").arg(bothShown));

                    /* ③ 空白处（两行的右边、没字的地方）拖：不该选字，该走"挪贴图" */
                    const QPoint blankBefore = pin->pos();
                    const QString blank = dragTo(0.90 * vw, 0.16 * vh, 0.95 * vw, 0.20 * vh);
                    check(blank.isEmpty(),
                          QStringLiteral("选字：空白处拖不选字（拖窗口那条路没被吃掉）"),
                          QStringLiteral("选到「%1」").arg(blank));
                    pin->move(blankBefore);      /* 这一下把窗口挪了，摆回去 */
                    settle();

                    /*
                     * ④ 选着字的时候点工具按钮 = **把这种标注贴到选中的那几段上**
                     *    （"划词即标注"）：不用自己去拖，位置按行框来 ——
                     *      荧光笔        行框那么高的色带，居中盖住字
                     *      波浪线 / 直线  贴行框底边（下划线）
                     *      删除线        穿行框中间
                     *
                     * 期望值自己按注入的那两行算：第一行 y=0.10 h=0.12（中心 0.16、
                     * 底边 0.22），第二行 y=0.30 h=0.12（中心 0.36、底边 0.42）。
                     * 行是归一化的、乘以 view 尺寸就是根坐标，标注存的是**图坐标**，
                     * 所以要再除以缩放（跟着 tool 那套换算来，别自己另算一套）。
                     */
                    {
                        const double zoomNow = pin->zoom();
                        auto lastShape = [pinRoot]() {
                            QVariant value;
                            QMetaObject::invokeMethod(pinRoot, "testLastShape",
                                                      Q_RETURN_ARG(QVariant, value));
                            return value.toMap();
                        };
                        auto pickTool = [pinRoot](const QString &name) {
                            QMetaObject::invokeMethod(pinRoot, "pickTool",
                                                      Q_ARG(QVariant, QVariant(name)));
                        };
                        auto shapeField = [&lastShape](const QString &key) {
                            return lastShape().value(key).toDouble();
                        };

                        const int countBefore = uiState().value(QStringLiteral("count")).toInt();

                        /* 荧光笔：选中第一行的前半段 -> 色带该正好盖住这一行的行框 */
                        dragTo(0.05 * vw, 0.16 * vh, 0.45 * vw, 0.16 * vh);
                        pickTool(QStringLiteral("highlight"));
                        settle();
                        const int afterHighlight = uiState().value(QStringLiteral("count")).toInt();
                        const double hlCenter = (shapeField(QStringLiteral("y1"))
                                                 + shapeField(QStringLiteral("y2"))) / 2 * zoomNow;
                        const double hlBand = shapeField(QStringLiteral("stroke")) * 2.4 * zoomNow;
                        check(afterHighlight == countBefore + 1
                                  && lastShape().value(QStringLiteral("kind")).toString()
                                         == QStringLiteral("highlight")
                                  && qAbs(hlCenter - 0.16 * vh) <= 1.5
                                  && qAbs(hlBand - 0.12 * vh) <= 2.0,
                              QStringLiteral("标注：选中字后点「荧光笔」= 色带正好盖住那一行"),
                              QStringLiteral("条数 %1->%2 / 中心 %3（行中心 %4）/ 带高 %5（行高 %6）")
                                  .arg(countBefore).arg(afterHighlight)
                                  .arg(hlCenter, 0, 'f', 1).arg(0.16 * vh, 0, 'f', 1)
                                  .arg(hlBand, 0, 'f', 1).arg(0.12 * vh, 0, 'f', 1));
                        /* 左右到选中的两端（拖的是 0.05~0.45） */
                        check(qAbs(shapeField(QStringLiteral("x1")) * zoomNow - 0.05 * vw) <= 2.0
                                  && qAbs(shapeField(QStringLiteral("x2")) * zoomNow - 0.45 * vw) <= 2.0,
                              QStringLiteral("标注：贴上去的那条左右就到选中的两端"),
                              QStringLiteral("x %1~%2 / 选中 %3~%4")
                                  .arg(shapeField(QStringLiteral("x1")) * zoomNow, 0, 'f', 1)
                                  .arg(shapeField(QStringLiteral("x2")) * zoomNow, 0, 'f', 1)
                                  .arg(0.05 * vw, 0, 'f', 1).arg(0.45 * vw, 0, 'f', 1));
                        /* 贴完选中该收掉（蓝色那层高亮别和刚贴的叠在一起） */
                        check(uiState().value(QStringLiteral("picked")).toString().isEmpty()
                                  && uiState().value(QStringLiteral("tool")).toString().isEmpty(),
                              QStringLiteral("标注：贴完把选中收掉、也没把工具切走"),
                              QStringLiteral("picked「%1」/ tool「%2」")
                                  .arg(uiState().value(QStringLiteral("picked")).toString(),
                                       uiState().value(QStringLiteral("tool")).toString()));
                        /*
                         * 荧光笔那条**不能**跟着变细：它是"涂一层"，线宽就是 stroke
                         * （带高 = ×2.4）—— 上面量过带高正好等于行高，这里钉住"没被
                         * 0.6 那个系数碰到"（不然按行高算出来的带子就白算了）。
                         */
                        check(qAbs(lastShape().value(QStringLiteral("width")).toDouble()
                                   - shapeField(QStringLiteral("stroke"))) < 0.01
                                  && shapeField(QStringLiteral("stroke")) >= 3.0,
                              QStringLiteral("标注：荧光笔没被「细一点」碰到（还是带子那么高）"),
                              QStringLiteral("画出来的宽 %1 / stroke %2")
                                  .arg(lastShape().value(QStringLiteral("width")).toDouble(),
                                       0, 'f', 2)
                                  .arg(shapeField(QStringLiteral("stroke")), 0, 'f', 2));

                        /* 波浪线 / 直线：贴行框底边（第二行底边 0.42）**并且要细** */
                        const QStringList underTools{ QStringLiteral("wavy"), QStringLiteral("line") };
                        for (const QString &name : underTools) {
                            dragTo(0.05 * vw, 0.36 * vh, 0.60 * vw, 0.36 * vh);
                            pickTool(name);
                            settle();
                            const QVariantMap shape = lastShape();
                            const double y = shape.value(QStringLiteral("y1")).toDouble() * zoomNow;
                            const double width = shape.value(QStringLiteral("width")).toDouble();
                            const double amp = shape.value(QStringLiteral("amp")).toDouble();
                            check(shape.value(QStringLiteral("kind")).toString() == name
                                      && qAbs(y - 0.42 * vh) <= 1.5,
                                  QStringLiteral("标注：%1 贴在行框底边（当下划线用）")
                                      .arg(name == QStringLiteral("wavy") ? QStringLiteral("波浪线")
                                                                          : QStringLiteral("直线")),
                                  QStringLiteral("y %1（行底边 %2）")
                                      .arg(y, 0, 'f', 1).arg(0.42 * vh, 0, 'f', 1));
                            /* 用户嫌 3px 太粗：0.6 倍 -> 1.8，且波浪振幅跟着细下来 */
                            check(width >= 1.2 && width <= 2.0 && amp <= width * 1.2 + 0.01,
                                  QStringLiteral("标注：%1 是细的（0.6 倍，振幅跟着收）")
                                      .arg(name == QStringLiteral("wavy") ? QStringLiteral("波浪线")
                                                                          : QStringLiteral("直线")),
                                  QStringLiteral("线宽 %1 / 振幅 %2（原来 3 / 3.6）")
                                      .arg(width, 0, 'f', 2).arg(amp, 0, 'f', 2));
                        }

                        /* 删除线：穿行框中间（第二行中心 0.36），也是细的 */
                        dragTo(0.05 * vw, 0.36 * vh, 0.60 * vw, 0.36 * vh);
                        pickTool(QStringLiteral("strike"));
                        settle();
                        const double strikeY = shapeField(QStringLiteral("y1")) * zoomNow;
                        check(lastShape().value(QStringLiteral("kind")).toString()
                                      == QStringLiteral("strike")
                                  && qAbs(strikeY - 0.36 * vh) <= 1.5,
                              QStringLiteral("标注：删除线穿过行框中间"),
                              QStringLiteral("y %1（行中间 %2）")
                                  .arg(strikeY, 0, 'f', 1).arg(0.36 * vh, 0, 'f', 1));
                        const double strikeW =
                            lastShape().value(QStringLiteral("width")).toDouble();
                        check(strikeW >= 1.2 && strikeW <= 2.0,
                              QStringLiteral("标注：删除线也是细的（0.6 倍）"),
                              QStringLiteral("线宽 %1（原来 3）").arg(strikeW, 0, 'f', 2));

                        /*
                         * 成品图（复制 / 存盘 / 交给模型的那张）是 **C++ 另一份画法**，
                         * 两份靠注释对齐 —— 注释拦不住走样，所以在这里真量一次粗细：
                         * 画一条直线，数成品图上"变了的那一列"里有几个像素。
                         */
                        {
                            const QImage beforeImg = pin->composedImage();
                            QVariant drewLine;
                            QMetaObject::invokeMethod(pinRoot, "testDraw",
                                                      Q_RETURN_ARG(QVariant, drewLine),
                                                      Q_ARG(QVariant, QVariant(QStringLiteral("line"))),
                                                      Q_ARG(QVariant, QVariant(40.0)),
                                                      Q_ARG(QVariant, QVariant(0.5 * vh)),
                                                      Q_ARG(QVariant, QVariant(200.0)),
                                                      Q_ARG(QVariant, QVariant(0.5 * vh)));
                            const QImage afterImg = pin->composedImage();
                            const double dpr = afterImg.devicePixelRatio() > 0
                                                   ? afterImg.devicePixelRatio() : 1.0;
                            const int col = qBound(0, qRound(120.0 / zoomNow * dpr),
                                                   afterImg.width() - 1);
                            int thick = 0;
                            for (int y = 0; y < afterImg.height() && y < beforeImg.height(); ++y) {
                                const QRgb a = beforeImg.pixel(col, y);
                                const QRgb b = afterImg.pixel(col, y);
                                if (qAbs(qRed(a) - qRed(b)) + qAbs(qGreen(a) - qGreen(b))
                                        + qAbs(qBlue(a) - qBlue(b)) > 40)
                                    ++thick;
                            }
                            check(drewLine.toBool() && thick >= 1
                                      && thick <= qRound(2.6 * dpr),
                                  QStringLiteral("标注：成品图上那条线也是细的（C++ 那份画法）"),
                                  QStringLiteral("那一列变了 %1 个像素（原来 ~%2）")
                                      .arg(thick).arg(qRound(3.6 * dpr)));
                            QMetaObject::invokeMethod(pinRoot, "undo");   /* 量完撤掉 */
                            settle();
                        }

                        /* 没选中字的时候：点工具还是"选好工具准备拖"，不加东西 */
                        const int beforeNoSelect = uiState().value(QStringLiteral("count")).toInt();
                        pickTool(QStringLiteral("wavy"));
                        settle();
                        check(uiState().value(QStringLiteral("count")).toInt() == beforeNoSelect
                                  && uiState().value(QStringLiteral("tool")).toString()
                                         == QStringLiteral("wavy"),
                              QStringLiteral("标注：没选中字时点工具 = 只是把工具选上（没乱加）"),
                              QStringLiteral("条数 %1 / tool「%2」")
                                  .arg(uiState().value(QStringLiteral("count")).toInt())
                                  .arg(uiState().value(QStringLiteral("tool")).toString()));

                        /* 量完收摊：工具收掉、刚加的这几条撤掉（后面的检查不认它们） */
                        pickTool(QString());
                        const int added = uiState().value(QStringLiteral("count")).toInt()
                                          - countBefore;
                        for (int i = 0; i < added; ++i)
                            QMetaObject::invokeMethod(pinRoot, "undo");
                        settle();
                        check(uiState().value(QStringLiteral("count")).toInt() == countBefore,
                              QStringLiteral("标注：量完能收干净（刚加的那几条撤销掉）"),
                              QStringLiteral("条数 %1（期望回到 %2）")
                                  .arg(uiState().value(QStringLiteral("count")).toInt())
                                  .arg(countBefore));
                    }

                    /* 量完把行清掉，免得影响后面"点一下"那些检查 */
                    pin->setOcrLinesForTest(QVariantList());
                    settle();
                }

                /*
                 * 真认字那条路：自己画一张有字的图，交给 Windows 自带 OCR 认一遍。
                 * 没有语言包（或 SDK 里没有 cppwinrt）就跳过 —— 那是环境没有，
                 * 不是这次改动坏了。
                 */
                if (PinOcr::available()) {
                    QImage paper(560, 170, QImage::Format_ARGB32);
                    paper.fill(Qt::white);
                    {
                        QPainter painter(&paper);
                        painter.setPen(Qt::black);
                        painter.setFont(QFont(QStringLiteral("Microsoft YaHei"), 30));
                        painter.drawText(QRect(20, 15, 520, 60), Qt::AlignLeft | Qt::AlignVCenter,
                                         QStringLiteral("HELLO 12345"));
                        painter.drawText(QRect(20, 95, 520, 60), Qt::AlignLeft | Qt::AlignVCenter,
                                         QStringLiteral("图上选字"));
                    }
                    /* 先确认这张图真画上字了（不然量的是"画图失败"） */
                    int ink = 0;
                    for (int y = 0; y < paper.height(); y += 2) {
                        const QRgb *row = reinterpret_cast<const QRgb *>(paper.constScanLine(y));
                        for (int x = 0; x < paper.width(); x += 2)
                            if (qGray(row[x]) < 128)
                                ++ink;
                    }
                    /*
                     * 和真跑那条路一样，在**工作线程**里认：GUI 主线程是 STA 单元，
                     * WinRT 的异步回调要靠它自己抽消息，在那儿 .get() 干等容易出怪
                     * 结果（真跑时是 QThreadPool 的线程，见 PinWindow::startOcr）。
                     */
                    QVariantList lines;
                    QString ocrError;
                    std::thread worker([&lines, &ocrError, &paper]() {
                        lines = PinOcr::recognize(paper, &ocrError);
                    });
                    worker.join();
                    QString joined;
                    for (const QVariant &entry : lines)
                        joined += entry.toMap().value(QStringLiteral("text")).toString()
                                  + QStringLiteral("|");
                    check(ink > 100 && !lines.isEmpty()
                              && joined.contains(QStringLiteral("HELLO"), Qt::CaseInsensitive),
                          QStringLiteral("选字：Windows 自带 OCR 认得出画上去的字（%1）")
                              .arg(PinOcr::language()),
                          QStringLiteral("认出 %1 行：%2 / 墨点 %3 / err=%4")
                              .arg(lines.size()).arg(joined).arg(ink).arg(ocrError));
                    if (!lines.isEmpty()) {
                        const QVariantMap firstLine = lines.first().toMap();
                        const double lw = firstLine.value(QStringLiteral("w")).toDouble();
                        const double lx = firstLine.value(QStringLiteral("x")).toDouble();
                        const double ly = firstLine.value(QStringLiteral("y")).toDouble();
                        /* 行框得落在图上、且是"一行"那么大（不是整张图、也不是一个点）*/
                        check(lx >= 0.0 && ly >= 0.0 && lw > 0.10 && lw < 0.98,
                              QStringLiteral("选字：给出来的行框落在图上合理的位置（0~1 的归一化值）"),
                              QStringLiteral("x=%1 y=%2 w=%3")
                                  .arg(lx, 0, 'f', 3).arg(ly, 0, 'f', 3).arg(lw, 0, 'f', 3));
                    }
                } else {
                    out() << "  --    跳过：这台机器上没有 Windows OCR 语言包（选字用不了）"
                          << Qt::endl;
                }

                /*
                 * 「翻译」（认整张图那个键）在贴图上到底响不响 —— 用户报的
                 * "点了「翻译」什么都没发生"。
                 *
                 * 病根不在 QML 那边：PinWindow::composedImageUrl() 忘了写
                 * Q_INVOKABLE，QML 里 `root.pinWin.composedImageUrl()` 直接抛
                 *     TypeError: Property 'composedImageUrl' … is not a function
                 * translateAll() 第一行就断了 —— 界面上什么都不发生，连卡片都不摆
                 * （那句话只进 qWarning，GUI 程序的 stderr 抓不到，所以特别像"死"
                 * 了一样）。这条量的就是"界面这层走通了没有"。
                 *
                 * 这里**不发真请求**：接口地址留空，`chatUrl()` 就是空串，
                 * LlmClient::postVision 当场回一句"还没配置接口地址" —— 摆出来
                 * （status 非空）就算响。不留空、指一个没人听的端口也行，但
                 * Windows 上那个连接会一直挂着（实测 6 秒还没出错），自检不值得
                 * 等它。
                 */
                if (llm) {
                    QSettings settings;
                    const QString savedBase = llm->apiBase();
                    const QString savedMode = llm->mode();
                    const QString savedOcr = llm->ocrModel();
                    const bool hadBase = settings.contains(QStringLiteral("translate/apiBase"));
                    const bool hadOcr = settings.contains(QStringLiteral("translate/ocrModel"));

                    llm->setMode(QStringLiteral("api"));
                    llm->setApiBase(QString());
                    llm->setOcrModel(QStringLiteral("自检视觉模型"));

                    auto cardState = [pinRoot]() {
                        QVariant value;
                        QMetaObject::invokeMethod(pinRoot, "testCardState",
                                                  Q_RETURN_ARG(QVariant, value));
                        return value.toMap();
                    };
                    auto clickTranslate = [pinRoot, &settle]() {
                        QMetaObject::invokeMethod(pinRoot, "translateAll");
                        settle();
                    };
                    /* 等到请求收尾（连不上那个端口时很快就出错）：忙的时候
                       「翻译」这个键是灰的，"能不能再点"要等这一下结束再量 */
                    auto waitNotBusy = [&cardState, &settle]() {
                        for (int i = 0; i < 40; ++i) {
                            settle();
                            if (!cardState().value(QStringLiteral("ocrBusy")).toBool())
                                return;
                        }
                    };
                    auto describe = [](const QVariantMap &state) {
                        return QStringLiteral("visible=%1 status=%2")
                            .arg(state.value(QStringLiteral("visible")).toBool() ? 1 : 0)
                            .arg(state.value(QStringLiteral("status")).toString());
                    };

                    clickTranslate();
                    waitNotBusy();
                    const QVariantMap first = cardState();
                    check(first.value(QStringLiteral("visible")).toBool()
                              && !first.value(QStringLiteral("status")).toString().isEmpty(),
                          QStringLiteral("贴图：点「翻译」之后识别卡片摆得出来（界面这条链走通了）"),
                          describe(first));
                    /* 卡在"忙"上的话，「翻译」这个键就永远是灰的、点不动 */
                    check(!first.value(QStringLiteral("ocrBusy")).toBool(),
                          QStringLiteral("贴图：这一下之后没卡在「忙」上（「翻译」还能再点）"));

                    /*
                     * 关掉卡片再点一次。
                     *
                     * 卡片里的 ✕ 以前直接写 card.visible = false，把外面
                     * `visible: hasResult` 那条绑定打断了 —— 关过一次之后 hasResult
                     * 再变真也摆不出来（用户报的"点了没反应"的第二个成因）。
                     * 现在 ✕ 只发 closeRequested，外面清 hasResult。
                     */
                    QMetaObject::invokeMethod(pinRoot, "testCardClose");
                    settle();
                    clickTranslate();
                    waitNotBusy();
                    const QVariantMap second = cardState();
                    check(second.value(QStringLiteral("visible")).toBool(),
                          QStringLiteral("贴图：卡片关掉之后再点「翻译」，卡片还摆得出来"),
                          describe(second));

                    llm->setMode(savedMode);
                    if (hadBase)
                        llm->setApiBase(savedBase);
                    else
                        settings.remove(QStringLiteral("translate/apiBase"));
                    if (hadOcr)
                        llm->setOcrModel(savedOcr);
                    else
                        settings.remove(QStringLiteral("translate/ocrModel"));
                    settings.sync();
                }

                /*
                 * 在图上单击（不拖）**不关**这张贴图 —— 左键 / 右键都一样。
                 *
                 * 用户报的"怎么左键、右键点一下就关掉了"：原来 pointerUp 里有一条
                 * "随手点一下就收工"（这类贴图工具的老习惯），而且它没分左右键。
                 * 现在单击只当"摸一下这张图"，关掉走工具条上的「关闭」/ Esc。
                 */
                QMetaObject::invokeMethod(pinRoot, "testTap", Q_RETURN_ARG(QVariant, tap),
                                          Q_ARG(QVariant, QVariant(0.0)),
                                          Q_ARG(QVariant, QVariant(0.0)));
                QMetaObject::invokeMethod(pinRoot, "testRightTap");
                settle();
                check(tap.toString() == QStringLiteral("idle") && pin->isVisible()
                          && shot->pinnedCount() == 1,
                      QStringLiteral("贴图：在图上单击（左键 / 右键，不拖）不关掉它"),
                      QStringLiteral("返回 %1 / 贴图 %2 个 / 露着 %3")
                          .arg(tap.toString()).arg(shot->pinnedCount())
                          .arg(pin->isVisible() ? 1 : 0));

                /*
                 * 收尾：走「关闭」那条路（工具条上那个「关闭」调的就是它），
                 * 关完必须从清单里划掉。**必须放最后** —— 关掉就删，这之后再碰
                 * pin 就是野指针（自检在这儿崩过一次：ASSERT 在 qlist.h 里，
                 * 看着像内存踩了，其实是访问已经析构掉的窗口）。
                 */
                pin->closePin();
                settle();
                check(shot->pinnedCount() == 0,
                      QStringLiteral("贴图：关掉之后从清单里划掉了（不留空条目）"));
                }
            }
        }

        /*
         * ============ 识别（框选之后交给大模型认内容） ============
         *
         * 自检**不发真请求**（那要一台配好的视觉模型）：发请求那一套在
         * src/SelfTestTranslate.cpp 第 10 节里拿假服务验。这里验的是界面这条
         * 链路 —— 选区那块图取不取得到、结果摆不摆得出来、卡片上的"复制 /
         * 加到图上"走不走得通。这条链路最容易坏的地方是"图根本没取到"
         * （选区坐标没按 dpr 换算）和"结果摆上去了但没法用"，两样都不会崩。
         */
        if (QObject *root = shot->overlayRoot()) {
            /* 选区和缩放比从根对象 / 冻结图上读（这一块在 if (overlay) 外面，那
               两个变量在它里面；读属性是那边同一份值） */
            const QRectF ocrSel = root->property("sel").toRectF();
            const QImage frozen =
                shot->imageForId(QStringLiteral("full%1").arg(shot->serial()));
            const double ocrDpr = frozen.devicePixelRatio() > 0 ? frozen.devicePixelRatio() : 1.0;

            QVariant imageUrl;
            QMetaObject::invokeMethod(root, "testSelectionImage", Q_RETURN_ARG(QVariant, imageUrl));
            const QString dataUrl = imageUrl.toString();
            check(dataUrl.startsWith(QStringLiteral("data:image/png;base64,")),
                  QStringLiteral("识别：选区那块图取得出来（界面点「识别」发的就是它）"),
                  dataUrl.left(32));

            /*
             * 这张图**必须就是选区那一块**：解回来量一下尺寸。
             * 少了这一条的话，"高 DPI 下少乘一个 dpr"这类错误看不出来 ——
             * 图是有一张，只是内容跟框的那块对不上。
             */
            if (dataUrl.startsWith(QStringLiteral("data:image/png;base64,"))) {
                const QImage cropped = QImage::fromData(
                    QByteArray::fromBase64(
                        dataUrl.mid(QStringLiteral("data:image/png;base64,").size()).toLatin1()));
                const int wantW = qRound(ocrSel.width() * ocrDpr);
                const int wantH = qRound(ocrSel.height() * ocrDpr);
                check(!cropped.isNull() && qAbs(cropped.width() - wantW) <= 2
                          && qAbs(cropped.height() - wantH) <= 2,
                      QStringLiteral("识别：那张图正好是框选的那一块（设备像素）"),
                      QStringLiteral("%1x%2 / 期望 %3x%4")
                          .arg(cropped.width()).arg(cropped.height()).arg(wantW).arg(wantH));
            }

            check(shot->selectionReady(ocrSel), QStringLiteral("识别：选区可用时按钮是亮的"));
            check(!shot->selectionReady(QRectF(ocrSel.x(), ocrSel.y(), 2, 2)),
                  QStringLiteral("识别：选区小到没意义时不当成可用（不至于发个空请求出去）"));

            /*
             * 工具条上那两个"识别"键**撤了**（用户说这个功能不要了），别哪天又被
             * 加回来。识别请求那套代码本身还留着（卡片 / selectionReady 那些检查
             * 照旧走），所以这里只量"工具条上没这两个键"。
             */
            check(root->findChild<QObject *>(QStringLiteral("screenshotRecognize")) == nullptr,
                  QStringLiteral("截图：工具条上不再有「识别」那个键（这个功能撤了）"));
            check(root->findChild<QObject *>(QStringLiteral("screenshotRecognizeLang")) == nullptr,
                  QStringLiteral("截图：工具条上不再有换识别语言那个 ▾ 了"));

            /*
             * 颜色：**就 24 种**，"更多颜色"那个系统取色框（QColorDialog）撤了。
             * 一半在 QML（调色板几条），一半在 C++（那个方法得真没了，不然
             * 界面上找不到入口、代码里还挂着个模态框）。
             */
            QVariant colorState;
            QMetaObject::invokeMethod(root, "colorState", Q_RETURN_ARG(QVariant, colorState));
            const QVariantMap colorMap = colorState.toMap();
            check(colorMap.value(QStringLiteral("palette")).toInt() == 24,
                  QStringLiteral("截图：调色板就是 24 种颜色（不多不少）"),
                  QStringLiteral("%1 种").arg(colorMap.value(QStringLiteral("palette")).toInt()));
            check(shot->metaObject()->indexOfMethod("pickColor(QString)") < 0,
                  QStringLiteral("截图：系统取色框那套（pickColor）删干净了"));

            /* 摆一份结果进卡片（等同模型回复到了） */
            QVariant shown;
            QMetaObject::invokeMethod(root, "testShowRecognition", Q_RETURN_ARG(QVariant, shown),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("Hello world"))),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("你好，世界"))));
            settle();
            check(shown.toBool(), QStringLiteral("识别：结果摆得出来"));

            QVariant state;
            QMetaObject::invokeMethod(root, "testRecognitionState", Q_RETURN_ARG(QVariant, state));
            const QVariantMap map = state.toMap();
            check(map.value(QStringLiteral("visible")).toBool(),
                  QStringLiteral("识别：卡片真的显示出来了"));
            check(map.value(QStringLiteral("original")).toString()
                      == QStringLiteral("Hello world"),
                  QStringLiteral("识别：卡片上是原文那一栏"),
                  map.value(QStringLiteral("original")).toString());
            check(map.value(QStringLiteral("translated")).toString()
                      == QStringLiteral("你好，世界"),
                  QStringLiteral("识别：卡片上是译文那一栏"),
                  map.value(QStringLiteral("translated")).toString());

            /* "加到图上"：识别出来的字变成一条普通文字标注（能接着改字号 / 拖动） */
            QMetaObject::invokeMethod(root, "testRecognitionAnnotate");
            settle();
            QVariant textsVariant;
            QMetaObject::invokeMethod(root, "textsData", Q_RETURN_ARG(QVariant, textsVariant));
            bool foundAnnotated = false;
            const QVariantList textList = textsVariant.toList();
            for (const QVariant &entry : textList) {
                if (entry.toMap().value(QStringLiteral("text")).toString()
                        == QStringLiteral("Hello world"))
                    foundAnnotated = true;
            }
            check(foundAnnotated,
                  QStringLiteral("识别：识别出来的字能落成一条文字标注（落到图上接着改）"),
                  QStringLiteral("标注 %1 条").arg(textList.size()));

            /* "复制"：识别出来的字进剪贴板 */
            QMetaObject::invokeMethod(root, "testRecognitionCopy");
            settle();
            check(QGuiApplication::clipboard()->text().contains(QStringLiteral("Hello world")),
                  QStringLiteral("识别：卡片上的「复制」把文字放进了剪贴板"),
                  QGuiApplication::clipboard()->text());
        }

        shot->endCapture();
        /*
         * 收工之后选区必须复位成整屏 —— 否则下次打开会带出上一次的框选轮廓
         * （用户报过："下次使用的时候会调出上次的截图框轮廓"）。
         * 这条钉住状态那一半；另一半是窗口表面残留，靠 Screenshot::endCapture 里
         * "强制渲染 + repaint" 解决 —— 那个只能实测（实测：残留帧 3 -> 0）。
         */
        if (QObject *root = shot->overlayRoot()) {
            const QRectF leftover = root->property("sel").toRectF();
            check(leftover.width() >= 1000 && leftover.height() >= 600,
                  QStringLiteral("截图：收工之后选区复位成整屏（下次不会带出上次的框）"),
                  QStringLiteral("残留选区 %1×%2").arg(leftover.width()).arg(leftover.height()));
        }

        check(!shot->overlayVisible() && !shot->active(),
              QStringLiteral("截图：收工之后选区窗口收起来、状态复位"
                             "（窗口留着复用，见 Screenshot::prewarm）"));

        QGuiApplication::clipboard()->setText(oldClipboard);
    }

    /*
     * 托盘右键菜单：用户要的是"托盘右键里能选截图"。
     *
     * 托盘图标本身点不出来（Windows 会把它塞进"隐藏的图标"浮出区，位置还随
     * 图标数量变），但**菜单对象**能直接拿 —— 所以验的是：菜单里确实有
     * 「截图…」这一条，而且触发它真的能开出选区窗口。
     */
    if (tray && shot) {
        QMenu *menu = tray->menu();
        const QList<QAction *> acts = menu ? menu->actions() : QList<QAction *>();
        QAction *shotAct = nullptr;
        for (QAction *a : acts) {
            if (a && a->text().contains(QStringLiteral("截图")))
                shotAct = a;
        }
        check(menu != nullptr && shotAct != nullptr,
              QStringLiteral("托盘：右键菜单里有「截图…」这一条"),
              QStringLiteral("菜单条目 %1 条").arg(acts.size()));

        /*
         * 退出相关的两条得在。原来这里是"退出…"+ 一个"完全退出 / 收进托盘"的选择框，
         * 用户实测那个框会在用截图快捷键的场景里挡住程序（模态，看着像卡死），
         * 要求去掉；现在改成菜单里两个明确条目，这条把菜单结构钉住。
         */
        {
            bool hide = false;
            bool quit = false;
            for (QAction *a : acts) {
                if (!a)
                    continue;
                if (a->text().contains(QStringLiteral("收进托盘")))
                    hide = true;
                if (a->text().contains(QStringLiteral("退出")))
                    quit = true;
            }
            check(hide && quit,
                  QStringLiteral("托盘：菜单里有「收进托盘」和「退出 SmartClip」两条"
                                 "（不再弹模态选择框）"));
        }

        /*
         * 图标资源。原来用的是 QIcon::fromTheme("edit-paste")，Windows 上没有
         * 图标主题、返回空图标，托盘上是一块空白；现在指向随包的 SVG，
         * 这条把资源路径钉住（前缀被改过就会红）。
         */
        check(!QIcon(QStringLiteral(":/icons/image.svg")).isNull(),
              QStringLiteral("托盘：图标资源 :/icons/image.svg 能加载（不是空白图标）"));

        /*
         * 白底。全局调色板是深色的，QMenu 默认跟着走 —— 但用户要白底，
         * 所以这是一处"特意覆盖"，很容易被以后某次改动顺手带回去
         * （谁把样式表删了、谁动了全局调色板都会）。直接把菜单画出来数像素。
         */
        if (menu) {
            menu->adjustSize();
            const QImage shot = menu->grab().toImage();
            /* 顺手留一张渲染图，人眼也能看一眼（临时目录里，和其它自检产物一起） */
            shot.save(dir.filePath(QStringLiteral("tray-menu.png")));
            int light = 0;
            int total = 0;
            for (int y = 0; y < shot.height(); y += 2) {
                for (int x = 0; x < shot.width(); x += 2) {
                    const QColor c = shot.pixelColor(x, y);
                    ++total;
                    if (c.red() > 200 && c.green() > 200 && c.blue() > 200)
                        ++light;
                }
            }
            const int pct = total > 0 ? light * 100 / total : 0;
            check(pct >= 60,
                  QStringLiteral("托盘：右键菜单是白底（不是跟界面走的深色）"),
                  QStringLiteral("亮像素 %1% / 菜单 %2x%3")
                      .arg(pct).arg(shot.width()).arg(shot.height()));
        }

        if (shotAct) {
            shotAct->trigger();
            for (int i = 0; i < 60 && !shot->active(); ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(25);
            }
            check(shot->active(),
                  QStringLiteral("托盘：点菜单里的「截图…」真的开出了选区窗口"));
            shot->endCapture();
        }
    }

    /*
     * 全局截图热键。
     *
     * 用户报的是"最小化之后截图快捷键不能用"—— 程序内那条 QAction 的上下文是
     * WindowShortcut，窗口没激活就不响。修法是额外注册一个系统级热键
     * （RegisterHotKey，见 EditorController::applyGlobalHotkey），这里验两件事：
     * 注册上了没有，以及"收到热键 -> 发命令 -> 开出截图"这条链走不走得通。
     */
    if (cmd && shot) {
        check(cmd->globalHotkeyActive(),
              QStringLiteral("截图键注册成了系统级热键（窗口没激活也能按）"),
              QStringLiteral("注册失败通常是组合键被别的程序占了"));

        /* 系统那边来的就是 WM_HOTKEY，回调里做的正是这一句 */
        cmd->activateCommand(QStringLiteral("shot"));
        for (int i = 0; i < 60 && !shot->active(); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(25);
        }
        check(shot->active(),
              QStringLiteral("全局热键那条路：命令一发，选区窗口就开出来了"));
        shot->endCapture();

        /*
         * 取消截图（Esc / 双击）必须落在"抓屏延时"里也算数。
         *
         * 用户报的就是这个：按了截图快捷键、马上按 Esc 取消，屏幕先亮起一整块
         * 全屏选区、要再按一次 Esc 才关得掉 —— 约 30ms 的延时（见
         * Screenshot::beginCapture）里用户已经取消了，可那会儿 endCapture
         * 什么都关不掉（没有 active 状态可收），延时到点 grabAndShow() 照样
         * 把选区窗口铺出来。
         *
         * 这里钉两层：取消之后窗口**一直**藏着（要等过延时，否则测不出"照样
         * 铺出来"那一半），以及收工时窗口表面是干净的全屏选区、没有虚线框
         * 残留（残留的就是用户看到的那块"全屏框"）。
         */
        {
            shot->beginCapture();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            shot->cancelCapture();     /* 界面那边 Esc / 双击叫的就是这个 */

            check(!shot->active() && !shot->overlayVisible(),
                  QStringLiteral("截图：抓屏延时里按取消，选区窗口立刻收起来"));

            /* 等过那段延时：窗口不许再冒出来 */
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 400) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            check(!shot->active() && !shot->overlayVisible(),
                  QStringLiteral("截图：取消之后延时到点也不会冒出全屏选区（取消要算数）"),
                  QStringLiteral("active=%1 / 窗口可见=%2")
                      .arg(shot->active() ? 1 : 0).arg(shot->overlayVisible() ? 1 : 0));
        }
    }

    /*
     * 关闭键那个问句：一块**独立的小卡片**，底下的界面原封不动。
     *
     * 这条检查的来历：用户在"闪一下"上折腾了三轮 —— 先是被模态框挡住（像卡死），
     * 后来铺透明遮罩（看不见，但铺满整窗，鼠标全被吃掉，编辑区点不动），
     * 最后要求"底下什么也别做"。现在的实现是 QML 侧一块 Popup.Window 卡片
     * （见 qml/components/AskCard.qml），结构上钉三点：没有模态、没有遮罩
     * （就是没有铺满整窗的窗口）、卡片本身比主窗口小得多。
     */
    {
        if (qmlRoot) {
            /*
             * 直接调界面上的入口，不走 C++ 侧那个 WindowHelper 单例 ——
             * 用 engine->singletonInstance 拿到的实例和 QML 里用的不是同一个
             * （改成本对象只负责发信号之后就露馅了：那边发信号，界面这边没反应）。
             * 真机上 ✕ → 信号 → 界面那条桥是好的（点 ✕ 会弹出卡片，已实测）。
             */
            QMetaObject::invokeMethod(qmlRoot, "openQuitAsk");
            for (int i = 0; i < 40; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                QThread::msleep(15);
            }

            check(!QApplication::activeModalWidget(),
                  QStringLiteral("窗口：退出问句是非模态的（不会把程序挡住）"));

            /*
             * 卡片是 Popup.Window 开出来的 **QQuickWindow**，不是 QWidget ——
             * 得看 QWindow 列表，`topLevelWidgets()` 里根本找不到它（第一版就栽在这）。
             *
             * 认的是**这一块卡片自己的窗**（quitAskCard 的 popupItem / contentItem
             * 所在那块，见 Main.qml 里那一行 objectName），而不是"可见小窗只能有
             * 一个"：便签窗口也是 330x300 的小窗，桌面上摆着便签时那种写法必红
             * （用户环境里跑出来的那两条红就是这么来的）。遮罩那种"铺满整窗"的
             * 窗口仍然会被 big 抓住 —— 它一定比主窗口还大。
             *
             * popupItem 这个属性名不一定在（拿不到就退回 contentItem，两个都在
             * 卡片那块原生窗里；和 StickyNoteWindow::raisePopupWindow 同一套取法）。
             */
            QPointer<QQuickWindow> cardWindow;
            if (QObject *card = qmlRoot->findChild<QObject *>(QStringLiteral("quitAskCard"))) {
                auto *item = card->property("popupItem").value<QQuickItem *>();
                if (!item)
                    item = card->property("contentItem").value<QQuickItem *>();
                if (item)
                    cardWindow = item->window();
            }
            int big = 0;
            QStringList seen;
            const auto tops = QGuiApplication::topLevelWindows();
            for (QWindow *w : tops) {
                if (!w->isVisible())
                    continue;
                seen << QStringLiteral("%1x%2").arg(w->width()).arg(w->height());
                if (w->width() >= 800 || w->height() >= 400)
                    ++big;        /* 主窗口那种大窗口；遮罩会在这里露馅 */
            }
            check(cardWindow && cardWindow->isVisible()
                      && cardWindow->width() < 800 && cardWindow->height() < 400 && big <= 1,
                  QStringLiteral("窗口：问句是一块小卡片，没有铺满整窗的遮罩"),
                  QStringLiteral("卡片 %1x%2 / 大窗 %3；可见顶层窗口：%4")
                      .arg(cardWindow ? cardWindow->width() : -1)
                      .arg(cardWindow ? cardWindow->height() : -1)
                      .arg(big).arg(seen.join(QStringLiteral("，"))));

            QMetaObject::invokeMethod(qmlRoot, "closeQuitAsk");
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            check(!cardWindow || !cardWindow->isVisible(),
                  QStringLiteral("窗口：问句收得掉（不留残窗）"),
                  QStringLiteral("卡片那块窗还在：%1")
                      .arg(cardWindow ? QStringLiteral("是") : QStringLiteral("已经没了")));
        }
    }

    /*
     * 「有未保存的改动」那张卡片：和退出问句同一个组件（AskCard.qml），
     * 三个按钮 保存 / 不保存 / 取消。
     *
     * 这条检查钉的是"问完之前不许关"。卡片是异步的（非模态原生小窗，
     * 见 AskCard.qml 开头），原来 Cmd.confirmSave() 那种"同步返回一个数字"
     * 的写法换成了 Main.qml 里的关闭队列（requestCloseTabs / answerSaveAsk）
     * —— 这里最容易出的错就是没等回答就把标签关了，那等于替用户选了"不保存"，
     * 未保存的改动会无声无息地没掉。
     */
    {
        const QString askPath = dir.filePath(QStringLiteral("ask-save.txt"));
        QFile askFile(askPath);
        if (askFile.open(QIODevice::WriteOnly)) {
            askFile.write("hello\n");
            askFile.close();
        }

        check(view->openFile(askPath) >= 0, QStringLiteral("未保存问句用例：打开一个文件"));
        const int before = view->documents().size();
        /*
         * 真改一笔（复制一行）。
         *
         * 不能用 view->setModified(true)：那个 setter 是**只能清标记**的
         * （Scintilla 没有反向的 SCI_SETMODIFY，见 EditorViewItem::setModified），
         * 传 true 是空操作 —— 第一版就栽在这，于是"没改动"那条路把标签直接关了，
         * 卡片压根没弹。
         */
        view->duplicateLine();
        check(view->modified(), QStringLiteral("未保存问句用例：改一笔 -> 已修改"));
        dispatch(QStringLiteral("closeTab"));
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            QThread::msleep(15);
        }

        check(uiState().value(QStringLiteral("saveAskOpened")).toBool(),
              QStringLiteral("未保存问句：有改动，卡片弹出来了"));
        check(view->documents().size() == before,
              QStringLiteral("未保存问句：还没回答，标签不许关"),
              QStringLiteral("还剩 %1 个").arg(view->documents().size()));

        /*
         * 按"不保存"（下标 1）：这一刻才真的关掉。
         *
         * 点的是卡片上的按钮那条路（Main.qml 的 clickSaveAsk -> AskCard.answer
         * -> answered -> answerSaveAsk），不是直接调 answerSaveAsk —— 后者
         * 卡片还开着，"答完就收"就验不到了。
         */
        QMetaObject::invokeMethod(qmlRoot, "clickSaveAsk", Q_ARG(QVariant, QVariant(1)));
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            QThread::msleep(15);
        }
        check(view->documents().size() == before - 1,
              QStringLiteral("未保存问句：答了「不保存」才关掉"),
              QStringLiteral("还剩 %1 个").arg(view->documents().size()));
        check(!uiState().value(QStringLiteral("saveAskOpened")).toBool(),
              QStringLiteral("未保存问句：答完卡片收掉了"));

        QFile::remove(askPath);
    }

    /*
     * 滚动条：两条轨道都得是正文底色，不许露出白带。
     *
     * 这条的来历：轨道原来写的是 background: transparent —— 那是"这一层不画"，
     * 露出来的是底下那一层。滑块的深灰一直是对的（说明样式表确实生效了），
     * 轨道却在有的机器上是一条 12px 的 #f2f2f2 白带（用户报的"这个滚动条白色
     * 背景去掉"）。现在轨道色写死成正文底色，这里抓两条滚动条**自己渲染出来的
     * 图**数一遍近白像素 —— 属性值看着都对、画出来不对，只能这么钉。
     */
    {
        const QString oldClipboard = QGuiApplication::clipboard()->text();
        const bool oldWrap = view->wrapEnabled();   /* 收尾要放回去 */

        check(view->newDocument() >= 0, QStringLiteral("滚动条用例：新建长文档"));
        view->setWrapEnabled(false);        /* 不换行，长行才会顶出横向滚动条 */
        QString big;
        for (int i = 1; i <= 200; ++i)
            big += QStringLiteral("line %1 : ").arg(i)
                   + QString(220, QLatin1Char('x')) + QLatin1Char('\n');
        QGuiApplication::clipboard()->setText(big);
        view->paste();
        settle();

        const QVariantList bars = view->scrollBarPixelStats();
        const int vWhite = bars.value(0).toInt();
        const int hWhite = bars.value(1).toInt();
        const bool vShown = bars.value(2).toBool();
        const bool hShown = bars.value(3).toBool();
        const uint vTrack = bars.value(4).toUInt();
        const uint hTrack = bars.value(5).toUInt();
        /* 底色 #1e1f22 按 QColor 的 RGB 顺序（和 packed() 的 BGR 不一样） */
        const uint paperRgb = 0x1E1F22u;

        out() << "        （竖条 白点 " << vWhite << " / 轨道 #"
              << QString::number(vTrack, 16) << " ；横条 白点 " << hWhite
              << " / 轨道 #" << QString::number(hTrack, 16) << "）" << Qt::endl;

        check(vShown && hShown,
              QStringLiteral("滚动条用例：长内容 + 长行时两条滚动条都在"),
              QStringLiteral("竖 %1 / 横 %2").arg(vShown).arg(hShown));
        check(vWhite == 0 && hWhite == 0,
              QStringLiteral("滚动条轨道里没有白底（露白带就是这条红）"),
              QStringLiteral("竖条 %1 个白点 / 横条 %2 个").arg(vWhite).arg(hWhite));
        check(vTrack == paperRgb && hTrack == paperRgb,
              QStringLiteral("滚动条轨道色就是正文底色"),
              QStringLiteral("竖 #%1 / 横 #%2（要 #1e1f22）")
                  .arg(vTrack, 6, 16, QLatin1Char('0'))
                  .arg(hTrack, 6, 16, QLatin1Char('0')));

        QGuiApplication::clipboard()->setText(oldClipboard);
        view->closeDocument(view->currentIndex());
        view->setWrapEnabled(oldWrap);
        settle();
    }

    /*
     * ======================================================================
     * 弹窗"露出来之后不许再变"
     * ======================================================================
     *
     * 这是那份"弹窗显示策略"里最该被钉住的一条，也是这个工程里最容易复发的一条：
     * **先 open() 再摆 / 先 show() 再量** —— 只要有一处这么写，屏幕上就是一闪。
     *
     * 规矩：**它第一次露出来的那一帧，就必须是它最终的样子**（量具见文件头的
     * surfaceStaysPut：从第一帧可见起连采几帧原生窗口的几何 + 窗口标志）。
     *
     * 摆在这一节的位置：它是最后一段"开开关关弹窗"的检查，后面只剩便签 / 翻译
     * 那两节（各自在别的文件里）。开在这里不会给前面几节的时序添乱。
     *
     * 覆盖到的四块表面（这个工程里能自己开起来的那些）：
     *   1) 退出问句     AskCard（Popup.Window）
     *   2) 设置面板     SettingsPanel（Popup.Window）
     *   3) 下拉菜单首开 DropdownMenu（Popup.Window）
     *   4) 下拉菜单子菜单 —— **把主窗口压矮**再开，强制走到"顶出宿主下沿"那条路
     *
     * 没覆盖的：DiffCard / DocCard（前者要真跑一遍对比，
     * 后一个要真的排一份文档进去；几何都是内容或固定值决定的，等它们各自的
     * 用例补到那一步时再往这儿加一条同样的采样就行）。QtWidgets 那三个输入框
     * 走的是 exec()，采样器够不着（见 EditorController 里的顺序注释）。
     */
    {
        /* 主窗口（QML 场景所在那块原生窗）：弹窗都是**另一块**窗口，不是它 */
        QQuickWindow *mainWindow = nullptr;
        if (auto *rootItem = qobject_cast<QQuickItem *>(qmlRoot))
            mainWindow = rootItem->window();

        /*
         * 按 objectName 找一块表面的**原生窗口**。
         *
         * Popup 型的（问句 / 设置面板 / 下拉菜单）要顺着内容项找它自己那块窗 ——
         * 和"问句是一块小卡片，没有铺满整窗的遮罩"那条一样的取法；
         * Window 型的（DiffCard / DocCard / 便签菜单）本身就是原生窗。
         * 找到主窗口就当没找到：那说明这块表面是**场景内浮层**（Popup.Item），
         * 它的几何不该拿主窗口来量。
         */
        auto surfaceWindow = [qmlRoot, mainWindow](const QString &objectName) -> QQuickWindow * {
            QObject *object = qmlRoot->findChild<QObject *>(objectName);
            if (!object)
                return nullptr;
            if (auto *asWindow = qobject_cast<QQuickWindow *>(object))
                return asWindow == mainWindow ? nullptr : asWindow;
            const char *props[] = { "popupItem", "contentItem" };
            for (const char *prop : props) {
                if (auto *item = object->property(prop).value<QQuickItem *>()) {
                    if (QQuickWindow *asPopupWindow = item->window())
                        return asPopupWindow == mainWindow ? nullptr : asPopupWindow;
                }
            }
            return nullptr;
        };

        /* 主窗口那个 QWidget（要把主窗口临时压矮，见下面子菜单那一条） */
        QWidget *hostWidget = nullptr;
        for (QWidget *candidate : QApplication::topLevelWidgets()) {
            if (candidate->isWindow() && candidate->isVisible()
                && candidate->windowFlags().testFlag(Qt::FramelessWindowHint)
                && candidate->width() >= 800) {
                hostWidget = candidate;   /* 主窗口是唯一那块"大"的无边框窗口 */
                break;
            }
        }
        const QRect hostWas = hostWidget ? hostWidget->geometry() : QRect();

        /* ---- 1. 退出问句（AskCard，Popup.Window） ---- */
        QMetaObject::invokeMethod(qmlRoot, "openQuitAsk");
        {
            QString detail;
            const bool stable =
                surfaceStaysPut([&] { return surfaceWindow(QStringLiteral("quitAskCard")); },
                                6, &detail);
            check(stable,
                  QStringLiteral("弹窗：退出问句露出来的第一帧就是最终样子（不在打开之后对中）"),
                  detail);
            QMetaObject::invokeMethod(qmlRoot, "closeQuitAsk");
            settle();
        }

        /* ---- 2. 设置面板（Popup.Window，尺寸绑在宿主上） ---- */
        dispatch(QStringLiteral("storage"));
        {
            QString detail;
            const bool stable =
                surfaceStaysPut([&] { return surfaceWindow(QStringLiteral("settingsPanel")); },
                                5, &detail);
            check(stable,
                  QStringLiteral("弹窗：设置面板露出来的第一帧就是最终样子"), detail);
            QMetaObject::invokeMethod(qmlRoot, "closeSettings");
            settle();
        }

        /* ---- 3. 下拉菜单首开（不带子菜单） ---- */
        dispatch(QStringLiteral("menu:文件"));
        {
            QString detail;
            const bool stable =
                surfaceStaysPut([&] { return surfaceWindow(QStringLiteral("dropdownMenu")); },
                                5, &detail);
            check(stable,
                  QStringLiteral("弹窗：下拉菜单露出来的第一帧就是最终样子"), detail);
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            settle();
        }

        /* ---- 4. 展开子菜单：**先压矮主窗口**，强制走到"顶出宿主下沿"那条路 ---- */
        if (hostWidget) {
            hostWidget->resize(900, 420);
            settle();
        }
        dispatch(QStringLiteral("menu:视图"));
        settle();
        {
            /*
             * 展开之前先记下这块**原生窗口**在屏幕上的左上角。
             *
             * 为什么量原生窗口、还要在"已经开着"的时候量：规矩是"位置在开之前
             * 一次定死，开出来之后只许长、不许挪"，而挪这一下是**同一个事件回合
             * 里**发生的，采样器（只在帧与帧之间看）追不到它 —— 只有拿"展开前 /
             * 展开后"两个点直接比。
             */
            QQuickWindow *menuWindow = surfaceWindow(QStringLiteral("dropdownMenu"));
            const QRect wasAt = menuWindow ? menuWindow->geometry() : QRect();
            const int shiftsBefore = uiState().value(QStringLiteral("menuOpenShifts")).toInt();

            QVariant opened;
            QMetaObject::invokeMethod(qmlRoot, "openSubmenuFor", Q_RETURN_ARG(QVariant, opened),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("menu:语言"))));
            settle();

            QQuickWindow *afterWindow = surfaceWindow(QStringLiteral("dropdownMenu"));
            const QRect nowAt = afterWindow ? afterWindow->geometry() : QRect();
            const QVariantMap ui = uiState();
            const int shiftsAfter = ui.value(QStringLiteral("menuOpenShifts")).toInt();
            const bool submenuUp = ui.value(QStringLiteral("submenuOpened")).toBool();
            const double hostH = hostWidget ? double(hostWidget->height()) : 0.0;

            check(wasAt.isValid() && nowAt.isValid() && nowAt.topLeft() == wasAt.topLeft(),
                  QStringLiteral("弹窗：展开子菜单时弹窗的左上角一动不动（只往下长）"),
                  QStringLiteral("%1,%2 -> %3,%4（%5x%6 -> %7x%8）")
                      .arg(wasAt.x()).arg(wasAt.y()).arg(nowAt.x()).arg(nowAt.y())
                      .arg(wasAt.width()).arg(wasAt.height())
                      .arg(nowAt.width()).arg(nowAt.height()));
            check(shiftsAfter == shiftsBefore,
                  QStringLiteral("弹窗：展开子菜单没有走到\"露着的时候挪位置\"那条兜底"),
                  QStringLiteral("兜底挪了 %1 次").arg(shiftsAfter - shiftsBefore));
            /*
             * 这一枪得真的打在那条路上：宿主压到 420 之后，"视图 + 语言"这份菜单
             * 展开起来（763，见 DropdownMenu 的 worstExpandedHeight）一定比宿主还高
             * —— 比宿主矮就说明宿主压得不够矮，上面两条是空的。
             */
            check(hostWidget && hostH < 600.0 && submenuUp && nowAt.height() > hostH,
                  QStringLiteral("弹窗：子菜单把窗口撑得比宿主还高（这一枪没打空）"),
                  QStringLiteral("宿主高 %1 / 弹窗 %2x%3 / 子菜单开=%4")
                      .arg(hostH).arg(nowAt.width()).arg(nowAt.height())
                      .arg(submenuUp ? 1 : 0));
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            settle();
        }
        if (hostWidget && hostWas.isValid()) {
            hostWidget->setGeometry(hostWas);
            settle();
        }
    }

    /*
     * ======================================================================
     * 最大化 / 还原：屏幕上不许露白、不许抽一下
     * ======================================================================
     * 用户报的原文："向主程序界面最大化切换，界面会出现闪动，还有白色的背影一闪。"
     *
     * 量法：切换的过程中**一帧一帧抓屏幕**（见 probeScreen），数两样东西 ——
     *   * 近白像素：露白就是它，白点落在哪儿也一并报出来（一眼能看出是整窗还是某一块）；
     *   * 整块的平均亮度："抽一下"那种压暗会在这一列数里露出来。
     * 同时数这次切换来了几拍 Move / Resize —— 每一拍都会重设一次圆角遮罩
     * （WindowHelper::eventFilter 里 Move / Resize 都调 applyRoundedMask），
     * 那一下落到 Windows 上是 SetWindowRgn，戳得越多屏幕上越容易抽。
     *
     * 抓屏一次要几十毫秒，采样间隔就是抓屏的耗时；只有一两帧的白可能抓不着 ——
     * 所以来回切两次，取最差的那一次。
     */
    {
        QWidget *host = nullptr;
        for (QWidget *candidate : QApplication::topLevelWidgets()) {
            if (candidate->isWindow() && candidate->isVisible()
                && candidate->windowFlags().testFlag(Qt::FramelessWindowHint)
                && candidate->width() >= 800) {
                host = candidate;   /* 主窗口是唯一那块"大"的无边框窗口 */
                break;
            }
        }
        QScreen *screen = host ? host->screen() : nullptr;
        if (host && screen) {
            const QRect area = screen->availableGeometry();
            const QRect before = host->geometry();

            /*
             * 取样条放在**主窗口自己那块矩形里**（切换成最大化之后这块地儿照样
             * 还是窗口的）。所以它变亮只有一个解释：**窗口这一块没画出来，
             * 透出底下的东西了** —— 这正是用户说的"白色的背影一闪"。
             */
            const QRect strip(before.x() + 160, before.y() + 160,
                              qMin(800, qMax(200, before.width() - 320)),
                              qMin(400, qMax(200, before.height() - 320)));

            /*
             * 先量几帧"没切换"的当基准。
             *
             * 这几帧必须**又暗又没白**：说明窗口在前台、量到的确实是它。
             * 窗口被别的程序压着时（比如启动自检的那个控制台 / 浏览器），
             * 量到的就是别人的画面 —— 那种数据不能拿来判红判绿，
             * 这一条就明说"这次没量成"，而不是报一个假红。
             */
            host->raise();
            host->activateWindow();
            settle();
            int baseWhite = 0;
            double baseLum = 0;
            for (int i = 0; i < 6; ++i) {
                const ScreenProbe probe = probeRect(screen, strip, QRect(), 4);
                baseWhite = qMax(baseWhite, probe.white);
                baseLum = qMax(baseLum, probe.lum);
                QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            }
            const bool measurable = baseWhite <= 40 && baseLum < 100.0;

            GeometryTally tally;
            host->installEventFilter(&tally);

            int whiteFrames = 0;
            int worstWhite = 0;
            double minLum = 1e9;
            double maxLum = -1e9;
            int frames = 0;
            QString worstWhere;
            QStringList trace;
            /* 四次切换分开记账：最大化 / 还原各两次 —— 只有分清方向，
               才看得出"是最大化那一下白"还是"第一次白、之后就不白了" */
            QStringList stepResult;

            /*
             * "探针角"：常规窗口之外、最大化窗口之内，而且最大化之后那儿画的是
             * **左上角那个橙色应用图标**（右上角那块顶栏）—— 橙色和桌面/壁纸一眼
             * 就分得开。
             *
             * 用它量什么：系统给最大化放一段"从旧位置放大过来"的动画时，界面的边
             * 要过一会儿才扫到这儿；这段时间就是动画的时长。用户报的"最大化时窗口
             * 会变到右边、还在放大"就是这一段。没有动画时它应该**立刻**就变色。
             */
            const QRect corner(24, 8, 48, 48);
            const ScreenProbe cornerBefore = probeRect(screen, corner, QRect(), 4);
            const double cornerR = cornerBefore.meanR;
            const double cornerG = cornerBefore.meanG;
            const double cornerB = cornerBefore.meanB;
            int coverMs = -1;          /* 从"按下去"到界面扫到这个探针角，用了多少毫秒 */
            bool stepCoverWanted = false;   /* 这一步要不要量探针角（只量最大化那几步） */
            QElapsedTimer coverClock;
            QStringList stepCover;

            auto sampleOnce = [&](const QString &tag, int step) {
                const ScreenProbe probe = probeRect(screen, strip, QRect(), 4);
                if (probe.samples == 0)
                    return;
                ++frames;
                minLum = qMin(minLum, probe.lum);
                maxLum = qMax(maxLum, probe.lum);
                /* 探针角：界面的边扫到它了没有（量"有没有一段放大动画在走"） */
                if (coverMs < 0 && stepCoverWanted) {
                    const ScreenProbe cornerNow = probeRect(screen, corner, QRect(), 4);
                    if (cornerNow.samples > 0) {
                        const double diff = qAbs(cornerNow.meanR - cornerR)
                                            + qAbs(cornerNow.meanG - cornerG)
                                            + qAbs(cornerNow.meanB - cornerB);
                        if (diff > 60.0)
                            coverMs = int(coverClock.elapsed());
                    }
                }
                if (probe.white > worstWhite) {
                    worstWhite = probe.white;
                    worstWhere = probe.where;
                }
                if (probe.white > 40) {
                    ++whiteFrames;
                    if (step >= 0)
                        stepResult[step] = QStringLiteral("白%1").arg(probe.white);
                    if (whiteFrames <= 4) {
                        trace << QStringLiteral("%1 #%2 白%3 亮%4")
                                     .arg(tag).arg(frames).arg(probe.white)
                                     .arg(qRound(probe.lum));
                    }
                    /*
                     * 把**这一帧本身**存下来（就是采样条那张 800x400，不再另抓整屏 ——
                     * 实测整屏要 80ms，存下来的已经是下一帧了，看不出白的是什么）。
                     * 只在真露白的时候写这一张，是留给下一个人"一眼看出透出来的是什么"的。
                     */
                    if (probe.white > 4000 && whiteFrames <= 2) {
                        const QString path = QDir::current().filePath(
                            QStringLiteral("maximize-flash-step%1.png").arg(step));
                        probe.shot.save(path);
                        trace << QStringLiteral("  白帧已存图：%1（白%2 亮%3）")
                                     .arg(path).arg(probe.white).arg(qRound(probe.lum));
                    }
                }
            };

#if defined(Q_OS_WIN)
            /* 圆角是靠遮罩裁的（不是靠窗口透明）—— 这一条钉住"四角真的被裁掉了"，
               免得哪天把窗口改成不透明之后，四角悄悄变成方的 */
            check(!host->mask().isEmpty() && !host->mask().contains(QPoint(0, 0)),
                  QStringLiteral("窗口：圆角遮罩落上了（左上角那一像素真的被裁掉）"),
                  host->mask().isEmpty() ? QStringLiteral("遮罩是空的")
                                         : QStringLiteral("遮罩在，但左上角没被裁"));
#endif

            /* 先把"系统转场动画关掉了没有"钉住：那条动画就是用户说的"窗口先跑到
               右边、还在放大"（见 DialogStyle.h 里 disableDwmTransitions 的说明） */
            check(uiState().value(QStringLiteral("transitionsDisabled")).toBool(),
                  QStringLiteral("窗口：主窗口关掉了系统转场动画（最大化不再\"从旧位置缩放过来\"）"),
                  QStringLiteral("transitionsDisabled=%1")
                      .arg(uiState().value(QStringLiteral("transitionsDisabled")).toBool() ? 1 : 0));

            if (measurable) {
                /*
                 * 四次切换：最大化 / 还原 / 最大化 / 还原。
                 *
                 * 方向要分开记：实测**只有最大化会白**，还原从来不白 —— 合成一行
                 * "露白 N 帧"就看不出这个区别了，下次谁改坏了也不知道改坏的是哪一半。
                 */
                for (int step = 0; step < 4; ++step) {
                    /*
                     * 每一步之前都重新"上台"一次，并且**先确认这一步量的是我们这块窗**。
                     *
                     * 为什么：这块窗的位置/可见性都对，但别的东西（浏览器）可能在这一步
                     * 中间抢到前台 —— 那时取样条里看到的是它，不是我们。这种数据**不能**
                     * 拿来判红判绿（有一次跑出来 328 帧"白"，全是这么来的，而窗口根本没透）。
                     * 判据用亮度：我们这块窗是深色的（基准 31），别人的白底页面一眼就分得开。
                     */
                    host->raise();
                    host->activateWindow();
                    settle();
                    const ScreenProbe pre = probeRect(screen, strip, QRect(), 4);
                    if (pre.lum > 100.0 || pre.white > 40) {
                        stepResult << QStringLiteral("没量成(亮%1)").arg(qRound(pre.lum));
                        continue;
                    }

                    const bool wasMaximized = qmlRoot->property("maximized").toBool();
                    const QString dir = wasMaximized ? QStringLiteral("还原 ")
                                                     : QStringLiteral("最大化");
                    stepResult << QStringLiteral("-");
                    /* 只对"最大化"量探针角（用户报的就是那一下） */
                    stepCoverWanted = !wasMaximized;
                    coverMs = -1;
                    coverClock.start();
                    QMetaObject::invokeMethod(qmlRoot, "toggleMaximize");
                    QElapsedTimer clock;
                    clock.start();
                    while (clock.elapsed() < 800) {
                        sampleOnce(dir + QStringLiteral("（第%1次）").arg(step), step);
                    }
                    if (stepCoverWanted)
                        stepCover << QStringLiteral("%1ms").arg(coverMs);

                    /*
                     * 最大化那几步：**必须**走系统那个最大化状态（WS_MAXIMIZE 置上）。
                     *
                     * 这条是"不会露白"的确定性判据：自己的几何 + 幕布那套要在一个
                     * 回合里同时改"窗口位置"和"窗口形状"，两者是两个 API 调用、
                     * 区域又是窗口内坐标，中间必然露出过一帧（用户报的白色边、
                     * 取消最大化也闪，见 WindowHelper::applyState 里那段记录）。
                     * 交给系统之后，几何 / 表面重建 / 重画由窗口管理器一手包办，
                     * 外面还有 DWM 那段转场盖着 —— 谁哪天把它改回"自己摆几何"，
                     * 这条立刻红。
                     */
                    if (stepCoverWanted) {
                        bool sysMax = false;
#if defined(Q_OS_WIN)
                        if (HWND h = reinterpret_cast<HWND>(host->winId()))
                            sysMax = (GetWindowLongPtr(h, GWL_STYLE) & WS_MAXIMIZE) != 0;
#endif
                        check(sysMax,
                              QStringLiteral("最大化：走的是系统最大化状态"
                                             "（DWM 那段转场因此会把表面重建盖住，不露白）"),
                              sysMax ? QString()
                                     : QStringLiteral("WS_MAXIMIZE 没置上 —— 几何是自己摆的，"
                                                      "中间会露出未画过的像素"));
                    }

                    /* 这一步跑完，窗口到底在哪儿、露着没露着 —— 白帧是不是"窗口不在那儿" */
                    trace << QStringLiteral("%1第%2次 跑完：窗 %3x%4@%5,%6 可见=%7 前台=%8 "
                                            "露出=%9 取样条在窗内=%10%11")
                                 .arg(dir).arg(step)
                                 .arg(host->width()).arg(host->height())
                                 .arg(host->x()).arg(host->y())
                                 .arg(host->isVisible() ? 1 : 0)
                                 .arg(host->isActiveWindow() ? 1 : 0)
                                 .arg(host->windowHandle() && host->windowHandle()->isExposed()
                                          ? 1 : 0)
                                 .arg(host->geometry().contains(strip) ? 1 : 0)
                                 .arg(nativeStyleText(host));
                }
            }
            host->removeEventFilter(&tally);
            /* 保险：回到常规状态（QML 那边的 maximized 就是 Win.maximized） */
            if (qmlRoot->property("maximized").toBool()) {
                QMetaObject::invokeMethod(qmlRoot, "toggleMaximize");
                settle();
            }

            out() << "        （最大化/还原：Move " << tally.moves << " 拍 / Resize "
                  << tally.resizes << " 拍；基准 白" << baseWhite << " 亮" << qRound(baseLum)
                  << "；抓了 " << frames << " 帧，露白 " << whiteFrames << " 帧；这一段最亮 "
                  << qRound(maxLum) << " / 最暗 " << qRound(minLum)
                  << "（基准与最暗差得越多，界面上那次\"压暗再回全亮\"就越看得出来）；"
                     "四次切换（最大化/还原/最大化/还原）："
                  << stepResult.join(QStringLiteral(" / ")) << "）" << Qt::endl;
            out() << "        （这几拍里的几何序列（去重）："
                  << tally.sequence.join(QStringLiteral(" -> ")) << "）" << Qt::endl;
            out() << "        （探针角（最大化之后那儿是橙色应用图标）：切换前 RGB "
                  << qRound(cornerR) << "," << qRound(cornerG) << "," << qRound(cornerB)
                  << "；界面扫到它用了：" << stepCover.join(QStringLiteral(" / "))
                  << "（-1 = 一直没扫到，说明探针角辨不出来））" << Qt::endl;
            for (const QString &line : trace)
                out() << "        （" << line << "）" << Qt::endl;

            if (!measurable) {
                out() << "        （窗口没在前台（基准 白" << baseWhite << " 亮"
                      << qRound(baseLum) << "），这一次量不了 —— 跳过这条检查）"
                      << Qt::endl;
            } else {
                /*
                 * 两条一起看：
                 *   * 白帧数必须是 0（露白就是用户报的"白色的背影一闪"）；
                 *   * 整段最亮的一帧也得是"暗的" —— 半透明窗口"空一帧"时透出来的
                 *     东西不一定白（可能是个深色窗口），那种漏法只看白点数是抓不到的。
                 * "没量成"的那些步不算数（窗口被别的程序压着，量到的不是它）。
                 */
                const bool skipped = stepResult.filter(QStringLiteral("没量成")).size() > 0;
                /*
                 * 两条一起看：
                 *   * 露白 0 帧（第 0.3 节那条）；最亮一帧也得是暗的；
                 *     "没量成"的那些步不算数（窗口被别的程序压着，量到的不是它）。
                 *   * 探针角那个数**采不到就不算缺陷**（-1）：切换现在只要 ~30ms，
                 *     而采一次屏幕要几十毫秒，采不到是量具的问题，不是画面的问题；
                 *     采到了就必须 <200ms。"会不会看到系统那段缩放动画"这件事
                 *     由上面那条 WS_MAXIMIZE 检查钉着（确定性判据），不靠这个数。
                 */
                const int slowestCover = [&]() {
                    int worst = -1;
                    for (const QString &s : stepCover) {
                        const int ms = s.left(s.size() - 2).toInt();
                        if (ms > worst)
                            worst = ms;
                    }
                    return worst;
                }();
                check(!skipped && whiteFrames == 0 && maxLum < 120.0
                          && (slowestCover < 0 || slowestCover < 300),
                      QStringLiteral("最大化/还原：不露白（表面重建被 DWM 那段转场盖住了）"),
                      QStringLiteral("露白 %1 帧；最亮 %2；界面铺到位用了 %3（<0 = 没采到；"
                                     "这里含了系统那段转场的时长，别拿它当性能指标）；%4")
                          .arg(whiteFrames)
                          .arg(qRound(maxLum))
                          .arg(slowestCover)
                          .arg(stepResult.join(QStringLiteral(" / "))));
            }
        }
    }

    /*
     * 便签那一节放在**最后**，而且整段在另一个文件里（src/SelfTestNotes.cpp）。
     *
     * 放最后是有意的：那一段会新建 / 删除便签窗口，也会短暂地开关便签菜单，
     * 摆在前面会给编辑区 / 截图那几节的时序添乱。单独成文件则是因为它自己也是
     * 一个入口（`--note-test`，不碰编辑区 / 截图 / 设置面板那些老毛病）。
     */
    if (notes) {
        SelfTest::runNotes(store, tray, cmd, notes);
        /* 通过 / 失败都并进来：只并失败的话，总数上会少了便签那几十项 */
        gPassed += SelfTest::notesPassed();
        gFailed += SelfTest::notesFailed();
    }

    /*
     * 翻译那一节也在最后（src/SelfTestTranslate.cpp）：它会叫出一张卡片窗口、
     * 起一个本地回环上的假模型服务，摆在前面同样会给编辑区 / 截图那几节添乱。
     * 它自己会把改过的配置写回去（见那个文件开头），自检不留痕。
     */
    if (cards && llm) {
        SelfTest::runTranslate(cards, llm, tray, speech);
        gPassed += SelfTest::translatePassed();
        gFailed += SelfTest::translateFailed();
    }

    out() << Qt::endl
          << "通过 " << gPassed << " 项，失败 " << gFailed << " 项" << Qt::endl;
    return gFailed == 0 ? 0 : 1;
}