#include "Checker.h"

#include "Translate.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QVariantMap>

#include <algorithm>

namespace {

/*
 * 每种语言的"符号环境"：字符串引号 + 注释起止。
 *
 * 为什么要这份表：本地规则里最容易出假阳性的就是"括号不配对"—— 而
 * `QString s = "(";` 这种（括号在字符串里）和 `// TODO: 补上 )` 这种
 * （在注释里）都是**完全合法**的代码。不把这两块剔掉，一个正常文件能被报出
 * 十几条假问题，用户看一次就再也不信这个功能了。
 *
 *   quotes      成对的字符串定界符（同进同出）
 *   lineCmt     行注释前缀
 *   blockStart  块注释开头
 *   blockEnd    块注释结尾
 *   escapes     反斜杠是否转义下一个字符（C 族、Python 是；批处理不是）
 */
struct SyntaxProfile {
    const char *language;
    QStringList quotes;
    QString lineComment;
    QString blockStart;
    QString blockEnd;
    bool escapes = true;
};

QList<SyntaxProfile> syntaxProfiles() {
    static const QList<SyntaxProfile> table = {
        {"cpp", {"\"", "'"}, "//", "/*", "*/", true},
        {"csharp", {"\"", "'"}, "//", "/*", "*/", true},
        {"java", {"\"", "'"}, "//", "/*", "*/", true},
        {"javascript", {"\"", "'", "`"}, "//", "/*", "*/", true},
        {"css", {"\"", "'"}, QString(), "/*", "*/", true},
        {"php", {"\"", "'"}, "//", "/*", "*/", true},
        {"go", {"\"", "'", "`"}, "//", "/*", "*/", true},
        {"rust", {"\"", "'"}, "//", "/*", "*/", true},
        {"python", {"\"", "'"}, "#", QString(), QString(), true},
        {"ruby", {"\"", "'"}, "#", QString(), QString(), true},
        {"perl", {"\"", "'"}, "#", QString(), QString(), true},
        {"bash", {"\"", "'"}, "#", QString(), QString(), true},
        {"yaml", {"\"", "'"}, "#", QString(), QString(), true},
        {"properties", {"\"", "'"}, "#", QString(), QString(), true},
        {"cmake", {"\"", "'"}, "#", "#[[", "]]", true},
        {"batch", {"\""}, "rem ", QString(), QString(), false},
        {"lua", {"\"", "'", "[[", "]]"}, "--", "--[[", "]]", true},
        {"sql", {"\"", "'"}, "--", "/*", "*/", false},
        {"verilog", {"\"", "'"}, "//", "/*", "*/", true},
        {"vhdl", {"\"", "'"}, "--", QString(), QString(), false},
        {"tex", {"\"", "'"}, "%", QString(), QString(), true},
        {"pascal", {"'"}, "//", "{", "}", false},
        {"plain", {}, QString(), QString(), QString(), false},
        {"markdown", {}, QString(), QString(), QString(), false},
        {"html", {}, QString(), "<!--", "-->", false},
    };
    return table;
}

const SyntaxProfile *profileFor(const QString &language) {
    static const QList<SyntaxProfile> table = syntaxProfiles();
    for (const SyntaxProfile &p : table) {
        if (language == QLatin1String(p.language))
            return &p;
    }
    return nullptr;
}

/*
 * 掩码：把"字符串里 / 注释里"的字符全部换成空格。
 *
 * 后面所有的括号配对、标点检查都在掩码文本上做 —— 位置一一对应（长度不变），
 * 但代码里的 `"("` 和注释里的 `)` 都不会再被当成语法符号。
 * 换行**保留**：行号不能乱。
 */
QString maskNonCode(const QString &text, const QString &language) {
    const SyntaxProfile *p = profileFor(language);
    if (!p)
        return text;

    QString out = text;   /* 先复制一份，再往里抹 */
    const int n = text.size();
    int i = 0;
    bool inBlock = false;
    QString activeQuote;   /* 非空 = 正在某个字符串里 */

    while (i < n) {
        const QChar c = text.at(i);

        if (c == QLatin1Char('\n')) {
            /* 行注释到行尾就结束；字符串跨行是不合法的（除了三引号那类，这里不追） */
            out[i] = QLatin1Char('\n');
            ++i;
            continue;
        }

        if (inBlock) {
            if (!p->blockEnd.isEmpty() && text.mid(i, p->blockEnd.size()) == p->blockEnd) {
                for (int k = 0; k < p->blockEnd.size() && i < n; ++k, ++i)
                    out[i] = QLatin1Char(' ');
                inBlock = false;
                continue;
            }
            out[i] = QLatin1Char(' ');
            ++i;
            continue;
        }

        if (!activeQuote.isEmpty()) {
            /* 字符串里：找同款定界符收尾（\ 转义看这门语言认不认） */
            if (p->escapes && c == QLatin1Char('\\') && i + 1 < n) {
                out[i] = QLatin1Char(' ');
                out[i + 1] = QLatin1Char(' ');
                i += 2;
                continue;
            }
            if (text.mid(i, activeQuote.size()) == activeQuote) {
                for (int k = 0; k < activeQuote.size() && i < n; ++k, ++i)
                    out[i] = QLatin1Char(' ');
                activeQuote.clear();
                continue;
            }
            out[i] = QLatin1Char(' ');
            ++i;
            continue;
        }

        /* 不在字符串 / 注释里：看这里是不是某个块的开始 */
        if (!p->blockStart.isEmpty() && text.mid(i, p->blockStart.size()) == p->blockStart) {
            for (int k = 0; k < p->blockStart.size() && i < n; ++k, ++i)
                out[i] = QLatin1Char(' ');
            inBlock = true;
            continue;
        }
        if (!p->lineComment.isEmpty()
            && text.mid(i, p->lineComment.size()) == p->lineComment) {
            /*
            * 行注释：抹到行尾。**不抹掉 \n**（上面的分支会把它留着）——
            * 行号全靠这些换行。
            */
            while (i < n && text.at(i) != QLatin1Char('\n')) {
                out[i] = QLatin1Char(' ');
                ++i;
            }
            continue;
        }

        for (const QString &q : p->quotes) {
            if (text.mid(i, q.size()) == q) {
                for (int k = 0; k < q.size() && i < n; ++k, ++i)
                    out[i] = QLatin1Char(' ');
                activeQuote = q;
                break;
            }
        }
        if (!activeQuote.isEmpty())
            continue;

        ++i;
    }
    return out;
}

/* 一行里第 col 个字符是哪个字符（越界给空），以及它的行 / 列 */
struct Pos {
    int row = 1;
    int col = 0;
};

/*
 * 半角标点 -> 对应的全角（提示语里写"该用全角「，」"用）。
 *
 * 中文正文里出现半角逗号句号是最常见的一类写法问题，报的时候直接把该用的
 * 那个字符写出来，用户不用自己去想全角是哪个。
 */
QString fullWidthOf(const QString &half) {
    static const QHash<QString, QString> table = {
        {QStringLiteral(","), QStringLiteral("，")}, {QStringLiteral("."), QStringLiteral("。")},
        {QStringLiteral(";"), QStringLiteral("；")}, {QStringLiteral(":"), QStringLiteral("：")},
        {QStringLiteral("!"), QStringLiteral("！")}, {QStringLiteral("?"), QStringLiteral("？")},
        {QStringLiteral("("), QStringLiteral("（")}, {QStringLiteral(")"), QStringLiteral("）")},
    };
    return table.value(half, half);
}

/* 文本按 \n 切行；\r 一并去掉（编辑器里那份正文两种换行都可能出现） */
QStringList splitLines(const QString &text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &l : lines) {
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
    }
    return lines;
}

/*
 * "3 条问题" / "未发现问题" —— 报条数的地方都走它。
 *
 * 0 条**不能**写成"0 条问题"（用户报过状态栏那句"只发现本地那 0 条问题"，
 * 读着像话没说完）；这个函数是唯一一处写这个话术的地方。
 */
QString issueCountText(int count) {
    return count == 0 ? QStringLiteral("未发现问题")
                      : QStringLiteral("%1 条问题").arg(count);
}

/* 同一件事的前半句："本地发现 3 条问题，…"；0 条时说"本地没问题" */
QString localCountPrefix(int count) {
    return count == 0 ? QStringLiteral("本地没问题，")
                      : QStringLiteral("本地发现 %1 条问题，").arg(count);
}

QVariantMap makeIssue(int row, int col, int endRow, int endCol, const QString &severity,
                      const QString &kind, const QString &message, const QString &snippet,
                      const QString &suggestion = QString()) {
    QVariantMap m;
    m.insert(QStringLiteral("row"), row);
    m.insert(QStringLiteral("col"), col);
    m.insert(QStringLiteral("endRow"), endRow);
    m.insert(QStringLiteral("endCol"), endCol);
    m.insert(QStringLiteral("severity"), severity);
    m.insert(QStringLiteral("kind"), kind);
    m.insert(QStringLiteral("message"), message);
    m.insert(QStringLiteral("snippet"), snippet);
    m.insert(QStringLiteral("source"), QStringLiteral("local"));
    m.insert(QStringLiteral("suggestion"), suggestion);
    return m;
}

/*
 * 括号配对。
 *
 * 在**掩码文本**上做（字符串和注释里的括号已经被抹掉了，见 maskNonCode）。
 * 位置（行 / 列）是在掩码文本上数的 —— 掩码不改变长度也不删换行，
 * 所以和原文一一对应。
 */
QList<QVariantMap> checkBrackets(const QString &masked, const QStringList &lines) {
    QList<QVariantMap> out;
    struct Open {
        QChar ch;
        int row;
        int col;
    };
    QList<Open> stack;

    int row = 1;
    int col = 0;
    for (int i = 0; i < masked.size(); ++i) {
        const QChar c = masked.at(i);
        if (c == QLatin1Char('\n')) {
            ++row;
            col = 0;
            continue;
        }
        const int thisCol = col++;

        if (c == QLatin1Char('(') || c == QLatin1Char('[') || c == QLatin1Char('{')) {
            stack.append({c, row, thisCol});
            continue;
        }
        if (c != QLatin1Char(')') && c != QLatin1Char(']') && c != QLatin1Char('}'))
            continue;

        const QChar want = c == QLatin1Char(')') ? QLatin1Char('(')
                          : c == QLatin1Char(']') ? QLatin1Char('[')
                                                  : QLatin1Char('{');
        if (stack.isEmpty()) {
            out.append(makeIssue(row, thisCol, row, thisCol + 1, QStringLiteral("error"),
                                 QStringLiteral("bracket"),
                                 QStringLiteral("多出来一个 %1，前面没有与之配对的 %2")
                                     .arg(c, want),
                                 thisCol < lines.value(row - 1).size()
                                     ? QString(lines.value(row - 1).at(thisCol))
                                     : QString()));
            continue;
        }
        if (stack.last().ch != want) {
            /* 类型不匹配：把栈顶那个也报出来，用户才知道该改哪边 */
            const Open open = stack.takeLast();
            out.append(makeIssue(open.row, open.col, open.row, open.col + 1,
                                 QStringLiteral("error"), QStringLiteral("bracket"),
                                 QStringLiteral("这里的 %1 和下面第 %2 行的 %3 配不上（中间还有没关掉的）")
                                     .arg(open.ch)
                                     .arg(row)
                                     .arg(c),
                                 QString()));
            continue;
        }
        stack.removeLast();
    }

    /* 收尾还剩在栈里的：没关掉的 */
    for (const Open &open : stack) {
        out.append(makeIssue(open.row, open.col, open.row, open.col + 1,
                             QStringLiteral("error"), QStringLiteral("bracket"),
                             QStringLiteral("%1 没有配对的 %2")
                                 .arg(open.ch)
                                 .arg(open.ch == QLatin1Char('(') ? QStringLiteral(")")
                                      : open.ch == QLatin1Char('[') ? QStringLiteral("]")
                                                                    : QStringLiteral("}")),
                             QString()));
    }
    return out;
}

/*
 * 中文那几条：标点混用 / 重复字 / 的地得 / 叠问号 / 行尾空白。
 *
 * 只对**含中文的行**做标点检查（一句纯英文里出现 `,` 是正确的，不是问题）。
 */
bool hasCjk(const QString &s) {
    static const QRegularExpression kCjk(QStringLiteral("[\\x{4e00}-\\x{9fff}]"));
    return kCjk.match(s).hasMatch();
}

QList<QVariantMap> checkChinese(const QStringList &lines, const QString &language) {
    QList<QVariantMap> out;
    const bool prose = language == QLatin1String("markdown") || language == QLatin1String("plain")
                       || language == QLatin1String("tex");

    for (int r = 0; r < lines.size(); ++r) {
        const QString &line = lines.at(r);
        const int row = r + 1;
        const bool cjk = hasCjk(line);

        /* 行尾空白：任何语言都算问题（很多工具链会因此报 diff） */
        if (line.endsWith(QLatin1Char(' ')) || line.endsWith(QLatin1Char('\t'))) {
            int cut = line.size();
            while (cut > 0 && (line.at(cut - 1) == QLatin1Char(' ')
                               || line.at(cut - 1) == QLatin1Char('\t')))
                --cut;
            out.append(makeIssue(row, cut, row, line.size(), QStringLiteral("info"),
                                 QStringLiteral("trailing-space"),
                                 QStringLiteral("行尾有多余的空白"),
                                 line.mid(cut)));
        }

        if (!cjk)
            continue;

        /* 中英标点混用：中文句子里出现半角 , . ; : ! ? ( ) */
        static const QRegularExpression kHalfPunct(
            QStringLiteral("([\\x{4e00}-\\x{9fff}])([,.;:!?])"));
        auto it = kHalfPunct.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int col = m.capturedStart(2);
            out.append(makeIssue(row, col, row, col + 1, QStringLiteral("warn"),
                                 QStringLiteral("punct"),
                                 QStringLiteral("中文句子里用了半角「%1」，一般该用全角「%2」")
                                     .arg(m.captured(2), fullWidthOf(m.captured(2))),
                                 m.captured(1) + m.captured(2),
                                 fullWidthOf(m.captured(2))));
        }

        /*
         * 重复字：同一个汉字连着来两遍。
         *
         * 叠字是正常写法（看看 / 试试 / 慢慢 / 人人 / 高高…），所以按**白名单**
         * 放行常见的那一批 —— 反过来"只报某些字"会漏掉太多。
         */
        static const QRegularExpression kRepeat(
            QStringLiteral("([\\x{4e00}-\\x{9fff}])\\1"));
        it = kRepeat.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString ch = m.captured(1);
            /*
             * 叠字是正常写法（看看 / 试试 / 慢慢 / 人人…），按白名单放行常见那批 ——
             * 反过来"只报某些字"会漏掉太多。
             *
             * 注意**的 / 了 / 是 不在白名单里**：它们连着出现几乎总是多打了一个
             * （"我的的书"），是最该报出来的一类。
             */
            if (QStringLiteral("人天年日个次点些多少大小高长常慢快轻仔试试看看想想说说走走坐坐歇歇")
                    .contains(ch))
                continue;
            const int col = m.capturedStart();
            out.append(makeIssue(row, col, row, col + 2,
                                 /*
                                  * 这里必须是 QStringLiteral：写成
                                  * QLatin1String("的") 会把 UTF-8 的三个字节当成
                                  * 三个 Latin-1 字符，和单个汉字的 QString 永远
                                  * 不相等 —— 于是这条分支静默地一直是 warn，
                                  * "我的的书"那种最该报 error 的也只画条黄线。
                                  */
                                 (ch == QStringLiteral("的") || ch == QStringLiteral("了"))
                                     ? QStringLiteral("error")
                                     : QStringLiteral("warn"),
                                 QStringLiteral("repeat"),
                                 QStringLiteral("「%1%1」连着重复了，是不是多打了一个？").arg(ch),
                                 m.captured()));
        }

        /*
         * 的地得。
         *
         * 这两条是**误报率最低**的写法错误（这两条之外的情况都太依赖语义，
         * 本地规则说不准，留给大模型那条路）：
         *   动词 + 的 + 很 / 不 / 太   —— "做的很好" 该是 "做得很好"
         *   得 + 名词                  —— 太粗，不做
         * 只报第一条。
         */
        static const QRegularExpression kDeVerb(
            QStringLiteral("([\\x{4e00}-\\x{9fff}])的(很|非常|太|不)"));
        it = kDeVerb.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int col = m.capturedStart(1) + 1;
            out.append(makeIssue(row, col, row, col + 1, QStringLiteral("warn"),
                                 QStringLiteral("de"),
                                 QStringLiteral("「%1的%2」里的「的」多半该写成「得」（补充说明动作的程度用「得」）")
                                     .arg(m.captured(1), m.captured(2)),
                                 QStringLiteral("的"), QStringLiteral("得")));
        }

        /* 「吗」的误用：吗 + 名词（"吗上"这种），只认最高频的那个组合 */
        static const QRegularExpression kMaWord(
            QStringLiteral("(吗)([\\x{4e00}-\\x{9fff}]{1,2})"));
        it = kMaWord.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            /* "吗啡"是药名，放行 */
            if (m.captured(2).startsWith(QStringLiteral("啡")))
                continue;
            /* 句子结尾的"吗"后面不该接字 —— 接了多半是想写"嘛"或写错了 */
            const int col = m.capturedStart(1);
            out.append(makeIssue(row, col, row, col + 1, QStringLiteral("info"),
                                 QStringLiteral("ma"),
                                 QStringLiteral("「吗%1」看着不通顺：是想写「嘛%1」，还是少了标点？")
                                     .arg(m.captured(2)),
                                 m.captured(1)));
        }

        /* 叠标点：?? / ！！ / 。。 这类 */
        static const QRegularExpression kDoublePunct(
            QStringLiteral("([?？!！。])\\1"));
        it = kDoublePunct.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const int col = m.capturedStart();
            /* ？？ 和 ！！ 是常见的强调写法，只报 "。。" */
            if (m.captured(1) != QStringLiteral("。"))
                continue;
            out.append(makeIssue(row, col, row, col + 2, QStringLiteral("warn"),
                                 QStringLiteral("double-punct"),
                                 QStringLiteral("连着两个句号"), m.captured(), QStringLiteral("。")));
        }

        /* 代码语言里不查这些散文规则 */
        Q_UNUSED(prose)
    }
    return out;
}

}  // namespace

/* ------------------------------------------------------------------ */

Checker::Checker(LlmClient *llm, QObject *parent)
    : QObject(parent), m_llm(llm) {
    if (m_llm) {
        /*
         * 模型那部分回来了。token 要对得上 —— 校验期间用户可能已经换了一份
         * 文档、或者又点了一次校验，旧结果必须丢掉（否则问题列表里会出现
         * 别的文件的行号）。
         */
        connect(m_llm, &LlmClient::finished, this,
                [this](const QString &token, const QString &text) {
                    if (token != m_token || m_token.isEmpty())
                        return;
                    m_token.clear();
                    const QList<QVariantMap> model = parseReply(text, m_text);
                    for (const QVariantMap &m : model)
                        m_local.append(m);
                    m_issues.clear();
                    for (const QVariantMap &m : m_local)
                        m_issues.append(m);
                    emit issuesChanged();
                    setBusy(false);
                    /*
                     * 一句话说清结论。
                     *
                     * 0 条**不能**写成"只发现本地那 0 条问题"（用户报过：读着像没说完）
                     * —— 那就是"未发现问题"。条数的话术统一走 issueCountText。
                     */
                    const int total = m_local.size() + model.size();
                    if (total == 0)
                        setStatus(QStringLiteral("未发现问题"));
                    else if (model.isEmpty())
                        setStatus(QStringLiteral("校验完成：%1（都是本地规则报的）")
                                      .arg(issueCountText(total)));
                    else
                        setStatus(QStringLiteral("校验完成：共 %1 条问题（其中模型补了 %2 条）")
                                      .arg(total)
                                      .arg(model.size()));
                    emit checked(model.size());
                });
        connect(m_llm, &LlmClient::failed, this,
                [this](const QString &token, const QString &error) {
                    if (token != m_token || m_token.isEmpty())
                        return;
                    m_token.clear();
                    m_lastError = error;
                    setBusy(false);
                    /*
                     * 模型那条路失败**不影响本地结果**：本地那几条已经列出来了，
                     * 状态里说清楚"模型那部分没跑成"，用户至少还能拿到本地结论。
                     */
                    setStatus(m_local.isEmpty()
                                  ? QStringLiteral("模型校验没跑成（%1）；本地也没发现问题")
                                        .arg(error)
                                  : QStringLiteral("模型校验没跑成（%1）；本地发现 %2 条问题")
                                        .arg(error)
                                        .arg(m_local.size()));
                    emit checked(0);
                });
        connect(m_llm, &LlmClient::settingsChanged, this,
                [this]() { emit llmStateChanged(); });
        /*
         * 本地服务起 / 停也要发一次：那句"（已启动）/（没启动）"是 modelSummary
         * 拼出来的，界面得跟着刷新 —— 不然用户在「模型」那一栏点了启动，
         * 切回「校验」看到的还是"没启动"。
         */
        connect(m_llm, &LlmClient::localRunningChanged, this,
                [this]() { emit llmStateChanged(); });
        /*
         * 等模型那几秒到几十秒要看得见。
         *
         * 本地模型冷启动是"起 llama-server + 加载 gguf"一整套，几十秒很正常；
         * 这期间校验自己那句话一直写着"正在问模型…"，看着就像卡死了（用户报过）。
         * Llm 那边其实一直在报进度（正在启动本地推理服务… / 模型加载中…），
         * 把它接过来，前面挂上本地已经发现几条。
         */
        connect(m_llm, &LlmClient::statusChanged, this, [this]() {
            if (!m_busy || m_token.isEmpty())
                return;
            const QString progress = m_llm->status();
            if (!progress.isEmpty())
                setStatus(localCountPrefix(m_local.size()) + progress);
        });
    }
}

bool Checker::llmReady() const {
    if (!m_llm)
        return false;
    /*
     * 两种模式要的东西不一样，**别混着看**：
     *
     *   本地模式：要的是"程序 + 模型文件"（接口地址是 127.0.0.1:端口，永远非空，
     *             拿它当判据等于永远"就绪" —— 原来就是这样，用户切到本地模型
     *             之后界面上照样报接口那个模型名）；
     *   接口模式：要的是"接口地址 + 模型名"（和 LlmClient::ask 的判据一致）。
     *
     * 注意这里判的是**配好了没有**，不是"服务起没起来"：本地那个没起来时
     * LlmClient::ask 会自己把它拉起来（见那里"配好一次，以后不用手点启动"），
     * 所以起没起来只影响界面上那句提示，不影响能不能走模型。
     */
    if (m_llm->mode() == QLatin1String("local"))
        return !m_llm->localExe().trimmed().isEmpty()
               && !m_llm->localModel().trimmed().isEmpty();
    return !m_llm->apiBase().trimmed().isEmpty()
           && !m_llm->model().trimmed().isEmpty();
}

/*
 * 「校验」那一栏顶上那行状态："现在用的是哪个模型"。
 *
 * 为什么这行在 C++ 拼、而不是 QML 里拼：判据（llmReady / wantsModel）就在这个
 * 类里，"哪句话配哪个状态"放一起才不会打架。以前 QML 里写的是
 * "模型就绪：" + Llm.model —— Llm.model 是**接口模式**的模型名，出厂默认
 * "deepseek-chat"：用户切到本地模型（自己的 gguf）之后，这行照样报
 * deepseek-chat，本地那个起没起来也不说，等于凭空写死了一个没在用的模型。
 */
QString Checker::modelSummary() const {
    if (!m_llm)
        return QString();

    if (m_llm->mode() == QLatin1String("local")) {
        if (!llmReady())
            return QStringLiteral("本地模型还没配好（见左边「模型」那一栏）"
                                  "—— 现在只会跑本地规则。");
        /* 只显示文件名：模型那一栏里那条路径整条太长，这里只是"用的是哪个" */
        const QString path = m_llm->localModel().trimmed();
        const int cut = qMax(path.lastIndexOf(QLatin1Char('/')),
                             path.lastIndexOf(QLatin1Char('\\')));
        const QString name = cut >= 0 ? path.mid(cut + 1) : path;
        return QStringLiteral("模型：本地 · %1%2")
            .arg(name,
                 m_llm->localRunning()
                     ? QStringLiteral("（已启动）")
                     : QStringLiteral("（没启动 —— 跑校验时会自动起来，加载要几秒到几十秒）"));
    }

    if (!llmReady())
        return QStringLiteral("接口模型还没配好（见左边「模型」那一栏）"
                              "—— 现在只会跑本地规则。");
    return QStringLiteral("模型：接口 · %1（%2）")
        .arg(m_llm->model().trimmed(), m_llm->apiBase().trimmed());
}

void Checker::setBusy(bool on) {
    if (m_busy == on)
        return;
    m_busy = on;
    emit busyChanged();
}

void Checker::setStatus(const QString &text) {
    if (m_status == text)
        return;
    m_status = text;
    emit statusChanged();
}

void Checker::clear() {
    /*
     * 清空时把 token 也扔掉：正在等的那条模型回复回来时就对不上了，
     * 于是被上面的 lambda 直接忽略 —— 这就是"过期结果不会污染新结果"。
     */
    m_token.clear();
    m_issues.clear();
    m_local.clear();
    m_text.clear();
    m_lastError.clear();
    setBusy(false);
    setStatus(QString());
    emit issuesChanged();
}

QString Checker::resultSummary() const {
    if (m_issues.isEmpty())
        return m_status.isEmpty() ? QStringLiteral("未发现问题") : m_status;
    return QStringLiteral("发现 %1 条问题").arg(m_issues.size());
}

/*
 * 走不走模型那条路。
 *
 * 两个条件：接口配好了、正文**不太长**。
 * 最后那条：校验要给行号，正文太长（几万字）时模型数行号会数错，
 * 而且超了上下文还得截断 —— 截断之后行号就彻底对不上了。
 * 所以超过 kMaxModelChars 就只跑本地规则，并在状态里说明。
 */
bool Checker::wantsModel(const QString &text) const {
    static constexpr int kMaxModelChars = 12000;
    return llmReady() && text.size() <= kMaxModelChars && !text.trimmed().isEmpty();
}

QString Checker::replyFormat() const {
    /*
     * 输出格式：一行一条，竖线分隔。
     *
     * 为什么不用 JSON：模型写 JSON 时经常漏引号 / 多逗号，解析失败就整份白跑；
     * 而这个格式哪怕多一行说明文字，我也能只挑出"看起来像记录"的行。
     * 最后一列（原文片段）是**定位和核实**用的 —— 见 parseReply。
     */
    return QStringLiteral("行号|列号|级别|类型|说明|原文片段");
}

QString Checker::promptFor(const QString &language) const {
    const bool code = language != QLatin1String("markdown")
                      && language != QLatin1String("plain")
                      && language != QLatin1String("tex");
    if (!code) {
        return QStringLiteral(
                   "你是一名中文校对员。逐字检查用户给出的中文文本里的**用词和写法错误**，"
                   "只报真正的错误：错别字、用词不当、的地得用错、搭配不当、重复啰嗦、"
                   "标点明显不对。不要报写作风格、不要改写句子、不要提出润色建议 —— "
                   "没发现问题就什么都不输出。\n"
                   "输出格式（每行一条，竖线分隔，不要编号、不要表格、不要解释）：\n"
                   "%1\n"
                   "其中「原文片段」必须是**你报的那一处**在原文里逐字出现的一小段"
                   "（4 到 12 个字），用来核对位置，不要改写它。")
            .arg(replyFormat());
    }
    return QStringLiteral(
               "你是一名代码审查员。检查用户给出的代码里的**语法错误和明显写错的符号**："
               "括号引号不配对、语句没结束、关键字拼错、结构不完整、缩进导致的作用域错误。"
               "只报确定的问题；风格、命名、性能这些一律不报；拿不准就不要报。\n"
               "输出格式（每行一条，竖线分隔，不要编号、不要代码块、不要解释）：\n"
               "%1\n"
               "其中「原文片段」必须是**你报的那一处**在原文里逐字出现的一小段"
               "（4 到 20 个字符），用来核对位置，不要改写它。")
        .arg(replyFormat());
}

/* ------------------------------------------------------------------ */
/* 分析                                                                */
/* ------------------------------------------------------------------ */

QList<QVariantMap> Checker::analyzeLocal(const QString &text, const QString &language) const {
    const QStringList lines = splitLines(text);
    const QString masked = maskNonCode(text, language);

    QList<QVariantMap> out = checkBrackets(masked, lines);
    out += checkChinese(lines, language);

    /* 按行、列排一遍：卡片里显示的顺序要和正文一致 */
    std::stable_sort(out.begin(), out.end(), [](const QVariantMap &a, const QVariantMap &b) {
        const int ar = a.value(QStringLiteral("row")).toInt();
        const int br = b.value(QStringLiteral("row")).toInt();
        if (ar != br)
            return ar < br;
        return a.value(QStringLiteral("col")).toInt() < b.value(QStringLiteral("col")).toInt();
    });
    return out;
}

int Checker::checkLocalOnly(const QString &text, const QString &language) {
    m_token.clear();
    m_text = text;
    m_language = language;
    m_lastError.clear();
    m_local = analyzeLocal(text, language);
    m_issues.clear();
    for (const QVariantMap &m : m_local)
        m_issues.append(m);
    setBusy(false);
    setStatus(QStringLiteral("本地规则：") + issueCountText(m_local.size()));
    emit issuesChanged();
    return m_local.size();
}

int Checker::check(const QString &text, const QString &language, const QString &filePath) {
    Q_UNUSED(filePath)

    const int localCount = checkLocalOnly(text, language);

    if (!llmReady()) {
        setStatus(QStringLiteral("还没配好大模型（设置 → 模型），只跑了本地规则：")
                  + issueCountText(localCount));
        return localCount;
    }
    if (!wantsModel(text)) {
        setStatus(QStringLiteral("正文太长了，只跑了本地规则：") + issueCountText(localCount));
        return localCount;
    }

    setBusy(true);
    setStatus(localCountPrefix(localCount) + QStringLiteral("正在问模型…"));
    m_lastError.clear();
    /*
     * 只发正文本身：**不发文件名、不发路径**。校验是内容层面的事，
     * 用户机器上的目录结构没必要交给远端服务。
     */
    m_token = m_llm->ask(promptFor(language), text, QStringLiteral("正在校验…"));
    return localCount;
}

/* ------------------------------------------------------------------ */
/* 解析模型回复                                                        */
/* ------------------------------------------------------------------ */

namespace {

/* 去掉首尾的空白和一对包裹引号（模型很爱给片段加引号） */
QString unquote(QString s) {
    s = s.trimmed();
    if (s.size() >= 2) {
        const QChar a = s.at(0);
        const QChar b = s.at(s.size() - 1);
        if ((a == QLatin1Char('"') && b == QLatin1Char('"'))
            || (a == QLatin1Char('\'') && b == QLatin1Char('\''))
            || (a == QLatin1Char('「') && b == QLatin1Char('」')))
            s = s.mid(1, s.size() - 2);
    }
    return s.trimmed();
}

}  // namespace

QList<QVariantMap> Checker::parseReply(const QString &reply, const QString &text) const {
    QList<QVariantMap> out;
    const QStringList lines = splitLines(text);
    const int lineCount = lines.size();

    const QStringList rows = splitLines(reply);
    for (const QString &raw : rows) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        /* 只认"竖线分隔、第一段是行号"的行：多出来的说明文字自动被跳过 */
        const QStringList parts = line.split(QLatin1Char('|'));
        if (parts.size() < 5)
            continue;

        bool ok = false;
        const int row = parts.at(0).trimmed().toInt(&ok);
        if (!ok || row < 1 || row > lineCount)
            continue;
        const int col = qMax(0, parts.at(1).trimmed().toInt());
        const QString severity = parts.at(2).trimmed().toLower();
        const QString kind = parts.at(3).trimmed();
        const QString message = parts.at(4).trimmed();
        const QString snippet = unquote(parts.value(5));

        if (message.isEmpty())
            continue;

        /*
         * 核实：拿它报的那一小段在**它报的那一行附近**找一遍。
         *
         * 这一步是整条链上最重要的防线 —— 模型编出来的问题是这个功能最伤的
         * 失败方式（用户点进去发现那一行根本没这回事，之后就再也不用了）。
         * 找不到就直接丢掉。所以提示词里反复强调"片段必须逐字来自原文"。
         */
        int foundRow = -1;
        int foundCol = -1;
        if (!snippet.isEmpty()) {
            bool found = false;
            for (int d = 0; d <= 2 && !found; ++d) {
                for (const int probe : {row - 1 + d, row - 1 - d}) {
                    if (probe < 0 || probe >= lineCount)
                        continue;
                    const int at = lines.at(probe).indexOf(snippet);
                    if (at >= 0) {
                        found = true;
                        foundRow = probe + 1;
                        foundCol = at;
                        break;
                    }
                }
            }
            if (!found)
                continue;
        } else {
            /* 没给片段：只认行号，列号按模型给的（可能不准，界面上仍能定位到那一行） */
            foundRow = row;
            foundCol = col;
        }

        QVariantMap m;
        m.insert(QStringLiteral("row"), foundRow);
        m.insert(QStringLiteral("col"), foundCol);
        m.insert(QStringLiteral("endRow"), foundRow);
        m.insert(QStringLiteral("endCol"),
                 snippet.isEmpty() ? foundCol : foundCol + snippet.size());
        m.insert(QStringLiteral("severity"),
                 severity == QLatin1String("error") ? QStringLiteral("error")
                                                    : QStringLiteral("warn"));
        m.insert(QStringLiteral("kind"),
                 kind.isEmpty() ? QStringLiteral("llm") : QStringLiteral("llm-") + kind);
        m.insert(QStringLiteral("message"), message);
        m.insert(QStringLiteral("snippet"), snippet);
        m.insert(QStringLiteral("source"), QStringLiteral("llm"));
        m.insert(QStringLiteral("suggestion"), QString());
        out.append(m);
    }
    return out;
}
