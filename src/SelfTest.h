#pragma once

class QObject;
class ClipboardStore;
class Screenshot;

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
 *       写入失败上报 / 截图（选区 -> 加文字 -> 合成）。
 *
 * 返回 0 表示全部通过（main.cpp 据此作为进程退出码）。
 */
namespace SelfTest {

bool enabled(int argc, char **argv);

/* qmlRoot 是 Main.qml 的根对象；run() 通过它的 dispatch() 发命令 */
int run(QObject *qmlRoot, ClipboardStore *store, Screenshot *screenshot = nullptr);

}  // namespace SelfTest
