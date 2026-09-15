#pragma once

#include "DocConvert.h"

#include <QMap>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantList>

class ClipboardStore;

/*
 * "把文件识别成笔记"这条路的**总管**（QML 单例 Doc，见 src/main.cpp）。
 *
 * 它管三件事：
 *   1) **设置**：用哪条命令、档位、认完要不要自动打开（QSettings 的 doc/ 下）；
 *   2) **排队**：用户一次拖进来五个文件，一个一个来（同时跑五个 Python，每个
 *      都要几百 MB 内存和整个 CPU，机器直接趴下）；
 *   3) **收尾**：认完把 Markdown + 图片交给 ClipboardStore::createNote 落成笔记，
 *      并把新笔记的路径报给界面（界面选中它、滚到它）。
 *
 * 为什么要有这一层而不是让 QML 直接调 DocConvert：
 *   * DocConvert 是纯函数 + 一个 Task，没有"当前状态"（在认哪个文件、第几个、
 *     什么时候该落盘），而这些都要给界面看；
 *   * 落盘要 ClipboardStore，而 QML 那边不该知道"assets 怎么落、笔记怎么建"。
 *
 * 线程：转换跑在 QThreadPool 里（DocConvert::Task），回来用信号投递。
 *      **同一时刻只有一个任务在跑**（见队列那段）。
 */
class DocImport final : public QObject {
    Q_OBJECT

    /*
     * 识别程序那条命令（默认 python + 随包脚本，见 DocConvert::defaultRunnerCommand）。
     * 换成别的引擎就是换这一条 —— 这是"可插拔"唯一的接口。
     */
    Q_PROPERTY(QString runner READ runner WRITE setRunner NOTIFY settingsChanged)
    /* 档位词（fast / balanced / best），接在命令后面当脚本的参数 */
    Q_PROPERTY(QString tier READ tier WRITE setTier NOTIFY settingsChanged)
    /*
     * 用哪个 Python 解释器（空串 = 自动，即 PATH 里的 python）。
     *
     * 为什么必须有这一项：识别那三个包（rapid-doc / paddleocr / docling）通常是
     * 装在**某个虚拟环境**里的，而 PATH 里的 python 往往是系统那个 —— 装了却
     * 报 "No module named 'rapid_doc'"，用户根本看不出是解释器不对。设置面板
     * 上给一个"选择…"直接指到 venv 里的 python.exe。
     */
    Q_PROPERTY(QString pythonPath READ pythonPath WRITE setPythonPath NOTIFY settingsChanged)
    /* 认完自动打开那份新笔记 */
    Q_PROPERTY(bool openWhenDone READ openWhenDone WRITE setOpenWhenDone
                   NOTIFY settingsChanged)
    /* 认完把源文件也复制一份进笔记目录的 assets（默认关：多数人只要文字） */
    Q_PROPERTY(bool keepSource READ keepSource WRITE setKeepSource NOTIFY settingsChanged)

    /* 现在在忙（界面据此转圈 / 禁用按钮） */
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    /* 一句话状态："正在识别 第 2/5 个：xxx.pdf" / "识别不了这个文件：…" */
    Q_PROPERTY(QString status READ status NOTIFY stateChanged)
    /* 手里还有几个没认（含正在认的那个） */
    Q_PROPERTY(int pending READ pending NOTIFY stateChanged)
    /* 这一次一共要认几个（0 = 没在干活） */
    Q_PROPERTY(int total READ total NOTIFY stateChanged)
    /* 上一轮认出来的笔记路径清单（界面据此选中 / 提示"建了 3 份笔记"） */
    Q_PROPERTY(QStringList created READ created NOTIFY createdChanged)
    /* 最近一次失败的原因（空串 = 没出错）；界面上把它显示出来 */
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)

public:
    explicit DocImport(ClipboardStore *store = nullptr, QObject *parent = nullptr);

    QString runner() const;
    void setRunner(const QString &value);
    QString tier() const;
    void setTier(const QString &value);
    QString pythonPath() const;
    void setPythonPath(const QString &value);
    bool openWhenDone() const;
    void setOpenWhenDone(bool on);
    bool keepSource() const;
    void setKeepSource(bool on);

    bool busy() const { return m_running || !m_queue.isEmpty(); }
    QString status() const { return m_status; }
    int pending() const { return m_queue.size(); }
    int total() const { return m_total; }
    QStringList created() const { return m_created; }
    QString error() const { return m_error; }

    /*
     * 把一批文件排进队列（一次拖拽 / 一个"打开"对话框）。认完一个落一份笔记。
     * paths 里的目录会被忽略（拖文件夹进来时不至于报一堆错）。
     */
    Q_INVOKABLE void enqueue(const QStringList &paths);

    /* 取消：停掉正在跑的那个，清空队列 */
    Q_INVOKABLE void cancel();

    /* 这条命令能不能跑（空串 = 能）。界面显示"还没配好"用。 */
    Q_INVOKABLE QString runnerProblem() const;

    /* 这个文件认得认不了（空串 = 能）。拖进来时先问一句，别等到跑完才报错。 */
    Q_INVOKABLE QString problemFor(const QString &path) const;

    /* 认得认得了的文件过滤器（"打开"对话框用） */
    Q_INVOKABLE QString fileFilter() const;

    /*
     * 随包脚本对应的一条命令（设置里那三个引擎按钮用）。
     *
     * 传脚本名（"doc_runner_rapid.py" 这种），回来的就是 `python "...脚本"` ——
     * 用的解释器是上面那个 pythonPath（空串则自动找 PATH 里的）。
     */
    Q_INVOKABLE QString commandForScript(const QString &script) const;

    /* 随包脚本的名字清单（自检 / 界面想知道有哪些可选项时用） */
    Q_INVOKABLE QStringList shippedScripts() const;

    /* 一个文件大概是什么（"PDF 文档"），界面上提示用 */
    Q_INVOKABLE QString kindFor(const QString &path) const;

    /*
     * 主窗口是不是"用户看得见"—— 卡片据此决定显示还是收起来。
     *
     * 为什么要有这个开关：卡片是**独立置顶的原生窗口**，不跟着主窗口隐藏。
     * 主窗口收进托盘（或者最小化）之后，它会孤零零留在桌面上。
     *
     * 但自检里主窗口是**故意不显示**的（见 main.cpp 那几个自检分支）—— 那时候
     * "主窗口不可见"是正常的，不是"用户收起来了"。自检跑起来第一件事就是把它
     * 置成 true，免得卡片被这条守卫立刻收掉、几何一条都量不到。
     */
    Q_PROPERTY(bool windowUsable READ windowUsable WRITE setWindowUsable
                   NOTIFY windowUsableChanged)
    bool windowUsable() const { return m_windowUsable; }
    void setWindowUsable(bool on);

    /* 清掉上一次的结果 / 错误（界面关掉提示时调） */
    Q_INVOKABLE void clearResult();

    /* 把最后建出来的那份笔记打开（卡片上"打开笔记"按钮） */
    Q_INVOKABLE void openLast();

    /*
     * 退出前收尾：取消在跑的、清空队列。
     * main.cpp 在事件循环还活着的时候调一次（和 llm.shutdown() 同一个位置）——
     * 留到析构那会儿，子进程可能已经被跟着收掉，Qt 会在收尾阶段报
     * "QProcess: Destroyed while process is still running"。
     */
    void shutdown();

    /*
     * 自检用：把"卡片该在的屏幕坐标"算出来（依赖主窗口当前几何，所以得传进来）。
     *
     * 为什么这个算式放在 C++ 而不是 QML 里比：QML 那边的 `window.x/width` 和
     * 主窗口 QWidget 的几何不是同一套（窗口隐藏时更明显 —— 实测 QML 给出
     * window.x=-324 这种值）。拿宿主 QWidget 的几何算期望值、再和卡片实际的
     * x/y 比，才是同一个参照系。
     *
     * 注意：不能放在下面 `signals:` 之后 —— moc 会把那儿的东西全当信号声明，
     * 报 "Not a signal declaration"（踩过）。
     */
    Q_INVOKABLE QPoint expectedCardPos(int hostX, int hostY, int hostW, int hostH,
                                       int cardW, int cardH) const;

    /*
     * "上一次推给界面的卡片位置"（x, y）。
     *
     * 自检量这个，而不是量卡片的屏幕坐标 —— 自检里主窗口是**隐藏的**，那时候
     * 宿主 QWidget 的 geometry 和 QML 窗口的几何对不上（实测差一个窗口偏移），
     * 拿它算"期望值"去比是自欺欺人。能可靠验的是：**位置确实被推过去了**
     * （推一次就报一次），以及推的那个算式本身对不对（expectedCardPos，纯函数）。
     */
    Q_INVOKABLE QPoint lastCardPos() const { return m_lastCardPos; }

    /*
     * 卡片的几何参数（**唯一真相在这儿**）。
     *
     * QML 那边（DocCard.qml）只负责画，位置和留白都从这里推过去
     * （见 publishCardGeometry）；main.cpp 那个定时器也用这几个数算右下角。
     * 以前这几个值散在 QML / main.cpp 两处，改一个忘一个就错位。
     */
    /* 卡片宽度 */
    Q_INVOKABLE int cardWidth() const { return 340; }
    /* 估算高度：main.cpp 先按它算个 y，QML 那边再按实际高度校一次 */
    Q_INVOKABLE int cardHeightHint() const { return 118; }
    /* 离主窗口右边留多少 */
    Q_INVOKABLE int cardMargin() const { return 16; }
    /*
     * 卡片底边离**主窗口底边**留多少。
     *
     * = 状态栏高度（26，见 StatusBar.qml 的 implicitHeight）+ 卡片底下那圈留白
     * （16，**和右边那个 cardMargin 一样** —— 用户要的是"四边留白一致"）。
     * 只写 26 的话卡片底边正好贴着状态栏上沿，看着很挤；写 36（留白 10）底下
     * 又比右边窄，看着不齐。
     */
    Q_INVOKABLE int cardBottomGap() const { return 26 + 16; }

    /*
     * 把"进度卡片该在哪儿"（**屏幕坐标**）告诉界面。
     *
     * 为什么位置要由 C++ 给、而不是 QML 自己用 `window.x + window.width - …` 算：
     * 实测 QML 那个 ApplicationWindow 的 `x`/`y` 和宿主 QWidget 的 geometry
     * **不是同一套坐标系** —— 宿主摆在 (300,160) 时 QML 读出来是 x=0，于是
     * 卡片被算到屏幕中间去（用户报的"不在右下角"就是这个）。
     * 只有 C++ 这边（EditorViewItem 的宿主 widget）知道真实几何。
     *
     * 界面那边监听这个信号，把坐标转发给 DocCard（见 Main.qml）。
     */
    Q_INVOKABLE void publishCardGeometry(int x, int y);

signals:
    /* 卡片该挪到哪（屏幕坐标，见 publishCardGeometry） */
    void cardGeometryChanged(int x, int y);

    void settingsChanged();
    void stateChanged();
    void createdChanged();
    /* 主窗口能不能用变了（卡片据此决定收不收起来） */
    void windowUsableChanged();
    /* 全部认完了（成功几份、失败几份）—— 界面据此提示一下 */
    void finished(int succeeded, int failed);
    /*
     * 要打开某份笔记（openLast 发出来的）。
     *
     * 为什么不在这里直接打开：打开笔记要动编辑器 / 左树那一套（EditorController
     * + Main.qml 的 activateFolder），那些东西不属于"识别"这件事。这一层只报
     * "该打开这个文件了"，由界面那边接住 —— 和 RecognitionCard 把动作交回给
     * CaptureOverlay 是同一个分工。
     */
    void openRequested(const QString &path);

private:
    /* 从队列里取下一个开始跑；队列空就收尾 */
    void pump();
    void onTaskFinished(const DocConvert::Result &result);
    void setStatus(const QString &text);
    void persist(const QString &key, const QVariant &value);

    ClipboardStore *m_store = nullptr;

    QString m_runner;
    QString m_tier;
    QString m_pythonPath;
    bool m_openWhenDone = true;
    bool m_keepSource = false;
    /* 主窗口能不能用（见 Q_PROPERTY 的说明）；默认能 */
    bool m_windowUsable = true;
    /* 上一次推给界面的卡片位置（见 lastCardPos） */
    QPoint m_lastCardPos;

    /* 排队等着认的文件 */
    QStringList m_queue;
    /*
     * "这一个正在跑"。
     *
     * **不能**用"队列非空"代替：pump() 一取走最后那个文件，队列就空了，而这时
     * 它才刚开始跑 —— 光看队列的话 busy 会在"开始跑"到"跑完"这整段时间里报
     * false（界面上就是转圈闪一下就没，看着像卡住了）。实测踩过。
     */
    bool m_running = false;
    /* 正在认的那个（落盘时要用它当标题） */
    QString m_current;
    /*
     * 正在跑的那个任务（取消要它）。
     *
     * QPointer 而不是裸指针：任务是 QRunnable，跑完由**线程池**删。裸指针在
     * "任务已经跑完、回调还没被投递到主线程"那一小段窗口里就是个悬空的；
     * QPointer 在对象被删时自动变空，cancel() 那边判断一次就够了。
     */
    QPointer<DocConvert::Task> m_task;

    int m_total = 0;
    int m_done = 0;
    QStringList m_created;
    QString m_status;
    QString m_error;
};
