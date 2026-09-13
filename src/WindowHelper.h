#pragma once

#include <QObject>
#include <QRect>

class QQuickWindow;

/*
 * 最大化 / 还原。
 *
 * 这里最终选的是"瞬变 + 内容淡入"，不是窗口几何动画。原因是实测出来的：
 *
 *   1) 逐帧改窗口尺寸本身几乎是免费的 —— 拿一个纯色最小窗口做对照，
 *      同样的动画能跑 35 帧 / 平均间隔 7.1ms（140fps）。
 *   2) 真正的开销是"每帧重排版 + 重绘整个界面"。这个界面在 4K 下每帧
 *      要 20ms 左右，几何动画只能拿到 15~20 帧、还夹着 38ms 的尖峰。
 *   3) 编辑区内容一多（比如三千多行的文本），每帧重排还要重新处理那些
 *      文本项，卡顿明显加剧 —— 内容越多越卡，这条路没有上限。
 *
 * 所以不再逐帧改几何：窗口尺寸一次到位，界面做一次短促的淡入来掩盖
 * 这次跳变。代价是没有"窗口长大"的过程感，换来的是任何内容量下都不卡、
 * 也不会出现半成品画面被合成导致的回闪。
 *
 * （走过的弯路，留个记录免得再踩：试过"抓快照 + GPU 缩放"绕开逐帧
 *   窗口缩放，结果更卡 —— 抓一张 4K 图本身就要几十毫秒，而窗口缩放
 *   根本不是瓶颈，方向就是错的。）
 *
 * 状态语义仍然是"最大化"：透明窗口、任务栏、还原后尺寸都交给窗口管理器。
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
     * 界面监听它做一次淡入（见 Main.qml 的 interfaceRoot）：
     * 置 true 后界面立刻压暗一点、再由动画回到全亮。
     * 它是一次性的通知，不是"动画进行中"那种持续状态。
     */
    Q_PROPERTY(bool transitioned READ transitioned NOTIFY transitionedChanged)

    /* 窗口圆角半径（0 = 直角；最大化时会自动按直角处理） */
    Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius
               NOTIFY cornerRadiusChanged)

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
    int cornerRadius() const { return m_cornerRadius; }

    /* 供外部（main.cpp）在窗口显示后主动刷新一次圆角遮罩 */
    Q_INVOKABLE void refreshMask();

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

    // 触发一次"切换过"的通知，界面据此淡入
    void notifyTransition();

    /*
     * 把窗口裁成圆角矩形。
     *
     * 窗口是无边框 QWidget，透明 + 圆角必须自己来。用 QRegion 遮罩，
     * 这是各平台上最可靠的做法（Windows 上落到 SetWindowRgn）。
     * 最大化时不做圆角，和原生最大化窗口一致。
     */
    void applyRoundedMask();

    QWidget *m_widget = nullptr;

    /* QWidget 拿屏幕的方式和 QWindow 不同，这里统一包一层（可能返回 nullptr） */
    QScreen *screenOf() const;

    bool m_maximized = false;
    bool m_transitioned = false;

    int m_cornerRadius = 10;

    // 用户最后摆出来的常规窗口矩形，点还原时回到这里
    QRect m_normalRect;

    // 展开时记下还原矩形，避免中途被 resize 事件污染
    QRect m_restoreAnchor;
};
