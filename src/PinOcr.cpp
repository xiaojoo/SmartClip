#include "PinOcr.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVariantMap>
#include <algorithm>
#include <cmath>

/*
 * 唯一碰 WinRT 的地方。
 *
 * WinRT 的头（winrt/*.h）要放在 Qt 前面：它们会牵进来一大堆 Windows 声明和宏
 * （min/max、interface 之类），让 Qt 先展开反而容易撞上。这个 .cpp 之外没有第二
 * 个文件 include 它们，所以那堆宏也就在这里止步。
 *
 * 链接上要 windowsapp.lib（WinRT 的激活走 RoGetActivationFactory）—— 见
 * CMakeLists.txt 里那一段（找不到 cppwinrt 头就关掉这个功能，见文件末尾的兜底）。
 */
#ifdef SMARTCLIP_HAS_WINOCR

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winrt/Windows.Foundation.Collections.h>
#  include <winrt/Windows.Foundation.h>
#  include <winrt/Windows.Globalization.h>
#  include <winrt/Windows.Graphics.Imaging.h>
#  include <winrt/Windows.Media.Ocr.h>
#  include <winrt/Windows.Storage.Streams.h>
#  include <winrt/base.h>

#  include <QImage>
#  include <cstring>

namespace {

using winrt::Windows::Globalization::Language;
using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Media::Ocr::OcrLine;
using winrt::Windows::Media::Ocr::OcrResult;
using winrt::Windows::Media::Ocr::OcrWord;
using winrt::Windows::Storage::Streams::Buffer;

/*
 * WinRT 调用前得先有 COM 单元。
 *
 * 工作线程是 QThreadPool 复用的，所以进出要成对；但**已经初始化过的线程不要动**
 * —— GUI 主线程在 Qt 里通常是 STA（拖放 / 剪贴板要它），这时候
 * init_apartment(MTA) 会抛 RPC_E_CHANGED_MODE，而那个单元本来就能跑 WinRT，
 * 照用就行（所以只在"这次是我初始化的"时候才 uninit）。
 */
struct Apartment {
    bool mine = false;

    Apartment() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            mine = true;
        } catch (...) {
            /* 这个线程已经有单元了：不接管、也不拆 */
        }
    }
    ~Apartment() {
        if (!mine)
            return;
        try {
            winrt::uninit_apartment();
        } catch (...) {
        }
    }

    Apartment(const Apartment &) = delete;
    Apartment &operator=(const Apartment &) = delete;
};

/* 先中文，再系统语言，再英文 —— 三个都建不出来就是没装语言包 */
OcrEngine makeEngine() {
    try {
        if (auto engine = OcrEngine::TryCreateFromLanguage(Language(L"zh-Hans-CN")))
            return engine;
    } catch (...) {
    }
    try {
        if (auto engine = OcrEngine::TryCreateFromUserProfileLanguages())
            return engine;
    } catch (...) {
    }
    try {
        if (auto engine = OcrEngine::TryCreateFromLanguage(Language(L"en-US")))
            return engine;
    } catch (...) {
    }
    return nullptr;
}

}  // namespace

bool PinOcr::available() {
    Apartment apartment;
    /* 临时那个 engine 在本行结束就析构，早于 apartment —— 顺序别调 */
    return makeEngine() != nullptr;
}

QString PinOcr::language() {
    Apartment apartment;
    const OcrEngine engine = makeEngine();
    if (!engine)
        return QString();
    const winrt::hstring tag = engine.RecognizerLanguage().LanguageTag();
    /* engine 后声明先析构，还是早于 apartment */
    return QString::fromWCharArray(tag.c_str(), int(tag.size()));
}

QVariantList PinOcr::recognize(const QImage &image, QString *error) {
    QVariantList out;
    if (error)
        error->clear();
    if (image.isNull()) {
        if (error)
            *error = QStringLiteral("没有图像");
        return out;
    }

    Apartment apartment;
    const OcrEngine engine = makeEngine();
    if (!engine) {
        if (error)
            *error = QStringLiteral("建不出 OCR 引擎（没装语言包？）");
        return out;
    }

    try {
        /*
         * 引擎对图片边长有上限（OcrEngine::MaxImageDimension，一般 2600）：超了就
         * 等比缩下来。坐标是归一化的，缩了也不用往回换。
         */
        QImage src = image;
        const int maxDim = int(OcrEngine::MaxImageDimension());
        if (maxDim > 0 && (src.width() > maxDim || src.height() > maxDim))
            src = src.scaled(maxDim, maxDim, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (src.width() < 1 || src.height() < 1)
            return out;

        /* 只吃 BGRA8：Qt 的 ARGB32 在内存里正好是 B G R A（小端），直接搬 */
        if (src.format() != QImage::Format_ARGB32
            && src.format() != QImage::Format_ARGB32_Premultiplied)
            src = src.convertToFormat(QImage::Format_ARGB32);

        try {
            SoftwareBitmap bitmap(BitmapPixelFormat::Bgra8, src.width(), src.height(),
                                  BitmapAlphaMode::Premultiplied);
            /*
             * 填像素：Buffer(capacity) 只给了**容量**，Length 还是 0 ——
             * 不把 Length 设上，CopyFromBuffer 拷进去的是 0 字节（图是空的/花的，
             * 引擎那边就报一句看不懂的错）。这两个都得设。
             */
            Buffer buffer(uint32_t(src.sizeInBytes()));
            buffer.Length(uint32_t(src.sizeInBytes()));
            std::memcpy(buffer.data(), src.constBits(), size_t(src.sizeInBytes()));
            bitmap.CopyFromBuffer(buffer);

            const OcrResult result = engine.RecognizeAsync(bitmap).get();
            const double iw = double(src.width());
            const double ih = double(src.height());

            for (const OcrLine &line : result.Lines()) {
                const winrt::hstring text = line.Text();
                if (text.empty())
                    continue;

                /* 行框 = 这一行所有词框的并集（OcrLine 自己不给外框） */
                double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
                for (const OcrWord &word : line.Words()) {
                    const winrt::Windows::Foundation::Rect r = word.BoundingRect();
                    x0 = std::min(x0, double(r.X));
                    y0 = std::min(y0, double(r.Y));
                    x1 = std::max(x1, double(r.X + r.Width));
                    y1 = std::max(y1, double(r.Y + r.Height));
                }
                if (x1 <= x0 || y1 <= y0)
                    continue;

                auto unit = [](double v) { return std::max(0.0, std::min(1.0, v)); };
                out.append(QVariantMap{
                    { QStringLiteral("text"),
                      QString::fromWCharArray(text.c_str(), int(text.size())) },
                    { QStringLiteral("x"), unit(x0 / iw) },
                    { QStringLiteral("y"), unit(y0 / ih) },
                    { QStringLiteral("w"), unit((x1 - x0) / iw) },
                    { QStringLiteral("h"), unit((y1 - y0) / ih) },
                });
            }
        } catch (const winrt::hresult_error &e) {
            /* 出岔子：把已经认出来的给出去，顺带把原因带上（哪一步失败看得见） */
            if (error)
                *error = QStringLiteral("OCR 调用失败：")
                         + QString::fromWCharArray(e.message().c_str(), int(e.message().size()));
        } catch (...) {
            if (error)
                *error = QStringLiteral("OCR 调用失败（未知错误）");
        }
    } catch (...) {
        if (error)
            *error = QStringLiteral("OCR 调用失败（准备图像那一步）");
    }
    return out;
}

#else  /* !SMARTCLIP_HAS_WINOCR */

/*
 * 兜底：这台机器上没有 cppwinrt 头（或者不是 Windows 目标）。
 * 界面那边看 ocrAvailable=false，就把"Windows 自带"这个选项收起来 ——
 * 另一个引擎（PP-OCR 本机程序）不受影响。
 */
bool PinOcr::available() { return false; }
QString PinOcr::language() { return QString(); }
QVariantList PinOcr::recognize(const QImage &, QString *error) {
    if (error)
        *error = QStringLiteral("这份构建没有带上 Windows OCR（SDK 里没找到 cppwinrt）");
    return QVariantList();
}

#endif  /* SMARTCLIP_HAS_WINOCR */

/* ===========================================================================
 * 另外两条引擎用的那一套（不依赖 WinRT，所以放在 #endif 外头）
 *
 *   * PP-OCR：跑本机程序（见头文件里那段约定）
 *   * JSON 解析：PP-OCR 那个程序写出来的文件就靠它（宽容一点：外面裹了 ```json、
 *     套了一层 {"lines": …}、坐标写成像素或者归一化，都认）
 * ======================================================================== */

namespace {

/* 把最外层那对括号抠出来：模型爱把 JSON 包在 ```json 里、或者前后加一句废话 */
QString extractJson(const QString &raw) {
    const int square = raw.indexOf(QLatin1Char('['));
    const int brace = raw.indexOf(QLatin1Char('{'));
    int begin = -1;
    QChar closer;
    if (square >= 0 && (brace < 0 || square < brace)) {
        begin = square;
        closer = QLatin1Char(']');
    } else if (brace >= 0) {
        begin = brace;
        closer = QLatin1Char('}');
    }
    if (begin < 0)
        return raw.trimmed();
    const int end = raw.lastIndexOf(closer);
    return (end > begin) ? raw.mid(begin, end - begin + 1) : raw.mid(begin);
}

/* 从一条记录里拿 box：box 数组 / 散着的 x,y,w,h 都认 */
bool boxOf(const QVariantMap &item, double *x, double *y, double *w, double *h) {
    const QVariant box = item.value(QStringLiteral("box"));
    if (box.canConvert<QVariantList>()) {
        const QVariantList list = box.toList();
        if (list.size() >= 4) {
            *x = list.at(0).toDouble();
            *y = list.at(1).toDouble();
            *w = list.at(2).toDouble();
            *h = list.at(3).toDouble();
            return true;
        }
    }
    bool ok = false;
    const double vx = item.value(QStringLiteral("x")).toDouble(&ok);
    if (!ok)
        return false;
    *x = vx;
    *y = item.value(QStringLiteral("y")).toDouble();
    *w = item.value(QStringLiteral("w"), item.value(QStringLiteral("width")).toDouble()).toDouble();
    *h = item.value(QStringLiteral("h"), item.value(QStringLiteral("height")).toDouble()).toDouble();
    return true;
}

}  // namespace

QVariantList PinOcr::parseLinesJson(const QString &json, const QSize &imageSize, QString *error) {
    QVariantList out;
    if (error)
        error->clear();

    const QString text = extractJson(json);
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("结果不是能读的 JSON：") + parseError.errorString();
        return out;
    }

    QJsonArray array;
    if (doc.isArray())
        array = doc.array();
    else if (doc.isObject()) {
        const QJsonObject obj = doc.object();
        if (obj.value(QStringLiteral("lines")).isArray())
            array = obj.value(QStringLiteral("lines")).toArray();
        else if (obj.value(QStringLiteral("data")).isArray())
            array = obj.value(QStringLiteral("data")).toArray();
    }

    const double iw = double(imageSize.width());
    const double ih = double(imageSize.height());
    for (const QJsonValue &entry : array) {
        if (!entry.isObject())
            continue;
        const QVariantMap item = entry.toObject().toVariantMap();
        QString lineText = item.value(QStringLiteral("text")).toString();
        if (lineText.isEmpty())
            lineText = item.value(QStringLiteral("txt")).toString();
        if (lineText.isEmpty())
            continue;

        double x = 0, y = 0, w = 0, h = 0;
        if (!boxOf(item, &x, &y, &w, &h))
            continue;
        if (w <= 0 || h <= 0)
            continue;

        /* 四个数都很小 = 已经归一化了；否则当像素，除以图片尺寸 */
        const bool normalized = std::max(std::max(std::abs(x), std::abs(y)),
                                         std::max(std::abs(w), std::abs(h))) <= 1.5;
        if (!normalized) {
            if (iw <= 0 || ih <= 0)
                continue;
            x /= iw;
            y /= ih;
            w /= iw;
            h /= ih;
        }
        auto unit = [](double v) { return std::max(0.0, std::min(1.0, v)); };
        out.append(QVariantMap{
            { QStringLiteral("text"), lineText },
            { QStringLiteral("x"), unit(x) },
            { QStringLiteral("y"), unit(y) },
            { QStringLiteral("w"), unit(w) },
            { QStringLiteral("h"), unit(h) },
        });
    }
    return out;
}

QString PinOcr::runnerScriptPath() {
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dir.isEmpty())
        return QString();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/ppocr_runner.py");
    if (!QFileInfo::exists(path)) {
        QFile from(QStringLiteral(":/scripts/ppocr_runner.py"));
        if (from.open(QIODevice::ReadOnly)) {
            QFile to(path);
            if (to.open(QIODevice::WriteOnly | QIODevice::Truncate))
                to.write(from.readAll());
        }
    }
    return path;
}

QString PinOcr::defaultRunnerCommand() {
    const QString script = runnerScriptPath();
    auto quote = [](const QString &value) {
        return QLatin1Char('"') + value + QLatin1Char('"');
    };
    QString exe = QStandardPaths::findExecutable(QStringLiteral("python"));
    if (!exe.isEmpty())
        return quote(exe) + QLatin1Char(' ') + quote(script);
    /* Windows 上常见的另一个入口是 py 启动器 */
    exe = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!exe.isEmpty())
        return quote(exe) + QStringLiteral(" -3 ") + quote(script);
    /* 找不到也返回一条：runnerProblem() 会把"PATH 里找不到"摆到界面上 */
    return QStringLiteral("python ") + quote(script);
}

QString PinOcr::runnerProblem(const QString &command) {
    const QString cmd = command.trimmed();
    if (cmd.isEmpty())
        return QStringLiteral("还没填 PP-OCR 程序（设置 → 翻译 → 图上选字）");

    /* 第一段是程序名，可能带引号 */
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

QVariantList PinOcr::recognizeWithProgram(const QImage &image, const QString &command,
                                          QString *error) {
    QVariantList out;
    if (error)
        error->clear();
    if (image.isNull()) {
        if (error)
            *error = QStringLiteral("没有图像");
        return out;
    }
    const QString problem = runnerProblem(command);
    if (!problem.isEmpty()) {
        if (error)
            *error = problem;
        return out;
    }

    /* 图和结果都放临时目录里，跑完自动清掉 */
    QTemporaryDir dir;
    if (!dir.isValid()) {
        if (error)
            *error = QStringLiteral("临时目录建不出来");
        return out;
    }
    const QString imagePath = dir.filePath(QStringLiteral("pin.png"));
    const QString outPath = dir.filePath(QStringLiteral("lines.json"));
    if (!image.save(imagePath, "PNG")) {
        if (error)
            *error = QStringLiteral("底图存不成 PNG");
        return out;
    }

    /* 命令按命令行习惯切（引号里的空格不切）：QProcess::splitCommand 就是干这个的 */
    const QStringList parts = QProcess::splitCommand(command);
    if (parts.isEmpty()) {
        if (error)
            *error = QStringLiteral("这条命令看不懂：") + command;
        return out;
    }
    /*
     * 参数顺序：两个路径接在**整条命令的最后**。
     *
     *   <整条命令…> <图片.png> <结果.json>
     *
     * 为什么不能"紧跟程序名"：那条命令往往长这样
     *     python "...\ppocr_runner.py" medium
     * python 要求**脚本路径是它的第一个参数**，把图片插到前面就变成"让 python 去
     * 执行 pin.png"（实测报 `SyntaxError: Non-UTF-8 code starting with '\x89'`）。
     * 所以约定反过来：程序读**最后两个**参数当"图片 / 结果"，前面的都是命令自己的
     * 参数（随包脚本就是这么读的，见 resources/scripts/ppocr_runner.py）。
     *
     * 路径按**本机习惯**给（反斜杠）：QTemporaryDir 给的是正斜杠，Python 无所谓，
     * 但用户拿批处理 / 别的命令行工具当 runner 时，cmd 认不出 C:/… 这种写法
     * （报的还是"找不到文件"）。
     */
    QStringList args = parts.mid(1);
    args << QDir::toNativeSeparators(imagePath) << QDir::toNativeSeparators(outPath);

    QProcess process;
    process.setProgram(parts.first());
    process.setArguments(args);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(5000)) {
        if (error)
            *error = QStringLiteral("程序起不来：") + process.errorString();
        return out;
    }
    /* PP-OCR 首次跑要下模型、CPU 上也不快，给足 5 分钟 */
    if (!process.waitForFinished(300000)) {
        process.kill();
        process.waitForFinished(2000);
        if (error)
            *error = QStringLiteral("程序 5 分钟没跑完（首次跑要下模型？）");
        return out;
    }

    QFile file(outPath);
    if (!file.open(QIODevice::ReadOnly)) {
        const QString log = QString::fromUtf8(process.readAll()).trimmed().left(200);
        if (error)
            *error = QStringLiteral("程序没写出结果（退出码 %1）%2")
                         .arg(process.exitCode())
                         .arg(log.isEmpty() ? QString() : QStringLiteral("：") + log);
        return out;
    }
    const QString body = QString::fromUtf8(file.readAll());
    file.close();
    return parseLinesJson(body, image.size(), error);
}
