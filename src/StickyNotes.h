#pragma once

#include <QColor>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QString>
#include <QVariantList>
#include <QWidget>

class QTimer;

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

    /*
     * 一摞便签那三样（QML 侧只读，见 qml/notes/StickyNoteWindow.qml 的 TapHandler）：
     *
     *   inGroup     在不在某一摞里（决定菜单显示"组合成摞"还是"拆分组合"）
     *   groupSize   摞里有几块
     *   groupActive 是不是这一摞里**最上面那块**（完整露出来的那张纸）
     *
     * 都是现问总管算的（见 StickyNotes::groupMembers），窗口自己不缓存 ——
     * 归堆是总管的事，窗口只负责把状态给界面读。
     */
    Q_PROPERTY(bool inGroup READ inGroup NOTIFY groupChanged)
    Q_PROPERTY(int groupSize READ groupSize NOTIFY groupChanged)
    Q_PROPERTY(bool groupActive READ groupActive NOTIFY groupChanged)

    /*
     * 这一刻有没有一块便签正被拖到**这一块**身上（拖头部叠过来）。
     *
     * 界面上头部会亮一下 —— 不然用户不知道"松手会发生什么"。这是"组合"那个
     * 动作的落点提示（见 StickyNotes::updateDropTarget），和"排成一摞"无关。
     */
    Q_PROPERTY(bool dropPreview READ dropPreview NOTIFY dropPreviewChanged)

    /*
     * 这一块窗口左边那条标签条上画什么：**桌面上摆着的每一块便签**一个色块
     * （不只是一摞里的那几块 —— 见 groupTabs 的说明）。
     * 每条 { id, color, number, selected }；摆着的便签少于两块就是空表。
     */
    Q_PROPERTY(QVariantList groupTabs READ groupTabs NOTIFY tabsChanged)
    /*
     * 这一块在摞里、又不是露头那张 -> 界面上收成左边标签条上一个色块
     * （正文不显示，见 qml/notes/StickyNoteWindow.qml）。
     */
    Q_PROPERTY(bool tabbed READ tabbed NOTIFY groupChanged)

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

    /* ---- 组合 / 排列成摞（见上面那几个 Q_PROPERTY 的说明） ---- */
    bool inGroup() const;
    int groupSize() const;
    bool groupActive() const;
    bool dropPreview() const { return m_dropPreview; }
    /* 落点候选的标记（只由 StickyNotes 在拖动期间改） */
    void setDropPreview(bool on);

    /*
     * 把这块便签抽到它那一摞的最上面（界面上是"点了叠在下面的那张纸"）。
     *
     * 便签本身是**独立的原生窗口**，叠着的部分被上面那几块盖住，点下去落在
     * 最上面那块身上 —— 所以界面侧只能从"这一块被点了"这件事上做文章
     * （QML 那个 TapHandler 的 onTapped），不能指望系统帮我们分发。
     *
     * 只认"用户真的点了这一块"：这一块得是**当前活动窗口**（真点时窗口管理器
     * 会先激活它）。TapHandler 是旁听鼠标事件的，别处点一下、这一块刚好在那时
     * 候露出来也会顺带触发，所以这道判断不能省（见 .cpp 里的踩坑记录）。
     * force 给 true 就跳过这道判断 —— **只给自检用**（自检没法保证谁是活动
     * 窗口），界面上永远不传。
     *
     * 不在摞里、或者本来就在最上面，返回 false（界面据此不重排，免得白抖一下）。
     */
    Q_INVOKABLE bool promoteInGroup(bool force = false);

    /* ---- 左边那条标签条（一个色块一块便签） ---- */
    /*
     * 色块本身多大（正方形的一格，参考图里就是这个比例）。
     *
     * 单独一个数是因为**选中的那块要比其余的宽一截**（见 chipProtrude）：
     * 色块的右沿是对齐的，选中那块往纸外多伸出去一段，于是它的宽度不等于
     * 标签条的宽度。界面（StickyNoteWindow.qml）用同一个数画，两边不会各说各话。
     */
    static int chipSize();
    /*
     * 选中的那块往外多伸多少（其余几块缩回去这么多）。
     *
     * 参考图（用户给的 partThreeGif.gif 第 22 帧）里就是靠这个位移差表示
     * "现在看的是这一张"，不是靠描边 —— 描边我们也留着。
     */
    static int chipProtrude();
    /*
     * 界面画标签条要用的三个数（{ strip, chip, protrude }）。
     *
     * 走函数而不是抄死数字：QML 那边早先把条宽写成 `on ? 34 : 0`，C++ 一改
     * 宽度两边就对不上（选中那块伸出去的一截被窗口左边裁掉）。
     */
    Q_INVOKABLE QVariantMap chipMetrics() const;
    /*
     * 标签条占的宽度（色块 + 选中那块多伸出去的一截 + 它和便签纸之间那条缝）。
     *
     * 窗口 = 标签条 + 便签纸（见 frameRectFor）：窗口比卡片宽这么多，卡片
     * 本身的位置和尺寸一点没变 —— 自检量"色块整块落在纸外面"就是靠这个。
     */
    static int tabStripWidth();
    /*
     * 标签条上的色块：**桌面上摆着的每一块便签**一个（自己也在里面）。
     *
     * 这条标签条是**排列 / 切换**那件事（"现在看哪一块"），和"组合"（哪几块
     * 归到一起）没有关系 —— 没组合过的几块便签照样各有一个色块，点一下就能
     * 换到那一块。用户要的就是把这个和组合分开（早先这里只列同一摞里的成员，
     * 于是"想换一块看"必须先组合，两件事被绑死了）。
     *
     * 藏起来的那几块不列：它们不在桌面上，列出来点一下等于把它们叫出来。
     * 便签编号（第几个建出来的）排，顺序恒定 —— 换标签只挪"选中圈"。
     */
    QVariantList groupTabs() const;
    bool tabbed() const;
    /*
     * 标签条在不在：**这一块属于某一摞（组合）**就在（见 groupTabs）。
     *
     * 现在标签条只由**组合**决定（用户明确要求："这个左边的 tab 不是每个卡片
     * 都有，只有组合的才有"）：C++ 在组合建了 / 拆了 / 组里少了一块时调它，
     * 窗口据此决定自己的窗口宽度里让出多少给标签条（见 frameRectFor）。
     *
     * card 传"改这一条宽度**之前**量到的卡片矩形"（调用方一般先 cardRect()
     * 再调它）。宽度一变窗口矩形就变，函数内部会按这个位置把窗口重摆一次，
     * 卡片因此待在原地不动；传空矩形就自己现量一次。
     */
    void setTabStrip(bool on, const QRect &card = QRect());
    /*
     * 标签条占的宽度（0 = 没有）。
     *
     * 注意这个宽度是**窗口多出来的那一条**：窗口 = 标签条 + 卡片。
     * 便签自己的几何（note->geometry()）始终是**卡片**的位置和尺寸 ——
     * 界面、自检、吸附都以卡片为准，只有窗口才是多一条的那个框。
     */
    int tabMargin() const { return m_tabMargin; }
    /*
     * 卡片矩形 -> 窗口矩形：窗口右边 / 上边 / 下边跟卡片对齐，左边多让出
     * 标签条那一条（色块在纸外面、贴着桌面，纸一点没挪）。
     * 反过来的换算在 .cpp 的 cardRect()。
     */
    QRect frameRectFor(const QRect &card) const;
    /* 自检 / 对齐用：这一块**卡片**（不含标签条）坐在哪儿 */
    QRect cardRect() const;
    /*
     * 点左边标签条上的一个色块：把那一块便签抽到最前面（它成为"现在看的这块"）。
     *
     * 不动组合、不动可见性 —— 就是"看那一块"。摆着的每一块都有自己的标签条，
     * 所以点自己那一块是空操作（返回 false，界面据此不用重画）。
     */
    Q_INVOKABLE bool selectGroupTab(const QString &noteId);

    /* 组合状态变了（总管重排完那一摞之后发），界面据此刷新 */
    void notifyGroupChanged();
    /* 标签条的内容变了（新建 / 删除 / 收起 / 叫出来）：各窗口重画色块 */
    void notifyTabsChanged();

/*
 * 一摞便签：把这一块摆到算好的位置（只给 StickyNotes 同步整摞时用）。
 *
 * 它和 placeAt 的差别只有一处：这里的移动**不该**反过来再带一次整摞 ——
 * 整摞同步时每一块都会动，那些 moveEvent 要是各自再同步一遍，几块窗口会
 * 互相推着走（见 .cpp 里的实现）。
 */
    void setGroupGeometry(const QRect &rect);
    /*
     * 直接挪**窗口**（frame）：整摞一起搬时按"窗口位移"走，各块的卡片才会跟
     * 着移动同一个距离（卡片 = 窗口右移一个 tabMargin，而各块那条宽度不一定
     * 一样，按卡片算位移的话整摞会被拉歪）。同时把卡片位置写回 note。
     */
    void moveFrameTo(const QPoint &frameTopLeft);

    /* 把便签摆到屏幕上的 rect（逻辑像素）；同时写回 StickyNote 的几何 */
    void placeAt(const QRect &rect);
    /*
     * 拖动结束（松手 / 收尾）：把"用户正按着这一块"那个标记摘掉。
     *
     * 不摘的话，这一块**下一次**被挪动（换标签、整摞重排、程序摆位）还会按
     * "整摞同步"去算一遍位移 —— 窗口会莫名其妙地再跳一格（踩过：自检里
     * "把窗口摆到鼠标松开的地方"那一步，摆过去又自己走了 26px/34px）。
     */
    void endDragMarker();
    /*
     * 把便签摆到屏幕上的 rect，可以从 QML / 自检里调（Q_INVOKABLE）。
     *
     * placeAt 是普通成员函数，QMetaObject::invokeMethod 找不到它（报
     * "No such method StickyNoteWindow::placeAt(QRect)"）—— 自检要把便签摆到
     * 屏幕某个角上量菜单行为，得有这个口子。
     */
    Q_INVOKABLE void setPlacement(const QRect &rect) { placeAt(rect); }
    /* 记下当前几何（拖动 / 改大小之后调；load 时和 placeAt 分开） */
    void rememberGeometry();
    /*
     * 强制同步渲染一帧，把这一帧真推到屏幕上。两个地方要用它：
     *
     * 1) **补第一帧**：从文件恢复时，一摞里那几块是"show 出来 -> 同一趟里
     *    hide 掉"的手，一帧都没画上。第一次点它的标签，屏幕上就是约 2 帧
     *    **整块卡片都不在**（露桌面），用户报的"第一次点开有黑影 / 位置闪"
     *    就是这两帧。所以恢复那一趟在收起来之前先补一次（见 StickyNotes::start）。
     * 2) **堵改宽那一帧**：标签条出现/消失那一刻窗口宽度差 tabStripWidth()，
     *    而纸的左边界要等 QML 下一次布局才跟上，中间那一帧屏幕上就是整张纸
     *    偏一个标签条的宽度（用户："位移正好是左边 tab 的位置"）。所以在
     *    几何和标签状态都改完之后再补一次（见 StickyNotes::refreshTabs）。
     */
    void renderOneFrameNow();

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
    /*
     * 把这块便签里某个 Popup（QML 的 `Popup.Window`，就是头上那个颜色弹窗）的
     * **原生窗口**顶到置顶带最上面。
     *
     * 为什么非要有它：便签自己是"始终置顶"的（Qt::WindowStaysOnTopHint），弹窗
     * 也是一块独立原生窗 —— 两块都在置顶带里。点颜色按钮那一下被系统激活、抬到
     * 最上面的是**便签**，于是弹窗被压在下面：用户看到的就是"点一次之后，再点
     * 弹窗就被卡片盖住了"（弹窗被夹到便签里面时整块都看不见）。
     *
     * QML 的 Popup 不是 Window，没有 raise() 可调，所以只能进 C++：按 objectName
     * 找到那个 Popup，顺着它的 popupItem 摸到它那块 QQuickWindow 再顶一次。
     * 顶完**下一拍再顶一次**（窗口刚 show 出来的一瞬间排序偶尔会被系统重排回去，
     * 和 NoteMenu 里那个 raiseLater 是同一个道理）。
     */
    Q_INVOKABLE void raisePopupWindow(const QString &objectName);
    /* 复制正文 / 打开链接（转给 StickyNote） */
    Q_INVOKABLE void copyText();
    Q_INVOKABLE bool openLink(int index);
    Q_INVOKABLE void toggleStaysOnTop();
    /* 不透明度按档位走一格（菜单里 Opacity 那几条） */
    Q_INVOKABLE void setOpacityPercent(int percent);

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
    /* 组合状态变了（在不在摞里 / 摞里有几块 / 是不是最上面那块） */
    void groupChanged();
    /* 有没有一块正被拖到这一块身上（落点提示亮不亮） */
    void dropPreviewChanged();
    /* 标签条的内容变了（这一摞增减了成员 / 这一块进了或出了一摞），见 refreshTabs */
    void tabsChanged();
    /* 便签被藏起来了（StickyNotes 据此把它从"摆着的那批"里划掉） */
    void closed();

protected:
    /* 拖动 / 改大小结束：几何变了要落盘（见 StickyNotes::onWindowMoved） */
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void applyWindowFlags();

    /*
     * 一摞便签：拖动 / 改大小时把整摞带上（只在 beginDrag / beginResize 量过
     * 基准之后生效，见 .cpp 里那段说明）。
     *
     * 为什么必须自己搬：拖动走的是 windowHandle()->startSystemMove()，窗口
     * 管理器只认"被按住的那一块" —— 组里其余几块得由我们按同一份位移跟着挪。
     */
    void beginGroupDrag();
    /*
     * 整摞一起搬时的基准：按下那一刻每个成员在哪 + 被按住那块的基准。
     *
     * 用"按下时记下的位置"算位移，而不是"从现在的几何反推"：整摞的几何里有
     * 好几种偏移（层叠阶梯、标签条那一条），反推时要同时对上它们，差一格整摞
     * 就错位（实测：拖动只走了"位移 - 一个阶梯"）。按下时记一份就没有这种
     * 反推了 —— 位移就是"被按住那块现在的位置 - 它按下时的位置"。
     */
    void moveGroupBy(const QPoint &delta);
    void resizeGroupTo(const QSize &size);

    friend class StickyNotes;

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

    /*
     * 整摞拖动的基准：beginDrag() 那一刻记下"这一块在哪、整摞各块相对它
     * 差多少"，之后每次 moveEvent 都按"现在 - 基准"的位移摆其余几块
     * （见 .cpp 里 beginGroupDrag 的说明）。
     *
     * 用固定的基准、而不是"上一次的位置"，是因为便签的移动是窗口管理器在做：
     * 某一次事件被丢掉（拖动路上系统合帧）时，按"上一次"算会把误差留在那一摞
     * 里，越拖越开。
     */
    bool m_groupDragging = false;
    QPoint m_groupDragStart;
    QList<QPoint> m_groupDragOffsets;
    /*
     * 按下那一刻这一摞里每个成员（窗口）在哪 —— 整摞一起搬的基准。
     *
     * 拖动就是"被按住那块走了多少，其余几块也走多少"，有这份基准就不用从当前
     * 几何反推位移（那种反推要同时对上"层叠阶梯"和"标签条那一条"两种偏移，
     * 差一格整摞就错位 —— 实测踩过）。
     */
    QHash<StickyNoteWindow *, QPoint> m_groupDragStartPositions;

    /* 有没有一块便签正被拖到这一块身上（落点提示），见 dropPreview 属性 */
    bool m_dropPreview = false;

    /*
     * 左边那条标签条占的宽度（0 = 没有）。
     *
     * 它**占窗口尺寸**：窗口 = 卡片 + 这一条（见 frameRectFor），卡片自己待在
     * 原地 —— 色块因此落在纸外面、贴着桌面。
     */
    int m_tabMargin = 0;
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

    /*
     * 一键叠成一摞：把桌面上**摆着的每一块**便签（连着它们各自那一摞里藏着
     * 的成员）并成一摞，整摞吸附到**工作区右上角**（和 nextFreeRect 的第 0 格
     * 同一个落点），只露 keepFront 那一张纸，其余收成左边那排色块。
     *
     * 用户要的"全部自动一摞，不用拖动"：归堆这件事本来只有拖一块到另一块身
     * 上（dropNoteOn）和菜单里一块一块挑（groupWith）两条路，两条都要动手；
     * 这里给第三条 —— 点一下就走，摆法（重叠 + 右上角）由函数定，不看原来的
     * 位置。拖放那条路保留（老习惯还在用）。
     *
     * keepFront 给空就用当前露头那块（没有就取编号最小的）。露着的不足两块
     * 返回 false。
     */
    Q_INVOKABLE bool stackAll(StickyNote *keepFront = nullptr);

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
     * 收窗口之前先把"这次要收的那几块"标成不摆着了（keep 那一摞除外，给空就是
     * 全都标）。
     *
     * hideAll / hideOthers 都先走这一步再关窗口，顺序的理由见 .cpp —— 关一块会
     * 触发 repairGroup 那条"这一摞至少留一块露着"的兜底，逐块关会把同组那张
     * 收起来的纸又拉出来。返回标了几条。
     */
    int markHiddenExcept(StickyNoteWindow *keep);

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

    /* -----------------------------------------------------------------------
     * 组合 与 排列成摞（两件不同的事，别混）
     *
     * **组合**：把几块便签归到一摞里（同一个 groupId），动作是"拖一块便签的
     * 头部到另一块身上松手"（见 dropNoteOn / dropTargetFor）。归到一起之后
     * 它们就成了一个整体：拖动整摞一起走、点标签换纸。
     *
     * **摆法**：一摞在桌面上只占一格、只露一张纸（applyGroupLayout 摆位置、
     * showOnlyInGroup 把其余几块收起来）。单独一块便签没有标签条，也不受这套
     * 影响。
     *
     * 归属的规矩：一块便签只属于一摞（没有组中组）；**合并是允许的** —— 把一块
     * 丢到另一块身上时，两摞合成一摞。合并之后**落点那一块留在原地露着**，
     * 被拖的那块并进来、收成标签（见 dropNoteOn 的说明：一摞只摆一张纸，排
     * 第一的就是留下的那张）。
     * --------------------------------------------------------------------- */

    /*
     * 把 note 和 others 归成一摞（note 在最上面）。
     *
     * 两摞合成一摞也走这里：note 原来那一摞、others 各自那一摞，全部归到一起。
     * 已经归在一起的那些原样留着。一条能用的都没有（比如 others 里全是自己）
     * 返回 false，那种情况下不做任何改动。
     *
     * **谁留在桌面上**：note 原本已经归过摞就让它那一摞现在的露头那块继续露着
     * （往摞里加第三、第四块时原来那张不能消失），否则就是 note 自己。
     */
    Q_INVOKABLE bool groupWith(StickyNote *note, const QList<StickyNote *> &others);

    /* 拆开 note 所在的那一摞（各自留在原地，还是一块一块的便签） */
    Q_INVOKABLE bool ungroup(StickyNote *note);

    /* ---- 拖一块便签到另一块身上：这是"组合"那个动作 ---- */

    /*
     * 拖动开始时调一次：记住这是"用户正按着 note 的头部"。
     * （界面上是 beginDrag；松手由 dragPollTimer 那一拍认出来。）
     */
    void beginNoteDrag(StickyNoteWindow *window);    /*
     * 把 note 放到 target 身上（组合动作的落地那一下）。
     *
     * note 和 target 各自那一摞合成一摞：**target 留在原地露着**（用户是把
     * note 丢在它身上的），note 那一摞并进来、收成标签；轮到它时点标签换上来。
     * 返回 false 表示没真做（同一摞、或者哪一块不见了）。
     */
    Q_INVOKABLE bool dropNoteOn(StickyNote *note, StickyNote *target);
    /*
     * 正在拖的是不是这一块（界面用）。
     * at 不给就是"光标这会儿在屏幕上的位置"（界面上就这一种）；自检传一个点，
     * 因为自检不能像人那样按住鼠标把窗口拖到别处 —— 光标一动，被拖的窗口
     * 还在原地，算出来的落点没意义。
     */
    Q_INVOKABLE bool isDragging(StickyNote *note) const;
    /*
     * 这一块这会儿是不是拖动落点的候选（界面上头部会亮一下）。
     * at 的说明同 isDragging。
     */
    Q_INVOKABLE bool dropPreviewAt(StickyNote *note, const QPoint &at) const;
    /* 拖动落点候选 / 收尾（dragPollTimer 每拍调；自检也直接调它） */
    Q_INVOKABLE void updateDropTarget();
    Q_INVOKABLE void finishNoteDrag();

    /*
     * 把一摞摆到该在的那一格：active 指定谁露头（不给就按 activeInGroup 挑），
     * anchorAt 指定那一格的左上角（不给就用现在露头那块的位置）—— 拖一块便签
     * 叠到另一块身上时，得让落点那块留在原地，靠的就是它。
     */
    bool applyGroupLayout(const QString &groupId, StickyNoteWindow *active = nullptr,
                          const QPoint *anchorAt = nullptr);

    /*
     * 桌面这一刻**摆着的**便签（按便签编号排，卡片 -> 窗口）。
     *
     * 给"外面那个框要摆哪儿"用：便签自己的几何是卡片，窗口左边还多一条标签条
     * （见 frameRectFor），换标签 / 收起来时要把那一条也对上。
     */
    QList<StickyNoteWindow *> shownWindows() const;
    /*
     * 标签条的内容变了（组合建了 / 拆了 / 组里少了一块）：每一块便签重画色块，
     * 并按"**这一块自己在不在有两块以上的摞里**"重新量一次左边那条让不让。
     */
    void refreshTabs();
    /*
     * 一摞里**只留 keep 那一块露着**，其余几块摆到同一格再藏起来（keep 给空
     * 就按 activeInGroup 现挑）。桌面上"一摞 = 一张纸 + 左边那排标签"就靠它：
     * 点标签换纸（switchGroupTab）和从文件恢复（start）都走这里。
     *
     * 藏是**当场**的，不推到下一回合 —— 两头都量过：推迟会让旧那块窗口多露约
     * 46ms，而它压在新块下面、新块左边那 42 宽是透明的，旧纸会从那条缝里透出
     * 来（过渡拖到 6 帧，中间夹一帧旧纸的颜色）。黑影那一头改由 switchGroupTab
     * 在收之前先把新块这一帧真呈现出去解决。两笔账都写在 .cpp 那段里。
     */
    void showOnlyInGroup(const QString &groupId, StickyNoteWindow *keep = nullptr);
    /*
     * 点左边标签条上的一个色块：那一块换上来（其余几块收成色块）。
     *
     * 只有**组合过**的便签才有这条标签条（见 StickyNoteWindow::groupTabs），
     * 所以这里不带 groupId 的便签直接返回 false。
     */
    Q_INVOKABLE bool switchGroupTab(const QString &noteId);

    /*
     * 全部便签（给界面 / 自检用：拿一条便签数据换成 id 之类的现算）。
     *
     * 单独开这个口子而不是让 QML 读 store()：store() 不是 Q_INVOKABLE
     * （QML 里读到 undefined），而 StickyNote 也不是注册过的 QML 类型 ——
     * 返回列表比暴露数据源更直白。
     */
    Q_INVOKABLE QList<StickyNote *> noteList() const;

    /*
     * 一个摞的状态（自检用，也方便以后给界面读）：
     *   groupId / count / visibleCount / activeId / members（摞内 id，最上面那块排第一）
     * 没归到摞里的便签返回空表。
     */
    QVariantMap groupState(const QString &noteId) const;

    /* 这一摞里摆着的窗口，顺序就是层叠顺序（最上面那块排第一） */
    QList<StickyNoteWindow *> groupMembers(const QString &groupId) const;
    /* 同上，但只要 id（顺序一样；自检和 groupState 用它报"这一摞有哪几块"） */
    QStringList groupMemberIds(const QString &groupId) const;
    /* 摞里"露出来的那张纸"（它也是拖动/改大小时带动整摞的那一块） */
    StickyNoteWindow *activeInGroup(const QString &groupId) const;
    QString groupIdOf(StickyNoteWindow *window) const;

    /*
     * 拖 / 改大小时由**被按住的那一块**转发过来，整摞跟着走
     * （见 .cpp 里 StickyNoteWindow::beginGroupDrag 那段说明）。
     *
     * reference 是被按住的那一块，origin / size 是它**现在**的几何；
     * move / resize 说明这一次要同步哪一样（改动拖动只同步位置、
     * 改大小只同步尺寸）。
     */
    void syncGroupGeometry(StickyNoteWindow *reference, const QPoint &origin,
                           const QSize &size, bool move, bool resize);

    /*
     * 整摞一起搬：delta 是被按住那块（reference）**窗口**走了多少，其余几块
     * 按同一份位移平移（基准是 beginDrag 那一刻记下的位置）。
     * 见 StickyNoteWindow::beginGroupDrag 的说明。
     */
    void moveGroupByFrameDelta(StickyNoteWindow *reference, const QPoint &delta);

    /*
     * 便签纸的调色板（便签头上那个颜色弹窗按它摆色块）。
     *
     * 转发 StickyNote::palette() 那份表 —— 只有一处定义，新建便签的默认色
     * 和色板里能挑的颜色才不会各说各话。
     *
     * 用它的只有便签头上那个颜色弹窗：右键菜单里原来那条「更多颜色」（Qt 取色框）
     * 已经删掉了。
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
     * 便签菜单的状态（自检用）。
     *
     * 菜单是独立原生弹窗（Window），外面既点不出 hover、也不好量它摆在哪 ——
     * 只能进进程去问 QML：开着没、当前展开的是哪一块飞出面板（现在只剩透明度 /
     * 组合）、整份弹窗落在屏幕上的矩形。
     *
     * 主栏那几条条目的文字一并报出去（labels），自检按它钉"菜单里该有哪几条"、
     * "哪几条已经搬走 / 删掉"。
     */
    QVariantMap menuState(const QString &noteId) const;
    /* 让菜单展开某一块飞出面板（""=收起）：界面上是鼠标停上去触发的 */
    bool openMenuFlyout(const QString &noteId, const QString &kind);

signals:
    void changed();
    /* 新建 / 删除之后：界面（图标条那格的提示）刷新条数 */
    void arrangementChanged();
    /* 组合建了 / 拆了 / 重排了：摞里那几块的界面要跟着刷新 */
    void groupChanged();
    /* 拖动开始 / 结束 / 落点候选变了（界面刷新"正在拖""可以放这儿"的提示） */
    void dragChanged();
    /*
     * 标签条的内容变了（新建 / 删除 / 收起 / 叫出来一块）：**每一块摆着的
     * 便签**的标签条都要重画（色块列表是"摆着的那几块"，谁都得跟着变）。
     */
    void tabsChanged();

private:
    StickyNoteWindow *ensureWindow(StickyNote *note);
    /* 把一串便签 id 换成窗口（没建过 / 已经销毁的那几个直接跳过） */
    QList<StickyNoteWindow *> windowsForIds(const QStringList &ids) const;

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

    /* ---- 组合 与 排列成摞 ---- */
    /*
     * 把 ordered 这一串便签归成一摞（front 是露头那块，anchorAt 是整摞左上角
     * 落在哪），并摆成层叠的样子。groupWith / dropNoteOn / stackAll 都走这里
     * —— 归堆的逻辑只有这一份。
     */
    bool applyGroupInto(const QList<StickyNote *> &ordered, StickyNoteWindow *front,
                        const QPoint *anchorAt = nullptr);
    /* "把 dragged 放下去会落到谁身上"（同一摞 / 不在头部那条上都不算） */
    StickyNoteWindow *dropTargetFor(StickyNoteWindow *dragged, const QPoint &at) const;
    /* 把"落点候选中"的标记从所有便签上抹掉 */
    void clearDropPreview();
    /* 这一摞的界面状态变了：让摞里那几块各自刷新（并重排 z 序） */
    void notifyGroupWindows(const QString &groupId);
    /* 一块便签被藏起来 / 删掉了：把它的摞收拾干净（空摞要忘掉） */
    void forgetGroupIfEmpty(const QString &groupId);
    /*
     * 摞里露头那张纸被藏了 / 删了：换一块顶上并重排（没有别的就什么都没了）。
     * 返回收拾完这一刻"该露着"的那块窗口（一块都不该露着时返回 nullptr）——
     * 删窗口那条路要用它先补一帧再拆（见 StickyNotes::deleteNote）。
     */
    StickyNoteWindow *repairGroup(const QString &groupId);

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

    /*
     * 每一摞"这一刻长什么样"：摞内顺序 memberIds（第一块就是露头那张纸）。
     *
     * 它是**缓存**，不是真相：真相是每条便签身上的 groupId + store 里的先后。
     * 缓存只在"用户点过下面那张纸换了主角、或者拖过"之后才有意义 —— 没有缓存
     * （比如刚重启）时，露头那块就是摞里第一块可见的，顺序按 store 里的先后，
     * 和 groupMemberIds() 的默认算法一致。存 id 不存指针：删掉一块便签之后那份
     * 清单会自然地"对不上"，于是整套退回 store 的顺序重算。
     */
    struct GroupState {
        QStringList memberIds;
    };
    QHash<QString, GroupState> m_groups;
    /* 摞 id 的自增号（"g1"、"g2"…；见 StickyNote::groupId 的说明） */
    int m_nextGroupNumber = 1;

    /*
     * 正在拖的那一块（"拖头部" —— 界面上用户按住头部就是它）。
     *
     * 拖动本身交给窗口管理器（startSystemMove），所以 Qt 这边收不到"松手"，
     * 得靠 dragPollTimer 每拍问一次：光标压在哪块便签身上（落点候选）、鼠标
     * 键还按着没（按着就是还在拖）。见 updateDropTarget / finishNoteDrag。
     */
    QPointer<StickyNoteWindow> m_dragWindow;
    /*
     * 按下那一刻整摞里每个成员（窗口）在哪 —— 整摞一起搬的基准。
     *
     * 拖动就是"被按住那块走了多少，其余几块也走多少"，有这份基准就不用从当前
     * 几何反推位移（那种反推要同时对上"层叠阶梯"和"标签条那一条"两种偏移，
     * 差一格整摞就错位 —— 实测踩过）。
     */
    QHash<StickyNoteWindow *, QPoint> m_dragGroupStartPositions;
    /*
     * 这一块按下时**卡片**在哪：真拖动一定会离开这个点。
     *
     * 用来判"这一下算不算拖动"—— 自检里光标被摆在别处、窗口却"拖动"过，光标
     * 正好压着邻居，会被当成"拖到它身上松手"（组合），凭空把两块并成一摞
     * （踩过：拖动测试里整摞的露头换人、层叠错位）。真用户按下不动、直接松手
     * 也不会离开起点，所以这个闸门对真实操作没有影响。
     */
    QPoint m_dragStartCard;
    /* 拖动期间上一拍光标在哪 + 连着几拍没动（连着不动 = 用户停手了） */
    QPoint m_dragLastCursor;
    int m_dragStillTicks = 0;
    /* 这一刻的落点候选（光标压着的那一块；没有就是空） */
    QPointer<StickyNoteWindow> m_dropTarget;
    QTimer *m_dragPollTimer = nullptr;
};
