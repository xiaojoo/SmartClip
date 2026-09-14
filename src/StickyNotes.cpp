#include "StickyNotes.h"

#include "NoteThumbs.h"
#include "StickyNoteStore.h"

#include <QCoreApplication>
#include <QCursor>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QPixmap>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QResizeEvent>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantMap>

#include <algorithm>

namespace {

/*
 * ===========================================================================
 * 便签排查日志（打桩用，命令行开关控制）
 * ===========================================================================
 *
 * 组合 / 标签条那套逻辑出问题的时候，用户界面上的现象（"点了 tab 只剩两个
 * 色块"）在进程外面看不出来是谁把谁盖住了、谁被 hide 了，只能进去打点。
 *
 * 开关：
 *   * `--notes-log`  或 环境变量 `SMARTCLIP_NOTES_LOG=1`  -> 写到默认文件；
 *   * 环境变量 `SMARTCLIP_NOTES_LOG=<路径>`  -> 写到指定文件。
 *
 * 默认文件：`%TEMP%/smartclip-notes.log`（每跑一次覆盖，便于"点几下、发这一份"）。
 *
 * 只记"组合 / 标签条"这条链路：建组、点色块、换纸、收起同组其余几块、
 * 每块窗口这一刻的可见性 / 几何 / 标签条内容。不记正文、不记别处的日志。
 */
bool notesLogEnabled() {
    static const bool on = [] {
        if (!qEnvironmentVariableIsEmpty("SMARTCLIP_NOTES_LOG"))
            return true;
        return QCoreApplication::arguments().contains(QStringLiteral("--notes-log"));
    }();
    return on;
}

void notesLog(const QString &line) {
    if (!notesLogEnabled())
        return;
    /* 每跑一次覆盖一份：用户"点几下 -> 发日志"看到的就只有这一轮 */
    static QFile *file = [] {
        QString path = qEnvironmentVariable("SMARTCLIP_NOTES_LOG");
        if (path.isEmpty() || path == QLatin1String("1"))
            path = QDir::tempPath() + QStringLiteral("/smartclip-notes.log");
        auto *f = new QFile(path);
        if (f->open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            QTextStream(f) << "smartclip 便签日志 @ " << QDateTime::currentDateTime().toString()
                           << "\n";
        return f;
    }();
    if (!file->isOpen())
        return;
    QTextStream out(file);
    out << QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")) << "  " << line
        << "\n";
    out.flush();
}

/* 一块便签这一刻的样子（日志里到处都是这一串，抽出来） */
QString noteBrief(StickyNoteWindow *window) {
    if (!window || !window->note())
        return QStringLiteral("null");
    const QString id = window->note()->id().left(4);
    const bool inGroup = !window->note()->groupId().isEmpty();
    return QStringLiteral("%1[grp=%2 vis=%3 geo=%4,%5 %6x%7 tabs=%8 strip=%9]")
        .arg(id, inGroup ? window->note()->groupId() : QStringLiteral("-"))
        .arg(window->isVisible() ? 1 : 0)
        .arg(window->x()).arg(window->y()).arg(window->width()).arg(window->height())
        .arg(int(window->groupTabs().size()))
        .arg(window->tabMargin());
}

/* 新建便签的默认尺寸（逻辑像素）—— 便签是"随手记"，不该一上来就占半屏 */
constexpr int kDefaultW = 330;
constexpr int kDefaultH = 300;
constexpr int kMinW = 180;
constexpr int kMinH = 120;

/* 便签纸能缩到多小（一键排列时屏幕装不下就往下压，但不小于这个） */
constexpr int kArrangeMinW = 220;
constexpr int kArrangeMinH = 150;

/*
 * 位置夹进屏幕 / 摆回上次位置时用的偏移。
 *
 * 名字还叫 cascade 是因为它最早是"级联摆放"那套留下的：现在只用在
 * "上次那个坐标整块落在屏幕外，得找个地方放"这一种情况上 —— 往工作区
 * 左上角内缩一点点，看着不像贴着屏幕边。
 */
constexpr int kCascadeX = 26;
constexpr int kCascadeY = 26;

/* 排列时两块便签之间的间距 */
constexpr int kArrangeGap = 14;

/*
 * 新建便签时"空位扫描"用的间距和上限（见 StickyNotes::nextFreeRect）。
 *
 * kPlaceGap 是两块便签之间留的缝（要 > 0，不然紧挨着摆看着像一整块）；
 * kPlaceTries 是一次扫描最多试几个格子 —— 4K 屏一行能放十来个、
 * 十几行就是一百多个，够用了。全被占了就退回工作区左下角。
 */
constexpr int kPlaceGap = 14;
constexpr int kPlaceTries = 240;

inline int clampInt(int value, int low, int high) {
    return value < low ? low : (value > high ? high : value);
}

/*
 * ===========================================================================
 * 一摞便签（组合）：几何全在这里算
 * ===========================================================================
 *
 * 图景（用户明确要求的样子）：
 *
 *     ┌───────────────┐
 *     │ 便签 3        │  ← 桌面上只有**这一张纸**（"露头那块"）
 *     └───────────────┘
 *   左边一排文件夹标签（一个色块一块便签），点一下换另一块上来。
 *
 * 也就是说一摞**在桌面上只占一格**：同组其余几块窗口都藏起来、位置对齐到
 * 同一格（换标签是原地换纸，见 switchGroupTab），只有左边那排标签提示
 * "这一摞里还有几张"。
 *
 * 便签本身是**各自独立的原生窗口**，所以"一摞"只是几何上的约定：谁摆哪儿
 * 由这一组函数算，没有 parent/child 关系（也不能有 —— 那会把便签变成子窗
 * 口，多屏 / 贴边吸附 / 鼠标穿透锁定全都跟着变）。
 */

/* 组里各块按哪一块的尺寸对齐（露头那块改大小时，整摞跟着它） */
constexpr int kGroupMinW = kMinW;
constexpr int kGroupMinH = kMinH;

/* 便签身上那个组 id（统一从这一处取，免得各处再写一遍判空） */
inline QString groupIdOfNote(const StickyNote *note) {
    return note ? note->groupId() : QString();
}

/*
 * 一摞里每一块摆哪儿：**都摆在同一格**（用户要的"只显示一张卡片"）。
 *
 * 早先这里是"露头 + 依次往右下错开 (8, 30)"，让下面几张各露出一条标题栏。
 * 现在一摞在桌面上只有露头那一张纸是露着的（其余几块 hide 着、随时能被换
 * 上来，见 switchGroupTab），错开没有意义 —— 留着它还会让"换标签不是原地
 * 换纸"（被点的那块得从阶梯上搬回来）。
 *
 * 偏移仍然留成一份清单（全是 0）：applyPlacements / groupGeometryFor 那套
 * 是按"每块一个偏移"写的，清单一律为 0 就是"都对齐到 origin"。
 */
QList<QPoint> cascadeOffsets(int count) {
    QList<QPoint> offsets;
    offsets.reserve(qMax(0, count));
    for (int i = 0; i < count; ++i)
        offsets.append(QPoint(0, 0));
    return offsets;
}

/*
 * 这一摞要占多大（含层叠错开的那一段）。
 *
 * 传的是**已经摆好**的几个矩形（用想要的尺寸和想要的起点试算出来的），
 * 所以它是纯几何；窗口的当前几何不参与，免得"上一帧还没搬完"影响判断。
 */
QRect groupBoundsFor(const QRect &base, const QSize &size, const QList<QPoint> &offsets) {
    QRect bounds(base.topLeft(), size);
    for (const QPoint &offset : offsets)
        bounds = bounds.united(QRect(base.topLeft() + offset, size));
    return bounds;
}

/*
 * 挑一个"整摞都露在某块屏上"的起点。
 *
 * wanted 是理想起点（原来露头那块纸的左上角）。理想起点会让整摞探出屏幕时
 * 往回收一点 —— 探出去的部分用户既点不到也看不见，而组合是**整摞**的活动，
 * 夹的时候只能一起夹。
 *
 * 试三个方向（见下面 candidates 的注释），挑第一个装得下的；都装不下就
 * 退回"只把露头那块夹进屏幕"。
 */
QPoint fitCascadeOrigin(const QPoint &wanted, const QSize &size, int count,
                        const QRect &area) {
    const QList<QPoint> offsets = cascadeOffsets(count);
    const QRect probe(wanted, size);
    if (area.contains(groupBoundsFor(probe, size, offsets)))
        return wanted;

    const QPoint last = offsets.isEmpty() ? QPoint(0, 0) : offsets.last();
    const QPoint toLeft(qMin(0, area.left() - wanted.x()), 0);
    const QPoint toTop(0, qMin(0, area.top() - wanted.y()));
    const QPoint pullIn(toLeft.x(),
                        qMin(0, area.bottom() + 1 - wanted.y() - size.height() - last.y()));

    QList<QPoint> candidates;
    /* 1) 露头那块不动、把整摞往回收（最常见：整摞从屏幕右下探出去） */
    candidates << QPoint(pullIn.x(), pullIn.y());
    /* 2) 往回收会顶出左 / 上边：那就不动，让露头那块自己贴住屏幕角 */
    candidates << QPoint(toLeft.x(), toTop.y());
    /* 3) 起点在左上角、整摞往右下探出去：把整摞往回收 */
    candidates << QPoint(qMin(0, area.right() + 1 - wanted.x() - size.width() - last.x()),
                         qMin(0, area.bottom() + 1 - wanted.y() - size.height() - last.y()));

    for (const QPoint &offset : std::as_const(candidates)) {
        const QPoint origin = wanted + offset;
        if (area.contains(groupBoundsFor(QRect(origin, size), size, offsets)))
            return origin;
    }
    /* 屏幕小到根本放不下整摞：至少把露头那块夹进屏幕里 */
    return QPoint(clampInt(wanted.x(), area.left(), qMax(area.left(), area.right() + 1 - size.width())),
                  clampInt(wanted.y(), area.top(), qMax(area.top(), area.bottom() + 1 - size.height())));
}

/*
 * 一摞便签的"目标几何"（applyGroupLayout 和拖 / 改大小时同步都用它算一次）。
 *
 * targetSize 是理想尺寸：由**基准那块**（拖动时被按住的那一块、重排时露头
 * 那块）说了算 —— 组里各块尺寸对齐到它。
 *
 * originIndex 是"哪一块落在起点上"：重排时是 0（露头那块摆在整摞的左上），
 * 拖动同步时是负的偏移补偿（被按住的那块要留在它现在的位置上，整摞围着它
 * 对齐）。
 *
 * 返回的列表按 cascadeOffsets 的顺序（也就是层叠顺序）排，元素里带着是哪
 * 一块窗口 —— 调用方不用再去对下标。
 */
struct GroupPlacement {
    StickyNoteWindow *window = nullptr;
    QRect rect;
};

struct GroupGeometry {
    QSize size;
    QList<GroupPlacement> placements;
};

GroupGeometry groupGeometryFor(const QList<StickyNoteWindow *> &members,
                               const QSize &targetSize, const QPoint &origin,
                               const QList<QPoint> &offsets, int originIndex) {
    GroupGeometry out;
    out.size = QSize(qMax(kGroupMinW, targetSize.width()), qMax(kGroupMinH, targetSize.height()));
    const QPoint base = origin - (originIndex >= 0 && originIndex < offsets.size()
                                      ? offsets.at(originIndex) : QPoint(0, 0));
    for (int i = 0; i < members.size(); ++i) {
        GroupPlacement placement;
        placement.window = members.at(i);
        placement.rect = QRect(base + (i < offsets.size() ? offsets.at(i) : QPoint(0, 0)),
                               out.size);
        out.placements.append(placement);
    }
    return out;
}

/* 把一份算好的几何落到窗口上（唯一的落地口）
 *
 * reference 是"这一摞里被用户按住的那一块"（拖动时就是它）；给 nullptr 就是
 * 重排（每一块都按算好的位置摆）。
 */
void applyPlacements(const QList<GroupPlacement> &placements, StickyNoteWindow *reference) {
    for (const GroupPlacement &placement : std::as_const(placements)) {
        StickyNoteWindow *window = placement.window;
        if (!window)
            continue;
        /*
         * 按住的那一块已经由窗口管理器摆在原位了，这里**不能**再 setGeometry
         * —— 那会和用户手上的拖动打架（一帧一回弹）。它只需要落一次盘
         * （geometry 写回 note 就是落盘的入口，见 StickyNoteStore::touch）。
         */
        if (window == reference) {
            window->rememberGeometry();
            continue;
        }
        window->setGroupGeometry(placement.rect);
    }
}

}  // namespace

/* ===========================================================================
 * StickyNoteWindow
 * ======================================================================== */

StickyNoteWindow::StickyNoteWindow(StickyNote *note, QQmlEngine *engine, StickyNotes *owner)
    : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                          | Qt::Tool),
      m_note(note),
      m_engine(engine),
      m_owner(owner) {
    /*
     * 窗口**不设 QWidget 的 parent**。
     *
     * 两个原因：StickyNotes 是 QObject 不是 QWidget，本来也当不了 QWidget 的
     * parent；而且便签窗口是顶层窗口，父子关系交给 StickyNotes 自己那份清单
     * （m_windows）+ 它的析构函数管更直白（见那里的说明）。
     *
     * 需要"总管"的地方走 m_owner 这个显式指针：开取色框、删自己都要它。
     * 以前这里靠 parent() 反查，而 parent 一直是空 —— 点"更多颜色"什么也
     * 不会发生，也不报错，控制台干干净净，极难查。
     */
    setObjectName(QStringLiteral("StickyNoteWindow"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAutoFillBackground(false);
    setMinimumSize(kMinW, kMinH);
    setWindowTitle(QStringLiteral("SmartClip 便签"));

    /*
     * 不给 WA_ShowWithoutActivating：便签是要打字的。
     *
     * 新建出来直接就能敲（焦点在这一块上），而不是"先点一下才能输入"——
     * 和截图那个贴图窗口正好相反（那个只是看，不该抢焦点）。
     */
    /*
     * QQuickWidget 必须挂**主引擎**（StickyNotes 传进来的那个）。
     *
     * 不传的话它会自己 new 一个 QQmlEngine（实测：engine=0x…080 和主引擎
     * 0x…760 不是一个），后果有两个：便签窗口的 QML import 不到
     * SmartClip.Globals 那些单例，而且 image://stickythumb/… 这个提供者是
     * 挂在主引擎上的 —— 界面上会一直报 "Invalid image provider"（缩略图
     * 永远出不来）。截图那个选区窗口（Screenshot::createOverlay）用的是
     * `new QQuickWidget(m_engine, overlay)`，同一个道理。
     */
    m_view = m_engine ? new QQuickWidget(m_engine, this) : new QQuickWidget(this);
    m_view->setResizeMode(QQuickWidget::SizeRootObjectToView);
    m_view->setClearColor(Qt::transparent);

    /*
     * 便签要的东西用 **setInitialProperties** 交给 QML，而不是
     * rootContext()->setContextProperty()。
     *
     * 为什么：上下文属性在"对象构造期间"求值的绑定里读到的是 null ——
     * QML 那边报 "TypeError: Cannot read property 'palette' of null"，因为
     * Repeater 的委托是**创建期**就求值的。setInitialProperties 是 Qt 给的
     * "创建时就初始化根对象属性"的口子（QQuickWidget::setSource 内部会用
     * completeInitialization 处理它），根对象的属性一上来就有值，绑定不用
     * 绕道、也不会留下"依赖不可绑定属性"的告警。
     */
    QVariantMap props;
    props.insert(QStringLiteral("noteData"), QVariant::fromValue<QObject *>(m_note));
    props.insert(QStringLiteral("noteWindow"), QVariant::fromValue<QObject *>(this));
    props.insert(QStringLiteral("windowNumber"), m_noteNumber);
    m_view->setInitialProperties(props);

    /* QTP0001 = NEW 之后 QML 模块的资源前缀是 /qt/qml/<URI>（和截图那边一样） */
    m_view->setSource(QUrl(QStringLiteral("qrc:/qt/qml/SmartClip/qml/notes/StickyNoteWindow.qml")));
    if (m_view->status() == QQuickWidget::Error)
        qWarning("便签界面加载失败：StickyNoteWindow.qml");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_view, 1);

    resize(kDefaultW, kDefaultH);
}

StickyNoteWindow::~StickyNoteWindow() = default;

QObject *StickyNoteWindow::qmlRoot() const {
    return m_view ? m_view->rootObject() : nullptr;
}

QString StickyNoteWindow::qmlError() const {
    if (!m_view)
        return QStringLiteral("没有 QQuickWidget");
    if (m_view->status() != QQuickWidget::Error)
        return QString();
    /*
     * QQuickWidget 把错误放在 errors() 里（status() == Error 时它非空）。
     * 只取第一条：自检那边只要"有没有错、错在哪一行"，全量输出会淹掉日志。
     */
    const QList<QQmlError> errors = m_view->errors();
    if (errors.isEmpty())
        return QStringLiteral("QML 加载失败（没有细节）");
    return errors.first().toString();
}

QRect StickyNoteWindow::screenBounds() const {
    /*
     * 便签窗口所在那块屏的可用区域。
     *
     * 「⋯」菜单要用它来摆位（贴着鼠标，还要判断"子面板放左边还是右边"）。
     * 为什么不在 QML 里读 `Window.screen.availableGeometry`：那个 Screen
     * 是从 window 附着属性上来的，在 QWidget 托着的 QQuickWidget 里拿不到
     * （实测是 null，菜单于是退回一个假想的 1920x1080，位置全乱）。
     * C++ 这边 QWidget::screen() 一直有，一行就拿到。
     */
    const QScreen *screen = QWidget::screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QRect(0, 0, 1920, 1080);
    return screen->availableGeometry();
}

QPoint StickyNoteWindow::cursorPos() const {
    /* 屏幕坐标（QCursor::pos 就是这个口径，和菜单的 anchorX/anchorY 一致） */
    return QCursor::pos();
}

QRect StickyNoteWindow::noteRect() const {
    return QRect(QWidget::x(), QWidget::y(), QWidget::width(), QWidget::height());
}

int StickyNoteWindow::linkCount() const {
    return m_note ? m_note->linkCount() : 0;
}

void StickyNoteWindow::setNoteNumber(int number) {
    m_noteNumber = number;
    /*
     * 编号是 QML 根上的一个属性（见构造函数里的 setInitialProperties），
     * 所以这里改它就行 —— 界面上的"便签 3"直接绑着它。
     */
    if (QObject *root = qmlRoot())
        root->setProperty("windowNumber", number);
}

void StickyNoteWindow::applyWindowFlags() {
    const Qt::WindowFlags flags =
        Qt::Window | Qt::FramelessWindowHint | Qt::Tool
        | (m_staysOnTop ? Qt::WindowStaysOnTopHint : Qt::WindowFlags());
    if (windowFlags() == flags)
        return;

    /*
     * 改标志位会把原生窗口拆掉重建，所以几何要自己收好再放回去 ——
     * 不收的话关掉"置顶"那一刻便签会跳回左上角（Qt 的窗口标志变更会把
     * 窗口 hide 掉再 show，位置跟着默认值走）。
     */
    const bool wasVisible = isVisible();
    const QRect geo = geometry();
    setWindowFlags(flags);
    setGeometry(geo);
    if (wasVisible)
        show();
}

void StickyNoteWindow::setStaysOnTop(bool on) {
    if (m_staysOnTop == on)
        return;
    m_staysOnTop = on;
    applyWindowFlags();
    emit staysOnTopChanged();
}

void StickyNoteWindow::setLocked(bool on) {
    if (m_locked == on)
        return;
    m_locked = on;
    /*
     * 锁定 = 这块便签**不再响应鼠标**（正文点不动、拖不动、改不了大小、标签条
     * 也点不着），只剩头部那一排按钮还能点 —— 好让用户把它解开。
     *
     * 早先这里是 `setWindowFlag(Qt::WindowTransparentForInput)`：整块窗口鼠标
     * 穿透，"点它等于点底下的窗口"，手感确实最像"贴在桌面上的一张纸"。但那是
     * **窗口级**的开关 —— 连头上那个锁定按钮也一起穿透了，于是**锁上就再也解
     * 不开**（用户报的："这个锁定是单向的，只能锁定不能解锁"）。窗口级的穿透
     * 没法只留一小块给按钮，所以改成"窗口照收事件，交互由界面按 locked 关掉"
     * （见 StickyNoteWindow.qml 里各处 `enabled: !root.locked`）：
     *   * 头部（颜色 / 锁定 / ⋯）照常能点 -> 锁定按钮自己就能解锁；
     *   * 正文 / 拖动 / 改大小 / 标签条 / 链接 全部让开。
     *
     * 代价说清楚：锁着的便签现在会**吃掉**落在它身上的点击（不再传给底下的
     * 窗口）。要"完全穿透、又能解锁"就得再开一块独立小窗专门放解锁按钮，
     * 那是另一套代价（多一块窗口要跟着便签走），先不这么做。
     */
    emit lockedChanged();
}

void StickyNoteWindow::setOpacityPercent(int percent) {
    if (m_note)
        m_note->setOpacityPercent(percent);
}

void StickyNoteWindow::toggleStaysOnTop() {
    setStaysOnTop(!m_staysOnTop);
}

/*
 * 把这一块**卡片**摆到 rect（逻辑像素），窗口按标签条的位置换算（见
 * frameRectFor）；同时把卡片的位置写回 StickyNote 的几何。
 *
 * 参数一律是卡片矩形：调用方（摆位 / 层叠 / 排列）只管"这张纸在哪儿"，
 * 标签条那一条是窗口自己的事。
 */
void StickyNoteWindow::placeAt(const QRect &rect) {
    m_placing = true;
    setGeometry(frameRectFor(rect));
    m_placing = false;
    if (m_note)
        m_note->setGeometry(cardRect());
}

/*
 * 组合窗口：把这一块摆到算好的位置（只由 StickyNotes 在同步整摞时调）。
 *
 * 和 placeAt 的差别只有一处：这里要压住 m_groupDragging —— 整摞同步途中
 * 每一块自己也会收到 moveEvent，那一下不能再反过来搬一次整摞。
 */
void StickyNoteWindow::setGroupGeometry(const QRect &rect) {
    const bool wasDragging = m_groupDragging;
    m_groupDragging = false;
    placeAt(rect);
    m_groupDragging = wasDragging;
}

/*
 * 直接挪窗口（frame）：整摞一起搬时按"窗口位移"走。
 *
 * 为什么不用 placeAt（卡片矩形）：卡片 = 窗口右移一个 tabMargin，而露头那块
 * （margin 0）和其余几块（带标签条，margin 24）宽窄不一样 —— 按"卡片位移"
 * 同步，各块的窗口位移就会差 24，整摞越拖越歪（踩过）。窗口位移是硬位移。
 */
void StickyNoteWindow::moveFrameTo(const QPoint &frameTopLeft) {
    const bool wasDragging = m_groupDragging;
    m_groupDragging = false;
    m_placing = true;
    move(frameTopLeft);
    rememberGeometry();
    m_placing = false;
    m_groupDragging = wasDragging;
}

bool StickyNoteWindow::inGroup() const {
    return m_owner && !m_owner->groupIdOf(const_cast<StickyNoteWindow *>(this)).isEmpty();
}

int StickyNoteWindow::groupSize() const {
    if (!m_owner)
        return 0;
    const QString id = m_owner->groupIdOf(const_cast<StickyNoteWindow *>(this));
    return id.isEmpty() ? 0 : m_owner->groupMembers(id).size();
}

bool StickyNoteWindow::groupActive() const {
    if (!m_owner)
        return false;
    const QString id = m_owner->groupIdOf(const_cast<StickyNoteWindow *>(this));
    return !id.isEmpty() && m_owner->activeInGroup(id) == this;
}

bool StickyNoteWindow::promoteInGroup(bool force) {
    if (!m_owner)
        return false;
    /*
     * 只有"用户真的点了这一块"才算数：这一块得是当前活动窗口。
     *
     * 便签 QML 里那个 TapHandler 是**旁听**鼠标事件的（不能盖 MouseArea，否则
     * 编辑区选字、拖动把手全完蛋），而 Qt 会把鼠标事件发给所有收到它的窗口
     * —— 别处点一下、这一块刚好在那时候露出来，也会顺带触发这里。真被点时
     * 窗口管理器一定先把它激活（isActiveWindow 为真），所以这道判断能把那种
     * 误触挡住 —— 不然那一摞会在用户没点它的时候自己换主角（踩过：标签条
     * 跑到另一块窗口上、整摞的卡片全被推歪）。
     *
     * force 只给自检用（自检没法保证谁是活动窗口），界面上永远不传。
     */
    if (!force && !isActiveWindow())
        return false;
    const QString id = m_owner->groupIdOf(this);
    if (id.isEmpty())
        return false;
    if (m_owner->activeInGroup(id) == this)
        return false;   /* 已经是最上面那块：不用重排，免得白抖一下 */
    return m_owner->applyGroupLayout(id, this);
}

/*
 * 这一块窗口左边那条标签条上画什么：**它所在那一摞（组合）里的每一块**
 * 一个色块（自己也在里面）。
 *
 * 只有**组合过**的便签才有这条标签条（用户明确要求）：没组合过的单独一块
 * 便签左边干干净净，没有色块。一条便签归到某一摞里之后，界面上只摆那一张纸
 * （露头那块），其余几块就收成这排色块，点一下换上来（见 switchGroupTab）。
 *
 * 每条 { id, color, number, selected }；颜色 / 编号都在这儿算好：QML 那边只
 * 有窗口自己，翻不到别的便签的数据。摞里不足两块就是空表（那也没什么可切）。
 */
QVariantList StickyNoteWindow::groupTabs() const {
    QVariantList out;
    if (!m_owner || !m_note)
        return out;
    const QString groupId = m_note->groupId();
    if (groupId.isEmpty())
        return out;

    QList<StickyNoteWindow *> members = m_owner->groupMembers(groupId);
    if (members.size() < 2)
        return out;

    /*
     * 色块的先后按**便签编号**（= 这一块是第几个建出来的）排，固定不动 ——
     * **不跟层叠 / 露头顺序走**。
     *
     * 早先这里是直接吃 groupMembers 的顺序（那个顺序是"露头那块排第一"）：
     * 于是点一下标签，被点的那块就蹦到最前面、整排色块跟着换位。用户看到的是
     * "点了第二个，第二个跑到第一个去了"，同一个位置连点两下换的是不同的纸，
     * 也没法按位置记住"第几个是哪一张"。换标签只该换右边那张纸，左边这排
     * 标签一个都不许动。
     */
    std::sort(members.begin(), members.end(),
              [](StickyNoteWindow *a, StickyNoteWindow *b) {
                  return (a ? a->noteNumber() : 0) < (b ? b->noteNumber() : 0);
              });

    StickyNoteWindow *front = m_owner->activeInGroup(groupId);
    for (StickyNoteWindow *member : members) {
        if (!member || !member->note())
            continue;
        QVariantMap tab;
        tab.insert(QStringLiteral("id"), member->note()->id());
        tab.insert(QStringLiteral("color"), member->note()->color().name());
        tab.insert(QStringLiteral("number"), member->noteNumber());
        tab.insert(QStringLiteral("selected"), member == front);
        out.append(tab);
    }
    return out;
}

/* 这一块在摞里、而且不是露头那张 -> 收成左边标签条上一个色块（见 tabbed） */
bool StickyNoteWindow::tabbed() const {
    if (!m_owner || !m_note || m_note->groupId().isEmpty())
        return false;
    return m_owner->activeInGroup(m_note->groupId()) != this;
}

/*
 * 标签条占多宽：一个色块 + 它和便签纸之间那条缝。
 *
 * 界面（qml/notes/StickyNoteWindow.qml）用同一个数画色块和留白 —— 只有一个
 * 来源，两边不会各说各话。
 */
int StickyNoteWindow::tabStripWidth() {
    return 34;
}

/*
 * 标签条在不在：**这一块属于某一摞（组合）**就在（见 groupTabs）。
 *
 * 它决定窗口比卡片宽多少（见 frameRectFor）—— 所以这里真要动窗口几何，不只是
 * 记个标记：让出那一条得**往左边长**，卡片（便签纸）自己待在原地不动，色块
 * 才落在纸外面、贴着桌面。
 *
 * card 是调用方**改这一条宽度之前**量到的卡片矩形：宽度一变，窗口矩形就变
 * （frameRectFor 按左边那条算），得按这个卡片位置把窗口重新摆一次 —— 不重摆
 * 的话卡片会跟着窗口往左跳一个 tabStripWidth()（用户看到的就是"一组合，纸往
 * 左蹦一下"）。
 */
void StickyNoteWindow::setTabStrip(bool on, const QRect &card) {
    const int wanted = on ? tabStripWidth() : 0;
    if (wanted == m_tabMargin)
        return;

    const QRect keep = card.isNull() ? cardRect() : card;
    m_tabMargin = wanted;
    placeAt(keep);
}

/*
 * 卡片矩形 -> 窗口矩形。
 *
 * 窗口右边 / 上边 / 下边都跟卡片对齐，只在**左边**多让出标签条那一条：
 * 卡片（便签纸）不动，窗口往左长。这样：
 *   * 便签自己的几何（note->geometry()）从头到尾就是卡片，摆位 / 层叠 /
 *     吸附 / 自检都以它为准，不受标签条影响；
 *   * 色块画在窗口左边那一条里，整块落在纸外面、底下透出桌面 —— 参考图
 *     就是那个样子（用户要的"文件夹标签"）。
 */
QRect StickyNoteWindow::frameRectFor(const QRect &card) const {
    if (m_tabMargin <= 0)
        return card;
    QRect frame = card;
    frame.setLeft(card.left() - m_tabMargin);
    return frame;
}

/*
 * 窗口矩形 -> 卡片矩形：把左边那条标签条切掉。
 *
 * 真相是**窗口几何**（拖 / 改大小都是系统直接改它，见 beginDrag / moveEvent），
 * 卡片 = 窗口右移一个 tabMargin。不从 note->geometry() 读：那个字段是"卡片
 * 位置"的落盘副本，只有在 rememberGeometry() 里才刷新，而窗口被系统搬动
 * （用户拖动、自检直接 setGeometry）时它总是慢一拍 —— 拿它当真相的话，
 * 卡片位置会连着错一次（踩过：拖动落点判定整片失灵）。
 */
QRect StickyNoteWindow::cardRect() const {
    QRect frame = geometry();
    frame.setLeft(frame.left() + m_tabMargin);
    return frame;
}

/*
 * 点左边标签条上的一个色块：把那一块便签换上来（其余几块收成色块）。
 *
 * 只有**组合过**的便签才有这条标签条（见 groupTabs），所以没归到摞里的
 * 便签点了什么也不会发生（switchGroupTab 直接返回 false）。
 */
bool StickyNoteWindow::selectGroupTab(const QString &noteId) {
    notesLog(QStringLiteral("点色块 -> %1   这一块=%2")
                 .arg(noteId.left(4), noteBrief(this)));
    if (!m_owner)
        return false;
    const bool ok = m_owner->switchGroupTab(noteId);
    notesLog(QStringLiteral("点色块 <- %1   结果=%2").arg(noteId.left(4)).arg(ok ? 1 : 0));
    return ok;
}

void StickyNoteWindow::notifyGroupChanged() {
    emit groupChanged();
}

void StickyNoteWindow::notifyTabsChanged() {
    emit tabsChanged();
}

void StickyNoteWindow::setDropPreview(bool on) {
    if (m_dropPreview == on)
        return;
    m_dropPreview = on;
    emit dropPreviewChanged();
}

/*
 * ===========================================================================
 * 一整摞一起搬
 * ===========================================================================
 *
 * 拖动走的是 windowHandle()->startSystemMove()，而窗口管理器**只认被按住的
 * 那一块窗口** —— 摞里其余几块得由我们按同一份位移跟着挪。做法是按下时记
 * 下"这一摞现在长什么样"，往后每次 moveEvent 拿"现在 - 按下时"的位移去
 * 摆其余几块（见 StickyNotes::syncGroupGeometry）。
 *
 * 记的是按下那一刻的固定基准，不是"上一次的位置"：拖动路上系统会合帧、
 * 我们这边也可能漏掉一两次事件，按"上一次"算会把差值留在那一摞里越拉越开。
 *
 * 没归到摞里的单块便签也走这一套（members 只有它自己，等于什么都不用同步）
 * —— 因为"拖到另一块身上 = 组合"那个动作需要知道"用户正按着哪一块"。
 */
void StickyNoteWindow::beginGroupDrag() {
    m_groupDragging = true;
    m_groupDragStart = pos();
    m_groupDragOffsets.clear();
    m_groupDragStartPositions.clear();
    if (!m_owner)
        return;

    const QString id = m_owner->groupIdOf(this);
    if (id.isEmpty())
        return;
    const QList<StickyNoteWindow *> members = m_owner->groupMembers(id);
    if (members.size() < 2)
        return;

    /*
     * 整摞各块的基准由 StickyNotes::beginNoteDrag 记（它按窗口位置记，
     * 和这一摞里每块的窗口坐标系一致）—— 这里只记"被按住这块"那两份。
     */
    for (StickyNoteWindow *member : members) {
        if (member)
            m_groupDragOffsets.append(member->pos() - m_groupDragStart);
    }
}

/* 按住的那一块动了多少：整摞按同一份位移走 */
void StickyNoteWindow::moveGroupBy(const QPoint &delta) {
    if (!m_owner || !m_groupDragging)
        return;
    m_owner->moveGroupByFrameDelta(this, delta);
}

/*
 * 松手了：把"用户正按着这一块"那个标记摘掉。
 *
 * 这一步不能省（见头文件里的说明）：m_groupDragging 留着的话，这一块下一次
 * 被挪动还会按"整摞同步"多算一次位移，窗口会再跳一格。
 */
void StickyNoteWindow::endDragMarker() {
    m_groupDragging = false;
    m_groupDragOffsets.clear();
    m_groupDragStartPositions.clear();
}
/* 露头那块改了大小：整摞跟着它对齐（位置也重算，免得往屏幕外长） */
void StickyNoteWindow::resizeGroupTo(const QSize &size) {
    if (!m_owner)
        return;
    m_owner->syncGroupGeometry(this, cardRect().topLeft(), size, false, true);
}

void StickyNoteWindow::rememberGeometry() {
    /*
     * 存的是**卡片**的位置和尺寸（窗口左边那条标签条不算便签的地方）——
     * 界面 / 自检 / 摆位都以卡片为准，只有窗口才是多一条的那个框。
     *
     * m_placing 那一段不能走：placeAt 摆位时窗口要**先** setGeometry（这一步
     * 会同步发来 resize / move 事件），卡片的位置是那之后才算好的 —— 这里插
     * 一脚会把"卡片 = 窗口右移一个 tabMargin"的中间值当成卡片写进 note。
     */
    if (m_placing)
        return;
    if (m_note)
        m_note->setGeometry(cardRect());
}

void StickyNoteWindow::beginDrag() {
    /*
     * 拖动交给窗口管理器（和主窗口顶栏、贴图窗口同一个理由：贴边吸附、
     * 多屏 DPI 切换都归它管）。它不支持时退回自己搬 —— 这里不实现"自己搬"
     * 是因为便签是全平台都要跑的东西，startSystemMove 在 Windows 上一直有，
     * 真没有的话用户还能用托盘里的"排列便签"。
     */
    /*
     * 按下这一刻要办两件事：
     *   1) 记下整摞的基准（见 beginGroupDrag）—— 按住的是摞里的一块就整摞搬；
     *   2) 告诉总管"用户正按着这一块"（见 StickyNotes::beginNoteDrag）——
     *      松手时如果它压在另一块身上，那就是"组合"（拖头部叠到另一块上）。
     */
    beginGroupDrag();
    if (m_owner)
        m_owner->beginNoteDrag(this);
    if (windowHandle())
        windowHandle()->startSystemMove();
}

void StickyNoteWindow::beginResize() {
    /*
     * 组合窗口：只有露头那块（完整的那张纸）的右下角是能拖的 —— 下面几块
     * 露出来的角被上面那几块盖住了，点不到；真点到了也不该让整摞按它改
     * （尺寸以露头那块为准，见 resizeEvent）。
     */
    if (windowHandle())
        windowHandle()->startSystemResize(Qt::BottomEdge | Qt::RightEdge);
}

void StickyNoteWindow::closeNote() {
    /* 藏起来（不是删）：数据还在，托盘"显示全部便签"能再叫出来 */
    hide();
    emit closed();
}

void StickyNoteWindow::deleteNote() {
    /*
     * 真删由 StickyNotes 做（清单在它手里）。
     *
     * 用 QTimer::singleShot(0) 而不是当场调：这一下是从 QML（Shift+点 ✕）
     * 进来的，栈上还压着这个窗口的 QML 求值 —— 当场把窗口 delete 掉就拆了
     * 正在求值的那棵树。推到下一轮事件再走更干净。
     */
    if (!m_owner || !m_note)
        return;
    StickyNotes *manager = m_owner;
    StickyNote *note = m_note;
    QTimer::singleShot(0, manager, [manager, note]() { manager->deleteNote(note); });
}

void StickyNoteWindow::setNoteColor(const QString &color) {
    const QColor value(color);
    if (m_note && value.isValid())
        m_note->setColor(value);
}

void StickyNoteWindow::raisePopupWindow(const QString &objectName) {
    QObject *root = qmlRoot();
    if (!root || objectName.isEmpty())
        return;
    QObject *popup = root->findChild<QObject *>(objectName);
    if (!popup)
        return;

    /*
     * popupItem 是 Popup 自己那一项"内容容器"：`Popup.Window` 时它住在**弹窗那块
     * 独立 QQuickWindow** 里，所以 item->window() 拿到的正是那块原生窗。
     *
     * 退一步用 contentItem：它也在弹窗窗口里（老版本 Qt 上 popupItem 这个属性
     * 名不一定在），两条路哪条先拿得到就用哪条。
     */
    auto popupItemWindow = [popup]() -> QQuickWindow * {
        auto *item = popup->property("popupItem").value<QQuickItem *>();
        if (!item)
            item = popup->property("contentItem").value<QQuickItem *>();
        return item ? item->window() : nullptr;
    };

    QQuickWindow *popupWindow = popupItemWindow();
    if (!popupWindow)
        return;

    /*
     * 顶到置顶带最上面（QWindow::raise 在 Windows 上就是
     * SetWindowPos(HWND_TOP, SWP_NOACTIVATE)：只在置顶带里往上挪一格，不抢激活，
     * 便签该是激活的还是激活的）。
     */
    popupWindow->raise();

    /*
     * 再补一拍：窗口刚 show 出来的一瞬间排序偶尔会被系统重排回去，下一轮事件
     * 循环再顶一次就稳了（和 NoteMenu 的 raiseLater 同一个做法）。
     */
    QPointer<QQuickWindow> guard(popupWindow);
    QTimer::singleShot(0, popupWindow, [guard]() {
        if (guard)
            guard->raise();
    });
}

void StickyNoteWindow::copyText() {
    if (m_note)
        m_note->copyTextToClipboard();
}

bool StickyNoteWindow::openLink(int index) {
    return m_note ? m_note->openLink(index) : false;
}

void StickyNoteWindow::typeText(const QString &text) {
    /*
     * 走 QML 那个 TextEdit 写：它的 onTextChanged 会把内容回写到 note
     * （和用户敲字完全同一条路），所以这里只需要改 TextEdit 自己的 text。
     * 找不到那个对象（QML 没建起来）就直接落到数据上，至少数据是对的。
     */
    if (QObject *root = qmlRoot()) {
        if (QObject *edit = root->findChild<QObject *>(QStringLiteral("noteEditor"))) {
            edit->setProperty("text", text);
            return;
        }
    }
    if (m_note)
        m_note->setText(text);
}

void StickyNoteWindow::moveEvent(QMoveEvent *event) {
    QWidget::moveEvent(event);
    if (!m_placing)
        rememberGeometry();
    /*
     * 组合窗口：正在拖这一摞，整摞跟着走。
     *
     * 注意这里只跟 m_groupDragging（"用户正按着这一块"），**不跟** isMoving()
     * 之类的窗口状态：整摞同步时其余几块也会移动，那些事件不能再反过来搬
     * 一次整摞（setGroupGeometry 会把它们的 m_groupDragging 压掉）。
     */
    if (m_groupDragging)
        moveGroupBy(pos() - m_groupDragStart);
}

void StickyNoteWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (!m_placing)
        rememberGeometry();
    /* 便签大小变了，链接卡片那一栏的列数会跟着变 —— 界面自己绑，这里不管 */
    /*
     * 一摞便签：整摞按露头那块的尺寸对齐。
     *
     * 只让**最上面那一块**说了算：下面的纸露出来的只有标题栏，用户拖不到它
     * 的右下角；真拖到了（比如刚好露着一角）也不该让整摞跟着它走 —— 那样
     * 两块的尺寸会互相打架。
     *
     * 传的是**卡片**尺寸（窗口还含着左边那条标签条，见 cardRect）。
     */
    if (m_owner && m_owner->activeInGroup(m_owner->groupIdOf(this)) == this)
        resizeGroupTo(cardRect().size());
}

void StickyNoteWindow::closeEvent(QCloseEvent *event) {
    /*
     * 关窗口 = 藏起来（数据留着）。真正"删一条便签"走 deleteNote()，
     * 那条路上窗口是被 deleteLater 收掉的，不会走到这里。
     */
    event->ignore();
    closeNote();
}

/* ===========================================================================
 * StickyNotes
 * ======================================================================== */

StickyNotes::StickyNotes(QQmlEngine *engine, QObject *parent)
    : QObject(parent), m_engine(engine) {
    m_thumbs = new NoteThumbs(this);

    /*
     * 缩略图的取图口挂在主引擎上（和截图那边 image://shot/… 一样）：
     * 便签窗口的 QQuickWidget 用的是**同一个引擎**（见 main.cpp 把它传进来），
     * 所以这里登记一次，所有便签都能用 image://stickythumb/…。
     *
     * 引擎可能在构造时还没有（main.cpp 里便签比 QQuickWidget 先构造，见那里的
     * 声明顺序说明）—— 那种情况下由 attachEngine() 补上。
     */
    if (m_engine)
        attachEngine(m_engine);

    m_store = new StickyNoteStore(this);
    /* 必须在 load() 之前：从文件恢复的便签要靠它抽链接 / 取缩略图 */
    m_store->setThumbs(m_thumbs);

    /*
     * 拖动期间那一拍（见 m_dragWindow 的说明）。
     *
     * 拖动本身是窗口管理器在做（startSystemMove），Qt 这边**收不到松手事件**，
     * 所以只能按固定间隔问两件事：光标压在哪块便签身上（落点候选，界面上那一
     * 块的头部会亮）、鼠标键还按着没（松了就落地 —— 压在别的便签身上就是
     * "组合"）。
     *
     * 间隔取 60ms：再密也就是白问几次（光标位置是现成的），再疏落点的反馈就
     * 跟不上手了。定时器平时不跑（beginNoteDrag 才 start）。
     */
    m_dragPollTimer = new QTimer(this);
    m_dragPollTimer->setInterval(60);
    connect(m_dragPollTimer, &QTimer::timeout, this, [this]() {
        updateDropTarget();
        /*
         * 松手 = 鼠标键松开了，而且**光标已经停在原地两拍以上**。
         *
         * 为什么不只看鼠标键：这一拍从 beginNoteDrag 起就开始跑了，而"按着头部
         * 按下"那一下之后鼠标键的状态在这一刻未必读得到（合成事件、程序调
         * beginDrag 都这样）—— 只看键的话第一拍就会把这次拖动当成"没按着"收掉，
         * 落点提示根本来不及亮（自检里就是这么红的）。两拍不动 = 用户停手了。
         */
        if (!m_dragWindow)
            return;
        const QPoint cursor = QCursor::pos();
        if (cursor == m_dragLastCursor) {
            ++m_dragStillTicks;
            if (m_dragStillTicks >= 2 && !(QGuiApplication::mouseButtons() & Qt::LeftButton))
                finishNoteDrag();
            return;
        }
        m_dragLastCursor = cursor;
        m_dragStillTicks = 0;
    });
}

void StickyNotes::attachEngine(QQmlEngine *engine) {
    if (!engine || !m_thumbs)
        return;
    m_engine = engine;
    /*
     * addImageProvider 对同一个名字是**替换**语义，调两次不会报错也不会留两份，
     * 所以这里不必额外记"挂过没"。
     */
    m_engine->addImageProvider(QStringLiteral("stickythumb"), m_thumbs->imageProvider());
}

StickyNotes::~StickyNotes() {
    /*
     * 便签窗口要在**这个对象剩下那些东西之前**真的没掉：它们的 QML 里绑着
     * note（StickyNote，属于 m_store），而 m_store 是 this 的子对象；
     * QML 那棵树还活着时它的上下文属性被拆掉，退出时会踩到已经析构的对象上。
     *
     * 用 delete 而不是 deleteLater：延迟删除要等事件循环，而本对象一析构、
     * 循环也就快结束了，那些窗口根本轮不到被删。
     */
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window)
            delete window.data();
    }
    m_windows.clear();
}

void StickyNotes::shutdown() {
    /*
     * 进程退出前的那一次收尾（main.cpp 在返回前调，见那里的说明）。
     *
     * 必须先掐网络再落盘：缩略图的那些 QNetworkReply 留到 QApplication 拆完
     * 再收尾的话，Qt 网络层会在退出那一刻踩到已经拆掉的内部状态 ——
     * 症状是自检全过、退出码却是 0xC0000005（和有没有便签窗口无关，
     * 只要便签子系统的网络管理器建过就会复现）。
     */
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window)
            delete window.data();
    }
    m_windows.clear();

    if (m_thumbs)
        m_thumbs->shutdown();
    if (m_store)
        m_store->flush();
}

void StickyNotes::start() {
    m_store->load();
    m_nextNumber = 0;

    QStringList groups;
    const QList<StickyNote *> notes = m_store->notes();
    for (StickyNote *note : notes) {
        if (!note)
            continue;
        ++m_nextNumber;
        StickyNoteWindow *window = ensureWindow(note);
        window->setNoteNumber(m_nextNumber);

        /* 把上次摆的位置还回去（没摆过的留给 show 那一步找空位），见 placeWindow */
        placeWindow(window, note);

        if (note->visible())
            window->show();

        /* 记下这一份文件里出现过的组合，等窗口都露出来之后再摆那一摞 */
        if (!note->groupId().isEmpty() && !groups.contains(note->groupId()))
            groups.append(note->groupId());
    }

    /*
     * 从文件恢复组合。
     *
     * 顺序很重要：必须**等所有窗口都建好、该 show 的都 show 了**再摆每一摞
     * —— applyGroupLayout 只摆"露着的那几块"，早一步摆的话后面的窗口还没 show，
     * 位置会少算几块（那一摞就散了）。
     *
     * "哪一块在最上面"从 notes.json 的 groups 那一段读回来（见
     * StickyNoteStore::groupActiveId）：用户上次点过标签换过纸，这里得摆回
     * 他离开时的样子，不能永远拿文件里第一条当露头那块。
     *
     * 摆完还要**把同组其余几块收起来**（showOnlyInGroup）：恢复出来的那几块
     * 这会儿都露着、位置还叠在同一格，不收的话桌面上就是几张纸糊在一起，
     * 也谈不上"一摞只显示一张"（见 switchGroupTab 的说明）。
     */
    for (const QString &groupId : std::as_const(groups)) {
        StickyNoteWindow *front =
            qobject_cast<StickyNoteWindow *>(windowForId(m_store->groupActiveId(groupId)));
        if (!applyGroupLayout(groupId, front))
            continue;
        StickyNoteWindow *shown = front ? front : activeInGroup(groupId);
        showOnlyInGroup(groupId, shown);
    }

    /* 新建时的编号接着已有的往下数 */
    m_nextNumber = int(notes.size()) + 1;
    /*
     * 从文件恢复的那一摞：标签条（左边那条）该在的让它长出来 —— 用组合
     * 恢复完之后统一量一次（见 refreshTabs），和运行时那条路是同一个口子。
     */
    refreshTabs();
    emit changed();
}

QString StickyNotes::notesFilePath() const {
    return m_store->filePath();
}

int StickyNotes::lockedCount() const {
    int count = 0;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window && window->locked())
            ++count;
    }
    return count;
}

bool StickyNotes::unlockAll() {
    bool any = false;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window && window->locked()) {
            window->setLocked(false);
            any = true;
        }
    }
    if (any)
        emit changed();
    return any;
}

int StickyNotes::count() const {
    return m_store->count();
}

int StickyNotes::visibleCount() const {
    int visible = 0;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window && window->isVisible())
            ++visible;
    }
    return visible;
}

StickyNoteWindow *StickyNotes::ensureWindow(StickyNote *note) {
    if (StickyNoteWindow *existing = windowFor(note))
        return existing;

    auto *window = new StickyNoteWindow(note, m_engine, this);    connect(window, &StickyNoteWindow::closed, this, [this, window]() { onWindowClosed(window); });
    connect(window, &StickyNoteWindow::staysOnTopChanged, this, &StickyNotes::changed);
    /*
     * 换底色 -> 标签条上的色块跟着变。
     *
     * 色块的颜色是**每一块便签自己的底色**（见 StickyNoteWindow::groupTabs），
     * 而那份清单是 QML 绑在一个属性上的：不通知一声的话，用户改了 A 的底色，
     * B 窗口左边那个色块还挂着旧色（踩着过：自检里量到一整排"便签黄"，其实
     * 那几块早改了色）。所以改色也走 refreshTabs —— 和"新建 / 删除一块"同一条
     * 路，规则只有一处。
     */
    connect(note, &StickyNote::colorChanged, this, &StickyNotes::refreshTabs);
    m_windows.append(window);
    return window;
}

StickyNoteWindow *StickyNotes::windowFor(StickyNote *note) const {
    for (const QPointer<StickyNoteWindow> &window : m_windows) {
        if (window && window->note() == note)
            return window;
    }
    return nullptr;
}

void StickyNotes::onWindowClosed(StickyNoteWindow *window) {
    const QString groupId = groupIdOf(window);
    if (window && window->note()) {
        window->note()->setVisible(false);
        /* 存的是**卡片**（标签条那一条不算便签的地方，见 cardRect） */
        window->note()->setGeometry(window->cardRect());
    }
    /*
     * 组合窗口：收起来的那块如果正露着头，整摞得换一块顶上（不然那一摞的
     * "最上面那张纸"是块藏起来的窗口，剩下的几个只剩标题栏、点不着内容）。
     * 不在组里的话这一句什么都不做（绝大多数便签都是这种）。
     */
    if (!groupId.isEmpty())
        repairGroup(groupId);
    m_store->scheduleSave();
    emit changed();
}

QRect StickyNotes::workAreaFor(const QRect &hint) const {
    /*
     * 便签现在落在哪块屏上就按哪块屏的工作区算（多屏时不要把它摆到另一块
     * 屏上去）。hint 是便签当前的几何；拿不到屏幕就退回主屏。
     */
    QScreen *screen = hint.isNull() ? nullptr : QGuiApplication::screenAt(hint.center());
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return QRect(0, 0, 1280, 800);
    return screen->availableGeometry();
}

QRect StickyNotes::nextFreeRect(const QSize &size) const {
    /*
     * 给新便签找一块**没被占用**的桌面：从屏幕**右上角**开始，往左排，排满
     * 一行换下一行。
     *
     * 为什么从右上角起：右上角是桌面上最不容易挡事的地方 —— 图标在左边、
     * 任务栏在下边，弹出来的便签摆在那儿看着最自然（用户提的"默认显示在
     * 右上角"）。第二块往左挨着摆，所以连着开几块是"从右上角往左铺开"，
     * 和阅读顺序相反但符合"新东西从老地方长出来"的直觉，也不会一上来就
     * 盖住桌面左边那些图标。
     *
     * 步长必须**大于便签本身**：第一版写的是"每次斜着挪 26px"，12 个候选
     * 位置全落进第一块便签那 330x300 的矩形里（实测 step 0..11 全部 clash），
     * 于是每次都掉到兜底位置、连着开三块叠在一起。
     */
    QRect area;
    for (const QPointer<StickyNoteWindow> &window : m_windows) {
        if (window && window->isVisible()) {
            area = workAreaFor(window->geometry());
            break;
        }
    }
    if (area.isNull()) {
        if (QScreen *screen = QGuiApplication::primaryScreen())
            area = screen->availableGeometry();
        else
            area = QRect(0, 0, 1280, 800);
    }

    /*
     * 已经被占着的地方。
     *
     * 组合窗口按**整摞的包围盒**算一格，不是按组里每块便签各算一格：新便签
     * 要避开的是"那一摞占着的那片地方"，落进层叠的缝里看着就像被塞进那一摞
     * 中间了。包围盒拿每块便签的几何现算（那一摞实际摆出来就是这几个矩形）。
     */
    QList<QRect> taken;
    QStringList groupSeen;
    for (const QPointer<StickyNoteWindow> &entry : std::as_const(m_windows)) {
        StickyNoteWindow *window = entry.data();
        if (!window || !window->isVisible())
            continue;
        const QString groupId = groupIdOf(window);
        if (groupId.isEmpty()) {
            taken.append(window->cardRect());
            continue;
        }
        if (groupSeen.contains(groupId))
            continue;
        groupSeen.append(groupId);
        QRect bounds;
        for (StickyNoteWindow *member : groupMembers(groupId)) {
            if (member && member->isVisible()) {
                const QRect card = member->cardRect();
                bounds = bounds.isNull() ? card : bounds.united(card);
            }
        }
        if (!bounds.isNull())
            taken.append(bounds);
    }

    const int stepX = qMax(120, size.width() + kPlaceGap);
    const int stepY = qMax(90, size.height() + kPlaceGap);
    const int columns = qMax(1, (area.width() - kPlaceGap) / stepX);
    /*
     * 一次最多试几个格子。
     *
     * 名字别叫 slots —— Windows 的头文件里 slots 是个空宏（MFC 那套留下的），
     * 撞上它会报一串"const int: 在 = 前没有声明变量"这种看不懂的错。
     */
    const int cellCount = qMin(kPlaceTries,
                               columns * qMax(1, (area.height() - kPlaceGap) / stepY));

    for (int i = 0; i < cellCount; ++i) {
        const int column = i % columns;
        const int row = i / columns;
        /*
         * 第 0 列贴着工作区右沿，之后逐列往左退一个步长。
         *
         * 用 area.right() 反推而不是 area.x() + area.width() 那些算法：
         * QRect::right() 是**闭区间**的右端点（right = x + width - 1），
         * 减掉宽度之后正好是"整块便签刚好贴在右沿上"的左边坐标。
         */
        const int x = area.right() + 1 - size.width() - column * stepX;
        const int y = area.y() + row * stepY;
        /* 最后一行别探出工作区下沿 */
        const int clampedY = qMin(y, qMax(area.y(), area.bottom() - size.height() + 1));
        const QRect candidate(qMax(area.x(), x), clampedY, size.width(), size.height());

        bool clash = false;
        for (const QRect &rect : std::as_const(taken)) {
            if (rect.intersects(candidate)) {
                clash = true;
                break;
            }
        }
        if (!clash)
            return candidate;
    }

    /* 全被占了：摆在工作区左下角，至少是个确定的、看得见的位置 */
    return QRect(area.x(), qMax(area.y(), area.bottom() - size.height()), size.width(),
                 size.height());
}

void StickyNotes::placeWindow(StickyNoteWindow *window, StickyNote *note) {
    if (!window || !note)
        return;

    if (note->hasGeometry()) {
        /*
         * 摆过：把上次的位置还回去（见 start()）。
         *
         * 位置要夹进某块屏的工作区里：屏幕拔掉 / 分辨率改了之后，上次那个
         * 坐标可能整块落在屏幕外 —— 那样便签就成了"任务栏里看得见、桌面上
         * 找不到"。夹的时候按它现在落点所在的屏算（找不到就用主屏）。
         */
        QRect rect = note->geometry();
        const QRect area = workAreaFor(rect);
        if (!area.intersects(rect)) {
            rect.moveTo(area.x() + kCascadeX, area.y() + kCascadeY);
        } else {
            rect.moveLeft(qBound(area.left(), rect.left(), qMax(area.left(),
                                                                area.right() - rect.width() + 1)));
            rect.moveTop(qBound(area.top(), rect.top(), qMax(area.top(),
                                                            area.bottom() - rect.height() + 1)));
        }
        window->placeAt(rect);
        return;
    }

    /* 没摆过：找一块空桌面 */
    const QSize size(kDefaultW, kDefaultH);
    window->placeAt(QRect(nextFreeRect(size).topLeft(), size));
}

StickyNote *StickyNotes::createNote() {
    StickyNote *note = m_store->create();
    StickyNoteWindow *window = ensureWindow(note);
    window->setNoteNumber(m_nextNumber++);

    placeWindow(window, note);
    note->setVisible(true);
    window->show();
    window->raise();
    window->activateWindow();

    m_store->scheduleSave();
    emit changed();
    return note;
}

QStringList StickyNotes::palette() const {
    return StickyNote::palette();
}

bool StickyNotes::hideOthers(StickyNoteWindow *keep) {
    /* 先标"不摆着了"（keep 那一摞除外），再关窗口 —— 顺序的理由见 markHiddenExcept */
    const int marked = markHiddenExcept(keep);
    bool any = false;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (!window || window == keep || !window->isVisible())
            continue;
        window->closeNote();
        any = true;
    }
    if (any || marked > 0) {
        m_store->scheduleSave();
        emit changed();
    }
    return any || marked > 0;
}

bool StickyNotes::revealLink(StickyNote *note, int index) {
    StickyNoteWindow *window = windowFor(note);
    if (!window)
        return false;
    QObject *root = window->qmlRoot();
    if (!root)
        return false;
    /*
     * 定位那一步在 QML 里（方法名 revealLink）：便签的正文是 QML 的 TextEdit，
     * "第几行第几列 -> 字符下标"只有它知道。C++ 这边只转发。
     */
    QVariant ok;
    QMetaObject::invokeMethod(root, "revealLink", Q_RETURN_ARG(QVariant, ok),
                              Q_ARG(QVariant, QVariant(index)));
    return ok.toBool();
}

void StickyNotes::deleteNote(StickyNote *note) {
    if (!note)
        return;

    /* 组合窗口：它属于哪个组先记下来，删完要收拾那一摞（见这一节末尾） */
    const QString groupId = note->groupId();

    StickyNoteWindow *window = windowFor(note);
    if (window)
        m_windows.removeAll(window);
    /*
     * 先把它从清单里摘掉（落盘时会少这一条），再删窗口。
     *
     * 窗口握着一个裸指针指向这条便签，所以它必须死在便签前面 ——
     * StickyNote 是 m_store 的子对象，这里 remove() 之后它只是 deleteLater，
     * 还活着，顺序上没问题。
     */
    m_store->remove(note);

    if (window) {
        /*
         * 当场 delete，不走 deleteLater。
         *
         * deleteLater 只是排一个事件，得等事件循环转回去才真的析构 ——
         * 自检是"一口气跑完再退出"的，中间那次 processEvents 不保证已经
         * 处理掉延迟删除，于是"删了但窗口还挂在屏幕上"（实测：后面那节
         * 数可见顶层窗口时数出多余的便签）。
         *
         * 同步删是安全的：进来的是界面上的删除动作（或自检直接调），而
         * 界面那条路已经由 StickyNoteWindow::deleteNote 用
         * QTimer::singleShot(0) 推到下一轮了，不会在 QML 求值中间拆窗口。
         */
        window->hide();
        delete window;
    }

    /*
     * 组合窗口：它从那一摞里没了，剩下几块要收拾 —— 露头那块被删就换一块顶上
     * 并重排，只剩一块了就直接散伙（见 repairGroup）。不在组里就什么都不做。
     */
    if (!groupId.isEmpty())
        repairGroup(groupId);

    emit changed();
    emit arrangementChanged();
}

void StickyNotes::showAll() {
    bool any = false;
    for (StickyNote *note : m_store->notes()) {
        if (!note)
            continue;
        StickyNoteWindow *window = ensureWindow(note);
        /* 位置按记录恢复（没摆过的找空位），见 placeWindow */
        placeWindow(window, note);
        note->setVisible(true);
        if (!window->isVisible()) {
            window->show();
            window->raise();
        }
        any = true;
    }
    if (any) {
        /*
         * 组合的那几摞得**按摞恢复**，不能就这儿 show 完算数。
         *
         * 一摞在桌面上只摆一张纸（其余几块收成标签条上的色块，见
         * switchGroupTab）。上面那一圈把每一块都 show 了出来 —— 同组那几块
         * 正好叠在同一格上，而**后 show 出来的那块压在最上面**；它刚从"收起来
         * 的那张纸"状态出来（content 不画），屏幕上就是一张**空白卡片**（用户
         * 在"拆分组合"那一段里截的就是这个样子）。start() 从文件恢复时走的是
         * applyGroupLayout + showOnlyInGroup 这一套，这里必须走同一套。
         */
        QStringList groups;
        for (StickyNote *note : m_store->notes()) {
            if (note && !note->groupId().isEmpty() && !groups.contains(note->groupId()))
                groups.append(note->groupId());
        }
        for (const QString &groupId : std::as_const(groups)) {
            StickyNoteWindow *front = activeInGroup(groupId);
            if (!applyGroupLayout(groupId, front))
                continue;
            StickyNoteWindow *shown = front ? front : activeInGroup(groupId);
            showOnlyInGroup(groupId, shown);
        }
        refreshTabs();
        m_store->scheduleSave();
        emit changed();
    }
}

int StickyNotes::markHiddenExcept(StickyNoteWindow *keep) {
    const QString keepGroup = groupIdOf(keep);
    int changed = 0;
    for (StickyNote *note : m_store->notes()) {
        if (!note || !note->visible())
            continue;
        if (keep && note == keep->note())
            continue;
        if (!keepGroup.isEmpty() && note->groupId() == keepGroup)
            continue;
        note->setVisible(false);
        ++changed;
    }
    return changed;
}

void StickyNotes::hideAll() {
    /*
     * 先把"这次要收的那几块"标成不摆着了，再关窗口 —— 顺序的理由见
     * markHiddenExcept：反过来的话，收掉露头那块之后兜底会把同组那张收起来的
     * 纸又 show 出来，一摞根本收不干净（实测：桌面上两摞，收完还剩 2 块摆着）。
     */
    const int marked = markHiddenExcept(nullptr);
    bool any = false;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window && window->isVisible()) {
            window->closeNote();
            any = true;
        }
    }
    if (any || marked > 0) {
        m_store->scheduleSave();
        emit changed();
    }
}

void StickyNotes::toggleShowAll() {
    if (visibleCount() > 0)
        hideAll();
    else
        showAll();
}

/* ===========================================================================
 * 组合（归到一摞里）与 排列成摞（摆法）
 * ======================================================================== */

QString StickyNotes::groupIdOf(StickyNoteWindow *window) const {
    return window && window->note() ? window->note()->groupId() : QString();
}

int StickyNotes::noteNumberFor(const QString &noteId) const {
    if (noteId.isEmpty())
        return 0;
    int number = 0;
    for (StickyNote *note : m_store->notes()) {
        ++number;
        if (note && note->id() == noteId)
            return number;
    }
    return 0;
}

QList<StickyNote *> StickyNotes::noteList() const {
    return m_store->notes();
}

QVariantList StickyNotes::groupMatesFor(const QString &noteId) const {
    QVariantList out;
    if (noteId.isEmpty())
        return out;

    StickyNoteWindow *self = qobject_cast<StickyNoteWindow *>(windowForId(noteId));
    const QString selfGroup = self && self->note() ? self->note()->groupId() : QString();

    int number = 0;
    for (StickyNote *note : m_store->notes()) {
        ++number;
        if (!note || note->id() == noteId)
            continue;
        StickyNoteWindow *window = windowFor(note);
        if (!window || !window->isVisible())
            continue;
        /*
         * 已经在这一摞里的不列（选了什么也不会发生）；**别的摞里的要列** ——
         * 选它就是"两摞并一摞"（见 groupWith）。
         */
        if (!selfGroup.isEmpty() && note->groupId() == selfGroup)
            continue;

        QVariantMap entry;
        entry.insert(QStringLiteral("id"), note->id());
        entry.insert(QStringLiteral("label"),
                     QStringLiteral("与「便签 %1」组合").arg(number));
        out.append(entry);
    }
    return out;
}

QList<StickyNoteWindow *> StickyNotes::windowsForIds(const QStringList &ids) const {
    QList<StickyNoteWindow *> out;
    for (const QString &id : ids) {
        if (auto *window = qobject_cast<StickyNoteWindow *>(windowForId(id)))
            out.append(window);
    }
    return out;
}

QList<StickyNoteWindow *> StickyNotes::groupMembers(const QString &groupId) const {
    return windowsForIds(groupMemberIds(groupId));
}

/*
 * 组里的便签 id，顺序 = 层叠顺序（第一个在最上面）。
 *
 * **谁是这一摞的成员看数据**（每条便签自己那个 groupId），缓存只负责"先后"。
 *
 * 这两件事原来都由缓存一个人说了算（缓存里那几块都还在这个组里就整个用它、
 * 不齐才回去翻数据），于是缓存一旦短了，短掉的那块就凭空从这一摞里消失：
 * 左边少一个色块还算轻的，重的是下一次"合进来"的名单也从这儿取（见
 * groupWith / dropNoteOn 都用 groupMembers），那一块会被彻底挤出去 —— 既露不
 * 出来也点不到，组 id 却还留在它身上（重启之后它变成桌上一块单独的便签）。
 * 用户报的"组合四个之后只显示三个 tab，再点一下就变成第三个了"就是这个。
 *
 * 缓存短了这件事以前是**必然**发生的：applyGroupLayout 按"露着的几块"写回成
 * 员清单，而一摞里收起来的那几块恰恰是不露着的（那里现在改成写全了）。这里
 * 再兜一层：不管缓存多短、多旧，只要便签自己还认这个组，它就还在这一摞里。
 *
 * 顺序：记下来的层叠顺序打头（第一个在最上面），缓存里没有的（刚加进来的、
 * 缓存漏掉的）按 store 里的先后接在后面 —— 顺序只影响"还没点过标签时谁露
 * 头"，少排一个不影响它算不算成员。
 */
QStringList StickyNotes::groupMemberIds(const QString &groupId) const {
    QStringList out;
    if (groupId.isEmpty())
        return out;

    /* 数据里认这个组、而且有窗口的那几条（窗口还没建出来的不算界面上的成员） */
    QStringList inGroup;
    for (StickyNote *note : m_store->notes()) {
        if (!note || note->groupId() != groupId)
            continue;
        if (windowFor(note))
            inGroup.append(note->id());
    }

    const auto cached = m_groups.constFind(groupId);
    if (cached != m_groups.constEnd()) {
        for (const QString &id : cached->memberIds) {
            if (inGroup.contains(id) && !out.contains(id))
                out.append(id);
        }
    }
    for (const QString &id : std::as_const(inGroup)) {
        if (!out.contains(id))
            out.append(id);
    }
    return out;
}

/*
 * 组里"露出来的那张纸"。
 *
 * 判据是**记下来的那一块**（notes.json 的 groups.active，见
 * StickyNoteStore::groupActiveId；applyGroupLayout / switchGroupTab 每次都把它
 * 写对），找不到再退回"顺序里第一块还愿意摆着的"。
 *
 * 这里**不能**按 isVisible() 判（原来就是那么写的："层叠顺序里第一块可见的"）：
 * 换标签的时候，新露头那块是"先 show() 出来、按新状态画好一帧，旧的那块才
 * hide()"的（见 switchGroupTab）—— 这中间两块都露着，"第一块可见的"会指回
 * **旧**的那块，新那块就还被当成收起来的那张纸（纸和色块都不画），屏幕上先
 * 空一拍。用户看到的"点一下标签，便签闪一下"就是这么来的。
 *
 * 判据用 note->visible()（用户意图："这一块还摆着"）而不是 isVisible()（窗口
 * 这会儿画没画）：换标签途中被临时 hide() 掉的那几块，note->visible() 仍然是
 * true（见 switchGroupTab 的说明）；真被用户"收起这块便签"的才是 false
 * （见 onWindowClosed）。
 */
StickyNoteWindow *StickyNotes::activeInGroup(const QString &groupId) const {
    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    if (members.isEmpty())
        return nullptr;

    const QString activeId = m_store ? m_store->groupActiveId(groupId) : QString();
    StickyNoteWindow *recorded = nullptr;
    if (!activeId.isEmpty()) {
        for (StickyNoteWindow *member : members) {
            if (member && member->note() && member->note()->id() == activeId) {
                recorded = member;
                break;
            }
        }
    }
    /* 记下来的那块还在、而且还要摆着 —— 就是它露头 */
    if (recorded && recorded->note() && recorded->note()->visible())
        return recorded;

    /*
     * 没记过（刚从文件恢复那一瞬间）、或者记下来的那块被用户收起来了：
     * 顺序里第一块还愿意摆着的顶上 —— 用户看到的"最上面那张完整的纸"必须
     * 真的是要摆着的那一块，不然那一摞只剩标题栏、点不进正文。
     */
    for (StickyNoteWindow *member : members) {
        if (member && member->note() && member->note()->visible())
            return member;
    }
    /* 这一摞一块都不打算摆着：按层叠顺序交差，界面据此不画内容 */
    return recorded ? recorded : members.first();
}

/*
 * 把一摞重排一遍：露头的 front 摆在"这一摞该在的那一格"（原位置或 anchorAt），
 * 同组其余几块**对齐到同一格**。
 *
 * 组合的几何只有这一个地方算 —— 建组、换标签、删掉一块之后收拾残局、从文件
 * 恢复，走的都是这里。
 *
 * 为什么不再"层层错开"：用户明确要求"组合的卡片只显示一张卡片，这一张卡片
 * 切换" —— 一摞在桌面上就是**一张纸 + 左边那排标签**，其余几块窗口收起来
 * （见 switchGroupTab），错开多少根本看不见。都摆在同一格还顺带保证"换标签
 * 是原地换纸"：被点的那一块本来就已经在这个位置上了。
 */
bool StickyNotes::applyGroupLayout(const QString &groupId, StickyNoteWindow *active,
                                   const QPoint *anchorAt) {
    if (groupId.isEmpty())
        return false;

    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    if (members.size() < 2)
        return false;

    /*
     * 只摆"摆着的那几块"：藏起来的那几块不动它（下次被叫出来时 placeWindow
     * 会把老坐标还回去，用户看到的就是它收起来之前待的地方）。
     * 组里一块都没露着就什么都不做。
     */
    QList<StickyNoteWindow *> visible;
    for (StickyNoteWindow *member : members) {
        if (member && member->isVisible())
            visible.append(member);
    }
    if (visible.isEmpty())
        return false;

    /* 露头那块：指定了就用它，没指定就用当前最上面那块 */
    StickyNoteWindow *front = active;
    if (!front || !visible.contains(front)) {
        front = activeInGroup(groupId);
        if (!front || !visible.contains(front))
            front = visible.first();
    }

    /*
     * 这一摞落在哪一格、多大（同组几块都按它对齐，见 cascadeOffsets）。
     *
     * anchorAt 给了就用它当那一格的左上角（拖一块便签叠到另一块身上时，得让
     * 被拖的那块留在鼠标松开的地方 —— 见 dropNoteOn）。没给就用**原来露头
     * 那块**现在的位置：换标签 / 抽一块上来时整摞不挪窝，用户看到的才是
     * "原地换纸"。
     */
    StickyNoteWindow *previousFront = activeInGroup(groupId);
    const StickyNoteWindow *anchor = previousFront ? previousFront : front;
    /*
     * 起点和尺寸都按**卡片**算（窗口左边那条标签条不算便签的地方，见
     * StickyNoteWindow::cardRect）：不然露头那块一变成"带标签条"的窗口，
     * 整摞就会被它多出来的那一条往右推、下边也会多出一截。
     */
    const QRect frontRect(anchorAt ? QRect(*anchorAt, front->cardRect().size())
                                   : anchor->cardRect());
    const QSize size = frontRect.size().expandedTo(QSize(kGroupMinW, kGroupMinH));
    const QRect area = workAreaFor(frontRect);
    const QList<QPoint> offsets = cascadeOffsets(visible.size());
    const QPoint origin = fitCascadeOrigin(frontRect.topLeft(), size, visible.size(), area);

    /* 顺序：露头那块排第一，其余按原来的层叠顺序跟在后面 */
    QList<StickyNoteWindow *> ordered;
    ordered.append(front);
    for (StickyNoteWindow *member : std::as_const(visible)) {
        if (member != front)
            ordered.append(member);
    }

    /*
     * 标签条那一条先定下来再摆（见 refreshTabs）：窗口 = 卡片 + 标签条，摆位
     * 时窗口尺寸得是对得上的。标签条只看**这一摞里有几块**（两块以上就有）。
     */
    refreshTabs();

    const GroupGeometry layout = groupGeometryFor(ordered, size, origin, offsets, 0);
    applyPlacements(layout.placements, nullptr);

    /*
     * 顺序记进缓存：groupMemberIds / activeInGroup 下一拍就按它回答，
     * QML 那边的 groupActive 也跟着对。
     *
     * 记的是**这一摞的全部成员**（不只是这会儿露着的那几块）：露头那块排第一，
     * 其余按原来的先后跟上 —— 收起来的那几块也是这一摞的成员，只是这会儿不
     * 参与层叠；等它们被叫出来时走 showAll -> placeWindow，这一摞会重新摆
     * 一遍，顺序照旧从这份清单接着排。
     *
     * 早先这里写的是 ordered（= front + **露着的那几块**）：每往摞里加一块就
     * 把上一块从清单里挤掉一个。色块少一个还算轻的，重的是下一次"合进来"的
     * 名单也从这份清单里取（见 groupWith / dropNoteOn 都走 groupMembers），被
     * 挤掉的那块从此既露不出来也点不到（用户报的"组合四个只显示三个 tab、
     * 再点一下就变成第三个了"就是这个）。
     */
    GroupState state;
    if (front && front->note())
        state.memberIds.append(front->note()->id());
    for (StickyNoteWindow *member : std::as_const(members)) {
        if (!member || !member->note() || member == front)
            continue;
        state.memberIds.append(member->note()->id());
    }
    m_groups.insert(groupId, state);
    m_store->setGroupActiveId(groupId, front->note() ? front->note()->id() : QString());

    notifyGroupWindows(groupId);
    return true;
}

/* 这一摞的界面状态变了（露头换人 / 摞里有几块）：让它们各自刷新一遍 */
void StickyNotes::notifyGroupWindows(const QString &groupId) {
    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    /*
     * 层叠顺序就是窗口的 z 序：**倒着抬一遍** —— 顺序里排最后的先抬，露头那块
     * 最后抬，于是它落在最上面。
     *
     * raise() 在 Windows 上就是真把窗口提到最前面，抬的先后 = 最终的 z 序。
     * 顺序里排第一的是"露头那张完整的纸"（见 groupMemberIds / applyGroupLayout），
     * 它必须在最上面；正着抬的话最后抬的是顺序里最后一块，露头那块反被压在底下
     * —— 层叠摆着时屏幕上是一张**空白纸**盖住正文（实测就是这样：便签 2 的正文
     * 被最后建的那一块整块盖住了）。
     *
     * raise() 只在"置顶那一带"里调顺序，不会把别的程序盖住，也不抢激活
     * （便签本来就是置顶的小窗，见 StickyNotes.h 开头）。
     */
    for (int i = members.size() - 1; i >= 0; --i) {
        StickyNoteWindow *member = members.at(i);
        if (!member)
            continue;
        member->notifyGroupChanged();
        member->raise();
    }
    emit groupChanged();
}

QVariantMap StickyNotes::groupState(const QString &noteId) const {
    QVariantMap out;
    StickyNoteWindow *window = qobject_cast<StickyNoteWindow *>(windowForId(noteId));
    if (!window || !window->note())
        return out;
    const QString groupId = window->note()->groupId();

    QStringList ids;
    int visible = 0;
    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    for (StickyNoteWindow *member : members) {
        if (!member || !member->note())
            continue;
        ids << member->note()->id();
        if (member->isVisible())
            ++visible;
    }
    StickyNoteWindow *front = activeInGroup(groupId);

    out.insert(QStringLiteral("groupId"), groupId);
    out.insert(QStringLiteral("count"), members.size());
    out.insert(QStringLiteral("visibleCount"), visible);
    out.insert(QStringLiteral("activeId"),
               front && front->note() ? front->note()->id() : QString());
    out.insert(QStringLiteral("members"), ids);
    out.insert(QStringLiteral("active"), front == window);
    return out;
}

bool StickyNotes::groupWith(StickyNote *note, const QList<StickyNote *> &others) {
    if (!note)
        return false;
    StickyNoteWindow *front = windowFor(note);
    if (!front || !front->isVisible()) {
        notesLog(QStringLiteral("组合请求被拒：%1 这块没露着（藏着的窗口挡不住，见函数说明）")
                     .arg(note->id().left(4)));
        return false;
    }
    {
        QStringList who;
        for (StickyNote *other : others)
            who << (other ? other->id().left(4) : QStringLiteral("null"));
        notesLog(QStringLiteral("组合请求 note=%1 + [%2]   现状 note=%3")
                     .arg(note->id().left(4), who.join(QStringLiteral(",")), noteBrief(front)));
    }

    /*
     * 先把"这一摞"和"要叠上来的那几摞"都理出来。
     *
     * 合并是允许的（把一块拖到另一块身上，两摞并一摞）：所以这里先收
     * note 那一摞（可能就它自己），再收 others 各自那一摞 —— 一块便签
     * 只属于一摞，但两摞可以合成一摞。
     *
     * 然后把 ordered 的第一个挑成**新摞的露头那块**：一摞在桌面上只摆一张
     * 纸（见 applyGroupInto / showOnlyInGroup），挑错了用户就会看见"原来那
     * 几张不见了"。规矩是：
     *   * 这一摞原本就有露头那张 -> 让它继续露着（往摞里加第三块时，原来那
     *     张必须留在这儿，用户报的就是这个："继续组合，就会隐藏之前的组合"）；
     *   * 原本没有（note 还没归过摞）-> 就是 note 自己。
     */
    QStringList orderedIds;
    if (!note->groupId().isEmpty()) {
        const QString previousActive = m_store->groupActiveId(note->groupId());
        /*
         * 记下来的那块**得真在这一摞里**才认。notes.json 里那条记录可能是陈的
         * （那块便签已经被删了、或者这个组已经作废了），照单全收的话会把不相干
         * 的一块拉进新摞、还让它露头 —— 露头那块是"这一摞只摆这一张纸"的那张，
         * 拉错了用户看见的就是"组合完还是原来那张，新加的这块不见了"。
         */
        if (!previousActive.isEmpty()
            && groupMemberIds(note->groupId()).contains(previousActive))
            orderedIds.append(previousActive);
    }
    orderedIds.append(note->id());
    for (StickyNoteWindow *member : groupMembers(note->groupId())) {
        if (member && member->note())
            orderedIds.append(member->note()->id());
    }
    for (StickyNote *other : others) {
        if (!other)
            continue;
        orderedIds.append(other->id());
        for (StickyNoteWindow *member : groupMembers(other->groupId())) {
            if (member && member->note())
                orderedIds.append(member->note()->id());
        }
    }
    /* 去重（同一个 id 可能从好几条路上被加进来），顺序保持第一次出现的先后 */
    orderedIds.removeDuplicates();

    QList<StickyNote *> ordered;
    ordered.reserve(orderedIds.size());
    for (const QString &id : std::as_const(orderedIds)) {
        auto *member = qobject_cast<StickyNoteWindow *>(windowForId(id));
        if (member && member->note())
            ordered.append(member->note());
    }
    if (ordered.isEmpty())
        return false;
    front = windowFor(ordered.first());
    if (!front || !front->isVisible())
        front = windowFor(note);

    /*
     * 什么都没变就别动（"再组合一次"什么也不该发生）。
     *
     * 判据是"others 里有没有一块本来不在这一摞里"—— 拿 ordered 的长度比不行
     * （合并两摞时长度会长，但那两摞里的每一块本来就各有归宿）。
     */
    bool anyNew = false;
    const QString previousGroup = note->groupId();
    for (StickyNote *other : others) {
        if (!other || other == note)
            continue;
        if (previousGroup.isEmpty() || other->groupId() != previousGroup) {
            anyNew = true;
            break;
        }
    }
    if (!anyNew)
        return false;

    return applyGroupInto(ordered, front);
}

/*
 * 把 ordered 这一串便签归成一摞（front 是露头那块），并摆成层叠的样子。
 *
 * groupWith 和 dropNoteOn 都走这里 —— 归堆的逻辑只有这一份。
 */
bool StickyNotes::applyGroupInto(const QList<StickyNote *> &ordered, StickyNoteWindow *front,
                                 const QPoint *anchorAt) {
    if (ordered.size() < 2 || !front || !front->note())
        return false;

    /*
     * 一张新 id 给这一摞（合并之后原来那几个组 id 都作废）。
     *
     * 每次都发新号，不复用落点那一摞的 id：复用了的话原来的"哪块在最上面"
     * 那份记录（notes.json 的 groups.active）会张冠李戴；而"是不是同一批成员"
     * 这件事判断起来容易错（顺序变了也算变），不值当。
     */
    const QString groupId = QStringLiteral("g%1").arg(m_nextGroupNumber++);

    for (StickyNote *member : ordered) {
        if (member)
            member->setGroupId(groupId);
    }
    m_store->scheduleSave();

    /* 顺序也要按 ordered 定下来（露头那块排第一） */
    GroupState state;
    for (StickyNote *member : std::as_const(ordered)) {
        if (member)
            state.memberIds.append(member->id());
    }
    m_groups.insert(groupId, state);

    if (!applyGroupLayout(groupId, front, anchorAt)) {
        /*
         * 摆不出来（比如只有一块露着）：把这一摞退回"各自单着"，别留个
         * 摆不了的组 id 在数据里。
         */
        for (StickyNote *member : std::as_const(ordered)) {
            if (member)
                member->setGroupId(QString());
        }
        m_groups.remove(groupId);
        return false;
    }

    /*
     * 归堆之后**只留露头那一张纸露着**，其余几块收成左边那排色块。
     *
     * 这一步不能省（用户报的"组合之后点 tab 出来一张空白卡片"就是这个）：
     * applyGroupLayout 只负责"摆到哪一格"，它摆的是**摆着的那几块** —— 刚归
     * 到一起时几块都还露着、位置又全叠在同一格上，屏幕上就是几张纸糊在一起，
     * 最上面那张正好是空白的。点标签也只是把另一张抬到最上面，底下那几层
     * 照旧糊着。
     */
    showOnlyInGroup(groupId, front);

    {
        QStringList members;
        for (StickyNoteWindow *member : groupMembers(groupId))
            members << noteBrief(member);
        notesLog(QStringLiteral("建组 %1 露头=%2 成员=%3")
                     .arg(groupId, front && front->note() ? front->note()->id().left(4)
                                                          : QStringLiteral("?"))
                     .arg(members.join(QStringLiteral(" | "))));
    }

    emit changed();
    emit arrangementChanged();
    return true;
}

bool StickyNotes::ungroup(StickyNote *note) {
    if (!note || note->groupId().isEmpty())
        return false;
    const QString groupId = note->groupId();

    /*
     * 拆开 = **当场摊开成几块单独的便签**（用户明确要求："拆分组合点击后，
     * 展开成单独的卡片"）。
     *
     * 早先这里是"各自留在原地"：而这一摞本来就叠在同一格上，于是拆完屏幕上
     * 还是只有一张纸（其余几块严丝合缝压在底下），看着就是"点了没反应"；更糟
     * 的是后 show 出来的那块会压在最上面，而它刚从"收起来的那张纸"状态出来
     * （content 不画），露出来的就是一张**空白卡片**（用户截了图）。
     *
     * 摆法：用户点的那一块**留在原地**（他正看着的就是它），其余几块按"找一
     * 块没被占的桌面"摆开 —— 和新建便签同一个算法（见 nextFreeRect），所以
     * 拆出来的纸和别的便签一样，不会互相盖住、也不会盖住别人。
     *
     * 收起来的那几块一律叫回来：藏起来是"摞里的另一张纸"那个状态（见
     * switchGroupTab），拆开之后它们就是普通便签，得能看见。
     */
    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    StickyNoteWindow *keep = windowFor(note);
    for (StickyNoteWindow *member : members) {
        if (!member || !member->note())
            continue;
        member->note()->setGroupId(QString());
        /*
         * 拆开的每一块都**回到桌面上**，判据不看 note->visible()。
         *
         * 这一块本来就在这一摞里、而这一摞是摆在桌面上的，拆开就该各自成一张
         * 卡片；何况 note->visible() 还可能是陈的（界面上摆着一张纸、清单里却
         * 写着 false —— 见 repairGroup 里那段说明）。原来这里是"visible() 为真
         * 才 show()"，用户那份清单六条全是 false 时，其余几块一块都不出来，
         * 看着就是"点了拆分组合没反应"。
         */
        member->note()->setVisible(true);
        if (!member->isVisible())
            member->show();
        member->rememberGeometry();
        member->notifyGroupChanged();
    }
    m_groups.remove(groupId);
    m_store->setGroupActiveId(groupId, QString());

    /* 拆开之后这一摞没了：左边那条标签条要收掉（各自变成普通便签） */
    refreshTabs();

    /*
     * 摊开：其余几块各找一块空桌面（见上面那段说明）。
     *
     * 放在"清空组 id"之后：nextFreeRect 是按"每一块现在占着哪儿"算的，组 id
     * 还挂着的话同组几块会被当成**一整摞**算一个包围盒（见那里的说明），拆出
     * 来的纸就会叠着那一摞的包围盒摆。
     */
    for (StickyNoteWindow *member : members) {
        if (!member || member == keep)
            continue;
        member->placeAt(nextFreeRect(member->cardRect().size()));
    }
    /* 用户点的那一块留在原地、而且留在最上面（不然它会被刚摊开的那几块压住） */
    if (keep)
        keep->raise();

    {
        QStringList members;
        for (StickyNoteWindow *member : groupMembers(groupId))
            members << noteBrief(member);
        notesLog(QStringLiteral("拆组 %1 成员=%2").arg(groupId, members.join(QStringLiteral(" | "))));
    }
    m_store->scheduleSave();
    emit groupChanged();
    emit changed();
    emit arrangementChanged();
    return true;
}

/* ===========================================================================
 * 拖一块便签到另一块身上 = 组合
 * ======================================================================== */

void StickyNotes::beginNoteDrag(StickyNoteWindow *window) {
    if (!window)
        return;
    clearDropPreview();
    m_dragWindow = window;
    m_dragStartCard = window->cardRect().topLeft();
    m_dragLastCursor = QCursor::pos();
    m_dragStillTicks = 0;
    m_dropTarget = nullptr;
    /*
     * 按下那一刻整摞各块在哪：往后"被按住那块走了多少，其余几块也走多少"。
     * 只有一块也记（值就等于它自己），逻辑不用分叉。
     */
    const QString groupId = groupIdOf(window);
    for (StickyNoteWindow *member : groupMembers(groupId)) {
        if (member)
            m_dragGroupStartPositions.insert(member, member->pos());
    }
    if (m_dragPollTimer)
        m_dragPollTimer->start();
}

/* 拖动期间每一拍：光标压在谁头上 -> 谁亮起"可以放这儿" */
void StickyNotes::updateDropTarget() {
    if (!m_dragWindow) {
        clearDropPreview();
        return;
    }
    StickyNoteWindow *target = dropTargetFor(m_dragWindow.data(), QCursor::pos());
    if (target != m_dropTarget) {
        clearDropPreview();
        m_dropTarget = target;
        if (m_dropTarget)
            m_dropTarget->setDropPreview(true);
    }
}

void StickyNotes::clearDropPreview() {
    for (const QPointer<StickyNoteWindow> &entry : std::as_const(m_windows)) {
        if (entry)
            entry->setDropPreview(false);
    }
    m_dropTarget = nullptr;
}

void StickyNotes::finishNoteDrag() {
    if (m_dragPollTimer)
        m_dragPollTimer->stop();

    StickyNoteWindow *dragged = m_dragWindow.data();
    if (!dragged) {
        clearDropPreview();
        return;
    }
    /*
     * 落点：先用拖动期间那一拍算好的（界面上就是"亮着的那一块"——用户看到
     * 哪块亮，松手就该落到哪块上）；没算过（比如光标一动不动、定时器还没
     * 响）就按松手这一刻的光标位置现算一次。
     */
    StickyNoteWindow *target = m_dropTarget.data();
    if (!target)
        target = dropTargetFor(dragged, QCursor::pos());
    m_dragWindow = nullptr;
    clearDropPreview();
    /* 松手了：拖动那个标记要摘掉，不然这一块下一次挪动会再多走一格 */
    if (dragged)
        dragged->endDragMarker();

    if (dragged->note())
        dragged->rememberGeometry();

    /*
     * 落在另一块身上 -> 组合（被拖的那块摆在鼠标松开的地方，见 dropNoteOn）；
     * 落在空处 -> 就是普通挪个位置，那一摞照旧（拖动路上整摞已经跟过来了）。
     */
    if (target && target->note() && dragged->note())
        dropNoteOn(dragged->note(), target->note());

    emit dragChanged();
    emit changed();
}

/*
 * 这一刻"把 dragged 放下去会落到谁身上"：
 *   * at 那一点压着的那一块（界面上就是光标位置）；只有**头部那一条**算数
 *     —— 便签叠着的时候身子是别人的，只有露出来的标题栏是"要叠到哪一块身上"
 *     最直觉的目标；
 *   * 不在同一摞里（已经在一摞里的两块之间再拖一次什么也不会发生）。
 */
StickyNoteWindow *StickyNotes::dropTargetFor(StickyNoteWindow *dragged, const QPoint &at) const {
    if (!dragged || !dragged->note())
        return nullptr;
    /*
     * 没真的动过就不算拖动：光标可能本来就停在别处（自检里就是这样），
     * 那样"松手"不该把两块并成一摞。真用户按下不动直接松手也不会离开起点。
     */
    if (dragged != m_dragWindow || dragged->cardRect().topLeft() == m_dragStartCard)
        return nullptr;
    StickyNoteWindow *target = nullptr;
    /* 从后往前找：后建的在置顶那一带里更靠上，两块叠着时该落在露出来的那块上 */
    for (int i = m_windows.size() - 1; i >= 0; --i) {
        StickyNoteWindow *window = m_windows.at(i).data();
        if (!window || window == dragged || !window->isVisible() || !window->note())
            continue;
        if (window->geometry().contains(at)) {
            target = window;
            break;
        }
    }
    if (!target)
        return nullptr;
    /*
     * 已经在同一摞里的不算落点：那一摞是一个整体，拖着自己的一块压到自己另一块
     * 身上什么也不该发生（不这么判的话，拖动整摞经过自己身上时会被当成"放下
     * 去组合"，那一摞的露头就会在半路换人、层叠跟着错位）。
     */
    const QString draggedGroup = dragged->note()->groupId();
    if (!draggedGroup.isEmpty() && target->note()->groupId() == draggedGroup)
        return nullptr;
    /*
     * 落点判定用**卡片**矩形：标签条那一条不是"便签的身子"，压在那儿不算
     * 拖到它身上（光标在色块上时，用户想点的是那张标签）。
     */
    const int headerH = 42;
    const QRect card = target->cardRect();
    const QRect head(card.x(), card.y(), card.width(), headerH);
    return head.contains(at) ? target : nullptr;
}

bool StickyNotes::dropNoteOn(StickyNote *note, StickyNote *target) {
    if (!note || !target || note == target)
        return false;
    StickyNoteWindow *dragged = windowFor(note);
    StickyNoteWindow *landing = windowFor(target);
    if (!dragged || !landing || !dragged->isVisible() || !landing->isVisible())
        return false;
    notesLog(QStringLiteral("拖放组合 被拖=%1 落点=%2   被拖=%3 落点=%4")
                 .arg(note->id().left(4), target->id().left(4), noteBrief(dragged),
                      noteBrief(landing)));
    /* 已经在这一摞里：什么都不用做 */
    if (!note->groupId().isEmpty() && note->groupId() == target->groupId())
        return false;

    /*
     * 顺序 = **落点那一块在最前**，然后它那一摞、再被拖的那一块和它那一摞。
     *
     * 为什么不是"被拖的那块排第一"：一摞在桌面上只摆露头那一张纸（见
     * applyGroupInto / showOnlyInGroup），排第一的就是留下的那张。用户是把
     * 被拖的那块**丢到 target 身上**的，所以该留在原地露着的是 target ——
     * 反过来（底下那摞整摞让位给被拖的）看起来就是"原来那几张不见了"
     * （用户报的："继续组合，就会隐藏之前的组合"）。
     */
    QList<StickyNote *> ordered;
    ordered.append(target);
    for (StickyNoteWindow *member : groupMembers(target->groupId())) {
        if (member && member->note() && member->note() != target)
            ordered.append(member->note());
    }
    if (!ordered.contains(note))
        ordered.append(note);
    for (StickyNoteWindow *member : groupMembers(note->groupId())) {
        if (member && member->note() && !ordered.contains(member->note()))
            ordered.append(member->note());
    }

    /*
     * 整摞落在哪儿：**落点那一块留在它现在的地方**（用户是把东西丢在它身上的），
     * 被拖的那一块跟着并进来、藏起来（轮到它时点标签换上来）。
     *
     * 用**卡片**的左上角（窗口左边那条标签条不算便签的地方）。
     */
    const QPoint anchor = landing->cardRect().topLeft();
    return applyGroupInto(ordered, landing, &anchor);
}

bool StickyNotes::isDragging(StickyNote *note) const {
    return note && m_dragWindow && m_dragWindow->note() == note;
}

/*
 * ===========================================================================
 * 左边那条标签条：**组合**里的便签互相切换
 * ===========================================================================
 *
 * 一块便签归到某一摞（组合）里之后，界面上**只摆那一张纸**（露头那块，
 * 见 activeInGroup），同组其余几块收成左边那排色块；点一个色块就把那一块
 * 换上来。没组合过的便签**没有标签条** —— 标签条是"这一组有几块"那件事的
 * 界面（用户明确要求：只有组合的才有）。
 *
 * 怎么做到"只露一块"：把其余几块的窗口藏起来、挪到露头那块那一格等着 ——
 * **窗口留着**（不是销毁），所以换回来是瞬时的，数据 / 界面状态（编辑位置、
 * 链接卡片）也都还在。藏起来的那几块便签自己在 notes.json 里仍然是"摆着"
 * （visible 不动，见 activeInGroup 的说明），下次启动还是这一摞。
 */
bool StickyNotes::switchGroupTab(const QString &noteId) {
    auto *target = qobject_cast<StickyNoteWindow *>(windowForId(noteId));
    if (!target || !target->note())
        return false;
    const QString groupId = target->note()->groupId();
    if (groupId.isEmpty())
        return false;

    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    if (members.size() < 2)
        return false;

    {
        QStringList before;
        for (StickyNoteWindow *member : members)
            before << noteBrief(member);
        notesLog(QStringLiteral("换纸 组=%1 目标=%2 成员=%3")
                     .arg(groupId, noteId.left(4))
                     .arg(before.join(QStringLiteral(" | "))));
    }

    /*
     * 点的是**已经露着的那一块**：什么都不用做。
     *
     * 判据用 groupActiveId（activeInGroup），**不能**用 isActiveWindow()：
     * 同组其余几块是藏着的窗口，它们在系统眼里不是活动窗口 —— 拿"活动窗口"
     * 当判据的话，点任何一个色块都会被当成"点的是别人"，连点自己那一块也
     * 会白跑一遍重排（用户看到的就是"点一下闪一下"）。
     */
    if (activeInGroup(groupId) == target) {
        notesLog(QStringLiteral("换纸：点的就是现在露着的那块，什么都不做"));
        return false;
    }

    /*
     * 整摞的"卡片位置"**按现在露着的那一块算**，不按被点的那一块算。
     *
     * 被点的那一块这会儿多半还坐在层叠阶梯上（刚组好、或者刚从文件恢复时，
     * 几块是各自错开 8 / 30 摆着的）—— 拿它自己的位置当基准的话，点一下标签
     * 整摞就"跳"到那一格去（用户报的"位置也有移动"）。换标签本来就该是**原地
     * 换纸**：位置由看得见的那一块说了算，被点的那块搬过来跟它对齐。
     */
    StickyNoteWindow *shown = activeInGroup(groupId);
    const QRect card = shown ? shown->cardRect() : target->cardRect();

    /*
     * 一、先把"谁露头"定下来，**再**动窗口。
     *
     * 只写 groupActiveId（notes.json 里那一条），**不重排成员顺序**：顺序就是
     * 标签条上色块的先后，动一下整排标签就换位（见 StickyNoteWindow::groupTabs
     * 里那段说明）。"谁露头"从这一步起就由 activeInGroup 按这个 id 回答 ——
     * 所以下面 notifyGroupChanged() 的时候，新露头那块已经**不是**"收起来的
     * 那张纸"了，纸和色块都画得出来。
     */
    m_store->setGroupActiveId(groupId, noteId);
    m_store->scheduleSave();
    /*
     * 二、新露头那块：原地摆好 -> 亮出来 -> 按新状态重画 -> **趁它还在屏幕上，
     * 先把这一帧真的推出去**，最后才去收旧的那块。
     *
     * 顺序反过来（原来就是"先把其余几块 hide 掉，再 show 新的"）会闪：
     * 几块便签是**各自独立的原生窗口**，系统不会替我们把这两下合成一帧 ——
     * 藏掉旧的和画出新的之间那一拍，屏幕上两块都没有，用户看到的就是
     * "点一下标签，便签闪一下"。
     *
     * grabFramebuffer() + repaint() 是这个工程里"强制同步渲染、把这一帧推到
     * 窗口上"的老配方（见 Screenshot.cpp 预渲染那两处）。
     */
    target->placeAt(card);
    /*
     * 换上来这块就是"现在摆在桌面上的那张纸"：数据也要跟着置回"摆着"。
     *
     * 不置的话两件事都会错：activeInGroup 按 note->visible() 挑露头那块（见
     * 那里的说明），目标那块要是还写着 visible=false，挑回来的还是**原来那张**
     * —— 目标就被当成"收起来的那张纸"，content 不画，屏幕上是一张**空白卡片**；
     * 而且下次启动按清单恢复时它就不见了。
     */
    if (target->note())
        target->note()->setVisible(true);
    if (!target->isVisible())
        target->show();
    target->raise();
    notesLog(QStringLiteral("  新露头 show+raise %1（%2）")
                 .arg(target->note() ? target->note()->id().left(4) : QStringLiteral("?"),
                      noteBrief(target)));
    target->notifyGroupChanged();
    if (auto *view = target->findChild<QQuickWidget *>()) {
        view->grabFramebuffer();
        view->repaint();
    }
    target->rememberGeometry();

    /* 三、其余几块现在才收（见 showOnlyInGroup） */
    showOnlyInGroup(groupId, target);

    /*
     * 换完纸要让**这一摞每块窗口**重新读一遍标签条（选中圈跟着搬家）。
     *
     * QML 那边的 groupTabs 是个绑定在 Q_PROPERTY 上的值，光发 groupChanged
     * 只会重画"在不在摞里 / 是不是露头"；那排色块谁被选中要 notifyTabsChanged
     * 才刷得动（踩过：切换之后色块的选中圈还留在上一张纸上，用户看着像"点了
     * 没反应"）。
     */
    refreshTabs();

    notifyGroupWindows(groupId);
    {
        QStringList after;
        for (StickyNoteWindow *member : groupMembers(groupId))
            after << noteBrief(member);
        notesLog(QStringLiteral("换纸完 组=%1 现在=%2").arg(groupId, after.join(QStringLiteral(" | "))));

        /*
         * 这一摞这会儿到底是谁在画：把**每一块成员**的窗口都抓一张缩略图，
         * 记下"有没有内容"（画出来的像素分布）。
         *
         * 为什么不用 widgetAt / topLevelAt：那两只要窗口真显示在桌面上才准，
         * 而且拿到的常常是别的进程的窗口。抓自己的窗口最直接 —— 用户看到
         * "只剩两个色块"时，这里会显示露头那块抓出来是空的（或者藏着的那块
         * 反而有内容），一眼能看出是哪一块在画。
         */
        for (StickyNoteWindow *member : groupMembers(groupId)) {
            if (!member || !member->note())
                continue;
            const bool isShown = activeInGroup(groupId) == member;
            /*
             * 抓这一块窗口现在画出来的样子。抓之前先强制同步画一帧
             * （grabFramebuffer + repaint 是这个工程里"把这一帧推出去"的老配方，
             * 见 Screenshot.cpp）：不然抓到的可能是上一帧，误判成"没画出来"。
             */
            if (auto *view = member->findChild<QQuickWidget *>()) {
                view->grabFramebuffer();
                view->repaint();
            }
            const QImage shot = member->grab().toImage();
            /*
             * 标签条那一格画出来什么：在第 17 列上从上往下取几个点，直接记
             * 颜色+透明度。比"数色带"糙，但一眼能看出是"色块没画"还是
             * "画了但我没认出来"（这一列是色块的正中间）。
             */
            QStringList stripSamples;
            for (int y = 0; y < 200; y += 20) {
                const QColor c = shot.pixelColor(17, y);
                stripSamples.append(QStringLiteral("%1@%2:a%3").arg(c.name()).arg(y).arg(c.alpha()));
            }
            const QString titlePatch = shot.pixelColor(60, 18).name();
            /* 临时：把这一格的标签条区域存成图（查完删） */
            shot.copy(0, 0, qMin(36, shot.width()), qMin(140, shot.height()))
                .save(QStringLiteral("H:/steward/build/chip-%1.png")
                          .arg(member->note()->id().left(4)));
            notesLog(QStringLiteral("  这一块画出来什么 %1 露头=%2 抓图=%3x%4 第17列=%5 标题处=%6")
                         .arg(member->note()->id().left(4))
                         .arg(isShown ? 1 : 0)
                         .arg(shot.width()).arg(shot.height())
                         .arg(stripSamples.join(QStringLiteral(" ")), titlePatch));
        }
    }
    emit changed();
    emit arrangementChanged();
    return true;
}

/*
 * 一摞里**只留 keep 那一块露着**，其余几块摆到同一格再藏起来。
 *
 * 桌面上"一摞 = 一张纸 + 左边那排标签"就靠这一句（用户要的"组合的卡片只显示
 * 一张卡片，这一张卡片切换"）。两处用到：
 *   * 点标签换纸（switchGroupTab）：新露头那块先亮出来、推一帧，之后才收旧的；
 *   * 从文件恢复（start）：恢复出来的那几块本来就都露着、位置还叠在同一格，
 *     不收的话桌面上就是几张纸糊在一起。
 *
 * keep 是"要留下的那一块"，给空就是按 activeInGroup 现挑一块。它自己不在这一摞
 * 里（或者这一摞只剩它一块）时什么都不做。
 */
void StickyNotes::showOnlyInGroup(const QString &groupId, StickyNoteWindow *keep) {
    if (groupId.isEmpty())
        return;
    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    if (members.size() < 2)
        return;

    StickyNoteWindow *shown = keep;
    if (!shown || !members.contains(shown))
        shown = activeInGroup(groupId);
    if (!shown)
        return;

    const QRect card = shown->cardRect();
    for (StickyNoteWindow *member : std::as_const(members)) {
        if (!member || member == shown)
            continue;
        member->placeAt(card);
        member->hide();
        notesLog(QStringLiteral("  收起 %1（%2）")
                     .arg(member->note() ? member->note()->id().left(4) : QStringLiteral("?"),
                          noteBrief(member)));
        /*
         * 藏完那一块**立刻把露头那张抬回来**。同组几块窗口的位置和尺寸是一模
         * 一样的（都摆在同一格、窗宽也一样），谁在最上面完全由窗口顺序决定 ——
         * hide() 会让本来压在上面的那块让位，露头那张就可能被后面那块盖住：
         * 屏幕上看到的是**别人**那一条标签条（层数、选中圈都对不上，用户报的
         * "点了 tab 之后只剩两个色块"就是这个）。
         */
        shown->raise();
    }
    notesLog(QStringLiteral("收起同组其余几块 留=%1  结果=%2")
                 .arg(shown->note() ? shown->note()->id().left(4) : QStringLiteral("?"),
                      noteBrief(shown)));
}

/*
 * 桌面上"这一刻摆着的那几块便签"（卡片矩形 -> 窗口），按便签编号排。
 *
 * 给"外面那个框要摆哪儿"用：便签自己的几何是卡片，窗口左边还多一条标签条
 * （见 frameRectFor），换标签 / 收起来时要把那一条也对上。
 */
QList<StickyNoteWindow *> StickyNotes::shownWindows() const {
    QList<StickyNoteWindow *> out;
    for (const QPointer<StickyNoteWindow> &entry : m_windows) {
        StickyNoteWindow *window = entry.data();
        if (window && window->note() && window->isVisible())
            out.append(window);
    }
    std::sort(out.begin(), out.end(), [](StickyNoteWindow *a, StickyNoteWindow *b) {
        return (a ? a->noteNumber() : 0) < (b ? b->noteNumber() : 0);
    });
    return out;
}

/*
 * 标签条的内容变了：**每一块便签**重画自己的那排色块，并且各自重新量一次
 * "要不要给标签条让出左边那一条"（组合建了 / 拆了 / 组里少了一块，都要跟着变）。
 */
void StickyNotes::refreshTabs() {
    for (const QPointer<StickyNoteWindow> &entry : std::as_const(m_windows)) {
        StickyNoteWindow *window = entry.data();
        if (!window)
            continue;
        /*
         * 让不让那条，看**这一块自己在不在有两块以上的摞里**：没组合过的便签
         * 一条都不让（左边干干净净），组合过的那几块都让一条（窗宽才一致）。
         *
         * 先记下现在那张纸在哪，再改那一条的宽度 —— setTabStrip 会按它把卡片
         * 摆回原位（不能只改标记：窗口矩形变了的话，卡片位置和 note 的几何
         * 都得跟着对，见 setTabStrip 的说明）。
         */
        const QRect card = window->cardRect();
        window->setTabStrip(window->groupTabs().size() > 1, card);
        window->notifyTabsChanged();
    }
}
bool StickyNotes::dropPreviewAt(StickyNote *note, const QPoint &at) const {
    if (!note)
        return false;
    /* 没在拖就不可能有落点 */
    if (!m_dragWindow)
        return false;
    return dropTargetFor(m_dragWindow.data(), at) == windowFor(note);
}


void StickyNotes::forgetGroupIfEmpty(const QString &groupId) {
    if (groupId.isEmpty())
        return;
    if (!groupMemberIds(groupId).isEmpty())
        return;
    m_groups.remove(groupId);
}

/*
 * 一摞里少了一块之后的收拾：露头那块没了就换一块顶上、整摞重排；
 * 组里只剩一块了就直接散伙（"组合"至少得有两块）。
 */
void StickyNotes::repairGroup(const QString &groupId) {
    if (groupId.isEmpty())
        return;
    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    forgetGroupIfEmpty(groupId);
    if (members.size() < 2) {
        /* 只剩一块（或者一块都不剩了）：那一块当普通便签，摞就没了 */
        if (members.size() == 1 && members.first() && members.first()->note()) {
            members.first()->note()->setGroupId(QString());
            if (members.first()->note()->visible() && !members.first()->isVisible())
                members.first()->show();
            members.first()->rememberGeometry();
            members.first()->notifyGroupChanged();
            /* 只剩它一块了：左边那条标签条收掉（它不再是"一摞"） */
            refreshTabs();
            m_store->scheduleSave();
            emit groupChanged();
        }
        return;
    }
    /*
     * 少了一块之后先保证"至少有一块是露着的"：露头那块被删 / 被收起来时，
     * 其余几块可能正收成标签条上的色块（hide 着）—— 那样这一摞就整摞不见了。
     * 这里把顺序里第一块可见的（还愿意摆着的）叫出来，再重排。
     */
    bool anyShown = false;
    for (StickyNoteWindow *member : members) {
        if (member && member->isVisible()) {
            anyShown = true;
            break;
        }
    }
    if (!anyShown) {
        for (StickyNoteWindow *member : members) {
            if (!member || !member->note() || !member->note()->visible())
                continue;
            /*
             * 叫出来的这块马上就是"这一摞摆着的那张纸"：**把它置回"摆着"**。
             *
             * show() 只改窗口、不改数据 —— 不置的话界面上摆着一张纸、清单里却
             * 写着 visible=false：下次启动它就不见了；按 note->visible() 挑人的
             * 地方（activeInGroup、拆分组合、收起来那几条）也全会认错。用户那份
             * 清单里六条全是 visible=false、桌面上却还摆着卡片，就是这么来的。
             */
            member->note()->setVisible(true);
            member->show();
            member->raise();
            break;
        }
    }
    applyGroupLayout(groupId);
}

/*
 * 拖 / 改大小时整摞跟着走（StickyNoteWindow 转发过来的）。
 *
 * 便签的移动是窗口管理器做的，这里只在**每次事件**之后把其余几块摆到"按基准
 * 算出来的位置"上 —— 不做插值也不做动画：整摞跟手才有"这是一叠纸"的感觉，
 * 动画反而像后面几块在迟滞。
 *
 * origin / size 都是**卡片**的（窗口左边那条标签条不算便签的地方）：整摞的
 * 相对关系是按卡片算的，按窗口算的话每块多出来的标签条会把它们越推越歪。
 */
/*
 * 整摞一起搬：被按住那块（reference）走了 delta，其余几块走同一份。
 *
 * 基准是 beginGroupDrag 那一刻每个成员的位置 —— 不从这里反推"该走多少"，
 * 也就不需要去对上"层叠阶梯"和"标签条那一条"两种偏移。
 */
void StickyNotes::moveGroupByFrameDelta(StickyNoteWindow *reference, const QPoint &delta) {
    if (!reference || delta.isNull())
        return;
    const QString groupId = groupIdOf(reference);
    if (groupId.isEmpty())
        return;

    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    if (members.size() < 2)
        return;

    for (StickyNoteWindow *member : std::as_const(members)) {
        if (!member || member == reference)
            continue;
        /* 没记过基准（半路加进来的）就退回"按同一份位移"平移 */
        const QPoint base = m_dragGroupStartPositions.value(member, member->pos());
        member->moveFrameTo(base + delta);
    }
}

void StickyNotes::syncGroupGeometry(StickyNoteWindow *reference, const QPoint &origin,
                                    const QSize &size, bool move, bool resize) {
    if (!reference || (!move && !resize))
        return;
    const QString groupId = groupIdOf(reference);
    if (groupId.isEmpty())
        return;

    const QList<StickyNoteWindow *> members = groupMembers(groupId);
    if (members.size() < 2)
        return;
    const int index = int(members.indexOf(reference));
    if (index < 0)
        return;

    /*
     * 层叠偏移按**左上的那一块**当第 0 级来算，而不是按 groupMembers 的顺序：
     * 顺序里第一个是"露头那块"（z 序在最上面），它不一定就在整摞的左上角
     * —— 比如从下面抽了一块上来之后，顺序是 [抽上来那块, 原来那块, …]，但
     * 占着左上角的还是原来那块。按顺序当第 0 级的话，整摞会按偏差整体平移一格
     * （拖动一次就跳一下）。
     */
    int baseIndex = 0;
    for (int i = 1; i < members.size(); ++i) {
        if (!members.at(i))
            continue;
        if (!members.at(baseIndex))
            continue;
        const QRect base = members.at(baseIndex)->cardRect();
        const QRect here = members.at(i)->cardRect();
        if (here.x() < base.x() || here.y() < base.y())
            baseIndex = i;
    }

    const QList<QPoint> offsets = cascadeOffsets(members.size());
    const QSize target = resize
        ? size.expandedTo(QSize(kGroupMinW, kGroupMinH))
        : QSize(qMax(1, size.width()), qMax(1, size.height()));

    if (move) {
        /*
         * 拖动那条路走 moveGroupByFrameDelta（见那里的说明）—— 这里只管改大小。
         */
        return;
    }

    /*
     * 改大小：被按住那块在自己位置上长大，其余几块按"整摞的左上角不动"重算，
     * 并顺手做一次"整摞别探出屏幕"的收边（纸变大之后整摞会往右下多探一截）。
     */
    const QPoint anchor = members.at(baseIndex)->cardRect().topLeft();
    const QPoint wanted = fitCascadeOrigin(anchor, target, members.size(),
                                           workAreaFor(QRect(anchor, target)));
    for (int i = 0; i < members.size(); ++i) {
        StickyNoteWindow *member = members.at(i);
        if (!member || member == reference)
            continue;
        member->setGroupGeometry(QRect(wanted + (i < offsets.size() ? offsets.at(i)
                                                                    : QPoint(0, 0)),
                                       target));
    }
    /* 被按住的那一块：位置由系统管，尺寸得自己跟上 */
    reference->rememberGeometry();
}

bool StickyNotes::arrangeAll() {
    /*
     * 一键排列：把摆着的那几块在它们所在的屏上摆成一个网格。
     *
     * 尺寸先按便签现在的平均大小算列数，再按"装不下就压一点"回调一次 ——
     * 目标是**一屏能看全**，而不是保持原尺寸铺到屏幕外面去（那就失去
     * "一键排列"的意义了）。压到 kArrangeMin* 就到底，剩下的靠滚动 / 手动挪。
     *
     * 组合窗口按**一整摞**占一格（见 applyGroupLayout 那段说明）：一摞便签在
     * 桌面上本来就是"一张纸 + 左边那排标签"，拆开排到网格里等于把它解散了
     * —— 那得用户自己在菜单里说。同组那几块摆在哪儿由 applyGroupLayout 负责。
     */
    struct Unit {
        StickyNoteWindow *window = nullptr;   /* 单块便签 */
        QString groupId;                      /* 或者一整摞 */
        StickyNoteWindow *front = nullptr;    /* 那一摞露头的那块 */
        QSize size;                           /* 整摞的包围盒尺寸（格子按它算） */
    };

    QList<Unit> units;
    QStringList groupSeen;
    for (const QPointer<StickyNoteWindow> &entry : std::as_const(m_windows)) {
        StickyNoteWindow *window = entry.data();
        if (!window || !window->isVisible())
            continue;
        const QString groupId = groupIdOf(window);
        if (groupId.isEmpty()) {
            Unit unit;
            unit.window = window;
            unit.size = window->cardRect().size();
            units.append(unit);
            continue;
        }
        if (groupSeen.contains(groupId))
            continue;
        groupSeen.append(groupId);

        Unit unit;
        unit.groupId = groupId;
        unit.front = activeInGroup(groupId);
        QRect bounds;
        for (StickyNoteWindow *member : groupMembers(groupId)) {
            if (!member || !member->isVisible())
                continue;
            const QRect card = member->cardRect();
            bounds = bounds.isNull() ? card : bounds.united(card);
        }
        if (bounds.isNull())
            continue;
        unit.size = bounds.size();
        units.append(unit);
    }
    if (units.isEmpty())
        return false;

    /* 用第一格的位置决定在哪块屏上排（用户看到的就是这块屏） */
    const QRect area = workAreaFor(units.first().window ? units.first().window->cardRect()
                                                        : units.first().front->cardRect());

    /* 平均尺寸（用户可能把某一块拉得很大，别让它一个人决定列数） */
    int sumW = 0;
    int sumH = 0;
    for (const Unit &unit : std::as_const(units)) {
        sumW += unit.size.width();
        sumH += unit.size.height();
    }
    int cellW = clampInt(sumW / units.size(), kArrangeMinW, 520);
    int cellH = clampInt(sumH / units.size(), kArrangeMinH, 480);

    /* 按网格摆：先算一遍有几个格子，装不下就把格子缩小一点再算 */
    for (int attempt = 0; attempt < 12; ++attempt) {
        const int usableW = qMax(1, area.width() - kArrangeGap);
        const int usableH = qMax(1, area.height() - kArrangeGap);
        const int columns = qMax(1, (usableW + kArrangeGap) / (cellW + kArrangeGap));
        const int rows = (units.size() + columns - 1) / columns;

        if (rows * (cellH + kArrangeGap) - kArrangeGap <= usableH)
            break;

        /* 太高了：先缩高度，缩到底还不行再缩宽度 */
        if (cellH > kArrangeMinH) {
            cellH = qMax(kArrangeMinH, cellH - 24);
            continue;
        }
        if (cellW > kArrangeMinW) {
            cellW = qMax(kArrangeMinW, cellW - 24);
            continue;
        }
        break;
    }

    const int usableW = qMax(1, area.width() - kArrangeGap);
    const int columns = qMax(1, (usableW + kArrangeGap) / (cellW + kArrangeGap));

    /*
     * 格子至少得装得下里面那一格东西：组合窗口的包围盒比单块便签大（层叠错开
     * 那一段也算），按平均数算出来的格子可能不够 —— 那就把它撑开，宁可整行
     * 高一点，也不能让两格叠在一起（"排列"的意义就是不重叠）。
     */
    for (const Unit &unit : std::as_const(units)) {
        cellW = qMax(cellW, unit.size.width());
        cellH = qMax(cellH, unit.size.height());
    }

    int index = 0;
    for (const Unit &unit : std::as_const(units)) {
        const int row = index / columns;
        const int column = index % columns;
        ++index;

        /*
         * 每一行**从右往左填**（第 0 格贴着工作区右沿）。
         *
         * 用户要的："排列现在是从左开始，改成从右开始" —— 连着开几块便签本来
         * 就是从右上角往左铺开的（见 nextFreeRect），一键排列接着这个方向走，
         * 排完最左边那一列不会空着、而右边那条缝留着，和新便签的落位对得上。
         *
         * 用 area.right() + 1 反推而不是 area.x() + area.width()：QRect::right()
         * 是**闭区间**的右端点，减掉格子宽度之后正好是"这一格贴着右沿"的左边
         * 坐标（和 nextFreeRect 同一个算法）。
         */
        const int x = area.right() + 1 - cellW - column * (cellW + kArrangeGap);
        const int y = area.y() + row * (cellH + kArrangeGap);

        if (unit.window) {
            unit.window->placeAt(QRect(x, y, cellW, cellH));
            unit.window->raise();
            continue;
        }
        /*
         * 一整摞：先把露头那块摆到格子的左上角并改成格子大小，再让整摞按它
         * 重排（applyGroupLayout 只认"露头那块现在在哪、多大"，所以这一句
         * 顺序不能反）。
         */
        if (unit.front) {
            unit.front->placeAt(QRect(x, y, cellW, cellH));
            applyGroupLayout(unit.groupId, unit.front);
        }
    }

    m_store->scheduleSave();
    emit arrangementChanged();
    emit changed();
    return true;
}

void StickyNotes::uiTrace(const QString &text) const {
    /* 转成 UTF-8 再打：中文注释 / 中文标签在控制台里不会变成问号 */
    qWarning("UI-TRACE %s", text.toUtf8().constData());
}

QVariantMap StickyNotes::menuState(const QString &noteId) const {
    QVariantMap state;
    QObject *root = windowRootForId(noteId);
    if (!root)
        return state;

    /* 便签窗口的 QML 里那个 NoteMenu（objectName 见 StickyNoteWindow.qml） */
    QObject *menu = root->findChild<QObject *>(QStringLiteral("noteMenu"));
    if (!menu)
        return state;

    state.insert(QStringLiteral("opened"), menu->property("opened").toBool());
    state.insert(QStringLiteral("flyout"), menu->property("flyoutKind").toString());
    state.insert(QStringLiteral("flyoutSide"), menu->property("flyoutSide").toString());
    state.insert(QStringLiteral("width"), menu->property("width").toDouble());
    state.insert(QStringLiteral("height"), menu->property("height").toDouble());
    /*
     * 主栏本身的宽度。
     *
     * 子面板现在是自己一块窗口（见 NoteMenu 的 flyoutWindow），主栏窗口的
     * width 就是主栏宽度；这条单独报出去，自检就不用写死 208 ——
     * "面板在不在主栏旁边"那几个判断都拿它当基准。
     */
    state.insert(QStringLiteral("paneWidth"), menu->property("entriesWidth").toDouble());
    state.insert(QStringLiteral("x"), menu->property("x").toDouble());
    state.insert(QStringLiteral("y"), menu->property("y").toDouble());
    state.insert(QStringLiteral("anchorX"), menu->property("anchorX").toDouble());
    state.insert(QStringLiteral("anchorY"), menu->property("anchorY").toDouble());
    state.insert(QStringLiteral("entryCount"), menu->property("entries").toList().size());
    /* 子面板挂哪边看 flyoutSide（自检拿它验证"右边不够就翻到左边"） */

    /*
     * 便签窗口自己的矩形（屏幕坐标）。
     *
     * 菜单要拿它来"躲开便签"：便签贴近屏幕右边时，菜单往右会长到屏幕外，
     * Qt 把它夹回来之后往往正好压在便签身上 —— 那看着就是"便签把菜单盖住了"。
     */
    if (auto *noteWindow = qobject_cast<StickyNoteWindow *>(windowForId(noteId))) {
        state.insert(QStringLiteral("noteRect"),
                     QRect(noteWindow->x(), noteWindow->y(),
                           noteWindow->width(), noteWindow->height()));
    }

    QStringList labels;
    const QVariantList entries = menu->property("entries").toList();
    for (const QVariant &item : entries) {
        const QVariantMap entry = item.toMap();
        if (entry.value(QStringLiteral("separator")).toBool())
            labels << QStringLiteral("---");
        else
            labels << entry.value(QStringLiteral("label")).toString();
    }
    state.insert(QStringLiteral("labels"), labels.join(QStringLiteral(" | ")));

    /* 弹窗在屏幕上的矩形（Popup 是独立原生窗口，x/y 就是屏幕坐标） */
    QVariant rect;
    QMetaObject::invokeMethod(menu, "screenRect", Q_RETURN_ARG(QVariant, rect));
    state.insert(QStringLiteral("screenRect"), rect);

    /* 子面板在屏幕上的矩形（没展开时是空矩形） */
    QVariant subRect;
    QMetaObject::invokeMethod(menu, "flyoutScreenRect", Q_RETURN_ARG(QVariant, subRect));
    state.insert(QStringLiteral("flyoutRect"), subRect);

    /* 便签窗口那块屏的可用区域（菜单摆位全靠它，错了就整块乱） */
    QVariant area;
    QMetaObject::invokeMethod(menu, "screenAreaForTest", Q_RETURN_ARG(QVariant, area));
    state.insert(QStringLiteral("screenArea"), area);

    return state;
}

bool StickyNotes::openMenuFlyout(const QString &noteId, const QString &kind) {
    QObject *root = windowRootForId(noteId);
    if (!root)
        return false;
    QObject *menu = root->findChild<QObject *>(QStringLiteral("noteMenu"));
    if (!menu)
        return false;
    /*
     * 把 QML 那头的返回值**透传出来**：openFlyoutForTest 在"菜单里根本没有挂着
     * 这个 kind 的条目"时返回 false（比如颜色那条已经不是子面板了，只剩
     * 透明度 / 组合）。早先这里不看返回值、一律 return true —— 自检就没法用这个
     * 口子量"这一条到底有没有子面板"（踩过：明明没有子面板，接口却说打开成功）。
     */
    QVariant opened;
    QMetaObject::invokeMethod(menu, "openFlyoutForTest", Q_RETURN_ARG(QVariant, opened),
                              Q_ARG(QVariant, QVariant(kind)));
    return opened.toBool();
}

QObject *StickyNotes::windowForId(const QString &id) const {
    for (const QPointer<StickyNoteWindow> &window : m_windows) {
        if (window && window->note() && window->note()->id() == id)
            return window;
    }
    return nullptr;
}

QObject *StickyNotes::windowRootForId(const QString &id) const {
    auto *window = qobject_cast<StickyNoteWindow *>(windowForId(id));
    return window ? window->qmlRoot() : nullptr;
}

QVariantMap StickyNotes::windowState(const QString &id) const {
    QVariantMap state;
    auto *window = qobject_cast<StickyNoteWindow *>(windowForId(id));
    if (!window || !window->note())
        return state;

    StickyNote *note = window->note();
    state.insert(QStringLiteral("id"), note->id());
    state.insert(QStringLiteral("text"), note->text());
    state.insert(QStringLiteral("color"), note->color().name());
    state.insert(QStringLiteral("textColor"), note->textColor().name());
    state.insert(QStringLiteral("shadeColor"), note->shadeColor().name());
    state.insert(QStringLiteral("visible"), window->isVisible());
    state.insert(QStringLiteral("staysOnTop"), window->staysOnTop());
    state.insert(QStringLiteral("locked"), window->locked());
    state.insert(QStringLiteral("opacity"), note->opacity());
    state.insert(QStringLiteral("opacityPercent"), note->opacityPercent());
    /*
     * 位置和尺寸报的是**卡片**（便签纸那张纸）的位置：窗口左边多出来的那一条
     * 标签条不是便签的地方。frame* 那三个是窗口自己的几何 —— 画面上真正占的
     * 那块矩形，量"没留残窗""窗口在屏幕里"时用得着。
     */
    const QRect card = window->cardRect();
    state.insert(QStringLiteral("x"), card.x());
    state.insert(QStringLiteral("y"), card.y());
    state.insert(QStringLiteral("w"), card.width());
    state.insert(QStringLiteral("h"), card.height());
    state.insert(QStringLiteral("frameX"), window->x());
    state.insert(QStringLiteral("frameY"), window->y());
    state.insert(QStringLiteral("frameW"), window->width());
    state.insert(QStringLiteral("frameH"), window->height());
    state.insert(QStringLiteral("tabMargin"), window->tabMargin());
    state.insert(QStringLiteral("linkCount"), note->linkCount());
    state.insert(QStringLiteral("number"), window->noteNumber());
    state.insert(QStringLiteral("toolTip"), QStringLiteral("便签 %1").arg(window->noteNumber()));
    /*
     * 窗口标志位（自检要钉住"置顶 + 无边框"这两条 —— 便签的全部意义就是
     * "钉在桌面上"，哪天有人顺手把标志位改回去，这条会红）。
     */
    state.insert(QStringLiteral("frameless"),
                 window->windowFlags().testFlag(Qt::FramelessWindowHint));
    state.insert(QStringLiteral("onTopFlag"),
                 window->windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    state.insert(QStringLiteral("toolWindow"), window->windowFlags().testFlag(Qt::Tool));
    /*
     * 这一刻有没有一块便签正被拖到这一块身上（界面上头部会亮一下）。
     * 自检要量"拖到头部上会亮、压在身子上不算"。
     */
    state.insert(QStringLiteral("dropPreview"), window->dropPreview());
    state.insert(QStringLiteral("qmlError"), window->qmlError());

    /* 界面那几个绑定（QML 侧算出来的值，C++ 这里量的是"真的画成这样了吗"） */
    if (QObject *root = window->qmlRoot()) {
        state.insert(QStringLiteral("cardCount"),
                     root->property("cardCount").toInt());
        state.insert(QStringLiteral("linksExpanded"),
                     root->property("linksExpanded").toBool());
        state.insert(QStringLiteral("editorFontPx"),
                     root->property("editorFontPx").toInt());
        state.insert(QStringLiteral("menuOpened"),
                     root->property("menuOpened").toBool());
        state.insert(QStringLiteral("headerCursor"),
                     root->property("headerCursor").toString());
        state.insert(QStringLiteral("headerHeight"),
                     root->property("headerHeight").toDouble());
        state.insert(QStringLiteral("noteMargin"),
                     root->property("noteMargin").toDouble());
        state.insert(QStringLiteral("editorWidth"),
                     root->property("editorWidth").toDouble());
    }
    return state;
}
