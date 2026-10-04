#include "CodeStyle.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QProcess>
#include <QStandardPaths>
#include <QVariant>
#include <QVariantMap>

namespace fmtstyle {
namespace {

/*
 * 内置默认值的载体：所有语言都从这一份出发，再套下面的覆写。
 * 值和 IDEA 的平台默认一致（出处见 CodeStyle.h 文件头），所以认不出来的语言
 * 退回它也不会跑出这套观感。
 */

struct Override {
    const char *language;
    const char *note;  // UTF-8：这行的取值是从哪儿来的（写进播种文件里，界面不铺）
    int indentSize = -1;
    int tabSize = -1;
    int continuationIndent = -1;
    int rightMargin = -1;
    int keepBlankLines = -1;
    int indentCaseFromSwitch = -1;  // -1 = 不覆写
    int newlineAtEnd = -1;
};

/*
 * 逐语言的覆写表。没列在这里的语言 = 平台默认（缩进 4 / 续行 8 / 行宽 120 /
 * case 缩进 / 大括号同行 / 空行≤2，见 CodeStyle.h）。
 *
 * 每一项后面括号的含义：
 *   「平台默认」= 查到 JetBrains 的字段初始化就是这值，这门语言没有覆写；
 *   「未公开」  = intellij-community 的开源树里**没有**这门语言的插件
 *                 （C++ / C# / JavaScript / CSS / SQL），拿不到它的覆写值，
 *                 于是沿用平台默认 —— 写清楚是免得以后有人以为这是我们编的。
 */
const Override kOverrides[] = {
    {"cpp", "C++ 插件不在开源仓库里（未公开覆写），沿用 IDEA 平台默认：缩进 4 / 续行 8 / 行宽 120 / 大括号同行", 4, 4, 8, 120, 2, 1, -1},
    {"csharp", "C# 插件不在开源仓库里（未公开覆写），沿用 IDEA 平台默认", 4, 4, 8, 120, 2, 1, -1},
    {"java", "IDEA 平台默认（CodeStyleSettings.java 的字段初始化）：4 / 8 / 120 / case 缩进 / 空行≤2", 4, 4, 8, 120, 2, 1, -1},

    // python 覆写：KEEP_BLANK_LINES_IN_CODE=1、BLANK_LINE_AT_FILE_END=true
    //（PyCodeStyleSettings / PyLanguageCodeStyleSettingsProvider）；
    // 行宽没覆写 -> 平台默认 120，**不是 black 自己的 88**
    {"python", "PyCodeStyleSettings：连续空行最多 1、文件末尾补一行；行宽未覆写 -> 平台默认 120（不是 black 默认的 88）", 4, 4, 8, 120, 1, -1, 1},

    // json 覆写：INDENT_SIZE=2（JsonLanguageCodeStyleSettingsProvider）
    // 另有一条「对象/数组一律换行」(OBJECT/ARRAY_WRAPPING=WRAP_ALWAYS)：
    // prettier 表达不了（塞得下就收成一行），内置那份反倒天然是每项一行
    {"json", "IDEA JSON：缩进 2；另有「对象/数组一律换行」这条 prettier 表达不了（它会把塞得下的对象收成一行，且没有开关），所以 JSON 走内置那份 —— 见 Formatter.cpp 的 builtinFirst", 2, 4, 8, 120, 0, -1, -1},

    {"html", "XML/HTML 插件不覆写缩进 -> 平台默认 4；「属性用双引号」那条命令行表达不了", 4, 4, 8, 120, 2, 1, -1},
    {"css", "CSS 插件不在开源仓库里（未公开覆写），沿用 IDEA 平台默认", 4, 4, 8, 120, 2, 1, -1},
    {"javascript", "JavaScript/TypeScript 插件不在开源仓库里（未公开覆写），沿用 IDEA 平台默认 4", 4, 4, 8, 120, 2, 1, -1},
    {"markdown", "Markdown 插件不覆写缩进 -> 平台默认 4", 4, 4, 8, 120, 2, 1, -1},

    // yaml 覆写：INDENT_SIZE=2、CONTINUATION_INDENT_SIZE=2（YAMLLanguageCodeStyleSettingsProvider）
    {"yaml", "IDEA YAML：缩进 2、续行 2（YAMLCodeStyleSettings）", 2, 4, 2, 120, 2, -1, -1},

    // bash 覆写：INDENT_SIZE=2、TAB_SIZE=2、SWITCH_CASES_INDENTED=false（ShCodeStyleSettings）
    {"bash", "IDEA Shell：缩进 2、case 不相对 switch 缩进（ShCodeStyleSettings）", 2, 2, 8, 120, 2, 0, -1},

    // properties：不覆写缩进；另有一项「键值分隔符两侧不留空格」，
    // 我们的清理不碰分隔符，那条只是记在这儿
    {"properties", "IDEA Properties 不覆写缩进 -> 平台默认；「分隔符两侧不留空格」这条落不了地", 4, 4, 8, 120, 2, 1, -1},
    {"plain", "纯文本没有 code style，这里只用得上空行收敛和末尾换行", 4, 4, 8, 120, 2, 1, -1},

    // 下面四家的工具本身不给风格选项（或本轮没做适配）：模版记着，但**不假装生效**
    {"go", "gofmt 恒定 tab 缩进、没有任何风格开关，这份模版改变不了它的输出", 4, 4, 8, 120, 2, -1, -1},
    {"rust", "rustfmt 默认 4 空格 / 100 列；行宽这一项靠我们传 --config max_width 才对得上模版", 4, 4, 8, 120, 2, -1, -1},
    {"php", "php-cs-fixer 的缩进被 PSR-12 钉死 4 空格，模版里只有行宽/空行这些它不吃", 4, 4, 8, 120, 2, -1, -1},
    {"ruby", "rubocop 吃 .rubocop.yml（IndentationWidth / IndentationStyle / LineLength / TrailingWhitespace），由这份模版生成", 2, 4, 8, 120, 2, -1, -1},
};

/* 文件里认得这些键（其余算写错了：报出来，但不影响其它项） */
const char *kIntKeys[] = {"indentSize", "tabSize", "continuationIndent", "rightMargin",
                          "keepBlankLines"};
const char *kBoolKeys[] = {"useTabs", "indentCaseFromSwitch", "declarationBraceOnNextLine",
                           "controlBraceOnNextLine", "keepSimpleStatementsOnOneLine",
                           "newlineAtEnd"};

bool isKnownKey(const QString &key) {
    if (key == QLatin1String("note"))
        return true;
    for (const char *k : kIntKeys)
        if (key == QLatin1String(k))
            return true;
    for (const char *k : kBoolKeys)
        if (key == QLatin1String(k))
            return true;
    return false;
}

void appendErr(QString &err, const QString &what) {
    if (!err.isEmpty())
        err += QLatin1Char('\n');
    err += what;
}

/* 缩进类最大 16，keepBlankLines 最大 10，行宽最大 1000（0 = 不限） */
int upperBoundFor(const QString &key) {
    if (key == QLatin1String("rightMargin"))
        return 1000;
    if (key == QLatin1String("keepBlankLines"))
        return 10;
    return 16;
}

QVariantMap styleToMap(const Style &s) {
    QVariantMap m;
    m.insert(QStringLiteral("indentSize"), s.indentSize);
    m.insert(QStringLiteral("tabSize"), s.tabSize);
    m.insert(QStringLiteral("continuationIndent"), s.continuationIndent);
    m.insert(QStringLiteral("useTabs"), s.useTabs);
    m.insert(QStringLiteral("rightMargin"), s.rightMargin);
    m.insert(QStringLiteral("indentCaseFromSwitch"), s.indentCaseFromSwitch);
    m.insert(QStringLiteral("declarationBraceOnNextLine"), s.declarationBraceOnNextLine);
    m.insert(QStringLiteral("controlBraceOnNextLine"), s.controlBraceOnNextLine);
    m.insert(QStringLiteral("keepSimpleStatementsOnOneLine"), s.keepSimpleStatementsOnOneLine);
    m.insert(QStringLiteral("keepBlankLines"), s.keepBlankLines);
    m.insert(QStringLiteral("newlineAtEnd"), s.newlineAtEnd);
    m.insert(QStringLiteral("note"), s.note);
    return m;
}

/*
 * 文件里的项覆盖到 s 上。
 *
 * 一个键写错（名字不认识 / 类型不对 / 超出范围）只丢那一条，其余照用 ——
 * 和配色方案同一套做法：手改文件时打错一个字符不该让整份模版失效，
 * 但也不能不吭声（错在哪条由 loadErrorFor() 交给界面和自检）。
 */
void applyMap(const QVariantMap &m, Style &s, QString &err) {
    for (auto it = m.constBegin(); it != m.constEnd(); ++it) {
        const QString key = it.key();
        const QVariant &value = it.value();

        if (!isKnownKey(key)) {
            appendErr(err, QStringLiteral("不认识这个项：「%1」（这一条没生效）").arg(key));
            continue;
        }

        bool handled = false;
        for (const char *k : kIntKeys) {
            if (key != QLatin1String(k))
                continue;
            handled = true;
            bool ok = false;
            const int n = value.toInt(&ok);
            if (!ok || n < 0 || n > upperBoundFor(key)) {
                appendErr(err, QStringLiteral("%1 得是 0 到 %2 之间的整数")
                                  .arg(key).arg(upperBoundFor(key)));
                break;
            }
            if (key == QLatin1String("indentSize"))
                s.indentSize = n;
            else if (key == QLatin1String("tabSize"))
                s.tabSize = n;
            else if (key == QLatin1String("continuationIndent"))
                s.continuationIndent = n;
            else if (key == QLatin1String("rightMargin"))
                s.rightMargin = n;
            else
                s.keepBlankLines = n;
            break;
        }
        if (handled)
            continue;

        for (const char *k : kBoolKeys) {
            if (key != QLatin1String(k))
                continue;
            handled = true;
            if (value.typeId() != QMetaType::Bool) {
                appendErr(err, QStringLiteral("%1 得写成 true 或 false").arg(key));
                break;
            }
            const bool b = value.toBool();
            if (key == QLatin1String("useTabs"))
                s.useTabs = b;
            else if (key == QLatin1String("indentCaseFromSwitch"))
                s.indentCaseFromSwitch = b;
            else if (key == QLatin1String("declarationBraceOnNextLine"))
                s.declarationBraceOnNextLine = b;
            else if (key == QLatin1String("controlBraceOnNextLine"))
                s.controlBraceOnNextLine = b;
            else if (key == QLatin1String("keepSimpleStatementsOnOneLine"))
                s.keepSimpleStatementsOnOneLine = b;
            else
                s.newlineAtEnd = b;
            break;
        }
        if (handled)
            continue;

        // 只剩 note
        if (value.typeId() == QMetaType::QString)
            s.note = value.toString();
        else
            appendErr(err, QStringLiteral("note 得是一句话"));
    }
}

/* styleFor 的缓存：按"修改时间 + 字节数"失效（改完文件立刻生效，不用重启） */
struct Cached {
    qint64 mtime = -1;
    qint64 bytes = -1;
    Style style;
    QString err;
};

QHash<QString, Cached> &cache() {
    static QHash<QString, Cached> c;
    return c;
}

}  // namespace

Style builtinFor(const QString &language) {
    Style s;  // = Style 的默认成员值 = IDEA 平台默认
    for (const Override &o : kOverrides) {
        if (language != QLatin1String(o.language))
            continue;
        if (o.indentSize >= 0)
            s.indentSize = o.indentSize;
        if (o.tabSize >= 0)
            s.tabSize = o.tabSize;
        if (o.continuationIndent >= 0)
            s.continuationIndent = o.continuationIndent;
        if (o.rightMargin >= 0)
            s.rightMargin = o.rightMargin;
        if (o.keepBlankLines >= 0)
            s.keepBlankLines = o.keepBlankLines;
        if (o.indentCaseFromSwitch >= 0)
            s.indentCaseFromSwitch = o.indentCaseFromSwitch != 0;
        if (o.newlineAtEnd >= 0)
            s.newlineAtEnd = o.newlineAtEnd != 0;
        s.note = QString::fromUtf8(o.note);
        break;
    }
    return s;
}

QStringList languageIds() {
    QStringList out;
    out.reserve(int(sizeof(kOverrides) / sizeof(kOverrides[0])));
    for (const Override &o : kOverrides)
        out << QString::fromLatin1(o.language);
    return out;
}

namespace {

/* 老位置：换到程序目录之前模版一直放这儿，只用来搬文件 */
QString legacyStylesDirPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/styles");
}

}  // namespace

/*
 * 模版目录选在哪儿：**程序自己那个目录**（`<安装目录>/styles`）。
 *
 * 用户 2026-10-05 点名要放这儿。三个理由站得住：
 *   * 安装包（Inno，PrivilegesRequired=lowest + {autopf} → %LOCALAPPDATA%\Programs\
 *     SmartClip）和绿色 zip 都在这台机上跑，模版是**跟着这份程序走**的东西，
 *     和 AppData 里那份数据库放一起反而难找；
 *   * 那一档是用户可写的（lowest 权限装的），所以"改完立刻生效"仍然成立；
 *   * 卸载程序目录时一起带走，不会在 AppData 里留一堆孤儿文件。
 *
 * 退回 AppData 只有一种情况：程序目录建不出来或不能写（装在只读介质上、
 * 或者老版本用管理员权限装进了 Program Files，普通权限跑起来写不进去）。
 * 那种时候宁可退回原来那一档，也不要"整个模版功能静默失效"。
 *
 * 结果缓存一次：这条路径每次格式化、每次刷新设置面板都要问。
 */
QString stylesDirPath() {
    static QString cached;
    if (!cached.isEmpty())
        return cached;

    const QString appDir = QCoreApplication::applicationDirPath();
    const QString inPackage = appDir + QStringLiteral("/styles");
    if ((QFileInfo(appDir).isWritable() || QDir(inPackage).exists())
        && QDir().mkpath(inPackage)) {
        cached = inPackage;
        return cached;
    }
    const QString legacy = legacyStylesDirPath();
    QDir().mkpath(legacy);
    cached = legacy;
    return cached;
}

/* 这个目录现在写不写得进去（界面上那句"改完立刻生效"得配真话） */
bool stylesDirWritable() {
    return QFileInfo(stylesDirPath()).isWritable();
}

void seedTemplates() {
    const QString dir = stylesDirPath();
    if (!QDir().mkpath(dir))
        return;
    const QString legacy = legacyStylesDirPath();

    for (const QString &id : languageIds()) {
        const QString path = QDir(dir).absoluteFilePath(id + QStringLiteral(".json"));
        if (QFile::exists(path))
            continue;

        /*
         * 老位置（AppData/styles）里有就先搬过来，不重写一份内置默认：
         * 上一版就把模版放那儿，用户可能已经改过几个数，换目录不能把他的改动丢在原地。
         */
        const QString from = QDir(legacy).absoluteFilePath(id + QStringLiteral(".json"));
        if (legacy != dir && QFile::exists(from) && QFile::copy(from, path))
            continue;

        QFile f(path);
        /*
         * NewOnly = 文件已存在就打开失败：播种只补空的，绝不覆盖用户改过的那份。
         * 先 exists() 再 WriteOnly 也是同一件事，但那是"看一眼再动手"，中间被
         * 别的进程建出来就会把人家的文件截断。
         */
        if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
            continue;
        f.write(QJsonDocument::fromVariant(styleToMap(builtinFor(id)))
                    .toJson(QJsonDocument::Indented));
    }
}

Style styleFor(const QString &language) {
    const QString path = QDir(stylesDirPath())
                             .absoluteFilePath(language + QStringLiteral(".json"));
    const QFileInfo fi(path);
    const qint64 mtime = fi.exists() ? fi.lastModified().toMSecsSinceEpoch() : -1;
    const qint64 bytes = fi.exists() ? fi.size() : -1;

    const auto cached = cache().constFind(language);
    /*
     * 时间和大小一起比：只认 mtime 的话，一秒内连着保存两次（编辑器就是这么干的）
     * 会拿到同一个时间戳，第二次改的内容就悄悄读不到了。
     */
    if (cached != cache().constEnd() && cached->mtime == mtime && cached->bytes == bytes)
        return cached->style;

    Style s = builtinFor(language);
    QString err;
    if (mtime >= 0) {
        QFile f(path);
        QJsonParseError pe{};
        if (!f.open(QIODevice::ReadOnly)) {
            err = QStringLiteral("打不开：%1（用的是内置默认）").arg(f.errorString());
        } else {
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
                /* 整份读不出来：一个字都不覆盖。半份模版比内置更难查 */
                err = QStringLiteral("第 %1 列读不下去：%2（这一份没生效，用的是内置默认）")
                          .arg(pe.offset + 1)
                          .arg(pe.errorString());
            } else {
                applyMap(doc.object().toVariantMap(), s, err);
            }
        }
    }

    Cached c;
    c.mtime = mtime;
    c.bytes = bytes;
    c.style = s;
    c.err = err;
    cache().insert(language, c);
    return s;
}

QString loadErrorFor(const QString &language) {
    styleFor(language);  // 没缓存过就顺带算一遍
    return cache().value(language).err;
}

/*
 * 从命令里认是哪个工具。
 *
 * 扫**每一个参数**而不是只看第一段：真实用法里工具经常被别人包一层
 * （"npx prettier --stdin-filepath x.js"、"node C:/…/prettier.cjs …"、
 * "pyenv exec black"），只看程序名的话这些全被判成"认不出"，模版就一条参数都不
 * 追加了 —— 而每一段都取 QFileInfo 的 fileName() 再比，所以 "--stdin-filepath"
 * 这种带横线的长参数不会被误认成工具名。
 */
Engine engineKind(const QString &command) {
    struct Name { const char *needle; Engine engine; bool prefixOnly; };
    static const Name names[] = {
        {"clang-format", Engine::ClangFormat, false},
        {"prettier", Engine::Prettier, false},
        {"black", Engine::Black, true},
        {"shfmt", Engine::Shfmt, false},
        {"gofmt", Engine::Gofmt, false},
        {"rustfmt", Engine::Rustfmt, false},
        {"rubocop", Engine::Rubocop, false},
        {"php-cs-fixer", Engine::PhpCsFixer, false},
    };
    const QStringList tokens = QProcess::splitCommand(command);
    for (const QString &token : tokens) {
        const QString name = QFileInfo(token).fileName().toLower();
        if (name.isEmpty())
            continue;
        for (const Name &n : names) {
            const bool hit = n.prefixOnly ? name.startsWith(QLatin1String(n.needle))
                                          : name.contains(QLatin1String(n.needle));
            if (hit)
                return n.engine;
        }
    }
    return Engine::Other;
}

/*
 * clang-format 的内联 --style。
 *
 * 键的挑选有两处讲究，都是在这台机器上量出来的（clang-format 23.1.2）：
 *   * AllowShortIf/BlocksOnASingleLine 写 **false** 而不是新版的枚举 Never ——
 *     新版照样收 false（实测），而 10 以前的版本只认 bool，写 Never 会在老版本上
 *     整条 --style 解析失败（unknown enumerated scalar，实测退出码 1）；
 *   * 不写 SpacesInParentheses / KeepBlankLines 这些**新版已经删掉**的键
 *     （空行只由 MaxEmptyLinesToKeep 管），也不写 BraceWrapping 里那个
 *     AfterControlStatement —— 它从 bool 变成了 enum，新旧两版不可能都吃同一个写法。
 *
 * 控制语句大括号要换行时只能用 BreakBeforeBraces: Allman，它会连带把声明的
 * 大括号也换行 —— 这点在界面那行小字里如实写出来，不假装是精确映射。
 */
QString clangFormatStyle(const Style &style) {
    QStringList kv;
    kv << QStringLiteral("BasedOnStyle: LLVM")
       << QStringLiteral("IndentWidth: %1").arg(style.indentSize)
       << QStringLiteral("TabWidth: %1").arg(style.tabSize)
       << QStringLiteral("ContinuationIndentWidth: %1").arg(style.continuationIndent)
       << QStringLiteral("UseTab: %1").arg(style.useTabs ? QStringLiteral("ForIndentation")
                                                         : QStringLiteral("Never"))
       << QStringLiteral("ColumnLimit: %1").arg(style.rightMargin)
       << QStringLiteral("IndentCaseLabels: %1").arg(
              style.indentCaseFromSwitch ? QStringLiteral("true") : QStringLiteral("false"))
       << QStringLiteral("MaxEmptyLinesToKeep: %1").arg(style.keepBlankLines)
       << QStringLiteral("SpaceBeforeParens: ControlStatements");

    if (!style.keepSimpleStatementsOnOneLine) {
        kv << QStringLiteral("AllowShortIfStatementsOnASingleLine: false")
           << QStringLiteral("AllowShortBlocksOnASingleLine: false")
           << QStringLiteral("AllowShortFunctionsOnASingleLine: None");
    }

    if (style.declarationBraceOnNextLine) {
        kv << QStringLiteral("BreakBeforeBraces: Custom")
           << QStringLiteral("BraceWrapping: {AfterClass: true, AfterEnum: true, "
                             "AfterFunction: true, AfterNamespace: true, "
                             "AfterStruct: true, AfterUnion: true}");
    } else if (style.controlBraceOnNextLine) {
        kv << QStringLiteral("BreakBeforeBraces: Allman");
    }

    return QStringLiteral("{%1}").arg(kv.join(QStringLiteral(", ")));
}

QStringList styleArgs(const Style &style, Engine engine) {
    switch (engine) {
    case Engine::ClangFormat:
        return {QStringLiteral("--style=") + clangFormatStyle(style)};
    case Engine::Prettier: {
        QStringList out{QStringLiteral("--tab-width"), QString::number(style.indentSize),
                        QStringLiteral("--print-width"), QString::number(style.rightMargin)};
        if (style.useTabs)
            out << QStringLiteral("--use-tabs");
        return out;
    }
    case Engine::Black:
        return {QStringLiteral("--line-length"), QString::number(style.rightMargin)};
    case Engine::Shfmt:
        // shfmt 的 -i 就是"一级缩进几格"，和模版的 indentSize 同一个意思；
        // IDEA Shell 默认 2 = shfmt 自己的默认，所以这一条多半是空跑，
        // 只有用户改了模版才真的改变输出。
        return {QStringLiteral("-i"), QString::number(style.indentSize)};
    case Engine::Rustfmt: {
        /*
         * rustfmt 是有开关的（`--config`）：`tab_spaces` 就是一级缩进、`max_width`
         * 就是行宽、`hard_tabs` 决定行首用不用 Tab。之前这一档留空、界面上还写着
         * "没有风格开关" —— 那句是错的（它默认 4 空格/100 列，我们模版是 4/120，
         * 行宽那一项本来就该传）。
         */
        QStringList kv;
        if (style.useTabs)
            kv << QStringLiteral("hard_tabs=true")
               << QStringLiteral("tab_spaces=%1").arg(style.tabSize);
        else
            kv << QStringLiteral("hard_tabs=false")
               << QStringLiteral("tab_spaces=%1").arg(style.indentSize);
        if (style.rightMargin > 0)
            kv << QStringLiteral("max_width=%1").arg(style.rightMargin);
        return {QStringLiteral("--config"), kv.join(QLatin1Char(','))};
    }
    case Engine::Rubocop:
    case Engine::PhpCsFixer:
        /* 这两个不吃命令行参数：rubocop 读 .rubocop.yml（见 engineConfigFile），
         * php-cs-fixer 的缩进被 PSR-12 钉死在 4 空格，模版没有它能吃的项。 */
        return {};
    case Engine::Gofmt:
    case Engine::Builtin:
    case Engine::None:
    case Engine::Other:
        break;
    }
    return {};
}

/*
 * 有些工具的风格只能给**配置文件**，命令行塞不进去 —— rubocop 就是这种。
 * 返回 (文件名, 内容)，没有就给空。文件写到那份临时文件旁边，rubocop 从被格式化
 * 的文件的目录往上找配置，正好找到它。
 *
 * 这四个 cop 的名字和取值是从 rubocop 自带的 config/default.yml 里查的
 * （Layout/IndentationWidth 的 Width、Layout/IndentationStyle 的 EnforcedStyle、
 *  Layout/LineLength 的 Max、Layout/TrailingWhitespace），不是凭印象写的。
 */
QPair<QString, QString> engineConfigFile(const Style &style, Engine engine) {
    if (engine != Engine::Rubocop)
        return {};

    QString yml = QStringLiteral(
        "# SmartClip 的格式模版生成的（styles/ruby.json），改那份文件再格式化一次就会重写\n"
        "AllCops:\n  NewCops: disable\n"
        "Layout/IndentationWidth:\n  Enabled: true\n  Width: %1\n"
        "Layout/IndentationStyle:\n  Enabled: true\n  EnforcedStyle: %2\n"
        "Layout/LineLength:\n  Enabled: true\n  Max: %3\n"
        "Layout/TrailingWhitespace:\n  Enabled: true\n")
                      .arg(style.indentSize)
                      .arg(style.useTabs ? QStringLiteral("tabs") : QStringLiteral("spaces"))
                      .arg(style.rightMargin > 0 ? style.rightMargin : 1000000);
    return {QStringLiteral(".rubocop.yml"), yml};
}

/* 这个引擎能吃掉模版的哪几项 */
QString honoredText(Engine engine) {
    switch (engine) {
    case Engine::ClangFormat:
        return QStringLiteral("clang-format：全项落地");
    case Engine::Prettier:
        return QStringLiteral("prettier：只有缩进 / 行宽");
    case Engine::Black:
        return QStringLiteral("black：只有行宽");
    case Engine::Shfmt:
        return QStringLiteral("shfmt：只有缩进");
    case Engine::Gofmt:
        return QStringLiteral("gofmt 没有风格开关，模版改不了它");
    case Engine::Rustfmt:
        return QStringLiteral("rustfmt：吃缩进 / Tab / 行宽");
    case Engine::Rubocop:
        return QStringLiteral("rubocop：吃缩进 / Tab / 行宽（写 .rubocop.yml）");
    case Engine::PhpCsFixer:
        return QStringLiteral("php-cs-fixer：按它自己的 @PSR12（4 空格、函数大括号换行），"
                              "模版里只有缩进对得上");
    case Engine::Builtin:
        return QStringLiteral("内置那三样只吃缩进 / 空行 / 末尾换行");
    case Engine::None:
        return QStringLiteral("本机没有可用的工具，模版还没落地");
    case Engine::Other:
        return QStringLiteral("认不出这个命令，不追加参数");
    }
    return QString();
}

QString summaryText(const Style &style, Engine engine, const QString &language) {
    /*
     * 大括号这两项谁能摆：clang-format 认得出大括号的语言（C / C++ / C# / Java）。
     * prettier 压根没有大括号位置的开关，内置 XML / JSON 也没有 —— 挂在别的行上
     * 就成了一行没人能兑现的数字。
     */
    const bool bracesShown = engine == Engine::ClangFormat
        && (language == QLatin1String("cpp") || language == QLatin1String("csharp")
            || language == QLatin1String("java"));

    QStringList parts;
    parts << QStringLiteral("缩进 %1%2")
                 .arg(style.indentSize)
                 .arg(style.useTabs ? QStringLiteral("(Tab)") : QStringLiteral("空格"));
    parts << QStringLiteral("续行 %1").arg(style.continuationIndent);
    parts << (style.rightMargin == 0 ? QStringLiteral("行宽不限")
                                     : QStringLiteral("行宽 %1").arg(style.rightMargin));
    parts << QStringLiteral("空行≤%1").arg(style.keepBlankLines);
    if (bracesShown) {
        parts << (style.declarationBraceOnNextLine
                      ? QStringLiteral("声明大括号换行")
                      : (style.controlBraceOnNextLine ? QStringLiteral("大括号全换行")
                                                     : QStringLiteral("大括号同行")));
        parts << (style.indentCaseFromSwitch ? QStringLiteral("case 缩进")
                                            : QStringLiteral("case 不缩进"));
    }
    return QStringLiteral("%1 ｜ %2").arg(parts.join(QStringLiteral(" · ")),
                                          honoredText(engine));
}

QVariantMap toMap(const Style &style) { return styleToMap(style); }

}  // namespace fmtstyle
