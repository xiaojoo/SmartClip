#include "ClipboardManager.h"
#include "ClipboardStore.h"
#include "EditorController.h"
#include "EditorViewItem.h"
#include "Screenshot.h"
#include "SelfTest.h"
#include "WindowHelper.h"

#include <QApplication>
#include <QAbstractNativeEventFilter>
#include <QColor>
#include <QGuiApplication>
#include <QPalette>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QQuickWidget>
#include <QSystemTrayIcon>
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
 * 背景：这些 QtWidgets 弹框（QMessageBox / QInputDialog）弹出时，用户往往正在
 * 主窗口里敲键盘（菜单助记键、上一秒的按键、输入法送出的字符…）。主窗口这时
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
     * 工具提示（ToolTip）+ QtWidgets 对话框的全局配色。
     *
     * 界面整个是深色的，但 Fusion 那个 ToolTip 模板的背景取的是调色板里的
     * toolTipBase、文字取 toolTipText（见 Qt 的 Fusion/ToolTip.qml），不设的话
     * 就是系统默认的浅色底 —— 鼠标停在按钮上弹出一块白，跟界面完全不搭
     * （用户报的就是"收起替换"上面那个"显示 / 隐藏替换行"）。
     *
     * 下面这几组角色管的是 QtWidgets 那几样东西：QMessageBox（Cmd.alert）、
     * QInputDialog（转到行 / 自定义参考线列）这类对话框。不设的话它们也是
     * 系统浅色，深色界面里点一下弹出一块白。
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
     * 主窗口用 QWidget 承载，而不是 QQmlApplicationEngine 直接开 QQuickWindow。
     *
     * 为什么必须这样（这是"编辑器真正嵌进去"能否成立的前提）：
     *   QScintilla 是 QWidget。要让它的圆角/裁剪跟随 QML 卡片，它必须和 QML
     *   处在同一个 QWidget 层级里。QQuickWindow 不是 QWidget、没有层级可挂，
     *   所以之前 QScintilla 只能退化成独立顶层窗口 —— 靠手算坐标跟随，会"分家"，
     *   QML 的 radius 也裁不到它。
     *
     *   QWidget(主窗口，无边框+透明)
     *     ├── QQuickWidget(整个 QML 界面)
     *     └── QsciScintilla(编辑器，同一层级 → 圆角/裁剪/QML 层级都成立)
     */
    QWidget host;
    host.setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    host.setAttribute(Qt::WA_TranslucentBackground);
    host.setAutoFillBackground(false);
    host.setWindowTitle(QStringLiteral("SmartClip — 剪贴板"));

    auto *quick = new QQuickWidget(&host);
    quick->setResizeMode(QQuickWidget::SizeRootObjectToView);
    /*
     * 不要给 QQuickWidget 设 WA_TranslucentBackground —— 实测那样整块会变黑。
     * 窗口透明由 QWidget 的 WA_TranslucentBackground + QML 根元素
     * color: "transparent" 加上圆角遮罩共同实现。
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

    /* QTP0001 = NEW 之后 QML 模块的资源前缀是 /qt/qml/<URI> */
    quick->setSource(QUrl(QStringLiteral("qrc:/qt/qml/SmartClip/Main.qml")));
    if (quick->status() == QQuickWidget::Error) {
        qWarning("QML 加载失败");
        return 1;
    }

    host.resize(1460, 900);
    host.show();

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
            result = SelfTest::run(quick->rootObject(), &store, &screenshot);
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
        return result < 0 ? 9 : result;
    }

    QSystemTrayIcon tray(QIcon::fromTheme("edit-paste"), &app);
    tray.setToolTip("SmartClip");
    tray.show();

    return app.exec();
}
