#pragma once

#include <QImage>
#include <QString>
#include <QVariantList>

/*
 * 贴图上"用鼠标把字选出来"要的那点东西：**图上每一行字在哪儿**。
 *
 * 用 Windows 自带那套 OCR（Windows.Media.Ocr），不走项目里那条视觉模型的路：
 * Llm.recognize 回来的是一整段文字、**没有坐标** —— 拿它叠文字层只能瞎猜位置，
 * 而"像文本一样拖选"必须知道每行的外框在哪儿。自带这套是本机离线、不联网、
 * 不用配模型，给的正是**词级外框**（OcrWord::BoundingRect），行框取词框的并集。
 *
 * 语言：先试中文（zh-Hans-CN），建不出来退到系统语言、再退英文。
 * 装没装语言包看 `C:\Windows\OCR\` 下面有哪几个目录（zh-cn / en-us）。
 *
 * 坐标一律**归一化**（0~1，相对整张图）：贴图能缩放、能放大缩小，归一化之后
 * 两边都不用换算，QML 那边乘一下显示尺寸就行。
 *
 * 注意：这个文件**不能**让别人 include 到 WinRT 的东西 —— WinRT 的头只出现在
 * PinOcr.cpp 里（那个文件里还带着一堆 Windows 宏，扩散出去迟早撞上别的源文件）。
 */
namespace PinOcr {

/* 这台机器上能不能用（引擎建得出来 = 装了至少一套语言包） */
bool available();

/* 实际用的语言标签（"zh-Hans-CN" / "en-US" …）；不能用时是空串 */
QString language();

/*
 * 认一张图。返回一行一条：{ text, x, y, w, h }，坐标是 0~1 的归一化值。
 * 认不出字 = 空列表；引擎不可用也是空列表（这两件事用 available() 分辨）。
 *
 * error 不为空时，出岔子会往里写一句人话（引擎没有 / 调用失败的原因）——
 * 自检和界面报错都用它，不然"认不出"和"引擎报错"在界面上长得一模一样。
 *
 * **会阻塞**（一屏几十到几百毫秒），必须在工作线程里调 —— 见 PinWindow::startOcr。
 */
QVariantList recognize(const QImage &image, QString *error = nullptr);

/* ===========================================================================
 * PP-OCR 那条路：**跑本机的一个程序**，不把 OCR 做进进程里
 * ======================================================================== */

/*
 * 为什么是"跑程序"而不是做进进程里：PP-OCRv6 的 ONNX（medium 检测 62MB + 识别
 * 73MB）还得配前后处理（检测框阈值、透视裁切、CTC 解码 + 字典）才跑得起来，
 * 那是几百行；而 RapidOCR / PaddleOCR 那边早把这一整套做完了，装完就是一条
 * Python 命令。所以这里只定一个**很窄的约定**，活交给本机那个程序：
 *
 *     <整条命令…>  <图片.png 路径>  <结果.json 路径>
 *
 * 两个路径接在**最后**（前面是命令自带的参数，比如脚本的档位词 `medium`）——
 * 因为 `python "…/ppocr_runner.py" medium` 这种命令里，脚本路径必须是 python 的
 * 第一个参数，图片只能往后排。随包脚本读的就是最后两个参数。
 *
 * 程序把结果写进最后那个文件：一行一条，box 是**像素**坐标
 * （x/y 左上角，w/h 宽高）：
 *     [{"text": "第一行", "box": [10, 20, 200, 30]}, ...]
 * 归一化（0~1）由这里做 —— 程序不用知道界面的坐标约定。
 *
 * 随包脚本见 resources/scripts/ppocr_runner.py（基于 RapidOCR），第一次用到时
 * 落到 AppData 下（qrc 里那份只读，落到磁盘上用户才能自己改）。
 */
QVariantList recognizeWithProgram(const QImage &image, const QString &command,
                                  QString *error = nullptr);

/* 默认那条命令：python + 随包脚本。找不到 python 就退成字面量 "python"，
   让 runnerProblem() 把"PATH 里找不到"摊到界面上 */
QString defaultRunnerCommand();

/* 随包脚本落在磁盘上的路径（不存在就从 qrc 写一份出去） */
QString runnerScriptPath();

/* 这条命令能不能跑：空串 = 能；否则一句人话（没填 / 找不到程序） */
QString runnerProblem(const QString &command);

/*
 * 把一段 JSON 解析成行：{ text, x, y, w, h }（坐标归一化 0~1）。
 *
 * **只有这一份**：PP-OCR 那个程序写出来的文件就靠它解析 / 兜底 / 归一化。
 * 宽容的地方：
 *   * 外面裹了 ```json 之类的代码块、或者前后夹了句废话 -> 抠出最外层那对括号；
 *   * {"lines": [...]} 和裸 [...] 都认；
 *   * box 写成 [x,y,w,h]，或者散着写 x/y/w/h，都认；
 *   * 四个数都 ≤ 1.5 当成已经归一化，否则当像素（除以 imageSize）。
 */
QVariantList parseLinesJson(const QString &json, const QSize &imageSize,
                            QString *error = nullptr);

}  // namespace PinOcr
