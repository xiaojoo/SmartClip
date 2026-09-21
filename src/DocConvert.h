#pragma once

#include <QImage>
#include <QObject>
#include <QRunnable>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>

/*
 * 文档 / 图片 -> Markdown（"识别成笔记"那条路）。
 *
 * 和 PinOcr 那三条路的关系：PinOcr 解决的是"图上选字"（要行级坐标，在一张
 * 已经很清楚的截图里框出字）。这里解决的是**整篇文件的结构**：PDF / 图片 /
 * Office 文档读进来，连版面、表格、公式、阅读顺序一起变成一份能存进笔记的
 * Markdown。两件事要的东西不一样，所以是两个模块。
 *
 * 架构还是 PinOcr 那套（"跑本机程序"，不把模型做进进程里），理由也一样：
 * 版面分析 / 表格结构 / 公式识别 / OCR 前后处理加一起是几千行，而 RapidDoc /
 * PaddleOCR / Docling 那边早把整条流水线做完了，装完就是一条 Python 命令。
 *
 * 约定（脚本那边按同一份写，见 resources/scripts/doc_runner_common.py）：
 *
 *     <整条命令…>  <输入文件路径>  <结果.json 路径>
 *
 * 两个路径接在**最后**（和 PinOcr 一样，被 `python 脚本.py` 的参数顺序逼的）。
 * 程序把结果写进最后那个文件：
 *
 *     {"markdown": "# 标题\n\n正文…", "images": {"fig1.png": "<base64>"}, ...}
 *
 * 为什么结果是 JSON 而不是直接一篇 Markdown：文档里的插图得落地成文件，才能
 * 在笔记里显示（笔记的规矩是图片放 assets/、正文写相对路径，见 ClipboardStore）。
 * Markdown 里那些 `images/xxx.png` 引用由 DocConvert 换成 `assets/xxx.png` 并
 * 真把文件写出去 —— 这一步只有 C++ 这边知道笔记存在哪儿。
 *
 * 引擎是**可插拔**的：同一个槽位，换一条命令行就是换一个引擎。默认那条是
 * RapidDoc（ONNX，随包模型，CPU 就够）；想上 PaddleOCR-VL / Granite-Docling
 * 这种重型的，用户在设置里换成对应的那条命令即可，本模块不用改。
 */
namespace DocConvert {

/*
 * 随包的默认命令（python + doc_runner_rapid.py [档位]）。
 *
 * pythonExe 非空就用它（就是用户在设置里指的那个解释器）；空串则自动找
 * PATH 里的 python / py，都找不到就退成字面量 "python"，让 runnerProblem()
 * 把"PATH 里找不到"摊到界面上 —— 和 PinOcr::defaultRunnerCommand() 一个做法。
 *
 * tier 是档位词（fast / best），空串 = 脚本自己的默认（balanced）。
 * 为什么档位要做成"命令的一部分"而不是单独的设置项：命令本身就是唯一的契约，
 * 用户想调什么都在那一条里调（跟 PinOcr 把 tiny/small/medium 接在命令后面一致）。
 */
QString defaultRunnerCommand(const QString &tier = QString(),
                             const QString &pythonExe = QString());

/* 随包脚本落在磁盘上的路径（不存在就从 qrc 写一份出去） */
QString runnerScriptPath(const QString &name);

/* 三个随包脚本的名字（qrc 和落盘用的是同一份） */
QStringList shippedScripts();
/* 判断一条命令用的是哪个随包脚本（给"装没装依赖"用）；不认识返回空串 */
QString scriptForCommand(const QString &command);

/* 这条命令能不能跑：空串 = 能；否则一句人话（没填 / 找不到程序） */
QString runnerProblem(const QString &command);

/*
 * 这次要处理的东西支持不支持：空串 = 支持；否则一句人话。
 *
 * 支持的（按扩展名判）：pdf / png / jpg / jpeg / bmp / webp / tif / tiff、
 * docx / xlsx / pptx（以及老的 doc / xls / ppt，交给脚本那边转）。
 *
 * 文件不存在 / 不是文件也会报（先说这两件，比"格式不认"更贴近用户的问题）。
 */
QString unsupportedReason(const QString &path);

/*
 * **只看扩展名**支不支持，不问这个文件在不在。
 *
 * 单独留一个是给"拖进来之前先问一句"用的：界面上可能拿到的只是一个名字
 * （还没落盘），这时候要判的是格式，不是存在性。unsupportedReason 是它加上
 * 存在性检查。
 */
QString unsupportedSuffixReason(const QString &path);

/* 从路径取一个能当笔记标题的名字（去扩展名） */
QString titleFor(const QString &path);

/* 按扩展名猜"大概是哪一类"，界面上给个提示用（"PDF 文档" / "图片" / "Office 文档"） */
QString kindFor(const QString &path);

/* 支持的文件对话框过滤器（主界面"打开"用） */
QString fileFilter();

/*
 * 一次转换的结果。markdown 是**已经处理好图片引用**的正文；images 是"要落到
 * 笔记目录 assets/ 的文件"（文件名 -> 数据），键就是 markdown 里引用的名字。
 */
struct Result {
    QString markdown;
    QMap<QString, QByteArray> images;
    QString title;
    /* 脚本报上来的引擎名 / 页数（没有就空 / 0）—— 只用来在界面上说一句"谁干的" */
    QString engine;
    int pages = 0;
    QString error;  /* 非空 = 这次失败了，markdown 不作数 */

    bool ok() const { return error.isEmpty() && !markdown.isEmpty(); }
};

/*
 * "这一次是被取消的，不是失败" —— 那句话说在哪儿、怎么认，只放这一处。
 *
 * 原来两边各写一遍字面量，而认的那一边写成了
 * `result.error != QLatin1String("已取消")`：中文走 Latin-1 是把 9 个 UTF-8
 * 字节当成 9 个字符，和 3 个汉字的 QString 永远不相等 —— 于是取消照样被记成
 * 一条错误，界面上多出一句"xxx.pdf：已取消"。词只留一份就再对不上一次的机会
 * 也没了，而且这条能写成自检（见 SelfTest::runDoc）。
 */
inline QString cancelledError() { return QStringLiteral("已取消"); }
inline bool isCancelledError(const QString &error) { return error == cancelledError(); }

/*
 * 转换一个文件。**会阻塞**（一页几秒到几十秒，首次跑还要下模型），必须在工作
 * 线程里调 —— 见 DocConvertTask。
 *
 * command 就是上面那条命令行；timeoutMs 是给脚本的上限（大文件 / 首次下模型
 * 要放宽）。error 那句人话写进 Result::error，不单独给。
 */
Result convert(const QString &path, const QString &command, int timeoutMs = 1800000);

/*
 * 解析脚本写出来的 JSON（宽容一点，和 PinOcr::parseLinesJson 一个路子）：
 *   * 外面裹了 ```json 或者前后夹了废话 -> 抠出最外层那对花括号；
 *   * images 里 base64 带不带头（data:image/png;base64,… 或裸串）都认；
 *   * markdown 里的图片引用会按 images 的键换成 assets/ 前缀。
 *
 * imagePrefix 是新引用用的前缀（笔记那边是 "assets"）；空串则不动引用。
 * 单独一个函数是为了能直接喂一份写死的结果做自检，不用真跑脚本。
 */
Result parseResultJson(const QString &json, const QString &imagePrefix = QStringLiteral("assets"),
                       const QSize &sourceSize = QSize());

/* ---------------------------------------------------------------------------
 * 异步那一层：把 convert 扔到线程池，界面不卡
 * ------------------------------------------------------------------------- */

/*
 * 一次转换任务：**直接扔给 QThreadPool**（它是个 QRunnable）。
 *
 * 和 PinWindow 里认字那个 OcrTask 同一个做法（那里面写清楚了为什么不能在这边
 * 直接跑），只是多一件事：**中途能取消** —— 一份几百页的 PDF 可能跑几分钟，
 * 用户改主意了得能停下（脚本是子进程，停它要 kill）。
 *
 * 用法：
 *
 *     auto *task = new DocConvert::Task(path, command, timeoutMs);
 *     QObject::connect(task, &Task::finished, receiver, handler);
 *     QThreadPool::globalInstance()->start(task);
 *
 * 生命周期：run() 跑完由线程池 delete（setAutoDelete(true)）。**别自己 delete**
 * —— deleteLater 和线程池的自删会撞上。
 *
 * 注意 finished 是从**池线程**里发出来的：连接的接收方在主线程时是自动队列化的
 * （Qt 的跨线程信号语义），接收方在别的线程就得自己保证线程安全。
 */
class Task final : public QObject, public QRunnable {
    Q_OBJECT

public:
    explicit Task(QString path, QString command, int timeoutMs, QObject *parent = nullptr);

    /* QRunnable：跑一次转换，发 finished（线程池调，不要自己调） */
    void run() override;

    /* 取消：置标志；run() 里那个等待循环最多 100ms 就把子进程 kill 掉 */
    void cancel();
    bool wasCanceled() const { return m_canceled.load(); }

signals:
    /* 认完了（成功或失败都在这一条里，看 Result::ok()） */
    void finished(const DocConvert::Result &result);

private:
    QString m_path;
    QString m_command;
    int m_timeoutMs;

    /*
     * 取消标志。**跨线程读**（界面线程置，池线程读），所以是 atomic ——
     * 普通 bool 在这里是数据竞争：编译器完全可以把循环里那次读提到循环外，
     * 那样取消就永远不生效了。
     */
    std::atomic<bool> m_canceled{false};
};

}  // namespace DocConvert

Q_DECLARE_METATYPE(DocConvert::Result)
