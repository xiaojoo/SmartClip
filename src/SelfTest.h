#pragma once

/*
 * QString 要**完整的类型**（不是前置声明）：下面 runDocQueue 的默认实参
 * 写的是 `QString()`，那需要构造函数可见。
 */
#include <QString>

class QObject;
class ClipboardStore;
class Screenshot;
class TrayIcon;
class EditorController;
class StickyNotes;
class TranslateCards;
class LlmClient;
class Speech;
class DocImport;
class Formatter;
class DiffEngine;
class DiffEngine;
class Checker;
class Summarizer;

/*
 * 自检模式：`SmartClip.exe --self-test`
 *
 * 为什么要有这个：编辑区是**原生 QScintilla 子窗口**，用系统级的合成键鼠
 * （SendKeys / SendInput）去点按钮，事件根本进不到那个原生窗口，
 * 界面能不能用就成了"只能靠眼睛看"。所以留一条自检路径：
 * 直接在进程里按命令走一遍真实链路
 *
 *   QML 的 dispatch()  ->  EditorViewItem（QScintilla）  ->  磁盘文件
 *
 * 覆盖：打开 / 编码识别 / 换行符 / 查找 / 全部高亮 / 全部替换 / 撤销 /
 *       注释切换 / 另存为 / BOM / 多标签切换关闭 / 剪贴板条目载入 /
 *       写入失败上报 / 截图（选区 -> 加文字 -> 合成）/ 托盘菜单里的截图 /
 *       截图识别（选区那块图取得出来 -> 结果摆进卡片 -> 复制 / 加到图上）/
 *       贴图窗口（划重点的四个工具 / 文字能改 / 撤销 / 缩放 / 成品图里真的有 /
 *       只有左键按住才拖、鼠标光划过不动、单击左/右键都不关 /「翻译」摆得出卡片 /
 *       图上选字：两个引擎（Windows 自带 / PP-OCR 本机程序）、PP-OCR 那条命令跑得通
 *       且结果解析得出来、拖选一段、跨行连着选、空白处还是拖窗口）/
 *       便签（新建 -> 换底色 -> 链接缩略图 -> 落盘 -> 一键排列 -> 收进托盘也能用）。
 *
 * 返回 0 表示全部通过（main.cpp 据此作为进程退出码）。
 */
namespace SelfTest {

bool enabled(int argc, char **argv);

/* `--note-test`：只跑便签那一节（见 runNotes） */
bool noteTestEnabled(int argc, char **argv);

/* `--translate-test`：只跑翻译那一节（见 runTranslate） */
bool translateTestEnabled(int argc, char **argv);

/* `--doc-test`：只跑文档识别那一节（见 runDoc） */
bool docTestEnabled(int argc, char **argv);

/*
 * `--tool-test`：只跑"新增工具"那一节（文件对比 / 格式化 / 校验 / Markdown
 * 预览，见 runTools）。
 *
 * 和便签 / 翻译 / 识别那几条同一个用意：这几件新东西的判断逻辑全在 C++ 里，
 * 不用开主窗口就能验，改它们的时候不必把 SelfTest.cpp 那几千行跑一遍。
 */
bool toolTestEnabled(int argc, char **argv);

/*
 * `--doc-e2e`：文档识别**真跑一遍**（造一份 PDF -> 调真脚本 -> 落成笔记）。
 *
 * 和 --doc-test 分开是因为它慢：真脚本第一次跑要几十秒（RapidDoc 首次加载模型），
 * 没装任何识别包的机器上还会直接失败。所以它**不**并进 --doc-test，也不并进
 * 全量自检 —— 要验"脚本那条路真的通"的时候单独跑这一条。
 */
bool docE2eEnabled(int argc, char **argv);

/*
 * `--doc-queue`：文档识别**走队列真跑一遍**（DocImport::enqueue -> 线程池里的
 * Task -> 落成笔记 -> finished 信号）。
 *
 * 和 --doc-e2e 的区别：那条直接调 DocConvert::convert（同步），验的是"脚本那条
 * 路通不通"；这一条走的是**界面真正走的那条路** —— enqueue 排队、Task 在线程池
 * 里跑、结果从 finished 信号回来、createNote 落盘。也就是把"零件都好用"和
 * "装起来也好用"之间那个缺口补上。
 */
bool docQueueEnabled(int argc, char **argv);

/* qmlRoot 是 Main.qml 的根对象；run() 通过它的 dispatch() 发命令 */
int run(QObject *qmlRoot, ClipboardStore *store, Screenshot *screenshot = nullptr,
        TrayIcon *tray = nullptr, EditorController *cmd = nullptr,
        StickyNotes *notes = nullptr, TranslateCards *cards = nullptr,
        LlmClient *llm = nullptr, Speech *speech = nullptr);

/*
 * 便签专用自检：**只测便签**，别的功能一律不碰。
 *
 * 为什么单独留一个入口：全量自检里截图 / 设置面板那几节有自己的时序问题
 * （偶发飘红，和便签无关），改便签的时候没必要每次都把它们跑一遍 —— 那些检查
 * 还会开选区窗口 / 弹卡片，界面上看着乱跳。这一条不显示主窗口、不开选区窗口，
 * 只建便签、量便签。
 *
 * store / tray / cmd 可以为空（对应的那几条检查会跳过），notes 必须有。
 * 返回失败项数（0 = 全过）。
 */
int runNotes(ClipboardStore *store, TrayIcon *tray = nullptr,
             EditorController *cmd = nullptr, StickyNotes *notes = nullptr);

/*
 * 便签自检跑完之后，它那 80 多项里通过了几项、失败了几项。
 *
 * 全量自检（run）要把这两笔并进自己的总计里 —— 只并失败数的话，总数上会显得
 * "便签那几十项凭空没了"（实测：便签 82 项全过，总数却还是 405）。
 */
int notesPassed();
int notesFailed();

/*
 * 翻译专用自检：**只测翻译 / 识别 / 朗读**（卡片界面 / 双向状态 / 请求那套的
 * token 契约 / 落盘 / 识别用哪个模型、图片按多模态格式发出去、回来的
 * "原文 ---- 译文"拆得开 / 朗读那个喇叭和系统语音合成），
 * 不碰编辑区、不碰截图，也不发真请求。
 *
 * 和便签那份同一个用意：改翻译的时候不用把 SelfTest.cpp 那几千行全跑一遍。
 * 它不显示主窗口，也不弹任何模态框。返回失败项数（0 = 全过）。
 *
 * speech 是朗读那个单例（见 src/Speech.h）：传了才会跑朗读那一节；那台机器上
 * 没装语音包时那一节只钉"界面画灰、点了不崩"，不判失败。
 */
int runTranslate(TranslateCards *cards, LlmClient *llm = nullptr, TrayIcon *tray = nullptr,
                 Speech *speech = nullptr);

/*
 * 翻译自检跑完之后，它那几十项里通过了几项、失败了几项。
 * 全量自检（run）要把这两笔并进自己的总计里 —— 理由同 notesPassed。
 */
int translatePassed();
int translateFailed();

/*
 * 文档识别专用自检：**只测"文件 -> Markdown -> 笔记"这条链路**
 * （结果 JSON 的解析 / 图片引用改写 / 命令拼法 / 笔记落盘 + assets），
 * 不跑真脚本（那要几十秒到几分钟，还依赖本机装没装那几个包）。
 *
 * 和便签 / 翻译那两份同一个用意：改识别的时候不用把 SelfTest.cpp 那几千行
 * 全跑一遍。不显示主窗口，也不弹任何模态框。返回失败项数（0 = 全过）。
 */
int runDoc(ClipboardStore *store = nullptr);

/*
 * 文档识别的端到端自检：真造一份 PDF、真调那条命令、真落成笔记，再把笔记清掉。
 *
 * 跑的是**设置里配的那条命令**（DocImport::runner），所以它验的正是用户实际
 * 会走的那条路。返回失败项数（0 = 全过）；脚本没装好会报出来而不是崩。
 */
int runDocE2e(ClipboardStore *store = nullptr);

/*
 * 文档识别队列自检：真文件 -> enqueue -> 线程池 -> 落成笔记。
 *
 * 会等 finished 信号（最多等两分钟），所以它**必须**跑在事件循环里
 * （main.cpp 那边用 QTimer::singleShot 起）。返回失败项数（0 = 全过）。
 *
 * doc **必须是 QML 里那个 Doc 单例本身**（不是新 new 一个）。踩过：一开始在这里
 * `DocImport doc(store)` 自己建了一个，于是"给它排队、去问 QML 里的单例"问的是
 * 两个对象 —— 界面那一侧（卡片开不开、busy 亮不亮）永远没被驱动过，检查全红，
 * 而且红得莫名其妙。
 *
 * qmlRoot 是 Main.qml 的根对象（拿 uiState 量卡片几何）；给 nullptr 就跳过
 * 卡片那一段。
 */
int runDocQueue(DocImport *doc, ClipboardStore *store, const QString &pythonExe,
                QObject *qmlRoot);

/*
 * 文档识别自检跑完之后，它那几十项里通过了几项、失败了几项。
 * 全量自检（run）要把这两笔并进自己的总计里 —— 理由同 notesPassed。
 */
int docPassed();
int docFailed();

/*
 * 新增工具那一节的自检：文件对比 / 格式化 / 校验 / Markdown 预览。
 *
 * 三个对象都由 main.cpp 传进来（它们是 QML 单例，界面和自检必须是**同一份**
 * —— 自己 new 一个的话，自检改的设置界面看不到，界面上开着的开关自检也读不到）。
 * 任何一个传空，对应的那一节就跳过。
 *
 * qmlRoot 是 Main.qml 的根对象：预览那份右键菜单、以及"打开 .md 之后能不能
 * 预览"这些只有 QML 那侧答得出来（见 editMenuPreviewActs / canPreviewMarkdown），
 * 传空就跳过那一节。返回失败项数（0 = 全过）。
 */
int runTools(Formatter *fmt, DiffEngine *differ, Checker *check, LlmClient *llm = nullptr,
             QObject *qmlRoot = nullptr);
int toolsPassed();
int toolsFailed();

/*
 * 汇总那一节的自检（`--summarize-test`，见 runSummarize）：一段时间里的原文
 * 取不取得对、模型的回复解析得对不对、草稿采纳之后有没有并进文档、
 * 归档能不能把剪贴板文件藏起来又只藏它，最后把设置面板那两栏真开出来量尺寸。
 *
 * 前半截全在 C++ 里判断（ClipboardStore 那几条 + Summarizer::parseReply），
 * 一条请求都不发 —— 模型整理得好不好只能靠人看，钉不住；这里钉的是"链路对不对"。
 */
bool summarizeTestEnabled(int argc, char **argv);

/*
 * sum 可以为空（那样解析回复那一节跳过）。**必须传 main.cpp 里那个 Sum 单例**：
 * 自检改的是 store 看到的磁盘和库，另建一份 Summarizer 就等于没测界面上那个。
 * llm 同一条约定 —— 流水线那一节要临时把接口地址改到本机一个假模型服务上，
 * 跑完按原样写回（见那个文件里 ScriptedLlm 那段）。
 *
 * qmlRoot 是 Main.qml 的根对象：最后一节要真开设置面板量「汇总」「归档」那两栏
 * （面板的尺寸绑在宿主窗口上，所以这条自检**会**把主窗口开出来，和 --tool-test
 * 同一个取舍）。传空就跳过界面那一节。返回失败项数（0 = 全过）。
 */
int runSummarize(ClipboardStore *store, Summarizer *sum = nullptr, QObject *qmlRoot = nullptr,
                 LlmClient *llm = nullptr);
int summarizePassed();
int summarizeFailed();

}  // namespace SelfTest
