#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QRect>
#include <QRegion>
#include <QString>

class QQuickWindow;

/*
 * 最大化 / 还原。
 *
 * 这一层做的是"一次到位、瞬间切换"：不做几何动画，也不做内容淡入。
 * 一次切换的顺序（实现在 .cpp 的 applyState，为什么必须是这个顺序那边写清楚了）：
 *
 *   ① 内容控件先按**目标尺寸**渲染一帧（此刻窗口还是旧尺寸，多出来的部分被
 *      窗口裁掉，屏幕上看不见）；
 *   ② 窗口一步摆到目标矩形；
 *   ③ 紧挨着那一帧只铺界面底色（内容控件临时退场，4K 纯色填充只要一两毫秒）；
 *   ④ 布局归位 + 当场合成内容那一帧；
 *   ⑤ 最后才把最大化状态置上（状态那一下不挪窗口）。
 *
 * ①~⑤ 都在同一个事件循环回合里。
 * 用户报的"最大化时窗口先跑到角上、再放大"漏出来的就是③之前的那一帧 ——
 * 窗口已经换完尺寸、内容还没按新尺寸重排重画，而"先 showMaximized() 再让 Qt
 * 自己把内容跟上来"那版要好几帧才补上（录屏量过：见 build\frames-*）。
 *
 * 那几帧**不是系统转场**：窗口的系统转场在 attachWidget 里用
 * DWMWA_TRANSITIONS_FORCEDISABLED 关掉了，DDA 录屏里那一下没有任何缩放动画，
 * 窗口矩形是"啪"一下到位的。
 *
 * 走过的弯路，留个记录免得再踩：
 *   * 逐帧改窗口尺寸做动画：改尺寸本身几乎免费（实测 35 帧 / 平均 7.1ms），
 *     但每帧都要重排 + 重绘整个界面，4K 下每帧 ~20ms，内容一多更卡，没有上限；
 *   * "抓快照 + GPU 缩放"绕开逐帧缩放：更卡 —— 抓一张 4K 图本身就要几十毫秒；
 *   * 拿窗口区域（SetWindowRgn）当幕布盖住换尺寸那一小段：区域用的是窗口内坐标，
 *     而"挪窗口"和"换形状"是两个 API 调用，先后怎么排都露出一帧；
 *   * 先 showMaximized()、再让 Qt 自己把内容跟上来：就是上面那好几帧；
 *   * 用 GDI 往窗口 DC 上直接刷底色：这个窗口屏幕上的内容只认 Qt 自己的合成，
 *     实测刷了 1.2 秒一帧都上不去（build\win-gdi-red-video.ps1）。
 *
 * 系统状态：最大化走 showMaximized()（任务栏右键 / Win+↓ / 贴边吸附这些语义才对），
 * 只是不让系统放那段转场。拖标题栏还原、Win+↑ / 系统菜单里那条"最大化"，都由这一层
 * 自己接（见 startSystemMove 和 MessageTrace）；贴边吸附最大化是系统自己摆的矩形，
 * 靠几何认（见 updateMaximizedFromWindow）。
 */
class QScreen;
class QWidget;

class WindowHelper final : public QObject {
    Q_OBJECT

    // 当前是否处于最大化
    Q_PROPERTY(bool maximized READ maximized NOTIFY maximizedChanged)

    /*
     * 刚刚发生了一次最大化 / 还原切换。
     *
     * 界面监听它做一次极短的淡入（见 Main.qml 的 interfaceRoot）：
     * 置 true 后界面轻轻回一下、再由动画回到全亮。
     * 它是一次性的通知，不是"动画进行中"那种持续状态。
     */
    Q_PROPERTY(bool transitioned READ transitioned NOTIFY transitionedChanged)

    /* 窗口圆角半径（0 = 直角；最大化时会自动按直角处理） */
    Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius
               NOTIFY cornerRadiusChanged)

    /*
     * 这个窗口的**系统转场动画**写进去了没有（见 src/DialogStyle.h 的
     * disableDwmTransitions，它返回 DwmSetWindowAttribute 那个 HRESULT）。
     *
     * 报给自检用，也进窗口变化日志（"系统转场="那一列）。
     *
     * 录屏实测（build\frames-*，DDA 抓的是合成器输出）：这台机器上最大化 / 还原
     * 那一下**没有任何缩放动画**，窗口矩形是"啪"一下到位的。所以用户报的
     * "先跑到角上、再放大"不是系统那套转场，而是自己这边漏出来的中间帧
     * （真正解决它的是 applyState() 里那套顺序，见 .cpp）。
     *
     * 这个属性反向读不了（DwmGetWindowAttribute 回 E_INVALIDARG），所以
     * "是不是这条把它关掉的"没法直接证；能确定的只是它现在没在放。
     * 平台上关不掉（非 Windows）时它一直是 false。
     */
    Q_PROPERTY(bool transitionsDisabled READ transitionsDisabled CONSTANT)

public:
    explicit WindowHelper(QObject *parent = nullptr);
    ~WindowHelper() override;

    /*
     * 主窗口由 main.cpp 以 QWidget 形式创建（见那里的说明），
     * 所以在 C++ 侧直接挂上，QML 不需要再调 attach。
     */
    void attachWidget(QWidget *widget);

    bool maximized() const { return m_maximized; }
    bool transitioned() const { return m_transitioned; }
    bool transitionsDisabled() const { return m_transitionsDisabled; }
    int cornerRadius() const { return m_cornerRadius; }

    /* 供外部（main.cpp）在窗口显示后主动刷新一次圆角遮罩 */
    Q_INVOKABLE void refreshMask();

    /*
     * 让 QML 也能往窗口变化日志里写一行（见下面"窗口变化日志"那段）。
     *
     * 现在用它记"切换那一小段里各块布局的宽度"（Main.qml 里的 layoutProbe）——
     * 那个问题（布局算一拍还是两拍）只有 QML 那侧看得见，而日志只有这一份。
     */
    Q_INVOKABLE void traceMark(const QString &what) { trace(what); }

    /*
     * QML 那些"每毫秒记一行"的诊断要不要开（读环境变量 SMARTCLIP_LAYOUT_PROBE）。
     *
     * **默认关**：那个探针（Main.qml 的 layoutProbe）自己在事件循环里很吵，
     * 开着的时候会把换尺寸那一下拖慢一个量级（实测：同一个 4K 渲染
     * 关着 ~5ms、开着 ~155ms）。要量布局是不是"算一拍就落定"时才打开。
     */
    Q_PROPERTY(bool probeEnabled READ probeEnabled CONSTANT)
    bool probeEnabled() const;

    /*
     * 交给窗口管理器做"拖边改大小 / 拖标题栏移动"。
     *
     * 这两个能力本来是 Window 的（QML 的 ApplicationWindow 才有），
     * 根元素换成 Rectangle 之后就没有了。底层还是 QWindow 提供，
     * 这里转发一层，QML 侧继续调 host.startSystemResize(...) 即可。
     */
    Q_INVOKABLE bool startSystemResize(int edges);
    Q_INVOKABLE bool startSystemMove();

public slots:
    // 最大化 / 还原之间切换（窗口按钮和顶栏双击都走这里）
    void toggleMaximize();
    void maximize();
    void restore();

    /* 最小化 / 关闭主窗口（QML 侧不再有 Window 对象，统一走这里） */
    void minimizeWindow();
    void closeWindow();

    /*
     * **真退出程序**：关闭键问句里的「完全退出」和文件菜单里的「退出」都走它。
     *
     * 两件事必须说清楚（.cpp 里展开了）：
     *   * 不能只 closeWindow()：Qt 默认"最后一个窗口关掉就退出"，而便签是各自
     *     独立的顶层窗口，主窗口关了还有别的窗口在，那条默认规则不成立；
     *   * 也不能用 `QCoreApplication::quit()`：它只是给 app 发一个 QEvent::Quit，
     *     而 QApplication::event() 收到它会先 closeAllWindows()，**只要还剩一个
     *     露着的顶层窗口就把这个事件吃掉** —— 便签窗口的 closeEvent 是
     *     "ignore + 藏起来"，于是每点一次退出只收起一块便签、程序不退
     *     （用户报的："有多个便签，点击退出会关闭一个便签，程序还是不会退出"）。
     *
     * 所以走 `QCoreApplication::exit(0)`：直接结束事件循环、**不碰任何窗口**
     * （用户要的"不要关闭便签，程序直接关闭"）。main.cpp 收尾时 notes.shutdown()
     * 会把便签清单落盘，下次启动那几块便签照旧显示。
     */
    void quitApp();

    /*
     * 关闭键（✕）问一句：**完全退出** 还是 **收进托盘**。
     *
     * 这里只发个信号，真正的问句是 QML 侧那一小块卡片
     * （qml/components/AskCard.qml，和下拉菜单同一类 Popup.Window 窗口）。
     * 为什么不在 C++ 里弹：底下的界面必须**原封不动** —— 不压暗、不遮住、
     * 不挡鼠标；而新建的顶层对话框又躲不掉"先映射空窗口、内容下一帧才画"
     * 那一帧闪烁（C++ 侧试过三种补法都治不干净）。见那个 QML 文件开头的说明。
     */
    void askQuit();

    /* 收进托盘：把主窗口藏了，程序继续跑（托盘图标、全局截图热键都还在） */
    Q_INVOKABLE void hideToTray();

    /* 窗口圆角半径（0 = 直角，最大化时会自动置 0） */
    void setCornerRadius(int r);

signals:
    /* 用户按了关闭键：请界面把"退出问句"那块卡片弹出来 */
    void quitRequested();

    void maximizedChanged();
    void transitionedChanged();
    void cornerRadiusChanged();

protected:
    // 盯着窗口自己的移动 / 缩放 / 状态变化：缓存还原矩形、同步最大化状态
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void applyState(bool maximize);
    void cacheNormalGeometry(const QRect &geometry);

    // 还原矩形落在已拔掉的显示器上时，拉回主屏
    QRect clampToScreen(const QRect &rect) const;

    // 把 m_maximized 对回窗口真实状态（含被系统吸附最大化的情形）
    bool updateMaximizedFromWindow();

    /* 内容控件摆到窗口里的某块位置，并让它**当场**按新尺寸渲染一帧（见 .cpp） */
    void placeContent(const QRect &contentInWindow);

    /*
     * 窗口在**系统那边**现在是什么形状（"空" = 整块矩形）。
     *
     * 只给日志用，但它是这一摊事里唯一的地面真相：幕布（直接 SetWindowRgn 设的、
     * Qt 不知道）有没有真的撤掉，只能问系统 —— 见 .cpp 里的说明。
     */
    QString windowRegionText() const;

    /*
     * 把"窗口刚变完尺寸"这一帧**当场**合成并刷上屏。
     *
     * 不刷的话，窗口 2ms 就变完了、内容也 2ms 内重画好了，但那一帧要等 Qt
     * 下一轮才合成上去 —— 屏幕上那 20~40ms 里还是旧尺寸那张画面贴在窗口左上角，
     * 看着就是"窗口先跑到左上角、然后内容再放大"（用户报的就是这个）。
     * 详见 .cpp 里的实现说明。
     */
    void flushContent();

    // 触发一次"切换过"的通知，界面据此淡入
    void notifyTransition();

    /* ------------------------------------------------------------------
     * 窗口变化日志（当前工作目录下的 window-trace.log）
     *
     * 记"窗口自己每一拍变成了什么样"：Qt 这一层的 Move / Resize / 状态 /
     * 显隐，WndProc 那一层的 WM_* 原始消息（含系统发来的目标矩形和命令），
     * 以及切换那一小段时间里的 QML 帧。排查"最大化时窗口先跑到右边、
     * 还在放大 / 树和正文之间那条缝在闪"这类问题时，复现一次就有序列可看。
     *
     * 为什么两层的都要记：Qt 的 Move / Resize 是**结果**，看不出"谁下的命令、
     * 系统是不是还在后面排了一次缩放动画"；只有 WM_WINDOWPOSCHANGING /
     * WM_SYSCOMMAND 那一层才看得到"这一拍是系统自己干的，还是我们让它干的"。
     *
     * 这个日志是**一直在写**的（每次切换十几行，不至于撑爆）：用户报的是
     * "偶尔才看到一下"的观感问题，要他们先加开关再复现，多半就复现不到了。
     * 每次启动把上一份删掉重开，所以它永远只反映最近这一次运行。
     * ---------------------------------------------------------------- */
    void trace(const QString &what);
    /* 记一条"窗口 + 它那些子控件此刻的几何"（子控件落后一拍就是缝在闪） */
    void traceSnapshot(const QString &tag);
    /* 切换后的一小段里记 QML 帧（帧是"画出来了没有"的唯一凭据） */
    void traceFrames(bool armed);

    class MessageTrace;
    MessageTrace *m_messageTrace = nullptr;

    QElapsedTimer m_traceClock;
    bool m_traceStarted = false;
    qint64 m_traceFramesUntil = -1;

    /*
     * 把窗口裁成圆角矩形。
     *
     * 窗口是无边框 QWidget，透明 + 圆角必须自己来。用 QRegion 遮罩，
     * 这是各平台上最可靠的做法（Windows 上落到 SetWindowRgn）。
     * 最大化时不做圆角，和原生最大化窗口一致。
     */
    void applyRoundedMask();

    QWidget *m_widget = nullptr;

    /*
     * 装整个 QML 界面那块控件（QQuickWidget）。
     *
     * 只给日志用：它的几何和窗口的几何**不是一回事**，而屏幕上看到的内容是它
     * 画的 —— "窗口已经变大了、内容还停在原样"这种一拍，只有把两者摆在一起
     * 才看得出来。见 .cpp 里 traceSnapshot。
     */
    QWidget *m_quickWidget = nullptr;

    /* QWidget 拿屏幕的方式和 QWindow 不同，这里统一包一层（可能返回 nullptr） */
    QScreen *screenOf() const;

    bool m_maximized = false;
    bool m_transitioned = false;

    /* 系统转场动画关掉了没有（attachWidget 里关的，见 DialogStyle.h） */
    bool m_transitionsDisabled = false;

    /*
     * 上面那件事**到底设上了没有**，一句话人话（日志里"系统转场="那一列）。
     *
     * 为什么不是现读的：DWMWA_TRANSITIONS_FORCEDISABLED 在文档里只标了配合
     * DwmSetWindowAttribute 用，反向读会回 E_INVALIDARG —— 所以只能记下写的时候
     * 那个 HRESULT。
     */
    QString m_transitionNote = QStringLiteral("未设");

    /*
     * 上一次真正落到窗口上的圆角遮罩。
     *
     * 形状没变就不重复 setMask —— 那一下在 Windows 上是 SetWindowRgn，
     * 会让整块窗口重画一次（见 .cpp 里 applyRoundedMask 的说明）。
     */
    QRegion m_appliedMask;
    bool m_maskApplied = false;

    /*
     * "正在换尺寸"那一小段（applyState 全程）。
     *
     * 这一小段里 WM_ERASEBKGND 要自己把背景擦成界面底色 —— 免得"从没画过的"
     * 像素（白色 / 花屏）被交给合成器。见 .cpp 里那个 case 的说明。
     */
    bool m_fastErase = false;

    /*
     * "换几何之前先把界面擦成底色"这一小段（见 .cpp 里 ②b）。
     *
     * 窗口变大的时候，系统会把"变大前那一张画面"拷到新表面的左上角 —— 拷过去的是
     * 整块界面的话，用户就会看到"界面跑到左上角、再放大"。这一小段里主窗口重画只铺
     * 界面底色、不合成内容控件，所以被拷过去的就是一片底色。
     */
    bool m_blankBackdrop = false;

    int m_cornerRadius = 10;

    // 用户最后摆出来的常规窗口矩形，点还原时回到这里
    QRect m_normalRect;

    // 展开时记下还原矩形，避免中途被 resize 事件污染
    QRect m_restoreAnchor;
};
