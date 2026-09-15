#include "DocConvert.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>

/*
 * 实现说明都写在头文件里了，这里只记几件"代码本身看不出来的事"。
 *
 * 1) 为什么图片引用要在 C++ 这边改写
 *    脚本不知道笔记存在哪，它只知道"我生成了 fig1.png"。落盘位置和正文里的
 *    相对路径都是笔记那边的规矩（assets/xxx.png，见 ClipboardStore 的头注释），
 *    所以改写这一下只能在知道笔记目录的这一侧做。
 *
 * 2) 为什么子进程要单独提到 runCommand 里
 *    Task::cancel() 必须能 kill 掉那个 QProcess。QProcess 是栈上的局部对象时，
 *    外面拿不到它 —— 于是把"跑命令"整个包一层，进程对象挂在 Task 上。
 *    （PinOcr::recognizeWithProgram 不需要取消，所以那边是栈上的局部 QProcess。）
 */

namespace {

/*
 * 把最外层那对花括号抠出来：模型爱把 JSON 包在 ```json 里，脚本也可能在多写了
 * 一行日志（比如 RapidDoc 的进度条走到了 stdout）。
 */
QString extractJsonObject(const QString &raw) {
    /* 先用"最后一个 { 开头、最后一个 } 结尾"试：日志在前时这样最稳 */
    const int open = raw.indexOf(QLatin1Char('{'));
    if (open < 0)
        return raw.trimmed();
    const int close = raw.lastIndexOf(QLatin1Char('}'));
    return (close > open) ? raw.mid(open, close - open + 1) : raw.mid(open);
}

/* 一串 base64 可能带 data URI 头 */
QByteArray decodeImage(const QString &value) {
    const int comma = value.indexOf(QLatin1Char(','));
    const QString body = (value.startsWith(QLatin1String("data:")) && comma > 0)
                             ? value.mid(comma + 1)
                             : value;
    return QByteArray::fromBase64(body.toLatin1());
}

/*
 * 把正文里的图片引用归一到 `<prefix>/<文件名>`。
 *
 * 认哪些写法：`](…)` 里以 images/ 开头、或者干脆就是那个文件名、或者带
 * `./` / 反斜杠的变体。只动**在 images 表里出现过**的那些名字，别的一律不碰
 * （正文里用户自己写的别的链接不能被误伤）。
 *
 * matched 顺手收下"这次真改写过的文件名"—— 调用方要靠它分辨"哪些引用还是野的"。
 */
QString rewriteImageRefs(const QString &markdown, const QStringList &names,
                         const QString &prefix) {
    if (names.isEmpty())
        return markdown;

    QString out = markdown;
    for (const QString &name : names) {
        /* 名字里可能有正则元字符，一律转义 */
        const QString escaped = QRegularExpression::escape(name);
        /*
         * 引用结尾：`)` 或者空白（有的脚本会写 ![x](a.png "标题")）。
         *
         * 这个结尾断言**不能**写成 `\s*(?=[)\s])` —— 那样 `a.png "c"` 里的空格
         * 会被 \s* 回溯着吃掉，结果变成 `assets/a.png"c"`（实测过）。写成
         * "非空白或行尾"就没有回溯的余地。
         */
        const QRegularExpression re(
            QStringLiteral(R"(\]\(\s*(?:[^)\s]*[\\/])?)") + escaped
            + QStringLiteral(R"((?=[)\s]|$))"));
        if (!re.match(out).hasMatch())
            continue;
        out.replace(re, QStringLiteral("](%1/%2").arg(prefix, name));
    }
    return out;
}

/*
 * 把**没拿到数据**的那些图片引用改成一句占位。
 *
 * 为什么要有这一步：RapidDoc 在 fast 档（关掉表格识别）下会把一个表格块判成
 * 图片，正文里写 `![](images/xxx.png)`，但那张图并不在它的 images 表里 —— 引用
 * 留着就是个断链，在笔记里表现为一个永远加载不出来的图。实测过（probe_doc.pdf
 * 的 fast 档）。
 *
 * 占位写成 `![图片 说明](引用)` —— 还是合法的 Markdown 图片语法，编辑器不会
 * 因为找不到图而报错，用户也看得出"这儿原来有张图"。
 */
QString placeholderForMissingImages(const QString &markdown,
                                    const QRegularExpression &imageRef) {
    QString out;
    out.reserve(markdown.size());
    int last = 0;
    auto it = imageRef.globalMatch(markdown);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString alt = match.captured(1).trimmed();
        const QString target = match.captured(2).trimmed();
        out += markdown.mid(last, match.capturedStart() - last);
        out += QStringLiteral("![图片 %1](%2)")
                   .arg(alt.isEmpty() ? QFileInfo(target).fileName() : alt, target);
        last = match.capturedEnd();
    }
    out += markdown.mid(last);
    return out;
}

/* 扩展名 -> 类别词（界面提示 / 支持判断共用一份） */
QString suffixOf(const QString &path) {
    return QFileInfo(path).suffix().toLower();
}

const QStringList &imageSuffixes() {
    static const QStringList list = {
        QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
        QStringLiteral("bmp"), QStringLiteral("webp"), QStringLiteral("tif"),
        QStringLiteral("tiff"), QStringLiteral("gif"),
    };
    return list;
}

const QStringList &officeSuffixes() {
    static const QStringList list = {
        QStringLiteral("docx"), QStringLiteral("xlsx"), QStringLiteral("pptx"),
        /* 老的二进制 Office 格式：脚本那边会先转 */
        QStringLiteral("doc"), QStringLiteral("xls"), QStringLiteral("ppt"),
    };
    return list;
}

/*
 * 真正跑一次：起子进程、分片等、读结果。
 *
 * cancel 传 nullptr 就是"不可取消"，也就是 convert() 那条同步路（自检里直接
 * 调它）。传一个 atomic 就是异步那条路：等待循环每 100ms 看一眼它。
 *
 * 返回类型要写全 DocConvert::Result —— 这个匿名命名空间在**全局**作用域里，
 * 不在 DocConvert 里面，光写 Result 编译器认不出来。
 */
DocConvert::Result runOnce(const QString &path, const QString &command, int timeoutMs,
                           std::atomic<bool> *cancel);

}  // namespace
namespace DocConvert {

QStringList shippedScripts() {
    return { QStringLiteral("doc_runner_common.py"),
             QStringLiteral("doc_runner_rapid.py"),
             QStringLiteral("doc_runner_paddleocr_vl.py"),
             QStringLiteral("doc_runner_granite.py") };
}

QString runnerScriptPath(const QString &name) {
    if (name.isEmpty())
        return QString();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return QString();
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + name;
    /*
     * 每次用到都拿 qrc 里那份覆盖一遍。
     *
     * 和 PinOcr 那边"只写一次"的做法不一样，是故意的：这几个脚本里
     * doc_runner_common.py 是**契约**（三个 runner 都 import 它），旧版本和新
     * runner 混着用会直接报错。升过一次级之后，磁盘上那份就成了"上一版的契约"。
     * 覆盖掉最省事，用户真想改就复制出去改（设置里填自己那条命令）。
     */
    QFile from(QStringLiteral(":/scripts/") + name);
    if (from.open(QIODevice::ReadOnly)) {
        const QByteArray bytes = from.readAll();
        QFile to(path);
        if (to.open(QIODevice::WriteOnly | QIODevice::Truncate))
            to.write(bytes);
    }
    return path;
}

QString defaultRunnerCommand(const QString &tier, const QString &pythonExe) {
    const QString script = runnerScriptPath(QStringLiteral("doc_runner_rapid.py"));
    auto quote = [](const QString &value) {
        return QLatin1Char('"') + value + QLatin1Char('"');
    };
    const QString tierArg = tier.trimmed().isEmpty() ? QString() : QLatin1Char(' ') + tier.trimmed();

    /* 用户在设置里指了哪个解释器就用哪个（venv 里的 python 要这样指） */
    const QString explicitExe = pythonExe.trimmed();
    if (!explicitExe.isEmpty())
        return quote(explicitExe) + QLatin1Char(' ') + quote(script) + tierArg;

    QString exe = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (!exe.isEmpty())
        return quote(exe) + QLatin1Char(' ') + quote(script) + tierArg;
    exe = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!exe.isEmpty())
        return quote(exe) + QStringLiteral(" -3 ") + quote(script) + tierArg;
    return QStringLiteral("python ") + quote(script) + tierArg;
}

QString scriptForCommand(const QString &command) {
    for (const QString &name : shippedScripts()) {
        if (command.contains(name, Qt::CaseInsensitive))
            return name;
    }
    return QString();
}

QString runnerProblem(const QString &command) {
    const QString cmd = command.trimmed();
    if (cmd.isEmpty())
        return QStringLiteral("还没填识别程序（设置 → 识别 → 文档识别）");

    /* 第一段是程序名，可能带引号（和 PinOcr::runnerProblem 同一套解析） */
    QString program = cmd;
    if (program.startsWith(QLatin1Char('"'))) {
        const int end = program.indexOf(QLatin1Char('"'), 1);
        program = (end > 1) ? program.mid(1, end - 1) : program.mid(1);
    } else {
        const int space = program.indexOf(QLatin1Char(' '));
        if (space > 0)
            program = program.left(space);
    }
    if (program.isEmpty())
        return QStringLiteral("这条命令看不懂：") + cmd;
    if (QFileInfo(program).isAbsolute()) {
        if (!QFileInfo::exists(program))
            return QStringLiteral("找不到程序：") + program;
    } else if (QStandardPaths::findExecutable(program).isEmpty()) {
        return QStringLiteral("PATH 里找不到：") + program
               + QStringLiteral("（装了 Python 的话，把它填成完整路径）");
    }
    return QString();
}

QString unsupportedSuffixReason(const QString &path) {
    if (path.trimmed().isEmpty())
        return QStringLiteral("没给文件");
    const QString suffix = suffixOf(path);
    if (suffix == QLatin1String("pdf") || imageSuffixes().contains(suffix)
        || officeSuffixes().contains(suffix))
        return QString();
    return QStringLiteral("这种格式还不认（.%1）：PDF、图片、Word / Excel / PPT 都行")
        .arg(suffix.isEmpty() ? QStringLiteral("?") : suffix);
}

QString unsupportedReason(const QString &path) {
    if (path.trimmed().isEmpty())
        return QStringLiteral("没给文件");
    const QFileInfo info(path);
    if (!info.exists())
        return QStringLiteral("找不到这个文件：") + path;
    if (!info.isFile())
        return QStringLiteral("这不是一个文件：") + path;
    return unsupportedSuffixReason(path);
}

QString titleFor(const QString &path) {
    const QString base = QFileInfo(path).completeBaseName().trimmed();
    return base.isEmpty() ? QStringLiteral("识别结果") : base;
}

QString kindFor(const QString &path) {
    const QString suffix = suffixOf(path);
    if (suffix == QLatin1String("pdf"))
        return QStringLiteral("PDF 文档");
    if (imageSuffixes().contains(suffix))
        return QStringLiteral("图片");
    if (!officeSuffixes().contains(suffix))
        return QStringLiteral("文件");
    if (suffix == QLatin1String("docx") || suffix == QLatin1String("doc"))
        return QStringLiteral("Word 文档");
    if (suffix == QLatin1String("xlsx") || suffix == QLatin1String("xls"))
        return QStringLiteral("Excel 表格");
    return QStringLiteral("PPT 演示");
}

QString fileFilter() {
    return QStringLiteral("能识别的文件 (*.pdf *.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff "
                          "*.docx *.xlsx *.pptx *.doc *.xls *.ppt);;"
                          "PDF (*.pdf);;图片 (*.png *.jpg *.jpeg *.bmp *.webp *.tif *.tiff);;"
                          "Office (*.docx *.xlsx *.pptx *.doc *.xls *.ppt);;所有文件 (*.*)");
}

Result parseResultJson(const QString &json, const QString &imagePrefix, const QSize &sourceSize) {
    Result result;
    Q_UNUSED(sourceSize);

    const QString text = extractJsonObject(json);
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("识别程序没给出能读的结果：")
                       + (parseError.error != QJsonParseError::NoError
                              ? parseError.errorString()
                              : QStringLiteral("不是一个 JSON 对象"));
        return result;
    }

    const QJsonObject obj = doc.object();

    /* 脚本自己报的错优先（它是"为什么没认出来"最准的一句话） */
    const QString scriptError = obj.value(QStringLiteral("error")).toString();
    if (!scriptError.isEmpty()) {
        result.error = scriptError;
        return result;
    }

    result.markdown = obj.value(QStringLiteral("markdown")).toString();
    result.title = obj.value(QStringLiteral("title")).toString();
    result.engine = obj.value(QStringLiteral("engine")).toString();
    result.pages = obj.value(QStringLiteral("pages")).toInt();

    /*
     * 图片：{"名字": "base64"}。也认 [{"name":…, "data":…}] 这种写法（脚本作者
     * 顺手写成数组的情况），少一次来回扯。
     */
    QStringList names;
    const QJsonValue images = obj.value(QStringLiteral("images"));
    if (images.isObject()) {
        const QJsonObject map = images.toObject();
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            if (!it.value().isString())
                continue;
            const QByteArray bytes = decodeImage(it.value().toString());
            if (bytes.isEmpty())
                continue;
            result.images.insert(it.key(), bytes);
            names << it.key();
        }
    } else if (images.isArray()) {
        for (const QJsonValue &entry : images.toArray()) {
            if (!entry.isObject())
                continue;
            const QJsonObject item = entry.toObject();
            const QString name = item.value(QStringLiteral("name")).toString();
            const QByteArray bytes = decodeImage(item.value(QStringLiteral("data")).toString());
            if (name.isEmpty() || bytes.isEmpty())
                continue;
            result.images.insert(name, bytes);
            names << name;
        }
    }

    if (!imagePrefix.isEmpty()) {
        if (!names.isEmpty())
            result.markdown = rewriteImageRefs(result.markdown, names, imagePrefix);

        /*
         * 剩下还指着 images/ 的引用 = 脚本没给数据的那几张（见
         * placeholderForMissingImages 的说明）。已经改成 assets/ 的那批不再匹配，
         * 所以这一步不会误伤。
         */
        static const QRegularExpression kLooseImageRef(
            QStringLiteral(R"(!\[([^\]\n]*)\]\(\s*(?:\.?[\\/])?images[\\/]([^)\n]+?)\s*\))"));

        /* 顺带把扫描到的图片名补进 images 以外的信息里也没意义 —— 只改写正文 */
        result.markdown = placeholderForMissingImages(result.markdown, kLooseImageRef);
    }

    if (result.markdown.trimmed().isEmpty())
        result.error = QStringLiteral("识别程序跑完了，但没读出任何内容（空白页？加密？）");
    return result;
}

/*
 * 子进程那一层。
 *
 * 做成"一个可选的取消对象"而不是两份代码：命令怎么拼、超时怎么算、结果怎么读
 * 只有这一份实现。cancel 传 nullptr 就是同步那条路（convert，自检里直接调）；
 * 传了就是个带 atomic 标志的对象，下面那个等待循环每 100ms 看一眼。
 *
 * 为什么用 DocCancel 这个小结构而不是 Task 自己：convert() 不该依赖 QObject
 * 的 Task（同步调用方没有 Task）。一个原子标志足够了。
 */
DocConvert::Result runOnce(const QString &path, const QString &command, int timeoutMs,
                           std::atomic<bool> *cancel) {
    Result result;
    const QString problem = unsupportedReason(path);
    if (!problem.isEmpty()) {
        result.error = problem;
        return result;
    }
    const QString runnerIssue = runnerProblem(command);
    if (!runnerIssue.isEmpty()) {
        result.error = runnerIssue;
        return result;
    }

    /* 结果写在临时目录里，跑完自动清掉 */
    QTemporaryDir dir;
    if (!dir.isValid()) {
        result.error = QStringLiteral("临时目录建不出来");
        return result;
    }
    const QString outPath = dir.filePath(QStringLiteral("doc.json"));

    /*
     * 参数顺序和 PinOcr 那条一样：两个路径接在整条命令的**最后**。
     * 前面是命令自己的参数（档位词之类），随包脚本读的就是最后两个。
     *
     * 路径按本机习惯给（反斜杠）：QTemporaryDir 给的是正斜杠，Python 无所谓，
     * 但用户拿批处理当 runner 时 cmd 认不出 C:/… 这种写法。
     */
    const QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) {
        result.error = QStringLiteral("这条命令看不懂：") + command;
        return result;
    }
    QStringList args = parts.mid(1);
    args << QDir::toNativeSeparators(path) << QDir::toNativeSeparators(outPath);

    QProcess process;
    process.setProgram(parts.first());
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);

    /*
     * 工作目录设在输入文件那儿。
     *
     * 为什么不随便找个目录：RapidDoc / Docling 这类流水线会在**当前目录**下建
     * 输出目录（output/），工作目录是临时目录的话那些文件跑完就没了（还好），
     * 是程序目录的话就把安装目录写脏了（不好）。放在输入文件旁边最不意外 ——
     * 而且相对路径的输入（用户从命令行传进来的）也只有这样才找得到。
     */
    process.setWorkingDirectory(QFileInfo(path).absolutePath());

    process.start();
    if (!process.waitForStarted(10000)) {
        result.error = QStringLiteral("识别程序起不来：") + process.errorString();
        return result;
    }

    /*
     * 分片等：每 100ms 回头看一眼"取消了没有 / 超时了没有"。
     *
     * 直接用 waitForFinished(整个超时) 的话，用户点"取消"要等这次转换自己跑完
     * 才生效 —— 一份大 PDF 就是几分钟，等于没有取消。
     */
    QElapsedTimer clock;
    clock.start();
    while (process.state() != QProcess::NotRunning) {
        if (cancel && cancel->load()) {
            process.kill();
            process.waitForFinished(3000);
            result.error = QStringLiteral("已取消");
            return result;
        }
        if (clock.elapsed() > timeoutMs) {
            process.kill();
            process.waitForFinished(3000);
            result.error = QStringLiteral("识别程序 %1 秒没跑完（大文件 / 首次跑要下模型，"
                                          "可以在设置里换一条更快的命令）")
                               .arg(timeoutMs / 1000);
            return result;
        }
        process.waitForFinished(100);
    }

    QFile file(outPath);
    if (!file.open(QIODevice::ReadOnly)) {
        /* 没写出结果：把脚本的输出带上（最后一段，报错一般在末尾） */
        const QString log = QString::fromUtf8(process.readAll()).trimmed();
        const QString tail = log.right(300);
        result.error = QStringLiteral("识别程序没写出结果（退出码 %1）%2")
                           .arg(process.exitCode())
                           .arg(tail.isEmpty() ? QString() : QStringLiteral("：") + tail);
        return result;
    }
    const QString body = QString::fromUtf8(file.readAll());
    file.close();
    return parseResultJson(body);
}

Result convert(const QString &path, const QString &command, int timeoutMs) {
    return runOnce(path, command, timeoutMs, nullptr);
}

/* ---------------------------------------------------------------------------
 * Task
 * ------------------------------------------------------------------------- */

Task::Task(QString path, QString command, int timeoutMs, QObject *parent)
    : QObject(parent), m_path(std::move(path)), m_command(std::move(command)),
      m_timeoutMs(timeoutMs) {
    /* 跑完由线程池删（见头文件里"生命周期"那段） */
    setAutoDelete(true);
}

void Task::cancel() {
    m_canceled.store(true);
}

void Task::run() {
    /*
     * 起子进程、分片等、读结果 —— 全在 runOnce 里，这里只把取消标志递进去。
     *
     * run() 是**线程池的线程**调的（QThreadPool::start(task)）。等待循环每
     * 100ms 看一眼 m_canceled，所以 cancel() 一置上，最多 100ms 子进程就被
     * kill 掉，马上带着"已取消"回来。
     */
    const Result result = runOnce(m_path, m_command, m_timeoutMs, &m_canceled);
    emit finished(result);
}

}  // namespace DocConvert
