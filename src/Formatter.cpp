#include "Formatter.h"

#include <QCoreApplication>
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
 *   inPlace   true = 工具直接改文件（black / php-cs-fixer / rubocop），
 *             false = 结果从 stdout 读
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
    /*
     * python 这一行原来写的是 `black -`（从标准输入读）。那是个死路：
     * 我们的调用约定会把临时文件路径**追加在末尾**，black 于是同时收到 `-` 和一个
     * 文件 —— 实测它先报 "cannot format -: [Errno 2]" 再退出码 123（整条格式化
     * 永远失败），而 QProcess 那头从不往 stdin 写东西，光给它 `-` 时会一直等，
     * 直到 30 秒超时被杀（GUI 线程那 30 秒是冻住的）。
     * 改成只给文件：black 对文件是**就地改写**（实测），所以 inPlace = true。
     */
    {"python",     "Python",      "black",        "black",                            true,  "py"},
    {"bash",       "Shell 脚本",  "shfmt",        "shfmt",                              false, "sh"},
    {"go",         "Go",          "gofmt",        "gofmt",                              false, "go"},
    /*
     * rust 那行原来写的是 `rustfmt`（inPlace = false）：它默认**就地改文件**、
     * 什么都不往 stdout 吐，于是我们那条"成功但没输出"的守卫必然报错 —— 这一档
     * 从来没用过。改成 inPlace = true（和 black / rubocop / php-cs-fixer 一样读回来）。
     *
     * 试过 `--emit stdout`：它输出的不是纯正文，前面还带一行
     * `\\?\C:\...\t.rs:` 加一个空行（那是它多文件模式的标头）—— 那样等于把垃圾
     * 写回编辑器，所以不用那条路。
     */
    {"rust",       "Rust",        "rustfmt",      "rustfmt",                            true,  "rs"},
    /*
     * php 那行原来写的是 `php-cs-fixer fix`：实测它**一个字都不改**就退出 0 ——
     * 没有 --rules 也没有配置文件时它不带任何规则。那样这一行就是个点了没反应的
     * 按钮，所以把规则写进默认命令（@PSR12 是它自己的默认那一套：4 空格、
     * 函数大括号换行）。
     */
    {"php",        "PHP",         "php-cs-fixer", "php-cs-fixer fix --rules=@PSR12", true,  "php"},
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

/*
 * 把 QJsonDocument 排出来的 4 空格缩进改成模版要的那个宽度。
 *
 * QJsonDocument 没有"缩进几格"这个接口（Indented 就是写死的 4 空格），而 IDEA 的
 * JSON 模版是 2 —— 只能重排一遍行首。
 *
 * 逐行重排对 JSON 是**安全**的：合法 JSON 的字符串里不许出现裸换行（控制字符必须
 * 转义成 \n），所以每一行行首的那些空格一定是结构缩进，不可能是字符串的内容。
 * 这一条不是"大概没问题"，是 JSON 语法保证的。
 */
QString reindentJson(const QString &text, int to) {
    if (to == 4)
        return text;

    const QStringList lines = text.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(lines.size());
    for (const QString &line : lines) {
        int level = 0;
        while (level < line.size() && line.at(level) == QLatin1Char(' '))
            ++level;
        // QJsonDocument 只发 4 的倍数；真出现别的数字说明这行不是它排的，原样留着
        if (level % 4 != 0) {
            out << line;
            continue;
        }
        out << QString((level / 4) * to, QLatin1Char(' ')) + line.mid(level);
    }
    return out.join(QLatin1Char('\n'));
}

/*
 * PATH 之外，clang-format 还可能躺在哪儿。
 *
 * 为什么要这一层：Visual Studio 和 Qt Creator 都**自带**一份 clang-format
 * （实测这台机上有 4 份：`<VS>\VC\Tools\Llvm\{x64,ARM64,}\bin` 和
 * `<Qt>\Tools\QtCreator\bin\clang\bin`），但它们都不把自己写进 PATH ——
 * 于是"只查 PATH"的判定在这台机器上是一句假话："clang-format（本机没装）"。
 *
 * 为什么只给 clang-format 做：prettier / black 是 npm / pip 装的，那两个安装器
 * 本来就把全局 bin 写进 PATH，PATH 查不到就是真没装 —— 再给它们列候选目录只是猜。
 *
 * vswhere 的路径是微软文档保证的固定位置，用它问 VS 装在哪，不猜盘符；
 * Qt 那份从 `QCoreApplication::libraryPaths()` 往上推（跑起来必然带着 Qt 目录），
 * 也不猜装在哪块盘。整份清单进程内算一次（vswhere 要起个子进程）。
 */
QStringList clangFormatCandidates() {
    static const QStringList cached = [] {
        QStringList out;

        const QString vswhere = QStringLiteral(
            "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe");
        if (QFileInfo::exists(vswhere)) {
            QProcess probe;
            probe.setProgram(vswhere);
            probe.setArguments({QStringLiteral("-all"), QStringLiteral("-property"),
                                QStringLiteral("installationPath")});
            probe.start();
            if (probe.waitForFinished(4000)) {
                const QStringList dirs = QString::fromLocal8Bit(
                                             probe.readAllStandardOutput())
                                             .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                for (const QString &raw : dirs) {
                    const QString vs = raw.trimmed();
                    if (vs.isEmpty())
                        continue;
                    out << QDir(vs).absoluteFilePath(
                               QStringLiteral("VC/Tools/Llvm/x64/bin/clang-format.exe"))
                        << QDir(vs).absoluteFilePath(
                               QStringLiteral("VC/Tools/Llvm/bin/clang-format.exe"));
                }
            }
        }

        const QStringList libs = QCoreApplication::libraryPaths();
        for (const QString &lp : libs) {
            QDir d(QFileInfo(lp).absoluteFilePath());
            for (int up = 0; up < 3; ++up) {
                if (!d.cdUp())
                    break;
            }
            out << d.absoluteFilePath(QStringLiteral("Tools/QtCreator/bin/clang/bin/clang-format.exe"));
        }

        out << QStringLiteral("C:/Program Files/LLVM/bin/clang-format.exe")
            << QStringLiteral("C:/Program Files (x86)/LLVM/bin/clang-format.exe");
        return out;
    }();
    return cached;
}

/*
 * 这份文件**所在项目**里装的那个工具。
 *
 * 真实情况就是这样：prettier / black 大多是 `npm i -D` / `pip install` 装进某个
 * 项目的 node_modules、.venv 里，全局 PATH 上什么都没有（这台机上实测：
 * `H:\mall-ui\node_modules\.bin\prettier.CMD` 能跑出 3.8.1，而 npm 全局 bin 是空的）。
 * 主流编辑器也都是"用这份代码自己钉住的那份工具"，所以从文件往上找一层比
 * 猜全局目录更对。
 *
 * 往上走到盘根为止（最多 12 层，防人把整个盘当项目根）：
 *   node_modules/.bin/<工具>[.cmd|.exe]      —— npm / pnpm 装的开发依赖
 *   {.venv,venv}/Scripts/<工具>.exe          —— Windows 上的 python 虚拟环境
 *   {.venv,venv}/bin/<工具>                  —— 同一件事的 unix 摆法
 */
QString projectLocalProgram(const QString &filePath, const QString &program) {
    if (filePath.isEmpty() || program.isEmpty())
        return QString();

    static const char *const kSuffixes[] = {"", ".cmd", ".exe", ".bat"};

    QDir dir = QFileInfo(filePath).absoluteDir();
    for (int up = 0; up < 12; ++up) {
        const QString root = dir.absolutePath();
        QStringList bases;
        bases << root + QStringLiteral("/node_modules/.bin/")
              << root + QStringLiteral("/.venv/Scripts/")
              << root + QStringLiteral("/venv/Scripts/")
              << root + QStringLiteral("/.venv/bin/")
              << root + QStringLiteral("/venv/bin/");
        for (const QString &base : std::as_const(bases)) {
            for (const char *s : kSuffixes) {
                const QString cand = base + program + QString::fromLatin1(s);
                if (QFileInfo::exists(cand))
                    return QDir(cand).absolutePath();
            }
        }
        if (!dir.cdUp())
            break;
    }
    return QString();
}

/*
 * npx 用过的包留在缓存里，也算"这台机器上有"。
 *
 * `npx prettier` 跑过一次之后，包会躺在 `%LOCALAPPDATA%\npm-cache\_npx\<hash>\
 * node_modules\.bin\prettier.cmd`（这台机上就有两份）。它不属于哪个项目，
 * 所以设置面板那种"没有文件上下文"的判定也能认它 —— 多个 hash 取修改时间最新的
 * 那份（多半就是最近用过的那个版本），不靠目录名排序碰运气。
 */
QString npxCacheProgram(const QString &program) {
    static const char *const kRoots[] = {"/npm-cache/_npx", "/.npm/_npx"};
    const QString local = qEnvironmentVariable("LOCALAPPDATA");
    if (local.isEmpty() || program.isEmpty())
        return QString();

    QString newest;
    QDateTime newestAt;
    for (const char *rel : kRoots) {
        QDir root(local + QString::fromLatin1(rel));
        if (!root.exists())
            continue;
        const QStringList hashes = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &h : hashes) {
            const QString cand = root.absoluteFilePath(
                h + QStringLiteral("/node_modules/.bin/") + program + QStringLiteral(".cmd"));
            const QFileInfo fi(cand);
            if (!fi.exists())
                continue;
            if (!newestAt.isValid() || fi.lastModified() > newestAt) {
                newestAt = fi.lastModified();
                newest = fi.absoluteFilePath();
            }
        }
    }
    return newest;
}

/*
 * 用户级工具目录：装了、但**没写进 PATH** 的那些地方。
 *
 * 为什么要有这一层（这台机上量的）：`pip install black` 落在
 * `D:\Program\Python3.12\Scripts\`、scoop 落在 `~\scoop\shims` —— 这两个本来就在
 * PATH 上，所以这一层对它们用不上；但 `go install mvdan.cc/sh/v3/cmd/shfmt@latest`
 * 落在 `%USERPROFILE%\go\bin`，而那一档**不在 PATH 上**（实测用户 PATH 里没有它）。
 * cargo / npm / bun / pnpm 的用户 bin 是同一类。
 *
 * 这一层对**任何**程序名都试（不像 clang-format 那样只有一家有"已知安装位置"），
 * 所以以后装 shfmt / rustfmt / rubocop 不用再改代码。
 *
 * SMARTCLIP_TOOL_DIRS（分号隔开）排在最前面：既是给"我的工具装在别处"留的口子，
 * 也是自检的测试缝 —— 缓存键带上了这个值，改了它立刻重查。
 */
QStringList userToolDirs() {
    QStringList out;

    const QStringList extraDirs = qEnvironmentVariable("SMARTCLIP_TOOL_DIRS")
                                      .split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString &d : extraDirs)
        out << d;

    const QString home = QDir::homePath();
    out << home + QStringLiteral("/go/bin")
        << home + QStringLiteral("/.cargo/bin")
        << home + QStringLiteral("/bin")
        << home + QStringLiteral("/.local/bin")
        << home + QStringLiteral("/.bun/bin")
        << qEnvironmentVariable("APPDATA") + QStringLiteral("/npm");

    /* pip --user 和"只给当前用户装的 Python"两种摆法：<root>\Python312\Scripts */
    const QStringList pyRoots{qEnvironmentVariable("APPDATA") + QStringLiteral("/Python"),
                              qEnvironmentVariable("LOCALAPPDATA")
                                  + QStringLiteral("/Programs/Python")};
    for (const QString &root : std::as_const(pyRoots)) {
        QDir r(root);
        if (!r.exists())
            continue;
        const QStringList kids = r.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &k : kids)
            out << r.absoluteFilePath(k + QStringLiteral("/Scripts"));
    }
    return out;
}

/* 在这些目录里找这个程序（Windows 上带 .exe 的先试，再试 npm / gem / composer 那种 .cmd|.bat 包装） */
QString programInUserDirs(const QString &base) {
    static const char *const kSuffixes[] = {".exe", ".cmd", ".bat", ""};
    const QStringList dirs = userToolDirs();
    for (const QString &d : dirs) {
        if (d.isEmpty())
            continue;
        for (const char *s : kSuffixes) {
            const QString cand = QDir(d).absoluteFilePath(base + QString::fromLatin1(s));
            if (QFileInfo::exists(cand))
                return QDir(cand).absolutePath();
        }
    }
    return QString();
}

/*
 * 这个程序名到底在不在，在的话给绝对路径。
 *
 * 顺序是有讲究的，四档：
 *   1. 用户自己排的 PATH（他写了什么就认什么，包括设置里填的完整路径）；
 *   2. **这份文件所在项目**里的 node_modules / venv（代码钉住的版本，见上面）；
 *   3. 已知落点：VS 的 Llvm、Qt Creator 自带的 clang、官方 LLVM 目录；
 *   4. npx 缓存。
 * origin 带回来的是"第几档中的"，界面那句括号要照着说 —— 项目里装的和全局装的
 * 不是一回事，说成一样的会误导下一个文件。
 */
QString resolveProgramPath(const QString &program, const QString &filePath,
                           QString *origin) {
    auto setOrigin = [origin](const char *o) { if (origin) *origin = QString::fromLatin1(o); };

    if (program.isEmpty())
        return QString();
    if (program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\'))) {
        if (QFileInfo::exists(program)) {
            setOrigin("path");
            return program;
        }
        return QString();
    }

    const QString onPath = QStandardPaths::findExecutable(program);
    if (!onPath.isEmpty()) {
        setOrigin("path");
        return onPath;
    }

    const QString project = projectLocalProgram(filePath, program);
    if (!project.isEmpty()) {
        setOrigin("project");
        return project;
    }

    const QString base = QFileInfo(program).fileName().toLower();

    /* 用户级 bin（go install / cargo install / npm -g 那些没进 PATH 的落点） */
    const QString inUserDirs = programInUserDirs(base);
    if (!inUserDirs.isEmpty()) {
        setOrigin("machine");
        return inUserDirs;
    }

    if (base.compare(QLatin1String("clang-format")) == 0
        || base.compare(QLatin1String("clang-format.exe")) == 0) {
        for (const QString &c : clangFormatCandidates()) {
            if (QFileInfo::exists(c)) {
                setOrigin("global");
                return c;
            }
        }
    }

    const QString npx = npxCacheProgram(base);
    if (!npx.isEmpty()) {
        setOrigin("npx");
        return npx;
    }
    return QString();
}

/*
 * 这一语言要不要**先试内置**，而不是先试装了的外部工具。
 *
 * 只有 JSON。IDEA 的 JSON 模版是"缩进 2 + 对象/数组一律换行"
 * （JsonLanguageCodeStyleSettingsProvider 的 INDENT_SIZE=2、JsonCodeStyleSettings
 *  的 OBJECT_WRAPPING / ARRAY_WRAPPING = WRAP_ALWAYS）。内置那份（QJsonDocument
 * 重排）天生就是每项一行；而 prettier 会把塞得进一行的对象**收成一行**
 * （实测 `{"a":{"b":2}}` → 一行），而且它没有开关能表达"一律换行"。
 * 所以 JSON 用内置那份更像这份模版，还不用装任何东西。
 *
 * 用户在设置里给 json 填了命令的话仍然优先（那是他明确指定的）。
 */
bool builtinFirst(const QString &language) {
    return language == QLatin1String("json");
}

}  // namespace

Formatter::Formatter(QObject *parent) : QObject(parent) {
    /*
     * 每种语言的格式模版文件（`<安装目录>/styles/<语言>.json`）：
     * 只把**缺的**补出来，已经存在的一个字不动（见 fmtstyle::seedTemplates）。
     * 放在构造函数而不是"第一次格式化的时候"，是因为设置面板一打开就要列这些行 ——
     * 那会儿还没格式化过任何东西。
     */
    fmtstyle::seedTemplates();
}

/* ------------------------------------------------------------------ */
/* 支持情况                                                            */
/* ------------------------------------------------------------------ */

/*
 * 命令行里第一段程序在不在（返回绝对路径，空 = 找不到）。
 *
 * 查四档：PATH → **这份文件所在项目**的 node_modules / venv → 已知落点（VS 的
 * Llvm、Qt Creator 自带的 clang、官方 LLVM 目录）→ npx 缓存。见上面
 * resolveProgramPath 那段（为什么只给 clang-format 列落点、为什么项目那份优先）。
 * 都不真去跑它 —— 那几个 inPlace 的工具跑一次就**就地改文件**，拿"测试"去试会动
 * 用户的正文。写了路径的（C:/tools/clang-format.exe）就直接看那个文件在不在。
 *
 * ===========================================================================
 * 为什么结果要缓存
 * ===========================================================================
 * findExecutable 是按 PATHEXT 把整条 PATH 扫一遍：本机 PATH 有 99 个目录，
 * 查一个**没装**的程序（设置里标着"（本机没装）"的那几行）就要十几毫秒；
 * 现在还要"从这份文件往上走 12 层 × 5 个目录"和列 npx 缓存目录，更不能现算 ——
 * toolList() 一次要查 15 个条目，实测原来那一轮 ≈190ms 全压在 GUI 线程上。
 *
 * 这条路以前是"谁问都现算"（设置面板刷新、右键菜单弹出、状态栏那句…），
 * 于是设置里从这一行点到下一行，界面就要冻两下（见 setToolFor 的说明）。
 * 缓存 2 秒不改变"装了工具就能用"：2 秒远短于"去装一个工具再回来"的时间，
 * 但足够把连点触发的那串刷新、以及同一轮里的重复查询（clang-format 要查 3 次、
 * prettier 5 次）全吃掉。
 *
 * 带路径的命令不缓存：那只是一次 QFileInfo::exists，而且用户可能正往那儿拷文件。
 */
QString Formatter::resolvedProgram(const QString &command, const QString &filePath) const {
    const QString program = programName(command);
    if (program.isEmpty())
        return QString();
    if (program.contains(QLatin1Char('/')) || program.contains(QLatin1Char('\\')))
        return QFileInfo::exists(program) ? program : QString();

    /*
     * 缓存键**带上文件所在目录**：同一个 prettier 在 A 项目的 node_modules 里有、
     * 在 B 项目里没有是两件事 —— 只按程序名缓存的话，先问 A 再问 B 会把 B 也报成
     * "有"，然后按下去执行不了。
     */
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    /*
     * 键里带上 SMARTCLIP_TOOL_DIRS：那一层是每次读环境的，值变了（自检会改它，
     * 用户自己设也是）就不能再吃上一次的结果。
     */
    const QString key = (filePath.isEmpty() ? program
                            : program + QLatin1Char('|') + QFileInfo(filePath).absolutePath())
                        + QLatin1Char('#') + qEnvironmentVariable("SMARTCLIP_TOOL_DIRS");
    const auto cached = m_foundCache.constFind(key);
    if (cached != m_foundCache.constEnd() && now - cached.value().second < kFoundCacheMs)
        return cached.value().first;

    const QString resolved = resolveProgramPath(program, filePath, nullptr);
    m_foundCache.insert(key, qMakePair(resolved, now));
    return resolved;
}

bool Formatter::programFound(const QString &command, const QString &filePath) const {
    return !resolvedProgram(command, filePath).isEmpty();
}

/*
 * 这个工具是从哪儿来的（界面那句括号用）：
 *   "project" 这份文件的 node_modules 或 venv · "npx" npx 留下的缓存
 *   "machine" 机器上的（PATH / VS 的 Llvm / Qt Creator / LLVM 目录）
 * 看解析出来的路径长什么样就分得开，不用再扫一遍 PATH。
 */
QString Formatter::programOrigin(const QString &command, const QString &filePath) const {
    const QString resolved = resolvedProgram(command, filePath);
    if (resolved.isEmpty())
        return QString();
    QString n = resolved.toLower();
    n.replace(QLatin1Char('\\'), QLatin1Char('/'));
    /*
     * npx 那条要**先判**：npx 缓存里的路径本身就叫
     * `<LOCALAPPDATA>/npm-cache/_npx/<hash>/node_modules/.bin/prettier.cmd`，
     * 先按 node_modules 分的话它会被归成"这份文件的项目里"（实测错过一次：
     * 面板上写着"项目里"，可那份文件根本不在任何项目里）。
     */
    if (n.contains(QLatin1String("/npm-cache/_npx/")) || n.contains(QLatin1String("/.npm/_npx/")))
        return QStringLiteral("npx");
    if (n.contains(QLatin1String("/node_modules/.bin/")))
        return QStringLiteral("project");
    if (n.contains(QLatin1String("/.venv/")) || n.contains(QLatin1String("/venv/")))
        return QStringLiteral("project");
    return QStringLiteral("machine");
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

    /*
     * 顺序：**外部工具优先，内置兜底** —— 但 JSON 反过来（见 builtinFirst）。
     *
     * 外部优先的理由是"用户装了 prettier 多半就想要 prettier 那套"；而 JSON 这一档
     * 装了 prettier 反而拿不到模版的形状（它会把能塞下的对象收成一行且没有开关），
     * 所以那一档先试内置。这条顺序必须和 format() 里挑工具的顺序**完全一致**，
     * 否则就会出现"菜单说能格式化、按下去用的却是另一个东西"。
     */
    const QString custom = toolFor(language);
    if (!custom.isEmpty())
        return programFound(custom, filePath) ? programName(custom) : QString();

    if (builtinFirst(language) && !builtinKindForLanguage(language).isEmpty())
        return QStringLiteral("内置格式化");

    if (const ToolEntry *t = entryForLanguage(language)) {
        if (programFound(QString::fromLatin1(t->command), filePath))
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

bool Formatter::toolAvailable(const QString &language, const QString &filePath) const {
    const QString custom = toolFor(language);
    if (!custom.isEmpty())
        return programFound(custom, filePath);
    if (const ToolEntry *t = entryForLanguage(language))
        return programFound(QString::fromLatin1(t->command), filePath);
    return false;
}

QVariantList Formatter::toolList(const QString &filePath) const {
    QVariantList out;
    for (const ToolEntry &t : kTools) {
        const QString language = QString::fromLatin1(t.language);
        QVariantMap m;
        m.insert(QStringLiteral("id"), language);
        m.insert(QStringLiteral("label"), QString::fromUtf8(t.label));
        m.insert(QStringLiteral("tool"), QString::fromLatin1(t.tool));
        m.insert(QStringLiteral("defaultCommand"), QString::fromLatin1(t.command));
        const QString custom = toolFor(language);
        m.insert(QStringLiteral("command"), custom);
        m.insert(QStringLiteral("available"), toolAvailable(language, filePath));
        /* 这个工具从哪来：机器上 / 这份文件的项目里 / npx 缓存（界面那句括号照它说） */
        m.insert(QStringLiteral("source"),
                 programOrigin(custom.isEmpty() ? QString::fromLatin1(t.command) : custom,
                               filePath));
        /* 这一行的格式模版摘要（观感数字 + 当前这个工具能落地几项），面板第二行用 */
        m.insert(QStringLiteral("style"), styleSummary(language, filePath));
        m.insert(QStringLiteral("styleError"), styleError(language));
        out.append(m);
    }
    return out;
}

/* ------------------------------------------------------------------ */
/* 内置格式化                                                          */
/* ------------------------------------------------------------------ */

QString Formatter::formatBuiltin(const QString &text, const QString &kind,
                                 const fmtstyle::Style &style) const {
    if (kind == QLatin1String("json")) {
        /*
         * JSON：交给 QJsonDocument 解析 + 重排（缩进按模版，IDEA 是 2 格）。
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
        QString out = reindentJson(
            QString::fromUtf8(doc.toJson(QJsonDocument::Indented)), style.indentSize);
        if (style.newlineAtEnd) {
            while (out.endsWith(QLatin1Char('\n')))
                out.chop(1);
            out += QLatin1Char('\n');
        }
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
            out.append(QString(indent * style.indentSize, QLatin1Char(' ')) + line);

            if (closing)
                depth = qMax(0, depth - 1);
            else if (!declaration && !selfClose && !voidTag && line.startsWith(QLatin1Char('<')))
                ++depth;
        }
        const QString joined = out.join(QLatin1Char('\n'));
        return style.newlineAtEnd ? joined + QLatin1Char('\n') : joined;
    }

    if (kind == QLatin1String("text")) {
        /*
         * 通用清理：去行尾空白、把连续空行收到模版允许的那个上限、末尾补换行。
         *
         * 任何语言都安全（它不碰缩进、不碰词法），所以是纯文本那档的兜底。
         * 注意**不缩进**：没有语法分析就去改缩进，效果只会是把代码弄乱。
         *
         * 行尾空白那条必须带 (?m)：QRegularExpression 默认的 $ 只匹配**整个
         * 字符串的结尾**（不是每一行的结尾），不加这个开关只有最后一行能被
         * 清掉（实测：`"a   \n\n\n\nb\t\n"` 清完是 `a   \n\nb\n` —— 中间那些
         * 行尾空白一个没动）。
         *
         * 空行上限来自模版：K 个空行 = K+1 个连续换行，所以"超过 K 个"就是
         * `\n{K+2,}`。原来这里写死收成一个空行（等于 K=1），而 IDEA 的
         * KEEP_BLANK_LINES_IN_CODE 是 2 —— 数字以模版为准。
         */
        static const QRegularExpression kTrailing(QStringLiteral(R"((?m)[ \t]+$)"));
        const int keep = qMax(0, style.keepBlankLines);
        const QRegularExpression blankRun(QStringLiteral("\\n{%1,}").arg(keep + 2));
        const QString runReplacement = QString(keep + 1, QLatin1Char('\n'));

        QString out = toLf(text);
        out.replace(kTrailing, QString());
        out.replace(blankRun, runReplacement);
        while (out.endsWith(QLatin1Char('\n')))
            out.chop(1);
        if (!out.isEmpty() && style.newlineAtEnd)
            out += QLatin1Char('\n');
        return out;
    }

    m_lastError = tr("没有可用的内置格式化器");
    return text;
}

/* ------------------------------------------------------------------ */
/* 外部工具                                                            */
/* ------------------------------------------------------------------ */

/*
 * 那条命令 + 当前语言的模版 -> 真正要执行的参数表。
 *
 * 用户已经写了同一个开关就不再追加：命令行里自己那份优先。规则是"参数表里出现过
 * 这个开关名（--tab-width 或者 --tab-width=4）就算写过"，所以
 *   clang-format --style=file          -> 不动（他自己指定了风格来源）
 *   prettier --stdin-filepath x.js     -> 追加 --tab-width 4 --print-width 120
 *   black -q -                         -> 追加 --line-length 120
 * 顺带一条边界：认不出程序名（用户填的是自己包的脚本）时**什么都不追加** ——
 * 猜错了会把人家的命令弄坏，而这种命令他本来就自己带风格。
 */
QStringList Formatter::withStyleArgs(const QString &command,
                                     const fmtstyle::Style &style) const {
    QStringList argv = QProcess::splitCommand(command);
    if (argv.isEmpty())
        return argv;

    const QStringList extra = fmtstyle::styleArgs(style, fmtstyle::engineKind(command));
    if (extra.isEmpty())
        return argv;

    /*
     * "已经写过"比的是**开关名**，不是整串：clang-format 这条追加的是
     * "--style={BasedOnStyle: LLVM, …}"，而用户可能写的是 "--style=file" ——
     * 整串相比永远不相等，于是两个 --style 一起发过去。
     *
     * 谁是最后一个谁算（实测 clang-format 23.1.2：同一份源码，
     * `--style={IndentWidth: 8} --style={IndentWidth: 2}` 出来 2 个空格，反过来
     * 出来 8 个）。我们追加的那条正好在后面，所以不去重的话**用户的 --style 会被
     * 我们悄悄盖掉** —— 那是把他亲手填的一行配置变成摆设。
     */
    auto flagName = [](const QString &token) {
        const int eq = token.indexOf(QLatin1Char('='));
        return eq > 0 ? token.left(eq) : token;
    };

    int i = 0;
    while (i < extra.size()) {
        const QString flag = extra.at(i);
        int last = i;  // flag 自己之外，它那几个不带横线的取值
        while (last + 1 < extra.size() && !extra.at(last + 1).startsWith(QLatin1Char('-')))
            ++last;

        const QString name = flagName(flag);
        bool already = false;
        for (const QString &a : std::as_const(argv)) {
            if (flagName(a) == name) {
                already = true;
                break;
            }
        }
        if (!already) {
            for (int k = i; k <= last; ++k)
                argv << extra.at(k);
        }
        i = last + 1;
    }
    return argv;
}

QString Formatter::formatExternal(const QString &text, const QStringList &argv,
                                  const QString &suffix, bool inPlace,
                                  const fmtstyle::Style &style, const QString &filePath,
                                  QString *outEngine) const {
    if (argv.isEmpty()) {
        m_lastError = tr("格式化命令是空的");
        return text;
    }
    const QString program = argv.first();
    if (outEngine)
        *outEngine = program;

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
     * 有的工具（rubocop）的风格只能写在配置文件里，命令行塞不进去 —— 写到这同一档，
     * 它从被格式化的文件往上找配置正好找到这份。反正我们本来就在临时目录里跑，
     * 用户项目里那份配置本来也读不到（这也是为什么观感必须由我们的模版给）。
     */
    const QPair<QString, QString> cfg = fmtstyle::engineConfigFile(
        style, fmtstyle::engineKind(argv.first()));
    if (!cfg.first.isEmpty()) {
        QFile cf(dir.filePath(cfg.first));
        if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate))
            cf.write(cfg.second.toUtf8());
    }

    /*
     * 命令行：{file} 换成临时文件路径；**没写占位**就把路径追加在末尾 ——
     * 表里那些默认命令（clang-format / gofmt / rustfmt）靠的就是这个约定。
     * 这里按**每个参数**换，不是在整条字符串上换：换完还要保持"一个参数一段"，
     * 拼回字符串再拆会把带空格的参数（--style={…}）炸成好几段。
     */
    QStringList args;
    bool hasPlaceholder = false;
    for (const QString &a : argv) {
        if (a.contains(QLatin1String("{file}"))) {
            hasPlaceholder = true;
            args << QString(a).replace(QStringLiteral("{file}"), path);
        } else {
            args << a;
        }
    }
    args.removeFirst();  // 第一段是程序名
    if (!hasPlaceholder)
        args.append(path);

    QProcess proc;
    /*
     * 先把程序名落成**绝对路径**再启动：VS / Qt Creator 自带的那份 clang-format
     * 不在 PATH 上，"能不能用"那一步已经认得它了，要是这里还按名字启动，
     * 就会出现"面板说能用、按下去报找不到程序"（两头的判断不是同一份）。
     */
    const QString exe = resolveProgramPath(program, filePath, nullptr);
    if (exe.isEmpty()) {
        m_lastError = tr("没找到 %1：请先安装它，或在设置 → 格式化里填上它的完整路径")
                          .arg(QFileInfo(program).fileName());
        return text;
    }
    proc.setProgram(exe);
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

    const bool cleanExit = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    if (!cleanExit && !inPlace) {
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
        /* 就地改文件的那几个（black / php-cs-fixer / rubocop）：改完从文件读回来 */
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
     * 就地改文件的那一类**退出码不为 0 不等于失败**：rubocop 本质是 linter，
     * 只要还有"报告出来的" offense 就退出 1。实测它把 `def f / 4空格 puts 1` 改成
     * 2 空格（模版的值）之后仍然退出 1 —— 因为还剩一条 `Style/FrozenStringLiteralComment`
     * 是它建议、但不该由我们自动加的。原来这条"非零即失败"会把一次成功的格式化扔掉。
     *
     * 所以这里改判**文件到底动没动**：
     *   动了 = 活干完了，收下；
     *   没动 + 非零退出 = 真失败（配置读错、语法错、权限不够），把 stderr 原样交出去。
     * black 语法错那种正是"没动 + 123"，仍然照报，不会漏。
     */
    if (!cleanExit && inPlace && result.trimmed() == text.trimmed()) {
        m_lastError = stdErr.isEmpty()
                          ? tr("%1 退出码 %2，正文一个字没动")
                                .arg(QFileInfo(program).fileName()).arg(proc.exitCode())
                          : stdErr;
        return text;
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

    /* 统一成 LF；末尾换行按模版（写盘时的 CRLF 转换由编辑器那边管） */
    result = toLf(result);
    while (result.endsWith(QLatin1Char('\n')))
        result.chop(1);
    if (style.newlineAtEnd)
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

    /*
     * 这个语言的格式模版（内置默认 + %APPDATA%/.../styles/<语言>.json）。
     * 外部工具那条路把它翻译成参数，内置那三样直接读它的缩进 / 空行 / 末尾换行。
     */
    const fmtstyle::Style style = fmtstyle::styleFor(language);

    /* 临时文件的扩展名：优先用**这份文件自己的**（.h / .cpp 会影响 clang-format 的判语言） */
    QFileInfo fi(filePath);
    const QString suffix = fi.suffix().isEmpty()
                               ? QString::fromLatin1(entry ? entry->suffix : "txt")
                               : fi.suffix();

    /* 1) 设置里为这个语言填的命令 */
    if (!custom.isEmpty()) {
        QString engine;
        const QString result = formatExternal(text, withStyleArgs(custom, style), suffix,
                                             entry ? entry->inPlace : false, style, filePath,
                                             &engine);
        m_lastEngine = engine;
        return result;
    }

    /* 1.5) JSON 这类"内置比外部工具更像这份模版"的，先走内置（见 builtinFirst，
     *      和 engineLabel 里那条顺序必须一模一样） */
    if (builtinFirst(language) && !builtin.isEmpty()) {
        const QString result = formatBuiltin(text, builtin, style);
        if (m_lastError.isEmpty())
            m_lastEngine = QStringLiteral("内置格式化");
        return result;
    }

    /* 2) 这个语言对口的默认工具（装了才用） */
    if (entry) {
        const QString command = QString::fromLatin1(entry->command);
        if (programFound(command, filePath)) {
            QString engine;
            const QString result = formatExternal(text, withStyleArgs(command, style), suffix,
                                                 entry->inPlace, style, filePath, &engine);
            m_lastEngine = engine;
            return result;
        }
    }

    /* 3) 内置的 json / xml */
    if (!builtin.isEmpty()) {
        const QString result = formatBuiltin(text, builtin, style);
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
        const QString result = formatBuiltin(text, QStringLiteral("text"), style);
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

/* ------------------------------------------------------------------ */
/* 模版（界面那侧问的三件事）                                          */
/* ------------------------------------------------------------------ */

QVariantMap Formatter::styleFor(const QString &language) const {
    return fmtstyle::toMap(fmtstyle::styleFor(language));
}

QString Formatter::stylesDir() const {
    return QDir(fmtstyle::stylesDirPath()).absolutePath();
}

/*
 * 模版目录写不写得进去。
 *
 * 面板上那句"改完立刻生效"是拿这个当真的：装在只读位置（老版本管理员权限
 * 装进 Program Files 那份）时这句就不成立，得当场标出来，不然用户拿编辑器
 * 存不进去还以为是程序没读文件。
 */
bool Formatter::stylesWritable() const {
    return fmtstyle::stylesDirWritable();
}

/*
 * 一行小字：模版本身 + 当前**真正会用上的那个工具**能吃掉几项。
 *
 * 挑工具的顺序和 engineLabel / format() **必须一模一样**（自定义命令 >
 * [JSON 的内置] > 默认命令 > 内置），不然会出现"面板说 prettier 只吃两样、
 * 按下去跑的却是内置那份"。写成常量会骗人：同一份模版在 clang-format 上全落得下，
 * 在 prettier 上只有缩进和行宽。
 */
QString Formatter::styleSummary(const QString &language, const QString &filePath) const {
    const fmtstyle::Style style = fmtstyle::styleFor(language);

    const QString custom = toolFor(language);
    if (!custom.isEmpty())
        return fmtstyle::summaryText(style, fmtstyle::engineKind(custom), language);

    if (builtinFirst(language) && !builtinKindForLanguage(language).isEmpty())
        return fmtstyle::summaryText(style, fmtstyle::Engine::Builtin, language);

    if (const ToolEntry *t = entryForLanguage(language)) {
        const QString command = QString::fromLatin1(t->command);
        if (programFound(command, filePath))
            return fmtstyle::summaryText(style, fmtstyle::engineKind(command), language);
    }

    /* 走到这儿是没装任何工具：内置那三样只在 json / html / 纯文本这几档还有用 */
    const QString builtin = builtinKindForLanguage(language);
    if (!builtin.isEmpty() || language == QLatin1String("plain")
        || language == QLatin1String("properties"))
        return fmtstyle::summaryText(style, fmtstyle::Engine::Builtin, language);

    return fmtstyle::summaryText(style, fmtstyle::Engine::None, language);
}

/* 模版文件读坏了要说的话（空串 = 这份文件没问题） */
QString Formatter::styleError(const QString &language) const {
    return fmtstyle::loadErrorFor(language);
}
