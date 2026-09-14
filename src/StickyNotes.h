#pragma once

#include <QColor>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QWidget>

class QQuickWidget;
class QQmlEngine;
class NoteThumbs;
class StickyNote;
class StickyNoteStore;
/* 便签总管（本文件末尾那个）；窗口要一个指回它的裸指针，见 m_owner */
class StickyNotes;

/*
 * 一块桌面便签的窗口。
 *
 * 就是**一个置顶无边框小窗 + 一个 QQuickWidget**，界面在
 * qml/notes/StickyNoteWindow.qml 里 —— 和主窗口（src/main.cpp 的 QWidget 托
 * QQuickWidget）、截图选区窗口（src/Screenshot.cpp）是同一个套路。
 *
 * 为什么不用 QML 的 Window：
 *   * 置顶 / 无边框 / 不进任务栏（Qt::Tool）都是窗口标志位，C++ 这边一行一个；
 *   * 拖动和拖边改大小在这个项目里统一走 startSystemMove / startSystemResize
 *     （贴边吸附、多屏 DPI 切换都归窗口管理器管，比手算增量跟手）；
 *   * 便签要能"关掉但留着数据"（下次从托盘再叫出来），窗口和数据是两件事，
 *     窗口的生死由 C++ 的 StickyNotes 管更直白。
 *
 * QML 侧拿到的上下文属性（见构造函数的 setContextProperty）：
 *   note      这条便签（StickyNote：正文 / 底色 / 链接清单）
 *   noteWin   这个窗口（拖动 / 改大小 / 关闭 / 置顶开关）
 *   noteWinId 身份（标题栏上"便签 3"里的那个号，自检也用）
 */
class StickyNoteWindow final : public QWidget {
    Q_OBJECT

    /* 便签纸编号（只是给标题栏显示用的序号，不是身份；身份见 StickyNote::id） */
    Q_PROPERTY(int noteNumber READ noteNumber WRITE setNoteNumber)

    /*
     * 置顶（默认开，菜单里能关掉，关了就被别的窗口压住）。
     *
     * pinned 是 QML 里给菜单打勾用的名字；staysOnTop 保留原属性名（自检在用）。
     */
    Q_PROPERTY(bool staysOnTop READ staysOnTop WRITE setStaysOnTop NOTIFY staysOnTopChanged)
    Q_PROPERTY(bool pinned READ staysOnTop NOTIFY staysOnTopChanged)

    /*
     * 锁定：整块便签**鼠标穿透**（点它等于点底下的窗口）。
     *
     * 对应 Windows 便签菜单里的 Lock Note。锁上之后这块便签就是"贴在桌面上
     * 的一张纸"：点不到、拖不动、也打不了字 —— 所以解锁必须有**另一个**
     * 入口，见 StickyNotes::unlockAll（托盘和主界面那两条菜单里都有）。
     */
    Q_PROPERTY(bool locked READ locked WRITE setLocked NOTIFY lockedChanged)

    /* 这条便签正文里的链接条数（链接那一栏的显示条件） */
    Q_PROPERTY(int linkCount READ linkCount NOTIFY linkCountChanged)

public:
    /*
     * note 是这条便签（归 StickyNotes 所有，别删）；engine 是主引擎（必须传：
     * QQuickWidget 不传就会自己建一个）；owner 是便签总管 —— 窗口要靠它开
     * 取色框、删自己，见 member 上的说明。
     */
    StickyNoteWindow(StickyNote *note, QQmlEngine *engine, StickyNotes *owner = nullptr);
    ~StickyNoteWindow() override;

    StickyNote *note() const { return m_note; }

    /*
     * QML 根对象（自检量界面状态用）。
     *
     * 注意 QQuickWidget 里那个根项**永远是 visible 的**，窗口藏起来它也不变
     * —— 判断"便签露没露在桌面上"要看窗口自己（见 isVisible）。
     */
    QObject *qmlRoot() const;

    /*
     * 界面加载失败时那条错误（成功返回空串）。
     *
     * 自检拿它钉住"便签窗口的 QML 真的建起来了"这件事：QQuickWidget 加载失败
     * 时不会崩，只是一个空白窗口 —— 光看"窗口在不在"是发现不了的。
     */
    QString qmlError() const;

    /* 「⋯」菜单要用：便签窗口所在那块屏的可用区域（屏幕坐标） */
    Q_INVOKABLE QRect screenBounds() const;

    /*
     * 「⋯」菜单要用：鼠标这会儿在屏幕上的位置。
     *
     * 为什么要绕 C++：QML 里没有取光标位置的原语（QCursor 是 QtGui 的 C++ 类，
     * 便签这份 QML 里写 QCursor 会报 "QCursor is not defined"）。而菜单判断
     * "要不要把子面板收掉"必须看光标的真实位置 —— 只看 hover 事件的话，鼠标
     * 从主栏那条缝挪到子面板上时面板会闪掉（见 NoteMenu 的 cursorInsideMenu）。
     */
    Q_INVOKABLE QPoint cursorPos() const;

    /*
     * 「⋯」菜单要用：便签窗口自己的矩形（屏幕坐标）。
     *
     * 菜单拿它来**躲开便签**：便签贴近屏幕右边时，子面板往右挂就会压在便签
     * 身上（或者被屏幕夹回来压上去），看着就是"便签把菜单盖住了"。有了这个
     * 矩形，菜单就能判断"右边那块被便签占着"并改挂左边。
     */
    Q_INVOKABLE QRect noteRect() const;

    int noteNumber() const { return m_noteNumber; }
    void setNoteNumber(int number);

    bool staysOnTop() const { return m_staysOnTop; }
    void setStaysOnTop(bool on);

    bool locked() const { return m_locked; }
    /*
     * Q_INVOKABLE 是必须的：菜单里"锁定（鼠标穿透）"那条从 QML 调它
     * （NoteMenu 的 fire() / StickyNoteWindow 的 handleMenuAct），
     * 只写成普通成员函数 QML 是看不见的 —— 报的是
     * "Property 'setLocked' of object … is not a function"。
     */
    Q_INVOKABLE void setLocked(bool on);

    int linkCount() const;

    /* 把便签摆到屏幕上的 rect（逻辑像素）；同时写回 StickyNote 的几何 */
    void placeAt(const QRect &rect);
    /*
     * 同上，但可以从 QML / 自检里调（Q_INVOKABLE）。
     *
     * placeAt 是普通成员函数，QMetaObject::invokeMethod 找不到它（报
     * "No such method StickyNoteWindow::placeAt(QRect)"）—— 自检要把便签摆到
     * 屏幕某个角上量菜单行为，得有这个口子。
     */
    Q_INVOKABLE void setPlacement(const QRect &rect) { placeAt(rect); }
    /* 记下当前几何（拖动 / 改大小之后调；load 时和 placeAt 分开） */
    void rememberGeometry();

    /* ---- QML 调的（界面动作） ---- */
    /* 标题栏上按住：交给窗口管理器拖动（和主窗口顶栏、贴图窗口同一个做法） */
    Q_INVOKABLE void beginDrag();
    /* 右下角按住：交给窗口管理器改大小 */
    Q_INVOKABLE void beginResize();
    /* 关掉这块便签：藏起来（数据还在，托盘里能再叫出来） */
    Q_INVOKABLE void closeNote();
    /*
     * 彻底删掉这条便签（数据一起没）。
     *
     * 入口在头部那个「⋯」菜单里（Delete Note）—— 便签就一个小窗，
     * 塞不下第二排按钮，而"删掉一条"又是必须有的出口（不然便签只会越攒越多）。
     */
    Q_INVOKABLE void deleteNote();
    /* 换底色 */
    Q_INVOKABLE void setNoteColor(const QString &color);
    /* 开系统取色框，返回 "#rrggbb"（取消返回空串） */
    Q_INVOKABLE QString pickColor();
    /* 复制正文 / 打开链接（转给 StickyNote） */
    Q_INVOKABLE void copyText();
    Q_INVOKABLE bool openLink(int index);
    Q_INVOKABLE void toggleStaysOnTop();
    /* 不透明度按档位走一格（菜单里 Opacity 那几条） */
    Q_INVOKABLE void setOpacityPercent(int percent);

    /*
     * 剪贴板里的纯文本（便签正文 Ctrl+V 粘贴用）。
     *
     * 为什么不直接在 QML 里用 `Clipboard.text`：便签这份 QML 跑在
     * QQuickWidget 的引擎里，那个 `Clipboard` 单例在这儿根本解析不到 ——
     * 一按 Ctrl+V 就刷 "ReferenceError: Clipboard is not defined"
     * （粘贴本身走了 TextEdit 的默认处理，所以看着还能贴上，日志里一堆红）。
     * 走 C++ 拿最稳，也顺便是"纯文本粘贴"（便签不吃外来样式）。
     */
    Q_INVOKABLE QString clipboardText() const;

    /*
     * 自检用：当作用户在编辑区里敲了字。
     *
     * 只调 note.setText() 是不够的 —— 那条路绕过了 QML 的 TextEdit，测不到
     * "编辑区 -> 数据 -> 链接卡片重排"这条真实链路（而且 TextEdit 那边还
     * 绑着旧内容，看着像没生效）。这里把窗口里那个 TextEdit 的 text 也改掉，
     * 走的就是用户敲字那条路（它的 onTextChanged 会把内容写回 note）。
     * 界面上没有入口 —— 便签的正文就是直接在编辑区里敲。
     */
    Q_INVOKABLE void typeText(const QString &text);

signals:
    void staysOnTopChanged();
    void lockedChanged();
    void linkCountChanged();
    /* 便签被藏起来了（StickyNotes 据此把它从"摆着的那批"里划掉） */
    void closed();

protected:
    /* 拖动 / 改大小结束：几何变了要落盘（见 StickyNotes::onWindowMoved） */
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void applyWindowFlags();

    StickyNote *m_note = nullptr;
    QQmlEngine *m_engine = nullptr;
    QQuickWidget *m_view = nullptr;
    /*
     * 便签总管（不归本对象所有）。
     *
     * 原来这里用的是 QObject 的 parent()，但 StickyNotes 是 QObject 不是
     * QWidget，当不了 QWidget 的 parent —— 于是窗口的 parent 一直是空，
     * `qobject_cast<StickyNotes *>(parent())` 拿到 null，"更多颜色"点下去
     * 静默无效（见 .cpp 构造函数里那段踩坑记录）。现在改成显式的裸指针。
     */
    StickyNotes *m_owner = nullptr;
    int m_noteNumber = 0;
    bool m_staysOnTop = true;
    /* 锁定（鼠标穿透），见 Q_PROPERTY 的说明 */
    bool m_locked = false;
    /*
     * 正在 placeAt() 里改几何：那会儿的 move/resize 事件不是用户拖的，
     * 不该反过来再写一次位置（会把刚摆好的坐标写歪）。
     */
    bool m_placing = false;
};

/*
 * 便签总管（QML 单例 Notes，见 src/main.cpp 的 qmlRegisterSingletonInstance）。
 *
 * 它一个人管三件事：
 *   1) **数据的生死**：StickyNoteStore（notes.json）+ 每条便签对应的窗口；
 *   2) **桌面上的摆放**：新建时找空位（见 nextFreeRect）、一键排列（arrangeAll）、
 *      全部叫出来 / 全部收起来；
 *   3) **入口**：托盘菜单、左侧图标条、文件菜单、全局热键最后都落到这里的
 *      createNote() / arrangeAll() / toggleShowAll()。
 *
 * 便签窗口**不设父子**（顶层窗口，和截图那个贴图窗口一样），所以它们的生死
 * 由 m_windows 这份清单负责：窗口 close 之后是 hide 而不是删（数据留着），
 * 真正删是 deleteNote()。进程退出时 QWidget 那套会按顶层窗口自己收尾。
 */
class StickyNotes final : public QObject {
    Q_OBJECT

    /* 一共几条便签 / 几条正摆在桌面上（托盘菜单和自检都用） */
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int visibleCount READ visibleCount NOTIFY changed)

    /* 便签纸编号从几号开始摆着（状态栏那种"3 / 8"的显示用不到，先留着） */
    Q_PROPERTY(QString notesFilePath READ notesFilePath CONSTANT)

public:
    /*
     * engine 是主窗口那个 QQuickWidget 的引擎（可为 nullptr，那样链接卡片就
     * 没有缩略图 —— 只有 image://stickythumb/… 取不到图而已，别的一切照常）。
     * 它只在构造函数里用一次（挂图片提供者），**不**登记成父子关系：
     * 引擎是宿主 QWidget 的子对象，而本对象比那个 QWidget 活得久（见 main.cpp
     * 里的声明顺序），挂上去就会"父先没、子跟着被删两次"。
     */
    explicit StickyNotes(QQmlEngine *engine = nullptr, QObject *parent = nullptr);
    ~StickyNotes() override;

    /*
     * 把缩略图的图片提供者挂到引擎上（幂等：同一个引擎调两次只会挂一份）。
     * main.cpp 里是在 quick（QQuickWidget）建好之后调的 —— 构造那会儿引擎
     * 还不存在。引擎传 nullptr 不做事。
     */
    void attachEngine(QQmlEngine *engine);

    /* 把 notes.json 读进来，恢复上次摆着的那些便签（启动时调一次） */
    void start();

    /*
     * 退出前的收尾：先把便签窗口关掉，再掐断缩略图的网络请求、落盘。
     *
     * main.cpp 在返回之前调一次（**必须**在事件循环还活着的时候）——
     * 那些网络 reply 留到进程收尾阶段才拆会踩空，症状是退出码 0xC0000005
     * （自检全过也会崩）。幂等。
     */
    void shutdown();

    /* notes.json 的位置（自检会念出来） */
    QString notesFilePath() const;

    int count() const;
    int visibleCount() const;

    /* 托盘 / 菜单 / 热键都走它：新建一条便签并摆到桌面上 */
    Q_INVOKABLE StickyNote *createNote();

    /* 这条便签的窗口（没建过 / 已经销毁返回 nullptr） */
    StickyNoteWindow *windowFor(StickyNote *note) const;

    /*
     * 一键排列：把所有**摆着**的便签在屏幕上摆成一个整齐的网格
     * （见 .cpp 里的说明：列数按工作区宽度算，行高按便签高度算，超出屏幕
     * 高度就缩一点）。没有可摆的返回 false。
     */
    Q_INVOKABLE bool arrangeAll();

    /* 全部叫到桌面上（托盘"显示全部便签"） */
    Q_INVOKABLE void showAll();
    /* 全部收起来（数据留着） */
    Q_INVOKABLE void hideAll();
    /* 有摆着的就全收起来，一条都没摆就把全部叫出来（图标条那一格） */
    Q_INVOKABLE void toggleShowAll();

    /*
     * 把除 keep 之外的便签都收起来（菜单里的「收起其他便签」）。
     *
     * keep 给空就是全部收起来。没有可收的返回 false。
     */
    Q_INVOKABLE bool hideOthers(StickyNoteWindow *keep);

    /*
     * 把这块便签正文里的第 index 条链接**滚进可视区**（菜单"链接"那一栏里
     * 点一条走这里）。
     *
     * 定位逻辑在 QML 里（它才知道 TextEdit 的光标怎么摆，见
     * StickyNoteWindow.qml 的 revealLink），C++ 这边只负责找到根对象转发。
     */
    Q_INVOKABLE bool revealLink(StickyNote *note, int index);

    /* 关掉一条并删掉它的数据（便签窗口上那个"删除"走这里） */
    Q_INVOKABLE void deleteNote(StickyNote *note);

    /*
     * 便签纸的调色板（「⋯」菜单里那个色板按它摆色块）。
     *
     * 转发 StickyNote::palette() 那份表 —— 只有一处定义，新建便签的默认色
     * 和菜单里能挑的颜色才不会各说各话。
     */
    Q_INVOKABLE QStringList palette() const;

    /*
     * 解开所有锁定的便签。
     *
     * 锁定的便签**鼠标穿透**、点不到（见 StickyNoteWindow::locked），所以
     * 解锁必须有另一个入口 —— 托盘菜单和主界面菜单里各有一条走这里。
     * 没有锁定的返回 false（菜单据此决定要不要灰掉）。
     */
    Q_INVOKABLE bool unlockAll();
    /* 锁着几条（菜单显示用） */
    Q_INVOKABLE int lockedCount() const;

    /* 开系统取色框（便签窗口调；父窗口用那块便签，弹窗才不会被压住） */
    QString pickColor(QWidget *parent, const QColor &current);

    /* 便签的正文 / 颜色变了：界面上的条数 / 链接卡片要跟着走（QML 绑定用） */
    StickyNoteStore *store() const { return m_store; }

    /* ---- 下面几个给自检用（见 src/SelfTest.cpp） ---- */
    QObject *windowForId(const QString &id) const;
    QObject *windowRootForId(const QString &id) const;
    /* 便签窗口的界面状态（自检量颜色 / 链接卡片 / 提示） */
    QVariantMap windowState(const QString &id) const;

    /*
     * 从 QML 往 stderr 打一行（临时排查用，和截图那边 Shot.uiTrace 同一套）。
     * 便签的界面点击进不到 C++ 时，这是唯一能确认"到底谁接到了这一下"的口子。
     */
    Q_INVOKABLE void uiTrace(const QString &text) const;

    /*
     * 「⋯」菜单的状态（自检用）。
     *
     * 菜单是独立原生弹窗（Popup.Window），外面既点不出 hover、也不好量它
     * 摆在哪 —— 只能进进程去问 QML：开着没、当前展开的是哪一块飞出面板、
     * 整份弹窗落在屏幕上的矩形、色板里第 0 格是什么颜色。
     */
    QVariantMap menuState(const QString &noteId) const;
    /* 让菜单展开某一块飞出面板（""=收起）：界面上是鼠标停上去触发的 */
    bool openMenuFlyout(const QString &noteId, const QString &kind);

signals:
    void changed();
    /* 新建 / 删除之后：界面（图标条那格的提示）刷新条数 */
    void arrangementChanged();

private:
    StickyNoteWindow *ensureWindow(StickyNote *note);
    /*
     * 把一块便签摆到桌面上。
     *
     * 摆过（note 有 geometry）就把上次的位置还回去，并夹进那块屏的工作区里
     * —— 屏幕拔掉 / 分辨率改了之后，旧坐标可能整块落在屏幕外，那样便签就
     * 成了"任务栏里看得见、桌面上找不到"。
     *
     * 没摆过就找一块没被别的便签占着的空位（见 nextFreeRect）。
     * start() / createNote() / showAll() 都走这一个入口，三处行为一致。
     */
    void placeWindow(StickyNoteWindow *window, StickyNote *note);
    /*
     * 给新便签找一块没被占用的桌面位置：**从屏幕右上角往左**按网格扫，
     * 排满一行换下一行；全被占满就退回工作区左下角。
     * （右上角是桌面上最不挡事的地方，见 .cpp 里的说明。）
     */
    QRect nextFreeRect(const QSize &size) const;
    /* 所有便签窗口所在的屏幕工作区（多屏时按便签现在在哪块屏算） */
    QRect workAreaFor(const QRect &hint) const;
    void onWindowClosed(StickyNoteWindow *window);

    QQmlEngine *m_engine = nullptr;
    NoteThumbs *m_thumbs = nullptr;
    StickyNoteStore *m_store = nullptr;
    /* 便签窗口清单（按创建顺序，排列时就是摆的顺序） */
    QList<QPointer<StickyNoteWindow>> m_windows;
    /*
     * 便签纸编号：每建一条 +1。
     *
     * 它只是"便签 3"里那个号，不参与身份（身份是 StickyNote::id）——
     * 从文件恢复时不重排编号会看着乱，所以 start() 里把计数器顶到
     * "已恢复的条数"往上。
     */
    int m_nextNumber = 1;
};
