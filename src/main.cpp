#include "ClipboardManager.h"
#include "ClipboardStore.h"
#include "EditorController.h"
#include "EditorViewItem.h"
#include "Screenshot.h"
#include "SelfTest.h"
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
    if (!noteTest && !translateTest) {
        host.resize(1460, 900);
        host.show();
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
        return result < 0 ? 9 : result;
    }

    if (translateTest) {
        int result = -1;
        QTimer::singleShot(600, &app, [&]() {
            result = SelfTest::runTranslate(&cards, &llm, &tray);
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
                                   &editorController, &notes, &cards, &llm);
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
        return result < 0 ? 9 : result;
    }

    const int code = app.exec();
    notes.shutdown();
    cards.shutdown();
    llm.shutdown();
    return code;
}
