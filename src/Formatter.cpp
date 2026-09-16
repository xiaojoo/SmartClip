#include "Formatter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QVariantMap>
#include <QXmlStreamReader>

namespace {

/*
 * 外部工具表。
 *
 * command 是**默认**命令（可在设置里按语言覆盖，见 Formatter::toolFor）。
 * 每一段用 QProcess::splitCommand 拆开，所以带参数的写法
 * （"prettier --stdin-filepath x.js"）在这里也成立。
 *
 *   language  编辑器语言 id（必须和 EditorViewItem 的 kLanguages 对上）
 *   label     设置面板里显示的名字
 *   tool      工具名（报错时说"本机没装 XX"用它）
 *   command   默认命令行
 *   inPlace   true = 工具直接改文件（php-cs-fixer / rubocop），false = 结果从 stdout 读
 *   suffix    临时文件的扩展名 —— 不少工具（prettier / black）按扩展名判语言
 *
 * 为什么 javascript 那档首选 prettier 而不是 clang-format：clang-format 也认
 * .js，但那支持是"顺带"的（它对 JS 的默认风格和 prettier 差得远）。装了
 * prettier 就用它，没装的话……见 pickExternal()，clang-format 不会顶上来抢
 * —— 它俩不是同一个工具，替用户换一套风格不是我们该做的决定。
 */
struct ToolEntry {
    const char *language;
    const char *label;
    const char *tool;
    const char *command;
    bool inPlace;
    const char *suffix;
};

const ToolEntry kTools[] = {
    {"cpp",        "C / C++",     "clang-format", "clang-format",                       false, "cpp"},
    {"csharp",     "C#",          "clang-format", "clang-format",                       false, "cs"},
    {"java",       "Java",        "clang-format", "clang-format",                       false, "java"},
    {"javascript", "JavaScript",  "prettier",     "prettier --stdin-filepath x.js",     false, "js"},
    {"json",       "JSON",        "prettier",     "prettier --stdin-filepath x.json",   false, "json"},
    {"html",       "HTML / XML",  "prettier",     "prettier --stdin-filepath x.html",   false, "html"},
    {"css",        "CSS",         "prettier",     "prettier --stdin-filepath x.css",    false, "css"},
    {"markdown",   "Markdown",    "prettier",     "prettier --stdin-filepath x.md",     false, "md"},
    {"yaml",       "YAML",        "prettier",     "prettier --stdin-filepath x.yaml",   false, "yaml"},
    {"python",     "Python",      "black",        "black -",                            false, "py"},
    {"bash",       "Shell 脚本",  "shfmt",        "shfmt",                              false, "sh"},
    {"go",         "Go",          "gofmt",        "gofmt",                              false, "go"},
    {"rust",       "Rust",        "rustfmt",      "rustfmt",                            false, "rs"},
    {"php",        "PHP",         "php-cs-fixer", "php-cs-fixer fix",                   true,  "php"},
    {"ruby",       "Ruby",        "rubocop",      "rubocop -a",                         true,  "rb"},
};

const ToolEntry *entryForLanguage(const QString &language) {
    for (const ToolEntry &t : kTools) {
        if (language == QLatin1String(t.language))
            return &t;
    }
    return nullptr;
}

/*
 * 内置格式化能处理哪些语言。
 *
 * json 只认 json（别去猜"这段 JS 是不是合法 JSON"）；
 * html 认 html / xml 那一族（QXmlStreamReader 是**严格 XML** 解析器，对 HTML5
 * 那些自闭合写法会报错 —— 报错时给的是行号，比默默改坏强）；
 * text 什么语言都能用（去行尾空白 + 收敛空行），是纯文本那个兜底。
 */
QString builtinKindForLanguage(const QString &language) {
    if (language == QLatin1String("json"))
        return QStringLiteral("json");
    if (language == QLatin1String("html"))
        return QStringLiteral("xml");
    return QString();
}

/*
 * "命令行里第一段程序在不在"的缓存有效期（毫秒）。
 *
 * 见 Formatter::programFound 的说明：够短，用户去装完工具再回来就会重查；
 * 够长，设置面板里连点一串输入框触发的那些刷新不会重复扫 PATH。
 */
constexpr qint64 kFoundCacheMs = 2000;

QString programName(const QString &command) {
    return QProcess::splitCommand(command).value(0);
}

/* 正文里出现过的换行统一成 LF（工具之间对 CRLF 的处理各不相同） */
QString toLf(const QString &text) {
    QString out = text;
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    out.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return out;
}

}  // namespace

Formatter::Formatter(QObject *parent) : QObject(parent) {}

/* ------------------------------------------------------------------ */
/* 支持情况                                                            */
/* ------------------------------------------------------------------ */

/*
 * 命令行里第一段程序在不在。
 *
 * 用 QStandardPaths::findExecutable 查，不真去跑它 —— 那两个 inPlace 的工具
 * 跑一次就会**就地改文件**，拿"测试"按钮去试会动用户的正文。
 * 写了路径的（C:/tools/clang-format.exe）就直接看那个文件在不在。
 *
 * ===========================================================================
 * 为什么结果要缓存
 * ===========================================================================
 * findExecutable 是按 PATHEXT 把整条 PATH 扫一遍：本机 PATH 有 99 个目录，
 * 查一个**没装**的程序（设置里标着"（没找到）"的那几行）就要十几毫秒，而
 * toolList() 一次要查 15 个条目 —— 实测一轮 ≈190ms，全压在 GUI 线程上。
 *
 * 这条路以前是"谁问都现算"（设置面板刷新、右键菜单弹出、状态栏那句…），
 * 于是设置里从这一行点到下一行，界面就要冻两下（见 setToolFor 的说明）。
 * 缓存 2 秒不改变"装了工具就能用"：2 秒远短于"去装一个工具再回来"的时间，
 * 但足够把连点触发的那串刷新、以及同一轮里的重复查询（clang-format 要查 3 次、
 * prettier 5 次）全吃掉。
 *
 * 带路径的命令不缓存：那只是一次 QFileInfo::exists，而且用户可能正往那儿拷文件。
 */
bool Formatter::programFound(const QString &command) const {
    const QString program = programName(command);
    if (program.isEmpty())
        return false;
    if (program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\')))
        return QFileInfo::exists(program);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto cached = m_foundCache.constFind(program);
    if (cached != m_foundCache.constEnd() && now - cached.value().second < kFoundCacheMs)
        return cached.value().first;

    const bool found = !QStandardPaths::findExecutable(program).isEmpty();
    m_foundCache.insert(program, qMakePair(found, now));
    return found;
}

QString Formatter::builtinKindFor(const QString &language) const {
    return builtinKindForLanguage(language);
}

QString Formatter::toolFor(const QString &language) const {
    return QSettings().value(QStringLiteral("format/tool/") + language).toString().trimmed();
}

void Formatter::setToolFor(const QString &language, const QString &command) {
    const QString trimmed = command.trimmed();
    const QString key = QStringLiteral("format/tool/") + language;

    /*
     * 值没变：不落盘、不发信号。
     *
     * 输入框的 editingFinished 是"失焦"就发（Qt 文档原话：Return/Enter 按下或
     * 输入框失去焦点 —— 不要求"改过内容"），所以用户在设置里从这一行点到下一行，
     * 每一行都会走到这里。原来无论变没变都落盘 + emit toolsChanged，而
     * toolsChanged 那头连着一次全表刷新（扫一遍 PATH + Repeater 整表重建），
     * "点一下输入框顿一下"的账主要就是这么来的（见 programFound 的实测数字）。
     */
    if (QSettings().value(key).toString().trimmed() == trimmed)
        return;

    if (trimmed.isEmpty())
        QSettings().remove(key);
    else
        QSettings().setValue(key, trimmed);
    emit toolsChanged();
}

QString Formatter::engineLabel(const QString &language, const QString &filePath) const {
    Q_UNUSED(filePath)

    /*
     * 顺序：**外部工具优先，内置兜底**。
     *
     * json 两边都能做：内置那份是"4 空格缩进、按解析器重排"，而用户在本机装了
     * prettier 时多半就是想要 prettier 那套（它读 .prettierrc，团队约定写在
     * 那里）。所以装了外部工具就用它，没装才退回内置 —— 这条顺序必须和
     * format() 里挑工具的顺序**完全一致**，否则就会出现"菜单说能格式化、
     * 按下去用的却是另一个东西"。
     */
    const QString custom = toolFor(language);
    if (!custom.isEmpty())
        return programFound(custom) ? programName(custom) : QString();

    if (const ToolEntry *t = entryForLanguage(language)) {
        if (programFound(QString::fromLatin1(t->command)))
            return QString::fromLatin1(t->tool);
    }
    if (!builtinKindForLanguage(language).isEmpty())
        return QStringLiteral("内置格式化");
    if (language == QLatin1String("plain"))
        return QStringLiteral("去行尾空白");
    return QString();
}

bool Formatter::supported(const QString &language, const QString &filePath) const {
    return !engineLabel(language, filePath).isEmpty();
}

bool Formatter::toolAvailable(const QString &language) const {
    const QString custom = toolFor(language);
    if (!custom.isEmpty())
        return programFound(custom);
    if (const ToolEntry *t = entryForLanguage(language))
        return programFound(QString::fromLatin1(t->command));
    return false;
}

QVariantList Formatter::toolList() const {
    QVariantList out;
    for (const ToolEntry &t : kTools) {
        const QString language = QString::fromLatin1(t.language);
        QVariantMap m;
        m.insert(QStringLiteral("id"), language);
        m.insert(QStringLiteral("label"), QString::fromUtf8(t.label));
        m.insert(QStringLiteral("tool"), QString::fromLatin1(t.tool));
        m.insert(QStringLiteral("defaultCommand"), QString::fromLatin1(t.command));
        m.insert(QStringLiteral("command"), toolFor(language));
        m.insert(QStringLiteral("available"), toolAvailable(language));
        out.append(m);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* 内置格式化                                                          */
/* ------------------------------------------------------------------ */

QString Formatter::formatBuiltin(const QString &text, const QString &kind) const {
    if (kind == QLatin1String("json")) {
        /*
         * JSON：交给 QJsonDocument 解析 + 重排（Indented = 4 空格）。
         *
         * 解析失败就**原样返回 + 报出错在第几个字节**：格式化一个坏 JSON 的
         * 唯一正确做法是告诉用户哪儿坏了，而不是把正文改成一堆空格。
         */
        QJsonParseError parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parse);
        if (parse.error != QJsonParseError::NoError) {
            m_lastError = tr("JSON 解析失败：%1（第 %2 个字节）")
                              .arg(parse.errorString())
                              .arg(parse.offset);
            return text;
        }
        QString out = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
        if (!out.endsWith(QLatin1Char('\n')))
            out += QLatin1Char('\n');
        return out;
    }

    if (kind == QLatin1String("xml")) {
        /*
         * XML / HTML 重排：先借 QXmlStreamReader **校验良构性并报出行号**
         * （它是个严格的 XML 解析器，报错信息比我们自己猜的准），
         * 再按标签切分重排。
         *
         * 不自己写扫描器去校验：漏掉一个转义实体就会把好文件判成坏的。
         */
        QXmlStreamReader check(text);
        while (!check.atEnd()) {
            check.readNext();
            if (check.hasError()) {
                m_lastError = tr("XML 不合法：%1（第 %2 行）")
                                  .arg(check.errorString())
                                  .arg(check.lineNumber());
                return text;
            }
        }

        /* 整份挤在一行时先在最外层标签之间切一刀，后面按行处理 */
        QString spaced = text;
        spaced.replace(QStringLiteral("><"), QStringLiteral(">\n<"));

        static const QRegularExpression kVoid(
            QStringLiteral(R"(^<(area|base|br|col|embed|hr|img|input|link|meta|param|source|track|wbr)\b)"));

        const QStringList lines = spaced.split(QLatin1Char('\n'));
        QStringList out;
        int depth = 0;
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty())
                continue;

            const bool closing = line.startsWith(QLatin1String("</"));
            const bool declaration =
                line.startsWith(QLatin1String("<?")) || line.startsWith(QLatin1String("<!"));
            const bool selfClose = line.endsWith(QLatin1String("/>"));
            const bool voidTag = kVoid.match(line).hasMatch();

            /* 结束标签先退一格再摆（它和开始标签同列） */
            const int indent = closing ? qMax(0, depth - 1) : depth;
            out.append(QString(indent * 2, QLatin1Char(' ')) + line);

            if (closing)
                depth = qMax(0, depth - 1);
            else if (!declaration && !selfClose && !voidTag && line.startsWith(QLatin1Char('<')))
                ++depth;
        }
        return out.join(QLatin1Char('\n')) + QLatin1Char('\n');
    }

    if (kind == QLatin1String("text")) {
        /*
         * 通用清理：去行尾空白、收敛连续空行、末尾补一个换行。
         *
         * 任何语言都安全（它不碰缩进、不碰词法），所以是纯文本那档的兜底。
         * 注意**不缩进**：没有语法分析就去改缩进，效果只会是把代码弄乱。
         *
         * 行尾空白那条必须带 (?m)：QRegularExpression 默认的 $ 只匹配**整个
         * 字符串的结尾**（不是每一行的结尾），不加这个开关只有最后一行能被
         * 清掉（实测：`"a   \n\n\n\nb\t\n"` 清完是 `a   \n\nb\n` —— 中间那些
         * 行尾空白一个没动）。
         */
        static const QRegularExpression kTrailing(QStringLiteral(R"((?m)[ \t]+$)"));
        static const QRegularExpression kBlankRun(QStringLiteral("\n{3,}"));


        QString out = toLf(text);
        out.replace(kTrailing, QString());
        out.replace(kBlankRun, QStringLiteral("\n\n"));
        while (out.endsWith(QLatin1Char('\n')))
            out.chop(1);
        if (!out.isEmpty())
            out += QLatin1Char('\n');
        return out;
    }

    m_lastError = tr("没有可用的内置格式化器");
    return text;
}

/* ------------------------------------------------------------------ */
/* 外部工具                                                            */
/* ------------------------------------------------------------------ */

QString Formatter::formatExternal(const QString &text, const QString &command,
                                  const QString &suffix, bool inPlace,
                                  QString *outEngine) const {
    if (outEngine)
        *outEngine = programName(command);

    /*
     * 临时目录 + 临时文件：工具要在**真实文件**上干活。
     *
     * 为什么不把正文从 stdin 喂进去、再从 stdout 读回来：受限环境（DSH 那类
     * sandbox）里"给子进程开管道"会被拒（EPERM，工具连启动都启动不了），
     * 而重定向到文件到处都能用。扩展名按语言给 —— prettier / black 靠它判语言。
     */
    QTemporaryDir dir;
    if (!dir.isValid()) {
        m_lastError = tr("建不了临时目录");
        return text;
    }
    const QString path = dir.filePath(QStringLiteral("clip.") + suffix);
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            m_lastError = tr("写不了临时文件");
            return text;
        }
        f.write(text.toUtf8());
    }

    /*
     * 命令行：{file} 换成临时文件路径；**没写占位**就把路径追加在末尾 ——
     * 表里那些默认命令（clang-format / gofmt / rustfmt）靠的就是这个约定。
     */
    QString line = command;
    const bool hasPlaceholder = line.contains(QLatin1String("{file}"));
    line.replace(QStringLiteral("{file}"), path);
    QStringList args = QProcess::splitCommand(line);
    if (args.isEmpty()) {
        m_lastError = tr("格式化命令是空的");
        return text;
    }
    const QString program = args.takeFirst();
    if (!hasPlaceholder)
        args.append(path);

    QProcess proc;
    proc.setProgram(program);
    proc.setArguments(args);
    proc.setWorkingDirectory(dir.path());
    proc.start();
    if (!proc.waitForStarted(8000)) {
        m_lastError = tr("没找到 %1：请先安装它，或在设置 → 格式化里填上它的完整路径")
                          .arg(QFileInfo(program).fileName());
        return text;
    }
    if (!proc.waitForFinished(30000)) {
        proc.kill();
        proc.waitForFinished(2000);
        m_lastError = tr("%1 跑了 30 秒还没结束，已中止").arg(QFileInfo(program).fileName());
        return text;
    }

    const QString stdOut = QString::fromUtf8(proc.readAllStandardOutput());
    const QString stdErr = QString::fromUtf8(proc.readAllStandardError()).trimmed();

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        /*
         * 工具报的错几乎总在 stderr（clang-format 报语法错、black 报缩进错），
         * 那才是用户需要看的东西；它一句话没说时才退回报退出码。
         */
        m_lastError = stdErr.isEmpty()
                          ? tr("%1 退出码 %2").arg(QFileInfo(program).fileName())
                                .arg(proc.exitCode())
                          : stdErr;
        return text;
    }

    QString result;
    if (inPlace) {
        /* 就地改文件的那两个（php-cs-fixer / rubocop）：改完从文件读回来 */
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            m_lastError = tr("读不回格式化结果");
            return text;
        }
        result = QString::fromUtf8(f.readAll());
    } else {
        result = stdOut;
    }

    /*
     * 成功但什么都没吐出来：**不认**。
     *
     * 把它当成功写回编辑器，等于一键清空用户的正文 —— 这是这个功能最不能出的
     * 事故。宁可报错、正文一动不动。
     */
    if (result.trimmed().isEmpty()) {
        m_lastError = stdErr.isEmpty()
                          ? tr("%1 没有输出内容").arg(QFileInfo(program).fileName())
                          : stdErr;
        return text;
    }

    /* 统一成 LF + 末尾一个换行（写盘时的 CRLF 转换由编辑器那边管） */
    result = toLf(result);
    if (!result.endsWith(QLatin1Char('\n')))
        result += QLatin1Char('\n');

    m_lastError.clear();
    return result;
}

/* ------------------------------------------------------------------ */
/* 入口                                                                */
/* ------------------------------------------------------------------ */

QString Formatter::format(const QString &text, const QString &language,
                          const QString &filePath) {
    m_lastError.clear();
    m_lastEngine.clear();

    if (text.isEmpty()) {
        m_lastError = tr("正文是空的，没什么可格式化的");
        return text;
    }

    const QString custom = toolFor(language);
    const ToolEntry *entry = entryForLanguage(language);
    const QString builtin = builtinKindForLanguage(language);

    /* 临时文件的扩展名：优先用**这份文件自己的**（.h / .cpp 会影响 clang-format 的判语言） */
    QFileInfo fi(filePath);
    const QString suffix = fi.suffix().isEmpty()
                               ? QString::fromLatin1(entry ? entry->suffix : "txt")
                               : fi.suffix();

    /* 1) 设置里为这个语言填的命令 */
    if (!custom.isEmpty()) {
        QString engine;
        const QString result =
            formatExternal(text, custom, suffix, entry ? entry->inPlace : false, &engine);
        m_lastEngine = engine;
        return result;
    }

    /* 2) 这个语言对口的默认工具（装了才用） */
    if (entry) {
        const QString command = QString::fromLatin1(entry->command);
        if (programFound(command)) {
            QString engine;
            const QString result = formatExternal(text, command, suffix, entry->inPlace,
                                                  &engine);
            m_lastEngine = engine;
            return result;
        }
    }

    /* 3) 内置的 json / xml */
    if (!builtin.isEmpty()) {
        const QString result = formatBuiltin(text, builtin);
        if (m_lastError.isEmpty())
            m_lastEngine = QStringLiteral("内置格式化");
        return result;
    }

    /*
     * 4) 什么工具都没有：
     *   * 纯文本 / INI 这类没有语法结构的 —— 走通用清理（去行尾空白、收敛空行），
     *     这是安全的，也确实有人就是想要这个；
     *   * 其它语言（C++ / Python…）—— **什么都不做**并报清楚。
     *     自己写个缩进器去猜 C++ 的层级只会把代码改坏，宁可让用户去装工具。
     */
    if (language == QLatin1String("plain") || language == QLatin1String("properties")) {
        const QString result = formatBuiltin(text, QStringLiteral("text"));
        if (m_lastError.isEmpty())
            m_lastEngine = QStringLiteral("去行尾空白");
        return result;
    }

    const QString tool = entry ? QString::fromLatin1(entry->tool) : QString();
    m_lastError = tool.isEmpty()
                      ? tr("这个语言没有可用的格式化器")
                      : tr("本机没装 %1：装好之后重开这个菜单就能用，也可以在设置 → 格式化里指定路径")
                            .arg(tool);
    return text;
}
