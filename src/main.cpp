#include "Checker.h"
#include "ClipboardManager.h"
#include "ClipboardStore.h"
#include "Diff.h"
#include "DocImport.h"
#include "EditorController.h"
#include "EditorViewItem.h"
#include "Formatter.h"
#include "PinWindow.h"
#include "Screenshot.h"
#include "SelfTest.h"
#include "Speech.h"
#include "StickyNotes.h"
#include "StickyNoteStore.h"
#include "Translate.h"
#include "TrayIcon.h"
#include "WindowHelper.h"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWidget>
#include <QSystemTrayIcon>
#include <QAction>
#include <QIcon>
#include <QKeySequence>
#include <QMenu>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#if defined(Q_OS_WIN)
#  include <windows.h>
#endif

namespace {

/*
 * 模态弹框出现时，把"错投的字符消息"吃掉，别让系统响那一声。
 *
 * 背景：这些 QtWidgets 弹框（QInputDialog、截图失败时那个 QMessageBox）弹出时，
 * 用户往往正在主窗口里敲键盘（菜单助记键、上一秒的按键、输入法送出的字符…）。
 * 主窗口这时
 * 已经被模态框挡住，Qt 收到 WM_CHAR 之后走 qt_try_modal() 直接拒收，消息落回
 * DefWindowProc —— Windows 对"没人处理的字符消息"的标准反应就是响一声
 * （QWindowsIntegration::beep() → MessageBeep(MB_OK)，系统"默认提示音"）。
 *
 * 这种字符消息本来就注定被丢掉（目标窗口被模态挡住），所以在这里截掉不会
 * 改变任何行为，只是不再出声。只针对 WM_CHAR / WM_SYSCHAR：按键消息
 * （WM_KEYDOWN）不动 —— 弹框自己的控件、Esc/回车、输入法都靠它。
 *
 * 只在有模态框时生效，且只拦"发给模态框之外窗口"的字符消息。
 */
class ModalBeepSilencer : public QAbstractNativeEventFilter {
public:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) override {
#if defined(Q_OS_WIN)
        if (eventType != QByteArrayLiteral("windows_generic_MSG"))
            return false;

        auto *msg = static_cast<MSG *>(message);
        if (msg->message != WM_CHAR && msg->message != WM_SYSCHAR)
            return false;

        const QWidget *modal = QApplication::activeModalWidget();
        if (!modal)
            return false;

        /* 发给模态框自己（或它的子窗口）的字符消息照常放行 */
        const HWND modalHwnd = reinterpret_cast<HWND>(modal->winId());
        const HWND modalRoot = modalHwnd ? GetAncestor(modalHwnd, GA_ROOT) : nullptr;
        if (msg->hwnd == modalHwnd || msg->hwnd == modalRoot)
            return false;
        if (HWND root = GetAncestor(msg->hwnd, GA_ROOT); root && root == modalRoot)
            return false;

        return true;  /* 错投的字符消息：丢掉，系统就不会 beep */
#else
        Q_UNUSED(eventType);
        Q_UNUSED(message);
        return false;
#endif
    }
};

}  // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName("SmartClip");
    app.setApplicationName("SmartClip");
    QQuickStyle::setStyle("Fusion");

    /*
     * 自检模式（挑一个，见 src/SelfTest.h）—— 只影响下面那几件事：
     *   * 主窗口不显示（省得自检时屏幕上一堆窗口乱跳）；
     *   * 识别那张卡片不许因为"主窗口不可见"就自己收掉（见 DocImport 的构造）。
     *
     * 必须在**任何单例构造之前**设好：DocImport 是在构造里读这个变量的。
     */
    if (SelfTest::enabled(argc, argv) || SelfTest::noteTestEnabled(argc, argv)
        || SelfTest::translateTestEnabled(argc, argv) || SelfTest::docTestEnabled(argc, argv)
        || SelfTest::docE2eEnabled(argc, argv) || SelfTest::docQueueEnabled(argc, argv)) {
        qputenv("SMARTCLIP_DOC_SELFTEST", "1");
    }

    /*
     * 工具提示（ToolTip）+ QtWidgets 对话框的全局配色。
     *
     * 界面整个是深色的，但 Fusion 那个 ToolTip 模板的背景取的是调色板里的
     * toolTipBase、文字取 toolTipText（见 Qt 的 Fusion/ToolTip.qml），不设的话
     * 就是系统默认的浅色底 —— 鼠标停在按钮上弹出一块白，跟界面完全不搭
     * （用户报的就是"收起替换"上面那个"显示 / 隐藏替换行"）。
     *
     * 下面这几组角色管的是 QtWidgets 那几样东西：QInputDialog（重命名 /
     * 转到行 / 自定义参考线列）这类对话框，还有截图失败时的 QMessageBox。
     * 不设的话它们是系统浅色，深色界面里点一下弹出一块白。
     *
     * 注意：提示 / 确认 / 未保存改动那三处**不再**依赖这里 —— 它们已经换成
     * QML 那套卡片（qml/components/AskCard.qml），颜色由那个组件自己写死。
     *
     * 注意：编辑区那个右键菜单**不再**依赖这里 —— 它已经换成 QML 那套菜单
     * （见 src/EditorViewItem.cpp 的 eventFilter 和 Main.qml 的
     * openEditorContextMenu），颜色由 DropdownMenu.qml 自己写死。
     * 想更黑就把下面这几处换成 #1e1f22（编辑区底色）或纯黑。
     */
    QPalette tipPalette = app.palette();
    tipPalette.setColor(QPalette::ToolTipBase, QColor(0x2b, 0x2d, 0x30));
    tipPalette.setColor(QPalette::ToolTipText, QColor(0xd6, 0xd7, 0xda));

    /* 菜单 / 对话框面板 */
    tipPalette.setColor(QPalette::Window, QColor(0x2b, 0x2d, 0x30));
    tipPalette.setColor(QPalette::WindowText, QColor(0xc8, 0xcc, 0xd1));
    tipPalette.setColor(QPalette::Base, QColor(0x2b, 0x2d, 0x30));
    tipPalette.setColor(QPalette::AlternateBase, QColor(0x31, 0x33, 0x35));
    tipPalette.setColor(QPalette::Text, QColor(0xc8, 0xcc, 0xd1));
    tipPalette.setColor(QPalette::Button, QColor(0x2b, 0x2d, 0x30));
    tipPalette.setColor(QPalette::ButtonText, QColor(0xc8, 0xcc, 0xd1));
    tipPalette.setColor(QPalette::Highlight, QColor(0x21, 0x42, 0x83));
    tipPalette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    /* 置灰项（菜单里当前不可用的命令）：和界面其它地方的次要文字一个色 */
    tipPalette.setColor(QPalette::Disabled, QPalette::WindowText, QColor(0x6f, 0x73, 0x7a));
    tipPalette.setColor(QPalette::Disabled, QPalette::Text, QColor(0x6f, 0x73, 0x7a));
    tipPalette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x6f, 0x73, 0x7a));

    app.setPalette(tipPalette);

    /* 弹框不再响那一声，见 ModalBeepSilencer 的说明 */
    static ModalBeepSilencer beepSilencer;
    app.installNativeEventFilter(&beepSilencer);

    ClipboardStore store;
    if (!store.open()) return 1;
    ClipboardManager clipboard(&store);
    clipboard.start();

    /*
     * 截图（选区 / 加文字 / 固定到桌面，见 src/Screenshot.h）。
     *
     * **必须声明在 host 之前**：它的图片提供者（image://shot/…）挂在
     * QQuickWidget 的引擎上，而那个引擎是 host 的子对象 —— 局部对象按
     * 声明的反序析构，声明在前才活得比引擎久。
     */
    Screenshot screenshot;

    /*
     * 便签（见 src/StickyNotes.h）。
     *
     * **必须声明在 host 之前**，和 Screenshot 同一个理由：便签窗口的
     * QQuickWidget 用的是 quick->engine()（单例、图片提供者都挂在那个引擎上），
     * 而引擎是 host 的子对象 —— 局部对象按声明的反序析构，声明在前才活得比
     * 引擎久。引擎这时候还没建，所以先构造（那时它登记不了图片提供者，
     * 见 StickyNotes::attachEngine），quick 建好之后再补一次。
     */
    StickyNotes notes;

    /*
     * 翻译（见 src/Translate.h）。
     *
     * llm 是 LLM 客户端（QML 单例 Llm）：配置 + 发请求 + 启动本机推理服务；
     * cards 是桌面翻译卡片的总管（QML 单例 Trans）：只有一张卡片，入口统一在
     * showCard() 上（图标条 / 托盘 / 快捷键都落到那里）。
     *
     * 这两个也**必须声明在 host 之前** —— 卡片窗口的 QQuickWidget 用的是
     * quick->engine()，而引擎是 host 的子对象（理由同上面 Screenshot / 便签）。
     */
    LlmClient llm;
    TranslateCards cards(&llm);

    /*
     * 编辑区校验（见 src/Checker.h）：中文用词 / 代码语法。
     *
     * 要 llm 是因为它那条"问大模型"的支路走 LlmClient::ask；本地规则那部分
     * 不依赖网络。开关在设置面板（QSettings 的 check/enabled，默认关）。
     */
    Checker checker(&llm);

    /*
     * 文件对比（见 src/Diff.h）：纯本地算法，谁都不依赖。
     */
    DiffEngine differ;

    /*
     * 代码格式化（见 src/Formatter.h）：认本机装了的外部工具（clang-format /
     * prettier / black…），没有工具时退回内置那三样（JSON 重排 / XML 缩进 /
     * 去行尾空白）。设置里可以按语言指定命令。
     */
    Formatter fmt;

    /*
     * 朗读（见 src/Speech.h）：翻译卡片上那个「播放」按钮用它把译文念出来。
     *
     * 声音走的是系统自带的语音合成（Windows SAPI），**不是模型念的** ——
     * 大模型只吐文字。引擎活在它自己的线程里，这里只是建起来。
     *
     * 和上面两个同一个理由声明在 host 之前：卡片窗口的 QML 一构造就会读到它。
     */
    Speech speech;

    /*
     * 文档 / 图片识别（见 src/DocImport.h）：把 PDF / 图片 / Office 文档认成
     * Markdown，落成一份笔记。
     *
     * 要 store 是因为"落成笔记"这件事只有它知道（assets 放哪、文件名怎么起）。
     * 和上面几个同一个理由声明在 host 之前：界面一构造就要读它的状态。
     */
    DocImport doc(&store);

    /*
     * 主窗口用 QWidget 承载，而不是 QQmlApplicationEngine 直接开 QQuickWindow。
     *
     * 为什么必须这样（这是"编辑器真正嵌进去"能否成立的前提）：
     *   QScintilla 是 QWidget。要让它的圆角/裁剪跟随 QML 卡片，它必须和 QML
     *   处在同一个 QWidget 层级里。QQuickWindow 不是 QWidget、没有层级可挂，
     *   所以之前 QScintilla 只能退化成独立顶层窗口 —— 靠手算坐标跟随，会"分家"，
     *   QML 的 radius 也裁不到它。
     *
     *   QWidget(主窗口，无边框 + 不透明，圆角靠遮罩裁)
     *     ├── QQuickWidget(整个 QML 界面)
     *     └── QsciScintilla(编辑器，同一层级 → 圆角/裁剪/QML 层级都成立)
     */
    /*
     * GPU 合成的试验结论（都试过、都量过，别再走一遍）：
     *
     * 目的：这块界面是 4K，QML 那层渲染进 FBO 之后合进 QWidget 的 backing store 是
     * **CPU** 干的（先把 4K 读回来、再画一遍），一次整窗合成 ~20ms —— 最大化时
     * "窗口已经变大、内容还没合成好"那几帧黑就是它。想让这段走 GPU，试了两版：
     *
     *   1. 宿主流着，里面套一层 QOpenGLWidget、QQuickWidget 挂到那层上：
     *      实测反而更慢（等于在 raster 宿主里又多了一次 "GL 层 → raster 宿主" 的读回；
     *      那笔"铺底色"要过一遍 4K FBO，日志里量到 52ms）；
     *   2. 顶层窗口本身就是 QOpenGLWidget（整条链路没有 raster backing store）：
     *      最大化中间帧从 5 帧变成 9 帧（144fps 录像），更差 —— QQuickWidget 那张纹理
     *      该读回还是读回（它在自己的上下文里渲染）。
     *
     * 结论：**这条捷径不通**。要真正省掉那次读回，只能把内容层从 QQuickWidget 换成
     * GPU 渲染的 QQuickWindow（QML 直接由 GPU 出图），而 QScintilla 是 QWidget、
     * 必须挂在 QWidget 层级里（见上面那段），所以要重新设计编辑区的挂载 ——
     * 那是项目级改造，不是调几行能解决的。素材：build\frames-gpuhost、frames-gputop。
     *
     * ---------------------------------------------------------------------------
     * 第三版：GPU 内容层（QQuickRenderControl + 隐藏 QQuickWindow + QOpenGLWidget）
     * —— **做完了、能跑、全量自检也全绿，但没换来性能，所以整份删掉了**（不是留在
     * 那里当旁路）。想回去看，代码在 git 的 53db0c1 里。
     *
     * 为什么删：它唯一的目的是省掉那次 4K 读回，而这一步没做到（原因见下），
     * 于是它只剩成本 —— 一条不跑的并行分支、三个调用点被迫改成间接层，
     * 之后每次改内容层和每次跑全量自检都要多考虑一份实现。（前两版 GPU 尝试
     * 也是这么处理的：留结论 + 素材，不留活代码。）
     *
     * 数字（同一份二进制、同一条判据，各多轮；脚本：build\win-maxframes.ps1）：
     *   最大化那一下的"空档"帧数（144fps）：
     *                         空状态              开一篇长文档
     *     老 QQuickWidget      4 / 5 / 4 帧        10 / 11 / 13 / 13 帧
     *     GPU 内容层           6 / 3 / 5 帧        14 / 12 / 15 / 15 帧
     *   全量自检两边都是 789 / 0，退出码都 0。
     *
     * 也就是：空状态**打平**（差在噪声里），开文档**稳定多 2~3 帧**。原因和第一版
     * 是同一个：QOpenGLWidget 那张 FBO 最终还是要合进**宿主的 raster backing store**，
     * 读回只是从 QQuickWidget 那张纹理挪到了这张纹理上。QML 那一帧变成"渲进自己的
     * 纹理 -> blit 进 QOpenGLWidget 的 FBO -> 合成进 backing store"，中间那步 blit
     * 是白送的 GPU 拷贝，最后那步该读回还是读回。
     *
     * 要真把这一步也搬上 GPU，只有让**顶层窗口本身**不再是 raster backing store
     * （整窗 GL 合成）—— 那又会碰上第二版踩过的坑（QML 那层会各自建上下文），
     * 得连着改 Qt Quick 的图形设备绑定，不是本项目能收得住的范围。
     *
     * 界面这条线的性能问题别再从"换渲染目标"这个方向找 —— 往"最大化那一下到底
     * 哪几帧在等什么"上找更划算（见 WindowHelper 里 applyState 的 ①~⑤）。
     *
     * 顺带记三条这次踩出来的硬约束（照 Qt 6.11.2 的 QQuickWidget 实现核过）：
     *   * Qt 6 的 QQuickRenderControl **必须**自己给一张渲染目标纹理，不给就报
     *     "QQuickWindow: No render target" 且整块空（Qt 5 那套"渲进当前 FBO"不成立）；
     *   * initialize() 之前要 setGraphicsDevice(fromOpenGLContext(本控件上下文))，
     *     让场景图用现有上下文，别自己另建一个；
     *   * 离屏窗口的几何要摆到控件的**屏幕坐标**上，**同时**重写 renderWindow()
     *     返回宿主真窗口 —— 少哪个都会坏（少前者弹窗挪错位置，少后者弹窗根本不出现）。
     * 素材：build\gpu-content-*.png、build\selftest-*.out、build\max-*144.mp4、
     * build\window-trace-{legacy,gpu}.log。临时诊断脚本 build\gpu-content-smoke.ps1
     * 也是那次留下的（现在没用了）。
     */
    QWidget host;
    /*
     * 无边框（圆角、自绘顶栏都靠这一条）。
     *
     * 试过"生来就是普通窗口（Qt::Window 带标题栏样式）+ WM_NCCALCSIZE 把非客户区压成 0"，
     * 想让 Windows 那段最大化转场放起来盖住空档 —— 实测**照样不放**（素材
     * build\frames-framed2），所以还是维持无边框。结论记在 WindowHelper.cpp 的
     * WM_NCCALCSIZE 那段注释里。
     */
    host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    /*
     * 主窗口**不透明**、底色就是界面底色 —— 不是 WA_TranslucentBackground。
     *
     * 这里踩过一次，记下来免得改回去（自检"最大化/还原：来回切四次，窗口一次都没露白"
     * 钉的就是它）：原来这个窗口是**半透明**的（WA_TranslucentBackground，四角靠
     * 透明露桌面），结果**每次最大化**都有一帧窗口是空的 —— 那一帧屏幕上看到的是
     * 窗口**底下的东西**。底下正好是个白底网页时，用户看到的就是"白色的背影一闪"
     * （实测：抓那一帧的像素，亮度 ~250、取样条 96% 是白的；把那一帧存下来一看，
     * 里面是浏览器那张白底网页。还原方向基本不白，白的是最大化那一下）。
     *
     * 为什么半透明的窗口会"空一帧"：窗口的尺寸一变，系统要重建它那张表面，
     * 而新表面画上内容之前是空的；半透明窗口的"空"= 透过去看底下，不透明窗口的
     * "空"= 露出自己的底色。所以这一条的正解不是"想办法提前画一帧"（那是在跟
     * 系统的表面重建赛跑），而是**让"空"看起来也是对的**：换成不透明 + 深色底。
     *
     * 顺带把"压暗 -> 回全亮"那次淡入也修对了（见 Main.qml 的 interfaceRoot）：
     * 半透明窗口上降透明度 = 整块界面变半透明、露出桌面；不透明窗口上降透明度
     * 才是注释里写的那个效果（压暗再回全亮）。
     *
     * 四角的圆角不受影响：真正负责裁圆角的是 WindowHelper 的**遮罩**
     * （setMask -> Windows 的 SetWindowRgn），透明那层只是"顺手"，
     * 自检里有一条量过"遮罩落上了、左上角真的被裁掉"。
     */
    host.setAutoFillBackground(true);
    {
        QPalette hostPal = host.palette();
        /* 和 contentRoot 之外那圈底同色（见 Main.qml），换窗口尺寸时露的就是它 */
        hostPal.setColor(QPalette::Window, QColor(0x31, 0x33, 0x35));
        host.setPalette(hostPal);
    }
    host.setWindowTitle(QStringLiteral("SmartClip — 剪贴板"));

    /*
     * 装整个 QML 界面的那块控件。
     *
     * 曾经有过第二条实现（GPU 内容层，QML 直接渲进 QOpenGLWidget 的 FBO），
     * 量下来不划算，已经整份删掉了 —— 结论、数字和踩过的坑见上面那段
     * "GPU 合成的试验结论"，代码在 git 53db0c1。
     */
    auto *quick = new QQuickWidget(&host);
    quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    /*
     * 关掉这块控件的多重采样（MSAA）。
     *
     * 为什么：QQuickWidget 的离屏渲染目标尺寸**跟着窗口走**，而 Qt 默认会给它
     * 带 MSAA —— 4K 下那意味着"颜色缓冲 + 深度模板缓冲再乘 4"，两三百 MB 的
     * 渲染目标，每次窗口尺寸变化都要重建。实测（build\window-trace.log 里那几条
     * "内容控件摆到 … 之前/之后"）：不关的时候，第一次按 4K 建这个目标要 ~140ms，
     * 关掉之后只剩 QML 自己的同步 + 渲染（~20ms）—— 用户看到的就是最大化那一下
     * "卡 0.15 秒"。
     *
     * 这块界面是纯 2D（文字 / 圆角卡片 / 图标），MSAA 本来也没什么用；
     * 真要锯齿了，QML 那边还有 Item 级的抗锯齿可用。
     */
    {
        QSurfaceFormat fmt = quick->format();
        fmt.setSamples(0);
        quick->setFormat(fmt);
    }
    /*
     * 不要给 QQuickWidget 设 WA_TranslucentBackground —— 实测那样整块会变黑。
     * 清成透明色是对的：QML 根元素是 color: "transparent"，空出来的地方就露
     * 宿主窗口那层深色底（见上面 host 的调色板），不会露桌面。
     */
    quick->setClearColor(Qt::transparent);

    auto *layout = new QVBoxLayout(&host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(quick);

    /*
     * 编辑器需要宿主 QWidget 才能把 QScintilla 挂成子控件；
     * 正文数据源同样交给它，QML 侧只需要调 load(id)。
     */
    EditorViewItem::setGlobalHostWidget(&host);
    EditorViewItem::setGlobalStore(&store);

    /*
     * 便签的单例先登记（缩略图那个图片提供者要等主界面加载完再挂，
     * 见下面 setSource 之后那一段）。
     */
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Notes", &notes);

    /*
     * 截图：主窗口（藏 / 恢复、对话框父窗口）+ QML 引擎（选区窗口那个
     * QQuickWidget 共用同一个引擎，见 Screenshot::setEngine）。
     */
    screenshot.setHostWidget(&host);
    screenshot.setEngine(quick->engine());

    /*
     * 最大化 / 还原（见 src/WindowHelper.h）。
     * 这里构造、随进程析构，用上下文属性交给 QML。
     */
    WindowHelper windowHelper;
    windowHelper.attachWidget(&host);

    /*
     * 编辑器命令中枢（见 src/EditorController.h）。
     *
     * 快捷键（Ctrl+N/O/S/F/H…）挂在宿主 QWidget 上，所以必须在
     * QML 加载之前 attach；文件对话框 / 消息框也用它。
     */
    EditorController editorController;
    editorController.setSelfTestMode(SelfTest::enabled(argc, argv));
    editorController.attachWidget(&host);

    /*
     * QML 类型注册必须在加载 QML 之前。
     * QQuickWidget 内部有引擎，这里用它的 rootContext 挂上下文属性。
     */
    qmlRegisterType<EditorViewItem>("SmartClip.Editor", 1, 0, "EditorView");

    /*
     * 用 QML 单例而不是上下文属性。
     *
     * 原因：QQuickWidget 加载 QML 时，顶层属性绑定的求值早于上下文属性
     * 变得可见 —— 症状是 winHelper 在 Component.onCompleted 里正常，
     * 但在根对象构造期间读到 null（TypeError: ... of null）。
     * 单例在对象构造前就已注册，绑定第一遍就能拿到。
     */
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Store", &store);
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Win", &windowHelper);
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Cmd", &editorController);
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Shot", &screenshot);
    /*
     * 翻译那两个也走单例（不是上下文属性），理由同上面那段：
     * 卡片窗口的 QML 在对象构造期间就要读到 Llm / Trans。
     */
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Llm", &llm);
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Trans", &cards);
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Speech", &speech);
    /* 文档识别那条路（见 src/DocImport.h）—— 界面上的进度卡片读它 */
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Doc", &doc);
    /* 代码格式化（右键菜单那一条） */
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Fmt", &fmt);
    /* 编辑区校验（中文用词 / 代码语法） */
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Check", &checker);
    /* 文件对比 */
    qmlRegisterSingletonInstance("SmartClip.Globals", 1, 0, "Differ", &differ);

    /* QTP0001 = NEW 之后 QML 模块的资源前缀是 /qt/qml/<URI> */
    quick->setSource(QUrl(QStringLiteral("qrc:/qt/qml/SmartClip/Main.qml")));
    if (quick->status() == QQuickWidget::Error) {
        qWarning("QML 加载失败");
        return 1;
    }

    /*
     * 便签要主引擎：缩略图的图片提供者（image://stickythumb/…）挂在它上面，
     * 而且每块便签窗口里的 QQuickWidget 也要用它（不然会各自 new 一个引擎，
     * 既 import 不到 SmartClip.Globals，也取不到缩略图）。
     *
     * 必须赶在 notes.start() 之前 —— 那一步会按上次的清单把便签窗口建出来。
     * addImageProvider 对同名是替换语义，重复挂也只会留一份。
     */
    notes.attachEngine(quick->engine());
    /* 翻译卡片同理：QQuickWidget 要主引擎才 import 得到 SmartClip.Globals */
    cards.attachEngine(quick->engine());

    /*
     * 预建并预热选区窗口（藏着）：抓屏那一刻只剩"换图 + show"。
     *
     * 必须放在 qmlRegisterSingletonInstance 之后 —— 选区窗口的 QML 要
     * `import SmartClip.Globals`，单例还没登记就建它会报 "module not installed"。
     * 不预热的话，QML 解析 / 场景图初始化 / 4K 首帧全落在"主窗口已经藏了、
     * 选区窗口还没出来"那段空档里，屏幕上露的就是桌面，看着像闪一下。
     */
    screenshot.prewarm();

    /*
     * 便签：恢复上次摆着的那几条。
     *
     * 放在这份单例注册之后 —— 便签窗口的 QML 要 `import SmartClip.Globals`
     * （排列便签那个按钮调的是 Notes.arrangeAll），模块还没装就建窗口会报
     * "module not installed"（和上面选区窗口预热同一个坑）。
     */
    notes.start();

    /*
     * 翻译卡片：上次退出时是摆着的就按上次的样子摆回来（见 TranslateCards::start）。
     * 和便签同一个位置、同一个理由 —— 必须在这份单例注册之后。
     */
    cards.start();

    /*
     * 看一眼"贴图窗口"长什么样（`SMARTCLIP_PIN_DEMO=1`）。
     *
     * 为什么要有这个口子：贴图窗口只能在**截图那一刻**由用户按出来（框选 ->
     * 固定到桌面），外面既点不到、也不好脚本化（要先按热键、再拖出一块选区）。
     * 调样式 / 排版的时候每次都得手动走一遍，很费劲。
     *
     * 这里直接在桌面上贴一块：底图是抓屏抓来的（那块屏的左上角 640x360），
     * 上面预先划一道荧光笔、加一条文字 —— 一眼就能看出工具条挤不挤、标注和
     * 底图对不对得齐。正常启动一个字都不做。
     */
    if (qEnvironmentVariableIsSet("SMARTCLIP_PIN_DEMO")) {
        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            /*
             * 底图多大。默认 640x360；量工具条 / 提示条这类"跟着窗口宽走"的排版时
             * 得先有一张**宽图**（贴图宽到一千多时工具条会不会撑满、键会不会挤在
             * 中间一小段里，只有这个尺寸看得出来）：SMARTCLIP_PIN_DEMO_SIZE=1178x400
             */
            QSize grabSize(640, 360);
            const QStringList sizeSpec = qEnvironmentVariable("SMARTCLIP_PIN_DEMO_SIZE")
                                             .split(QLatin1Char('x'), Qt::SkipEmptyParts);
            if (sizeSpec.size() == 2 && sizeSpec.at(0).toInt() > 0
                && sizeSpec.at(1).toInt() > 0)
                grabSize = QSize(sizeSpec.at(0).toInt(), sizeSpec.at(1).toInt());
            const QPixmap grabbed = screen->grabWindow(0, 0, 0, grabSize.width(),
                                                       grabSize.height());
            if (!grabbed.isNull()) {
                auto *pin = new PinWindow(grabbed.toImage(),
                                          screen->geometry().topLeft() + QPoint(200, 200),
                                          quick->engine(), &screenshot);
                if (QQuickItem *pinRoot = pin->qmlRoot()) {
                    QVariant result;
                    QMetaObject::invokeMethod(
                        pinRoot, "testDraw", Q_RETURN_ARG(QVariant, result),
                        Q_ARG(QVariant, QVariant(QStringLiteral("highlight"))),
                        Q_ARG(QVariant, QVariant(60.0)), Q_ARG(QVariant, QVariant(90.0)),
                        Q_ARG(QVariant, QVariant(320.0)), Q_ARG(QVariant, QVariant(102.0)));
                    QMetaObject::invokeMethod(
                        pinRoot, "testAddText", Q_RETURN_ARG(QVariant, result),
                        Q_ARG(QVariant, QVariant(60.0)), Q_ARG(QVariant, QVariant(150.0)),
                        Q_ARG(QVariant, QVariant(QStringLiteral("贴图上的字可以直接改"))));
                    QMetaObject::invokeMethod(
                        pinRoot, "testDraw", Q_RETURN_ARG(QVariant, result),
                        Q_ARG(QVariant, QVariant(QStringLiteral("wavy"))),
                        Q_ARG(QVariant, QVariant(60.0)), Q_ARG(QVariant, QVariant(190.0)),
                        Q_ARG(QVariant, QVariant(300.0)), Q_ARG(QVariant, QVariant(190.0)));
                }
                pin->show();
                /*
                 * `SMARTCLIP_PIN_DEMO=save`：把窗口和成品图各存一张到 build 下。
                 * 调样式的时候不想去截图（桌面上一堆窗口挡着，截出来的东西全靠
                 * 运气），存文件最省事 —— 看一眼就知道排版和标注对不对。
                 */
                if (qEnvironmentVariable("SMARTCLIP_PIN_DEMO") == QLatin1String("save")) {
                    QTimer::singleShot(900, &app, [pin]() {
                        pin->grab().save(QStringLiteral("pin-demo-window.png"));
                        pin->composedImage().save(QStringLiteral("pin-demo-composed.png"));
                        qWarning("PIN-DEMO pos=%d,%d size=%dx%d", pin->pos().x(), pin->pos().y(),
                                 pin->width(), pin->height());
                    });
                    /*
                     * 过一会儿再报一次位置：外面可以在这个空档里用**真鼠标**拖一下，
                     * 然后比这两个数 —— "拖动到底动不动"只有真鼠标能验（进程内调
                     * 那几个函数只能验到"我们自己搬得动"）。
                     */
                    QTimer::singleShot(12000, &app, [pin]() {
                        qWarning("PIN-DEMO after=%d,%d", pin->pos().x(), pin->pos().y());
                    });
                    /*
                     * 图上选字：等认字认完（贴上去就自动认一次），把**第一行**选上
                     * 再存一张 —— 看一眼"选中的高亮"和底图上的字对不对得齐。
                     * 对齐这件事自检量不了（自检摆的是假的行），只能眼睛看。
                     *
                     * 不固定等一个时间：认字那条路可能是 PP-OCR（小档两三秒、
                     * medium 十几秒，首次还要下模型），固定几秒经常扑空。这里每
                     * 500ms 看一眼认出来没有，好了就动手（最多等 40 秒）。
                     */
                    auto *ocrPoll = new QTimer(&app);
                    ocrPoll->setProperty("n", 0);
                    ocrPoll->setInterval(500);
                    QObject::connect(ocrPoll, &QTimer::timeout, &app,
                                     [pin, ocrPoll, &app]() {
                        const int n = ocrPoll->property("n").toInt() + 1;
                        ocrPoll->setProperty("n", n);
                        if (pin->ocrLines().isEmpty() && n < 60)
                            return;
                        ocrPoll->stop();
                        QQuickItem *pinRoot = pin->qmlRoot();
                        const QVariantList lines = pin->ocrLines();
                        if (!pinRoot || lines.isEmpty()) {
                            qWarning("PIN-DEMO ocr: 没认出字（lines=%d / %s）", int(lines.size()),
                                     qPrintable(pin->ocrMessage()));
                            return;
                        }
                        const QVariantMap line = lines.first().toMap();
                        const double w = double(pin->width());
                        const double h = double(pin->height());
                        const double y =
                            (line.value(QStringLiteral("y")).toDouble()
                             + line.value(QStringLiteral("h")).toDouble() * 0.5) * h;
                        const double x0 = (line.value(QStringLiteral("x")).toDouble() + 0.005) * w;
                        const double x1 =
                            (line.value(QStringLiteral("x")).toDouble()
                             + line.value(QStringLiteral("w")).toDouble() * 0.7) * w;
                        QVariant picked;
                        QMetaObject::invokeMethod(pinRoot, "testOcrDrag",
                                                  Q_RETURN_ARG(QVariant, picked),
                                                  Q_ARG(QVariant, QVariant(x0)),
                                                  Q_ARG(QVariant, QVariant(y)),
                                                  Q_ARG(QVariant, QVariant(x1)),
                                                  Q_ARG(QVariant, QVariant(y)));
                        qWarning("PIN-DEMO ocr: %d 行，选中「%s」", int(lines.size()),
                                 qPrintable(picked.toString()));
                        /*
                         * 存图隔一拍再抓：让场景图那一帧更新完（矩形高亮是属性驱动的，
                         * 理论上同步就有，隔一拍不亏）。
                         */
                        QTimer::singleShot(400, &app, [pin]() {
                            pin->grab().save(QStringLiteral("pin-demo-ocr.png"));
                        });
                        /*
                         * 再用"划词即标注"贴一道荧光笔上去（选中还在，点一下工具按钮
                         * 就贴上），存一张 —— 色带是不是正好盖住那一行、左右到不到
                         * 选中的两端，这个只能眼睛看（自检摆的是假的行）。
                         */
                        QTimer::singleShot(600, &app, [pin, pinRoot, &app]() {
                            QMetaObject::invokeMethod(
                                pinRoot, "pickTool",
                                Q_ARG(QVariant, QVariant(QStringLiteral("highlight"))));
                            QTimer::singleShot(250, &app, [pin]() {
                                pin->grab().save(QStringLiteral("pin-demo-ocr-hl.png"));
                            });
                        });
                        /* 再把「认字」菜单叫出来存一张：两个引擎的选项长什么样 */
                        QTimer::singleShot(1200, &app, [pin, pinRoot, &app]() {
                            QMetaObject::invokeMethod(pinRoot, "testOcrMenu",
                                                      Q_ARG(QVariant, QVariant(true)));
                            QTimer::singleShot(250, &app, [pin]() {
                                pin->grab().save(QStringLiteral("pin-demo-ocr-menu.png"));
                            });
                        });
                    });
                    ocrPoll->start();
                }
            }
        }
    }

    /*
     * 看一眼"一摞便签 + 左边标签条"长什么样（`SMARTCLIP_NOTES_DEMO=1`）。
     *
     * 只在设了这个环境变量时跑：建三块不同底色的便签、归到一摞里 —— 界面上
     * 就是左边三个色块、右边露出当前那张。它是**看效果**用的口子，正常启动
     * 一个字都不做。
     *
     * 清单在 start() **之前**就指到临时目录：不然这几块演示便签会写进用户自己的
     * notes.json（踩过：跑一次演示，用户的便签清单里就多出三张"第 N 张纸"），
     * 而且会把用户自己的便签一起显示出来。
     *
     * 已经有过演示清单（上一次跑剩下的）就不再建新的：直接让 start() 把它恢复
     * 出来 —— 恢复那条路（一摞只摆一张纸、其余几块收起来）和正常启动完全一样，
     * 正好用来看"重启之后是不是还是那个样子"。
     */
    if (qEnvironmentVariableIsSet("SMARTCLIP_NOTES_DEMO")) {
        const QString demoPath = QDir::tempPath() + QStringLiteral("/smartclip-notes-demo.json");
        const bool restoreExisting = QFileInfo::exists(demoPath);
        notes.store()->setFilePath(demoPath);
        if (restoreExisting) {
            notes.start();
        } else {
            QList<StickyNote *> demo;
            const QStringList colors{QStringLiteral("#ffe9a8"), QStringLiteral("#f7b6d2"),
                                     QStringLiteral("#c9b6f7")};
            for (int i = 0; i < colors.size(); ++i) {
                StickyNote *note = notes.createNote();
                if (!note)
                    continue;
                note->setColor(QColor(colors.at(i)));
                note->setText(QStringLiteral("第 %1 张纸：这份便签的底色是 %2。\n"
                                             "左边那排色块就是这一摞里的几张纸，点一下换一张。")
                                  .arg(i + 1).arg(colors.at(i)));
                demo.append(note);
            }
            /* 第一块（黄的）露头，其余两块跟着它 —— demo.first() 就是露头那张 */
            if (demo.size() > 1)
                notes.groupWith(demo.first(), demo.mid(1));
            notes.store()->flush();
        }
    }

    /*
     * 开发口子：`SMARTCLIP_QUIT_AFTER_MS=<毫秒>` —— 起来这么多毫秒之后走一遍
     * **真正的退出**（和关闭键问句里的「完全退出」、文件菜单里的「退出」同一条
     * 路：WindowHelper::quitApp）。
     *
     * 为什么要有它：便签窗口是各自独立的顶层窗口，"有便签的时候退不出程序"
     * 这类问题从外面既点不到、也不好判断进程到底退没退。用法：
     *     SMARTCLIP_NOTES_DEMO=1 SMARTCLIP_QUIT_AFTER_MS=8000
     * 起来三块便签、8 秒后自动退出；随后看进程是不是真没了、退出码是不是 0，
     * 以及那份临时清单里三块便签是不是还写着 visible=true（下次启动照旧显示）。
     */
    bool quitAfterOk = false;
    const int quitAfterMs = qEnvironmentVariableIntValue("SMARTCLIP_QUIT_AFTER_MS", &quitAfterOk);
    if (quitAfterOk && quitAfterMs > 0) {
        QTimer::singleShot(quitAfterMs, &app, [&windowHelper]() {
            qWarning("QUIT-TRACE SMARTCLIP_QUIT_AFTER_MS -> quitApp()");
            windowHelper.quitApp();
        });
    }

    /*
     * 便签专用自检（`SmartClip.exe --note-test`，见 src/SelfTest.h 的 runNotes）。
     *
     * 和下面那套全量自检分开：这一条**只测便签**，别的功能一律不碰。
     * 全量自检里截图 / 设置面板那几节有自己的时序问题（一轮跑下来会偶发飘红，
     * 和便签无关），只改便签的时候没必要每次都把那些跑一遍 —— 而且那些检查会
     * 开选区窗口 / 弹卡片，界面上看着乱跳。
     *
     * 这一条也不显示主窗口：省得跟着便签一起晃。
     */
    const bool noteTest = SelfTest::noteTestEnabled(argc, argv);
    /*
     * 翻译专用自检（`--translate-test`，见 src/SelfTestTranslate.cpp）。
     * 和便签那条同一个用意：只跑翻译那一节，不显示主窗口、不弹模态框。
     */
    const bool translateTest = SelfTest::translateTestEnabled(argc, argv);
    /*
     * 文档识别专用自检（`--doc-test`，见 src/SelfTestDoc.cpp）。
     * 一样只跑自己那一节：不跑真脚本（几十秒到几分钟），只验解析 / 改写 / 落盘。
     */
    const bool docTest = SelfTest::docTestEnabled(argc, argv);
    /*
     * 端到端那条（`--doc-e2e`）：真造 PDF、真调脚本、真落笔记。
     * 单独一个开关是因为它慢（首次跑几十秒）且依赖本机装了识别包；
     * 没装的话它会**报出来**而不是崩。
     */
    const bool docE2e = SelfTest::docE2eEnabled(argc, argv);
    /*
     * 队列那条（`--doc-queue`）：enqueue -> 线程池 -> 落成笔记，
     * 也就是界面真正走的那条异步路（和 --doc-e2e 的同步路互补）。
     */
    const bool docQueue = SelfTest::docQueueEnabled(argc, argv);
    /*
     * 看一眼"文档识别卡片"长什么样、位置对不对（`--doc-demo=<文件>`）。
     *
     * 为什么要有这个口子：卡片是**跟随识别进度**出现的一块原生小窗，正常要
     * "拖一份文档进去、等它认完"才看得到 —— 调它的位置 / 观感时每次都得手动走
     * 一遍，还未必抓得到那一瞬间。这条直接在启动时排一份真文件进去，窗口照常
     * 显示，卡片就摆在那儿，截图 / 肉眼都能看。
     *
     * 位置校验走自检（--doc-queue 里量了几何）；这个口子是给"看着对不对"用的
     * —— 层级那件事（有没有被原生编辑区盖住）只有眼睛和截图说了算。
     */
    QString docDemoPath;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.startsWith(QLatin1String("--doc-demo=")))
            docDemoPath = arg.mid(int(qstrlen("--doc-demo=")));
    }
    const bool docDemo = !docDemoPath.isEmpty();

    /*
     * 自动自检那几条**不显示**主窗口（省得屏幕上窗口乱跳、也免了截图干扰）；
     * 正常启动和 `--doc-demo` 要显示 —— 演示就是给人看卡片的。
     */
    if (docDemo || (!noteTest && !translateTest && !docTest && !docE2e && !docQueue)) {
        host.resize(1460, 900);
        host.show();
    }

    /*
     * 文档识别那张卡片的位置：**由这里推给界面**。
     *
     * 为什么不在 QML 里算：QML 那个 ApplicationWindow 的 x/y 和宿主 QWidget 的
     * geometry 不是同一套坐标系（实测宿主在 (300,160) 时 QML 读出来 x=0），
     * 拿它算会算到屏幕中间去。只有这里知道宿主窗口的真实几何。
     *
     * 用 60ms 轮询而不是接移动 / 改大小事件：窗口在这台机器上移动时并不总是
     * 产生 QWidget 的 moveEvent，轮询最稳。
     *
     * 建在自检分支**之前**：`--doc-queue` 里要验"位置真的被推过去了"，窗口虽然
     * 不显示但几何是有的（隐藏窗口也有 geometry），推过去的值正好拿来对算式。
     */
    {
        auto *cardGeo = new QTimer(&app);
        cardGeo->setInterval(60);
        QObject::connect(cardGeo, &QTimer::timeout, &doc, [&host, &doc]() {
            const QRect geo = host.geometry();
            /* 这几个数都在 DocImport 里（唯一真相），别在这儿写死 */
            doc.publishCardGeometry(geo.x() + geo.width() - doc.cardWidth() - doc.cardMargin(),
                                    geo.y() + geo.height() - doc.cardHeightHint()
                                        - doc.cardBottomGap());
        });
        cardGeo->start();
    }

    /*
     * 圆角遮罩要在原生窗口真正创建之后再落一次。
     *
     * 只靠 attachWidget 里那次和 resize 事件不够：首次调用时原生窗口
     * 可能还没建立，setMask 不会生效（实测四角只裁掉了一个）。
     * 这里 show() 之后立刻一次、事件循环起来再一次，确保落到最新几何上。
     */
    windowHelper.refreshMask();
    QTimer::singleShot(0, &app, [&windowHelper]() { windowHelper.refreshMask(); });

    /*
     * 托盘图标（任务栏右下角那个）：右键菜单里有"截图…"和便签那三条，
     * 主窗口被压着 / 缩在一边时不用先叫它出来就能截图、就能开一块便签。
     * 具体在那个类里，见 src/TrayIcon.h。
     *
     * 建在自检分支**之前**：自检要验那个菜单（见 src/SelfTest.cpp），
     * 拿不到对象就验不了。自检模式下它只是短暂亮一下，无害。
     */
    TrayIcon tray(&host, &screenshot, &editorController, &notes, &cards, &app);

    if (noteTest) {
        int result = -1;
        QTimer::singleShot(600, &app, [&]() {
            result = SelfTest::runNotes(&store, &tray, &editorController, &notes);
            app.quit();
        });
        QTimer::singleShot(40000, &app, []() {
            qWarning("便签自检超时，强制退出");
            ::exit(9);
        });
        app.exec();
        notes.shutdown();
        cards.shutdown();
        llm.shutdown();
        doc.shutdown();
        return result < 0 ? 9 : result;
    }

    if (translateTest) {
        int result = -1;
        QTimer::singleShot(600, &app, [&]() {
            result = SelfTest::runTranslate(&cards, &llm, &tray, &speech);
            app.quit();
        });
        QTimer::singleShot(40000, &app, []() {
            qWarning("翻译自检超时，强制退出");
            ::exit(9);
        });
        app.exec();
        /*
         * 不再调 cards.shutdown()：翻译自检自己会落盘并把改过的配置写回去
         * （见 SelfTestTranslate.cpp），这里再 save 一次会把那份恢复覆盖掉。
         */
        llm.shutdown();
        return result < 0 ? 9 : result;
    }

    if (docTest) {
        int result = -1;
        QTimer::singleShot(300, &app, [&]() {
            result = SelfTest::runDoc(&store);
            app.quit();
        });
        QTimer::singleShot(40000, &app, []() {
            qWarning("文档识别自检超时，强制退出");
            ::exit(9);
        });
        app.exec();
        /* 没有真跑脚本，也就没有子进程要收；shutdown 是幂等的，顺手调一下 */
        doc.shutdown();
        return result < 0 ? 9 : result;
    }

    /*
     * 新增工具那一节（`--tool-test`）：文件对比 / 格式化 / 校验 / Markdown 预览。
     *
     * 和便签 / 翻译 / 识别那几条一样，不显示主窗口、不弹任何窗口 ——
     * 这几件的判断逻辑全在 C++ 里（src/Diff.h、Formatter.h、Checker.h、
     * EditorController::markdownHtml），开窗口反而会引入时序问题。
     */
    if (SelfTest::toolTestEnabled(argc, argv)) {
        int result = -1;
        QTimer::singleShot(200, &app, [&]() {
            result = SelfTest::runTools(&fmt, &differ, &checker, &llm, quick->rootObject());
            app.quit();
        });
        QTimer::singleShot(30000, &app, []() {
            qWarning("工具自检超时，强制退出");
            ::exit(9);
        });
        app.exec();
        llm.shutdown();
        return result < 0 ? 9 : result;
    }

    if (docQueue) {
        int result = -1;
        /* 和 --doc-e2e 一样，可以指一个别的解释器：--doc-python=<路径> */
        QString pythonExe;
        for (int i = 1; i < argc; ++i) {
            const QString arg = QString::fromLocal8Bit(argv[i]);
            if (arg.startsWith(QLatin1String("--doc-python="))) {
                pythonExe = arg.mid(int(qstrlen("--doc-python=")));
                break;
            }
        }
        QTimer::singleShot(300, &app, [&]() {
            result = SelfTest::runDocQueue(&doc, &store, pythonExe, quick->rootObject());
            app.quit();
        });
        QTimer::singleShot(900000, &app, []() {
            qWarning("文档识别队列自检超时，强制退出");
            ::exit(9);
        });
        app.exec();
        doc.shutdown();
        return result < 0 ? 9 : result;
    }

    if (docE2e) {
        int result = -1;
        QTimer::singleShot(300, &app, [&]() {
            result = SelfTest::runDocE2e(&store);
            app.quit();
        });
        /*
         * 给足 15 分钟：首次跑要下/加载模型，慢的时候一分多钟一页。
         * 这不是"卡住了"，是这条自检本来就要真跑一遍。
         */
        QTimer::singleShot(900000, &app, []() {
            qWarning("文档识别端到端自检超时，强制退出");
            ::exit(9);
        });
        app.exec();
        doc.shutdown();
        return result < 0 ? 9 : result;
    }

    /*
     * 自检模式（`SmartClip.exe --self-test`，见 src/SelfTest.h）。
     *
     * 编辑区是原生子窗口，外面用合成键鼠点不动它，所以留这条进程内通道：
     * 事件循环起来之后按命令跑一遍真实链路（QML dispatch -> QScintilla -> 磁盘），
     * 结果作为退出码，不打开界面也不常驻。
     */
    if (SelfTest::enabled(argc, argv)) {
        int result = -1;
        /* 给 QML 引擎一点时间把原生子窗口（编辑区）真正建起来再跑检查 */
        QTimer::singleShot(600, &app, [&]() {
            result = SelfTest::run(quick->rootObject(), &store, &screenshot, &tray,
                                   &editorController, &notes, &cards, &llm, &speech);
            app.quit();
        });

        /*
         * 兜底：自检万一卡住（例如某条检查触发了模态框），进程会一直挂着，
         * 桌面上就留一个点不掉的窗口。超时直接结束进程，绝不留残留。
         */
        QTimer::singleShot(60000, &app, []() {
            qWarning("自检超时，强制退出");
            ::exit(9);
        });

        app.exec();
        /*
         * 收尾：便签的缩略图有在飞的网络请求，必须在事件循环还活着的时候
         * 掐掉（见 StickyNotes::shutdown）—— 留到析构那会儿，Qt 网络层会在
         * 进程退出时踩空，退出码变成 0xC0000005（自检全过也照样崩）。
         * 正常启动那条路同理，见下面 app.exec() 之后那一行。
         */
        notes.shutdown();
        /*
         * 翻译这边只收尾 LLM（本地推理服务进程），**不**再调 cards.shutdown()：
         * 翻译自检自己会落盘并把改过的配置写回去（见 SelfTestTranslate.cpp），
         * 这里再 save 一次会把那份恢复覆盖掉。
         */
        llm.shutdown();
        /* 识别那条路的子进程也在这里收掉（见 DocImport::shutdown） */
        doc.shutdown();
        return result < 0 ? 9 : result;
    }

    /*
     * 演示模式：等界面摆好，把那份文件排进识别队列（见上面 docDemo 的说明）。
     * 用事件循环起一次而不是直接调 —— 卡片要等 QML 那边把自己摆好（setHost）。
     */
    if (docDemo) {
        /*
         * 先把主窗口摆到一个**确定的**位置（而不是让系统随意放）。
         *
         * 为什么演示要管这个：卡片的位置是"主窗口右边一个 margin、下边留状态栏
         * 那么高"，主窗口在屏幕哪个角落决定了卡片在哪。摆在固定位置，截图裁哪块
         * 就是确定的，肉眼核对也不会因为窗口位置不同而看岔。
         */
        host.move(300, 160);
        QTimer::singleShot(400, &doc, [&doc, docDemoPath]() {
            doc.setTier(QStringLiteral("fast"));
            doc.enqueue({ docDemoPath });
        });
    }

    const int code = app.exec();
    notes.shutdown();
    cards.shutdown();
    llm.shutdown();
    doc.shutdown();
    return code;
}