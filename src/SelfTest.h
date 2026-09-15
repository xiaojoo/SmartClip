#pragma once

class QObject;
class ClipboardStore;
class Screenshot;
class TrayIcon;
class EditorController;
class StickyNotes;
class TranslateCards;
class LlmClient;

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

/* qmlRoot 是 Main.qml 的根对象；run() 通过它的 dispatch() 发命令 */
int run(QObject *qmlRoot, ClipboardStore *store, Screenshot *screenshot = nullptr,
        TrayIcon *tray = nullptr, EditorController *cmd = nullptr,
        StickyNotes *notes = nullptr, TranslateCards *cards = nullptr,
        LlmClient *llm = nullptr);

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
 * 翻译专用自检：**只测翻译 / 识别**（卡片界面 / 双向状态 / 请求那套的 token 契约 /
 * 落盘 / 识别用哪个模型、图片按多模态格式发出去、回来的"原文 ---- 译文"拆得开），
 * 不碰编辑区、不碰截图，也不发真请求。
 *
 * 和便签那份同一个用意：改翻译的时候不用把 SelfTest.cpp 那几千行全跑一遍。
 * 它不显示主窗口，也不弹任何模态框。返回失败项数（0 = 全过）。
 */
int runTranslate(TranslateCards *cards, LlmClient *llm = nullptr, TrayIcon *tray = nullptr);

/*
 * 翻译自检跑完之后，它那几十项里通过了几项、失败了几项。
 * 全量自检（run）要把这两笔并进自己的总计里 —— 理由同 notesPassed。
 */
int translatePassed();
int translateFailed();

}  // namespace SelfTest
