#include "StickyNotes.h"

#include "NoteThumbs.h"
#include "StickyNoteStore.h"

#include <QColorDialog>
#include <QClipboard>
#include <QCursor>
#include <QDialog>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QMoveEvent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantMap>

namespace {

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
     * 锁定 = 整块窗口**鼠标穿透**（Qt::WindowTransparentForInput）。
     *
     * 用这个而不是"把 MouseArea 全关掉"：穿透是窗口级的，连拖动、改大小、
     * 编辑区全都一起让开，点它等于点底下的窗口 —— 这才是"Lock Note"该有的
     * 手感（便签成了一张贴在桌面上的纸）。
     */
    const bool wasVisible = isVisible();
    const QRect geo = geometry();
    setWindowFlag(Qt::WindowTransparentForInput, on);
    setGeometry(geo);
    if (wasVisible)
        show();
    emit lockedChanged();
}

void StickyNoteWindow::setOpacityPercent(int percent) {
    if (m_note)
        m_note->setOpacityPercent(percent);
}

void StickyNoteWindow::toggleStaysOnTop() {
    setStaysOnTop(!m_staysOnTop);
}

QString StickyNoteWindow::clipboardText() const {
    /* 便签正文粘贴用（见头文件里的说明：QML 那边拿不到 Clipboard 单例） */
    return QGuiApplication::clipboard() ? QGuiApplication::clipboard()->text()
                                       : QString();
}

void StickyNoteWindow::placeAt(const QRect &rect) {
    m_placing = true;
    setGeometry(rect);
    m_placing = false;
    if (m_note)
        m_note->setGeometry(geometry());
}

void StickyNoteWindow::rememberGeometry() {
    if (m_note)
        m_note->setGeometry(geometry());
}

void StickyNoteWindow::beginDrag() {
    /*
     * 拖动交给窗口管理器（和主窗口顶栏、贴图窗口同一个理由：贴边吸附、
     * 多屏 DPI 切换都归它管）。它不支持时退回自己搬 —— 这里不实现"自己搬"
     * 是因为便签是全平台都要跑的东西，startSystemMove 在 Windows 上一直有，
     * 真没有的话用户还能用托盘里的"排列便签"。
     */
    if (windowHandle())
        windowHandle()->startSystemMove();
}

void StickyNoteWindow::beginResize() {
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

QString StickyNoteWindow::pickColor() {
    QColor current = m_note ? m_note->color() : QColor(QStringLiteral("#ffe9a8"));
    if (!m_owner)
        return QString();
    return m_owner->pickColor(this, current);
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
}

void StickyNoteWindow::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (!m_placing)
        rememberGeometry();
    /* 便签大小变了，链接卡片那一栏的列数会跟着变 —— 界面自己绑，这里不管 */
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
    }
    /* 新建时的编号接着已有的往下数 */
    m_nextNumber = int(notes.size()) + 1;
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
    if (window && window->note()) {
        window->note()->setVisible(false);
        window->note()->setGeometry(window->geometry());
    }
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

    QList<QRect> taken;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window && window->isVisible())
            taken.append(window->geometry());
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
    bool any = false;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (!window || window == keep || !window->isVisible())
            continue;
        window->closeNote();
        any = true;
    }
    if (any)
        emit changed();
    return any;
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
        m_store->scheduleSave();
        emit changed();
    }
}

void StickyNotes::hideAll() {
    bool any = false;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window && window->isVisible()) {
            window->closeNote();
            any = true;
        }
    }
    if (any)
        emit changed();
}

void StickyNotes::toggleShowAll() {
    if (visibleCount() > 0)
        hideAll();
    else
        showAll();
}

bool StickyNotes::arrangeAll() {
    /*
     * 一键排列：把摆着的那几块在它们所在的屏上摆成一个网格。
     *
     * 尺寸先按便签现在的平均大小算列数，再按"装不下就压一点"回调一次 ——
     * 目标是**一屏能看全**，而不是保持原尺寸铺到屏幕外面去（那就失去
     * "一键排列"的意义了）。压到 kArrangeMin* 就到底，剩下的靠滚动 / 手动挪。
     */
    QList<StickyNoteWindow *> windows;
    for (const QPointer<StickyNoteWindow> &window : std::as_const(m_windows)) {
        if (window && window->isVisible())
            windows.append(window);
    }
    if (windows.isEmpty())
        return false;

    /* 用第一块的位置决定在哪块屏上排（用户看到的就是这块屏） */
    const QRect area = workAreaFor(windows.first()->geometry());

    /* 平均尺寸（用户可能把某一块拉得很大，别让它一个人决定列数） */
    int sumW = 0;
    int sumH = 0;
    for (StickyNoteWindow *window : std::as_const(windows)) {
        sumW += window->width();
        sumH += window->height();
    }
    int cellW = clampInt(sumW / windows.size(), kArrangeMinW, 520);
    int cellH = clampInt(sumH / windows.size(), kArrangeMinH, 480);

    /* 按网格摆：先算一遍有几个格子，装不下就把格子缩小一点再算 */
    for (int attempt = 0; attempt < 12; ++attempt) {
        const int usableW = qMax(1, area.width() - kArrangeGap);
        const int usableH = qMax(1, area.height() - kArrangeGap);
        const int columns = qMax(1, (usableW + kArrangeGap) / (cellW + kArrangeGap));
        const int rows = (windows.size() + columns - 1) / columns;

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

    int index = 0;
    for (StickyNoteWindow *window : std::as_const(windows)) {
        const int row = index / columns;
        const int column = index % columns;
        ++index;

        const int x = area.x() + column * (cellW + kArrangeGap);
        const int y = area.y() + row * (cellH + kArrangeGap);
        window->placeAt(QRect(x, y, cellW, cellH));
        window->raise();
    }

    m_store->scheduleSave();
    emit arrangementChanged();
    emit changed();
    return true;
}

QString StickyNotes::pickColor(QWidget *parent, const QColor &current) {
    QColor start = current.isValid() ? current : QColor(QStringLiteral("#ffe9a8"));

    /*
     * 自己建对话框，而不是 QColorDialog::getColor()。
     *
     * 原因：便签默认就摆在屏幕**右上角**，而 QColorDialog 是"居中到父窗口"的
     * —— 父窗口贴右沿时，取色框有半个跑到屏幕外（实测：窗口落在 +3233+31，
     * 而屏幕只有 3840 宽，右边那截根本点不到）。这里改成建完自己把它摆到
     * **便签所在那块屏的中间**，无论便签贴在哪个角都点得到。
     *
     * DontUseNativeDialog：只有非原生实现才归 Qt 管几何（原生那个由系统摆，
     * 我们 move 不动它）。代价是长相是 Qt 那套；对这个"选个背景色"的用途
     * 够用了，而且和应用其它对话框是一套观感。
     */
    QColorDialog dialog(start, nullptr);
    dialog.setWindowTitle(QStringLiteral("便签背景颜色"));
    dialog.setOption(QColorDialog::DontUseNativeDialog, true);

    /* 摆到便签所在那块屏的中间（拿不到屏就退回主屏） */
    QScreen *screen = parent && parent->screen() ? parent->screen()
                                                 : QGuiApplication::primaryScreen();
    if (screen) {
        const QRect area = screen->availableGeometry();
        dialog.adjustSize();
        const QSize hint = dialog.sizeHint().expandedTo(dialog.minimumSize());
        dialog.move(area.x() + (area.width() - hint.width()) / 2,
                    area.y() + (area.height() - hint.height()) / 2);
    }

    if (dialog.exec() != QDialog::Accepted)
        return QString();
    const QColor picked = dialog.currentColor();
    return picked.isValid() ? picked.name() : QString();
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
    state.insert(QStringLiteral("swatchCount"), menu->property("swatches").toList().size());

    /* 色板第一格 / 打开那一刻的几条（自检核对色板内容和条目文字） */
    QVariant first;
    QMetaObject::invokeMethod(menu, "swatchAt", Q_RETURN_ARG(QVariant, first),
                              Q_ARG(QVariant, QVariant(0)));
    state.insert(QStringLiteral("firstSwatch"), first.toString());

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
    QMetaObject::invokeMethod(menu, "openFlyoutForTest", Q_ARG(QVariant, QVariant(kind)));
    return true;
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
    state.insert(QStringLiteral("x"), window->x());
    state.insert(QStringLiteral("y"), window->y());
    state.insert(QStringLiteral("w"), window->width());
    state.insert(QStringLiteral("h"), window->height());
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
