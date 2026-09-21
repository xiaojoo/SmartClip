#include "SelfTest.h"

#include "Checker.h"
#include "Diff.h"
#include "EditorController.h"
#include "EditorViewItem.h"
#include "Formatter.h"
#include "Translate.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QVariantList>
#include <QVariantMap>

#include <cstdio>

/*
 * 新增那几件工具的自检（`SmartClip.exe --tool-test`，见 SelfTest.h 的 runTools）。
 *
 * 和便签 / 翻译 / 识别那几份分开，理由一样：SelfTest.cpp 那几千行会开截图
 * 选区窗口、弹卡片、有它自己的时序问题；改格式化 / 对比 / 校验 / Markdown
 * 预览的时候只跑这一份就够了，而且**不显示主窗口、不弹任何窗口**。
 *
 * 这里钉的是"这些功能的**判断**对不对"，不是"界面好不好看"：
 *
 *   * Diff：插一行之后只有那一处算差异（LCS 的意义就在这）、改一行算 mod
 *     而不是"删一行加一行"、末尾换行不算多一行、统一补丁的 hunk 头对得上；
 *   * Formatter：JSON 重排真的重排且解析失败时报出字节位置、通用清理去行尾
 *     空白 / 收空行 / 末尾补换行、本机没装的工具**报名字**而不是悄悄不动、
 *     格式化后的正文**永远不是空**（最不能出的事故是"格式化把正文弄没了"）；
 *   * Checker：括号配对（含字符串 / 注释里的括号不算）、中英标点混用、
 *     重复字、的地得、行尾空白；以及"没问题时就是没问题"（不能满屏假阳性）；
 *     LLM 那条支路**不发真请求**（只验开关 / 提示词 / 解析）。
 *   * Markdown：标题 / 列表 / 表格 / 行内代码都渲染得出来，相对图片路径
 *     按文档目录改成绝对 file:// 路径（笔记里那些 `![](assets/x.png)` 全靠它）。
 *
 * 自检会动 QSettings 里 format/tool 那一组键（要试自定义命令那条路），
 * 跑完**按原样写回** —— 用户自己的配置不会被自检改掉。
 */

namespace {

int gToolPassed = 0;
int gToolFailed = 0;

void tcheck(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++gToolPassed;
        std::fputs("  ok    ", stdout);
    } else {
        ++gToolFailed;
        std::fputs("  FAIL  ", stdout);
    }
    std::fputs(what.toUtf8().constData(), stdout);
    if (!detail.isEmpty()) {
        std::fputs("   [", stdout);
        std::fputs(detail.toUtf8().constData(), stdout);
        std::fputs("]", stdout);
    }
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

void tout(const QString &line) {
    std::fputs("        ", stdout);
    std::fputs(line.toUtf8().constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

/* 某一行（0 基）的 kind（"same" / "del" / "add" / "mod"） */
QString kindAt(const QVariantList &rows, int index) {
    if (index < 0 || index >= rows.size())
        return QStringLiteral("<越界>");
    return rows.at(index).toMap().value(QStringLiteral("kind")).toString();
}

/* 把一份差异结果拼成 "same|del|add|…" 这样的短记号，断言里一眼能看 */
QString kindsOf(const QVariantList &rows) {
    QString out;
    for (const QVariant &r : rows) {
        if (!out.isEmpty())
            out += QLatin1Char('|');
        out += r.toMap().value(QStringLiteral("kind")).toString();
    }
    return out;
}

/* 变化（非 same）的行数 */
int changedCount(const QVariantList &rows) {
    int n = 0;
    for (const QVariant &r : rows) {
        if (r.toMap().value(QStringLiteral("kind")).toString() != QLatin1String("same"))
            ++n;
    }
    return n;
}

/* 某行有没有报出这个 kind 的问题 */
bool hasIssue(const QVariantList &issues, const QString &kind) {
    for (const QVariant &i : issues) {
        if (i.toMap().value(QStringLiteral("kind")).toString() == kind)
            return true;
    }
    return false;
}

/* 某一类问题的严重级（没这一类给空串） */
QString severityOf(const QVariantList &issues, const QString &kind) {
    for (const QVariant &i : issues) {
        const QVariantMap map = i.toMap();
        if (map.value(QStringLiteral("kind")).toString() == kind)
            return map.value(QStringLiteral("severity")).toString();
    }
    return QString();
}

}  // namespace

int SelfTest::runTools(Formatter *fmt, DiffEngine *differ, Checker *check, LlmClient *llm,
                       QObject *qmlRoot) {
    /* 每条检查立刻落盘：崩了也能看到崩在哪一条 */
    setvbuf(stdout, nullptr, _IONBF, 0);

    std::fputs("\n== 工具自检（对比 / 格式化 / 校验 / Markdown） ==\n", stdout);

    /*
     * 自检会写 QSettings（要试"自定义格式化命令"那条路），
     * 先把原来那份存下来，最后按原样放回去。
     */
    QSettings settings;
    const QVariant savedTool = settings.value(QStringLiteral("format/tool/plain"));

    /* ------------------------------------------------------------------
     * 1. 文件对比（src/Diff.h）
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 文件对比 --\n", stdout);
    if (!differ) {
        tcheck(false, QStringLiteral("对比引擎传进来了"));
    } else {
        /* 插一行：只有那一处是差异（这正是 LCS 存在的意义） */
        {
            const QString left = QStringLiteral("a\nb\nc\nd\n");
            const QString right = QStringLiteral("a\nb\nX\nc\nd\n");
            differ->compare(left, right, QStringLiteral("L.txt"), QStringLiteral("R.txt"));
            const QVariantList rows = differ->rows();
            tcheck(kindsOf(rows) == QStringLiteral("same|same|add|same|same"),
                  QStringLiteral("插一行：只有插进去的那一行算差异"),
                  QStringLiteral("%1（行数 %2）").arg(kindsOf(rows)).arg(rows.size()));
        }

        /* 删一行：左边多出来的报 del */
        {
            differ->compare(QStringLiteral("a\nb\nc\n"), QStringLiteral("a\nc\n"));
            const QVariantList rows = differ->rows();
            tcheck(kindsOf(rows) == QStringLiteral("same|del|same"),
                  QStringLiteral("删一行：报的是「左边多一行」"), kindsOf(rows));
        }

        /* 改一行：两边都有、内容不同 -> mod（不是"删一行 + 加一行"） */
        {
            differ->compare(QStringLiteral("a\nhello world\nc\n"),
                            QStringLiteral("a\nhello there\nc\n"));
            const QVariantList rows = differ->rows();
            tcheck(kindsOf(rows) == QStringLiteral("same|mod|same"),
                  QStringLiteral("改几个字：算「两边都改了」而不是删一行加一行"),
                  kindsOf(rows));
        }

        /* 末尾换行：一份带、一份不带，内容其实一样 */
        {
            differ->compare(QStringLiteral("a\nb\n"), QStringLiteral("a\nb"));
            QVariantList rows = differ->rows();
            tcheck(changedCount(rows) == 0,
                  QStringLiteral("末尾那个换行不算多出来的一行"),
                  QStringLiteral("%1 处差异（%2）").arg(changedCount(rows)).arg(kindsOf(rows)));
        }

        /* CRLF 和 LF 比：不算每一行都不同（\r 要去掉） */
        {
            differ->compare(QStringLiteral("a\r\nb\r\n"), QStringLiteral("a\nb\n"));
            tcheck(changedCount(differ->rows()) == 0,
                  QStringLiteral("CRLF 和 LF 比：内容一样就是没差异"));
        }

        /* 两份完全一样：说清楚，而且所有行都是 same */
        {
            differ->compare(QStringLiteral("a\nb\n"), QStringLiteral("a\nb\n"));
            const QVariantList rows = differ->rows();
            bool allSame = true;
            for (const QVariant &r : rows) {
                if (r.toMap().value(QStringLiteral("kind")).toString() != QLatin1String("same"))
                    allSame = false;
            }
            tcheck(allSame && differ->summary().contains(QStringLiteral("完全相同")),
                  QStringLiteral("一模一样的两份：结论是「完全相同」"), differ->summary());
        }

        /* 行号：左边第 2 行真的对到原文件的第 2 行 */
        {
            differ->compare(QStringLiteral("a\nb\nc\n"), QStringLiteral("a\nc\n"));
            const QVariantList rows = differ->rows();
            /* rows[1] 是那条 del：它左边有行号、右边没有（0 = 这一侧是补的空行） */
            const QVariantMap del = rows.value(1).toMap();
            tcheck(del.value(QStringLiteral("leftNo")).toInt() == 2
                       && del.value(QStringLiteral("rightNo")).toInt() == 0,
                  QStringLiteral("差异行两边各自的行号对得上（右边补空行时给 0）"),
                  QStringLiteral("左 %1 / 右 %2")
                      .arg(del.value(QStringLiteral("leftNo")).toInt())
                      .arg(del.value(QStringLiteral("rightNo")).toInt()));
        }

        /* 统一补丁：有 --- / +++ / @@ 三个头，而且改的那一行以 - / + 带出来 */
        {
            differ->compare(QStringLiteral("a\nold\nc\n"), QStringLiteral("a\nnew\nc\n"),
                            QStringLiteral("L.txt"), QStringLiteral("R.txt"));
            const QString patch = differ->unifiedDiff();
            tcheck(patch.startsWith(QStringLiteral("--- L.txt"))
                       && patch.contains(QStringLiteral("+++ R.txt"))
                       && patch.contains(QStringLiteral("@@")),
                  QStringLiteral("统一补丁有 --- / +++ / @@ 三个头"));
            tcheck(patch.contains(QStringLiteral("-old")) && patch.contains(QStringLiteral("+new")),
                  QStringLiteral("补丁里改掉的那一行是 -old / +new"),
                  patch.split(QLatin1Char('\n')).value(3));
        }

        /*
         * 空对非空：不能崩，结论要说得出来。
         *
         * 只钉"结果非空、有差异、而且差异行数在 1~2 之间"：尾巴上那个换行
         * 算不算"一个空行"两种口径都讲得通（`"x\n"` 按"行"拆是 ['x'] 还是
         * ['x',''] 取决于怎么定义），这里不把一个定义问题钉成断言。
         */
        {
            differ->compare(QStringLiteral(""), QStringLiteral("x\n"));
            const int changed = changedCount(differ->rows());
            tcheck(differ->hasResult() && changed >= 1 && changed <= 2,
                  QStringLiteral("空文件对一份有内容的：报出差异（1~2 行）"),
                  differ->summary());
        }

        /*
         * 磁盘读文件（对比的另一份是从磁盘读的）。
         * 中文 + UTF-8 要能读回来 —— 用本地编码读的话这里就会乱码。
         */
        {
            QTemporaryDir dir;
            const QString path = dir.filePath(QStringLiteral("cn.txt"));
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QStringLiteral("第一行\n第二行\n").toUtf8());
                f.close();
            }
            const QString text = differ->readFile(path);
            tcheck(text == QStringLiteral("第一行\n第二行\n"),
                  QStringLiteral("从磁盘读一份 UTF-8 中文文件，读回来还是那几个字"),
                  text.left(20));
        }

        /*
         * 两个忽略开关。钉的是**开关一按就按新口径重算**，不用重新调 compare：
         * setIgnoreCase 内部拿上次那两份正文再跑一遍（见 src/Diff.cpp）。
         */
        {
            differ->setIgnoreCase(false);
            differ->setIgnoreWhitespace(false);
            differ->compare(QStringLiteral("abc\ndef\n"), QStringLiteral("ABC\ndef\n"));
            const int off = differ->changes().size();
            differ->setIgnoreCase(true);
            const int on = differ->changes().size();
            tout(QStringLiteral("忽略大小写：关 %1 处 -> 开 %2 处").arg(off).arg(on));
            tcheck(off == 1 && on == 0,
                   QStringLiteral("忽略大小写：一按就重算，那处差异没了"));
            differ->setIgnoreCase(false);

            differ->compare(QStringLiteral("  abc\n"), QStringLiteral("abc\n"));
            const int wOff = differ->changes().size();
            differ->setIgnoreWhitespace(true);
            const int wOn = differ->changes().size();
            tout(QStringLiteral("忽略首尾空白：关 %1 处 -> 开 %2 处").arg(wOff).arg(wOn));
            tcheck(wOff == 1 && wOn == 0,
                   QStringLiteral("忽略首尾空白：一按就重算"));
            differ->setIgnoreWhitespace(false);

            /* 只忽略大小写时，缩进不同还得算差异（两个开关不能互相带出来） */
            differ->setIgnoreCase(true);
            differ->compare(QStringLiteral("  abc\n"), QStringLiteral("abc\n"));
            tcheck(differ->changes().size() == 1,
                   QStringLiteral("只开忽略大小写时，缩进不同仍然算一处差异"));
            differ->setIgnoreCase(false);
        }

        /*
         * 字级差异（mod 行里到底哪几个字变了）。
         * "alpha = 1;" -> "alpha = 2;" 只有那个数字变了，两边都该报第 9 列起 1 个字符。
         */
        {
            differ->compare(QStringLiteral("alpha = 1;\n"), QStringLiteral("alpha = 2;\n"));
            const QVariantMap row = differ->rows().value(0).toMap();
            const QVariantList lw = row.value(QStringLiteral("leftWords")).toList();
            const QVariantList rw = row.value(QStringLiteral("rightWords")).toList();
            tout(QStringLiteral("字级：左 [%1,%2] 右 [%3,%4]（kind=%5）")
                     .arg(lw.value(0).toInt()).arg(lw.value(1).toInt())
                     .arg(rw.value(0).toInt()).arg(rw.value(1).toInt())
                     .arg(row.value(QStringLiteral("kind")).toString()));
            tcheck(row.value(QStringLiteral("kind")).toString() == QLatin1String("mod")
                       && lw.size() == 2 && lw.at(0).toInt() == 8 && lw.at(1).toInt() == 1
                       && rw.size() == 2 && rw.at(0).toInt() == 8 && rw.at(1).toInt() == 1,
                   QStringLiteral("改一个字符：两边都只标第 9 列那一个字符"),
                   QStringLiteral("左 %1 / 右 %2").arg(QVariant(lw).toString(),
                                                       QVariant(rw).toString()));
        }
    }

    /* ------------------------------------------------------------------
     * 2. 格式化（src/Formatter.h）
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 格式化 --\n", stdout);
    if (!fmt) {
        tcheck(false, QStringLiteral("格式化器传进来了"));
    } else {
        /* 内置 JSON 重排：不依赖本机装没装 prettier */
        {
            const QString in = QStringLiteral("{\"b\":2,\"a\":[1,2]}\n");
            /* 先把 json 的自定义命令清掉，逼它走内置那条路 */
            const QVariant savedJson = settings.value(QStringLiteral("format/tool/json"));
            fmt->setToolFor(QStringLiteral("json"), QString());
            const QString out = fmt->format(in, QStringLiteral("json"), QStringLiteral("x.json"));
            settings.setValue(QStringLiteral("format/tool/json"), savedJson);

            /*
             * 本机装了 prettier 时走的是它（结果也合法），没装才走内置 ——
             * 所以这里只钉"结果是一份能解析回去、且内容和原来一样的 JSON"，
             * 不钉缩进几个空格。
             */
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8(), &err);
            tcheck(err.error == QJsonParseError::NoError && doc.isObject()
                       && doc.object().value(QStringLiteral("b")).toInt() == 2,
                  QStringLiteral("JSON 格式化：结果还能解析回去，内容没变"),
                  out.left(40).replace(QLatin1Char('\n'), QLatin1Char(' ')));
            tcheck(out.contains(QLatin1Char('\n')),
                  QStringLiteral("JSON 格式化：真的排成多行了"));
        }

        /* 坏 JSON：报出字节位置，而且正文**一个字都不动** */
        {
            const QVariant savedJson = settings.value(QStringLiteral("format/tool/json"));
            fmt->setToolFor(QStringLiteral("json"), QString());
            const QString broken = QStringLiteral("{\"a\": }");
            const QString out = fmt->format(broken, QStringLiteral("json"),
                                            QStringLiteral("x.json"));
            settings.setValue(QStringLiteral("format/tool/json"), savedJson);
            tcheck(out == broken,
                  QStringLiteral("坏 JSON：正文原样不动（最不能出的事故是格式化把正文弄没了）"));
            tcheck(fmt->lastError().contains(QStringLiteral("解析失败"))
                       || !fmt->lastError().isEmpty(),
                  QStringLiteral("坏 JSON：报了一句人能看懂的原因"), fmt->lastError());
        }

        /* 通用清理：行尾空白 / 连续空行 / 末尾换行 */
        {
            fmt->setToolFor(QStringLiteral("plain"), QString());
            const QString messy = QStringLiteral("a   \n\n\n\nb\t\n");
            const QString out = fmt->format(messy, QStringLiteral("plain"),
                                            QStringLiteral("x.txt"));
            tcheck(out == QStringLiteral("a\n\nb\n"),
                  QStringLiteral("纯文本清理：去行尾空白、连续空行收成一个、末尾补换行"),
                  QString(out).replace(QLatin1Char('\n'), QLatin1String("\\n")));
        }

        /* 本机没装的工具：要**报名字**，不能悄悄什么都不做 */
        {
            fmt->setToolFor(QStringLiteral("cpp"),
                            QStringLiteral("绝对不存在的格式化工具-xyz"));
            const QString in = QStringLiteral("int main(){return 0;}\n");
            const QString out = fmt->format(in, QStringLiteral("cpp"), QStringLiteral("x.cpp"));
            tcheck(out == in, QStringLiteral("工具跑不起来：正文原样不动"));
            tcheck(fmt->lastError().contains(QStringLiteral("绝对不存在的格式化工具-xyz")),
                  QStringLiteral("工具跑不起来：错误里点名了是哪个命令"),
                  fmt->lastError());
            tcheck(!fmt->supported(QStringLiteral("cpp")),
                  QStringLiteral("工具跑不起来：菜单那条会置灰（supported 为假）"));
            fmt->setToolFor(QStringLiteral("cpp"), QString());
        }

        /* 没装 clang-format 的机器上，"这个语言能不能格式化"要说真话 */
        {
            const bool can = fmt->supported(QStringLiteral("cpp"));
            tcheck(can == (fmt->engineLabel(QStringLiteral("cpp")) != QString())
                       || !can,
                  QStringLiteral("能不能格式化 = 引擎名非空（菜单和实现同一份判断）"),
                  QStringLiteral("supported=%1 engine=%2")
                      .arg(can ? 1 : 0).arg(fmt->engineLabel(QStringLiteral("cpp"))));
        }

        /* toolList：每个语言一行，字段齐全 */
        {
            const QVariantList list = fmt->toolList();
            tcheck(!list.isEmpty(), QStringLiteral("工具表不是空的"));
            const QVariantMap first = list.value(0).toMap();
            tcheck(first.contains(QStringLiteral("id"))
                       && first.contains(QStringLiteral("defaultCommand")),
                  QStringLiteral("工具表每行都有 id / 默认命令（设置面板要显示）"));
        }
    }

    /* ------------------------------------------------------------------
     * 3. 校验（src/Checker.h）—— 只跑本地规则，不发真请求
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 校验（本地规则） --\n", stdout);
    if (!check) {
        tcheck(false, QStringLiteral("校验器传进来了"));
    } else {
        /* 括号配对：多一个、少一个、类型不匹配 */
        {
            check->checkLocalOnly(QStringLiteral("int main() {\n    return 0;\n"), QStringLiteral("cpp"));
            tcheck(hasIssue(check->issues(), QStringLiteral("bracket")),
                  QStringLiteral("括号没关：报出来了"), check->resultSummary());

            check->checkLocalOnly(QStringLiteral("int main() {\n    return 0;\n}\n"),
                                  QStringLiteral("cpp"));
            tcheck(!hasIssue(check->issues(), QStringLiteral("bracket")),
                  QStringLiteral("括号配平：不报（不能满屏假阳性）"), check->resultSummary());

            check->checkLocalOnly(QStringLiteral("int main(] {\n}\n"), QStringLiteral("cpp"));
            tcheck(hasIssue(check->issues(), QStringLiteral("bracket")),
                  QStringLiteral("括号类型配错：报出来了"));
        }

        /* 字符串 / 注释里的括号不算语法符号 */
        {
            check->checkLocalOnly(QStringLiteral("const char *s = \"(\";\n// 还有 ) 这个\n"),
                                  QStringLiteral("cpp"));
            tcheck(!hasIssue(check->issues(), QStringLiteral("bracket")),
                  QStringLiteral("字符串和注释里的括号不报（这块最容易出假阳性）"),
                  check->resultSummary());
        }

        /* 中文里用半角逗号 */
        {
            check->checkLocalOnly(QStringLiteral("今天天气不错,我们去散步。\n"),
                                  QStringLiteral("markdown"));
            bool punct = false;
            for (const QVariant &i : check->issues()) {
                const QVariantMap m = i.toMap();
                if (m.value(QStringLiteral("kind")).toString() == QLatin1String("punct")
                    && m.value(QStringLiteral("suggestion")).toString() == QStringLiteral("，"))
                    punct = true;
            }
            tcheck(punct, QStringLiteral("中文句子里的半角逗号：报出来并给出全角写法"),
                  check->resultSummary());
        }

        /* 「的」-> 「得」：做的很好 */
        {
            check->checkLocalOnly(QStringLiteral("他做的很好。\n"), QStringLiteral("markdown"));
            tcheck(hasIssue(check->issues(), QStringLiteral("de")),
                  QStringLiteral("「做的很好」报出来（建议改成「得」）"), check->resultSummary());
        }

        /* 重复字：的的 */
        {
            check->checkLocalOnly(QStringLiteral("这是我的的书。\n"), QStringLiteral("markdown"));
            tcheck(hasIssue(check->issues(), QStringLiteral("repeat")),
                  QStringLiteral("「的的」报出来"), check->resultSummary());
            /*
             * 严重级也得钉：原来只钉了"报没报"，而"的 / 了 报 error"那条分支
             * 写成了 QLatin1String("的")（中文走 Latin1 永不相等），静默地一直是
             * warn 也没人发现。报没报和报成什么颜色是两件事。
             */
            tcheck(severityOf(check->issues(), QStringLiteral("repeat"))
                       == QStringLiteral("error"),
                  QStringLiteral("「的的」按 error 报（不是黄线）"),
                  severityOf(check->issues(), QStringLiteral("repeat")));
        }

        /* 其它叠字仍然只是提醒：白名单外的字不等于"多打了一个"那么确定 */
        {
            check->checkLocalOnly(QStringLiteral("这是那那回事。\n"), QStringLiteral("markdown"));
            tcheck(severityOf(check->issues(), QStringLiteral("repeat"))
                       == QStringLiteral("warn"),
                  QStringLiteral("「要要」按 warn 报"),
                  severityOf(check->issues(), QStringLiteral("repeat")));
        }

        /* 行尾空白：任何语言都报，而且是 info（不吓人） */
        {
            check->checkLocalOnly(QStringLiteral("hello   \n"), QStringLiteral("cpp"));
            tcheck(hasIssue(check->issues(), QStringLiteral("trailing-space")),
                  QStringLiteral("行尾空白报出来"));
        }

        /* 干净的一段：不要报任何东西 */
        {
            check->checkLocalOnly(QStringLiteral("# 标题\n\n这是一段正常的中文，没有问题。\n"),
                                  QStringLiteral("markdown"));
            tcheck(check->issues().isEmpty(),
                  QStringLiteral("干净正文：一条都不报"), check->resultSummary());
        }

        /* 问题按行号排好序（编辑区里画波浪线的顺序要跟正文一致） */
        {
            check->checkLocalOnly(QStringLiteral("第一行有,问题\n第二行也有,问题\n"),
                                  QStringLiteral("markdown"));
            const QVariantList issues = check->issues();
            bool sorted = true;
            for (int i = 1; i < issues.size(); ++i) {
                if (issues.at(i - 1).toMap().value(QStringLiteral("row")).toInt()
                    > issues.at(i).toMap().value(QStringLiteral("row")).toInt())
                    sorted = false;
            }
            tcheck(sorted, QStringLiteral("问题清单按行号排好序"));
        }

        /* 提示词：两份（中文 / 代码），都写死了行号格式和"只报错误" */
        {
            const QString zh = check->promptFor(QStringLiteral("markdown"));
            const QString code = check->promptFor(QStringLiteral("cpp"));
            tcheck(zh.contains(check->replyFormat())
                       && code.contains(check->replyFormat()),
                  QStringLiteral("两份提示词都写死了输出格式（模型得按行号回）"),
                  check->replyFormat());
            tcheck(zh.contains(QStringLiteral("校对")) && code.contains(QStringLiteral("语法")),
                  QStringLiteral("中文用「校对员」、代码用「语法」那一套提示词"));
            tcheck(zh.contains(QStringLiteral("原文片段")),
                  QStringLiteral("提示词要求给逐字片段（回来要靠它核对位置、挡住编造）"));
        }

        /*
         * 接口没配好：wantsModel 必须是假（点了校验也不能偷偷发请求；
         * 配好之后才走模型那条路）。
         */
        {
            if (llm && llm->apiBase().trimmed().isEmpty()) {
                tcheck(!check->wantsModel(QStringLiteral("随便一段中文")),
                      QStringLiteral("接口没配：不问模型（不偷偷发请求）"));
            }
            /* 超长正文：只跑本地（模型数行号会不准） */
            tcheck(!check->wantsModel(QString(20000, QLatin1Char('x'))),
                  QStringLiteral("正文太长：只跑本地规则（模型数行号会不准）"));
            check->clear();
        }

        /* 空正文：不报问题，也不崩 */
        {
            check->checkLocalOnly(QString(), QStringLiteral("plain"));
            tcheck(check->issues().isEmpty() && check->resultSummary().contains(QStringLiteral("未发现")),
                  QStringLiteral("空正文：没问题也没崩"), check->resultSummary());
        }
    }

    /* ------------------------------------------------------------------
     * 4. Markdown 渲染（EditorController::markdownHtml）
     * ------------------------------------------------------------------ */
    std::fputs("\n-- Markdown 预览 --\n", stdout);
    {
        EditorController controller;

        const QString md = QStringLiteral(
            "# 标题\n\n"
            "正文里有 `code` 和 **粗体**。\n\n"
            "- 第一项\n"
            "- 第二项\n\n"
            "| 列 A | 列 B |\n|---|---|\n| 1 | 2 |\n\n"
            "```\nint main() {}\n```\n\n"
            "> 引用\n");

        const QString html = controller.markdownHtml(md, QString());
        tcheck(!html.isEmpty(), QStringLiteral("渲染出来了（非空）"));
        /* QTextDocument 的 toHtml 出来的是它那套受控标签 */
        tcheck(html.contains(QStringLiteral("<h1")) || html.contains(QStringLiteral("font-size")),
              QStringLiteral("标题渲染成了标题标签"));
        tcheck(html.contains(QStringLiteral("<table")) || html.contains(QStringLiteral("<td")),
              QStringLiteral("表格渲染出来了（Text.MarkdownText 那条路不支持表格）"));
        tcheck(html.contains(QStringLiteral("<code")) || html.contains(QStringLiteral("Consolas")),
              QStringLiteral("行内代码用等宽字体那一档"));
        tcheck(html.contains(QStringLiteral("d6d7da")),
              QStringLiteral("配色是深色主题那一套（默认是白纸黑字，会把正文藏起来）"));

        /*
         * 相对图片路径：按文档所在目录改成绝对 file:// 路径。
         *
         * 图上必须**真的存在**：QTextDocument 读 markdown 时会试着加载那张图，
         * 加载不到（或者是不认识的 URL）就**整张丢弃**，HTML 里连 <img> 都没有。
         * 所以这里先造一个真文件 —— 拿一个不存在的路径试，量到的是"图被丢掉"，
         * 而不是"路径改写没生效"，两件事很容易混。
         */
        {
            QTemporaryDir imgDir;
            const QString imgPath = imgDir.filePath(QStringLiteral("pic.png"));
            {
                QFile f(imgPath);
                if (f.open(QIODevice::WriteOnly))
                    f.write(QByteArray::fromBase64(
                        "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=="));
            }
            const QString mdImg = QStringLiteral("![](pic.png)\n");
            const QString html2 = controller.markdownHtml(mdImg, imgDir.path());

            /* 改写算出来的那条绝对路径（和实现同一条 QUrl::fromLocalFile） */
            const QString expectUrl =
                QUrl::fromLocalFile(QDir(imgDir.path()).absoluteFilePath(QStringLiteral("pic.png")))
                    .toString(QUrl::FullyEncoded);
            tcheck(html2.contains(expectUrl),
                  QStringLiteral("相对图片路径按文档目录改成绝对路径（不这么改预览全是裂图）"),
                  html2.contains(QStringLiteral("<img"))
                      ? html2.mid(html2.indexOf(QStringLiteral("<img")), 120)
                      : QStringLiteral("<HTML 里没有 img：图被丢掉了>"));
            tcheck(html2.contains(QStringLiteral("<img")),
                  QStringLiteral("图片渲染出来了（不是被丢掉）"));
        }

        /* 空正文：返回空串（界面据此显示"这份文档是空的"） */
        tcheck(controller.markdownHtml(QStringLiteral("   \n\n"), QString()).isEmpty(),
              QStringLiteral("只有空白的正文：返回空串（界面显示那句提示）"));

        /* 点链接的白名单：只放行 http/https/mailto/file */
        tcheck(!controller.openExternal(QStringLiteral("javascript:alert(1)")),
              QStringLiteral("javascript: 这类链接被拒（预览里的链接要过白名单）"));
        tcheck(!controller.openExternal(QStringLiteral("shell:calc")),
              QStringLiteral("shell: 这类链接被拒"));
        tcheck(!controller.openExternal(QString()),
              QStringLiteral("空链接被拒"));
    }

    /* ------------------------------------------------------------------
     * 5. Markdown 预览里的右键菜单（QML 那侧，只有 --self-test 跑得到）
     * ------------------------------------------------------------------ */
    if (qmlRoot) {
        /*
         * 读界面状态（和 src/SelfTest.cpp 里那个同名函数同一套做法：
         * 界面把要核对的量塞进 uiState() 这一个对象里，自检拿它一次读全）。
         */
        const auto uiState = [qmlRoot]() {
            QVariant result;
            QMetaObject::invokeMethod(qmlRoot, "uiState", Q_RETURN_ARG(QVariant, result));
            return result.toMap();
        };

        std::fputs("\n-- Markdown 预览的右键菜单 --\n", stdout);

        /* 造一份 .md：canPreviewMarkdown 认的是扩展名，得让它真的能预览 */
        QTemporaryDir mdDir;
        const QString mdPath = mdDir.filePath(QStringLiteral("note.md"));
        {
            QFile f(mdPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QStringLiteral("# 标题\n\n正文一段，用来看预览。\n\n- 甲\n- 乙\n").toUtf8());
                f.close();
            }
        }

        QMetaObject::invokeMethod(qmlRoot, "openTreeFile", Q_ARG(QVariant, QVariant(mdPath)));
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        /*
         * 先钉"打开 .md 之后可以预览"。
         *
         * 判据是"切一次之后状态真的**变了**"，不是"切完一定在预览"：
         * 打开 .md 时预览开不开取决于上次退出时的偏好
         * （markdownPreviewPreferred），自检不该假设其中一种。
         * "能切"本身就说明 canPreviewMarkdown 为真 —— 不是 .md 的话
         * toggleMarkdownPreview 直接 return，状态一动不动。
         */
        const bool previewBefore = uiState().value(QStringLiteral("markdownPreview")).toBool();
        QMetaObject::invokeMethod(qmlRoot, "toggleMarkdownPreview");
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const bool previewAfter = uiState().value(QStringLiteral("markdownPreview")).toBool();
        tcheck(previewAfter != previewBefore,
              QStringLiteral("打开 .md 之后能切（说明 canPreviewMarkdown 为真）"),
              QStringLiteral("%1 -> %2").arg(previewBefore ? 1 : 0).arg(previewAfter ? 1 : 0));

        /*
         * 上面那一次可能是**切出去**了；菜单核对必须在预览那一侧做，
         * 所以这里按需再切一次。
         */
        if (!previewAfter) {
            QMetaObject::invokeMethod(qmlRoot, "toggleMarkdownPreview");
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
        }
        tcheck(uiState().value(QStringLiteral("markdownPreview")).toBool(),
              QStringLiteral("核对菜单之前确实在预览里"),
              QStringLiteral("preview=%1")
                  .arg(uiState().value(QStringLiteral("markdownPreview")).toBool() ? 1 : 0));

        /*
         * 预览正文必须是**裸 TextEdit**，不能退回 TextArea。
         *
         * Qt 6.9 起 TextArea 自带一套右键菜单（Fusion / Basic 的 TextArea.qml 里
         * 写着 `ContextMenu.menu: TextEditingContextMenu`），而"挂一个空
         * `Menu { }`"是关不掉的：那块空面板照样 popup —— Fusion 的 Menu 底是个
         * 写死 200x20 的白矩形（Fusion/Menu.qml 的 implicitWidth / implicitHeight），
         * 实测预览左上角会多出一块白方块（用户报的就是这个，截图里量到
         * 198x18 纯白 + 1px #ABABAB 边框）。
         *
         * TextEdit 没有 ContextMenu 这个附加类型，是**根本弹不出来**，不是"关掉了"。
         * 断言按类名比，不引私有头；body 的 objectName 见 MarkdownView.qml。
         */
        {
            QObject *body = qmlRoot->findChild<QObject *>(QStringLiteral("markdownBody"));
            const QString cls = body ? QString::fromLatin1(body->metaObject()->className())
                                     : QString();
            tcheck(body != nullptr,
                   QStringLiteral("找得到预览正文（objectName: markdownBody）"), cls);
            tcheck(cls == QLatin1String("QQuickTextEdit"),
                   QStringLiteral("预览正文是裸 TextEdit（不是自带右键菜单的 TextArea）"), cls);
        }

        /*
         * 预览那份菜单：**改正文的命令全灰**，复制 / 全选 / 复制全文可用。
         *
         * 这条钉的是"预览里的右键换成了程序自己那套菜单"的第二半 ——
         * 前半（样式自带的那套右键菜单从一开始就不存在）见 MarkdownView.qml：
         * 正文是只读 TextEdit，不带 Control 那套 ContextMenu。
         */
        {
            QVariant acts;
            QMetaObject::invokeMethod(qmlRoot, "editMenuPreviewActs",
                                      Q_RETURN_ARG(QVariant, acts));
            const QVariantList list = acts.toList();
            QString joined;
            for (const QVariant &a : list)
                joined += (joined.isEmpty() ? QString() : QStringLiteral(" | ")) + a.toString();

            const auto actOf = [&list](const QString &name) -> QString {
                for (const QVariant &a : list) {
                    const QString s = a.toString();
                    if (s.startsWith(name + QLatin1Char(':')))
                        return s;
                }
                return QString();
            };

            tcheck(actOf(QStringLiteral("undo")) == QLatin1String("undo:0")
                       && actOf(QStringLiteral("paste")) == QLatin1String("paste:0")
                       && actOf(QStringLiteral("cut")) == QLatin1String("cut:0"),
                  QStringLiteral("预览里「撤销 / 剪切 / 粘贴」是灰的（那些会改正文）"),
                  joined);
            tcheck(actOf(QStringLiteral("copyPreview")) == QLatin1String("copyPreview:0"),
                  QStringLiteral("预览里那条「复制」走的是预览自己的动作（copyPreview）"));
            tcheck(actOf(QStringLiteral("selectAllPreview"))
                       == QLatin1String("selectAllPreview:1"),
                  QStringLiteral("预览里「全选」可用（全选的是预览里的正文）"));
            tcheck(actOf(QStringLiteral("toggleReadOnly")) == QLatin1String("toggleReadOnly:0"),
                  QStringLiteral("预览里「只读模式」是灰的（预览本来就只读）"));
            tcheck(!joined.contains(QStringLiteral("toggleMarkdownPreview:0")),
                  QStringLiteral("预览里「Markdown 预览」那一条还亮着（可以切回源码）"));
            tcheck(actOf(QStringLiteral("copyAll")) == QLatin1String("copyAll:1"),
                  QStringLiteral("预览里「复制全文」可用（复制的是同一份文档）"));
        }

        /*
         * 真的在预览里"点一次右键"：走的是 MarkdownView 报坐标 -> Main.qml 弹
         * 自己那套菜单这条路（见 MarkdownView.qml 的 rightClickCatcher 和
         * Main.qml 的 openPreviewContextMenu）。钉的是菜单真的开了、而且开在
         * 鼠标那一点上 —— 只验菜单数据的话，"信号没接上"这种错核不出来。
         */
        {
            const double px = 420.0;
            const double py = 260.0;
            QVariant opened;
            QMetaObject::invokeMethod(qmlRoot, "simulatePreviewRightClick",
                                      Q_RETURN_ARG(QVariant, opened),
                                      Q_ARG(QVariant, QVariant(px)),
                                      Q_ARG(QVariant, QVariant(py)));
            const QVariantMap ui = uiState();
            const double mx = ui.value(QStringLiteral("menuX")).toDouble();
            const double my = ui.value(QStringLiteral("menuY")).toDouble();
            tcheck(opened.toBool() && ui.value(QStringLiteral("menuOpened")).toBool(),
                  QStringLiteral("在预览里点右键：程序自己那套菜单真的弹出来了"),
                  QStringLiteral("opened=%1 menuOpened=%2")
                      .arg(opened.toBool() ? 1 : 0)
                      .arg(ui.value(QStringLiteral("menuOpened")).toBool() ? 1 : 0));
            tcheck(qAbs(mx - px) < 0.5 && qAbs(my - py) < 0.5,
                  QStringLiteral("预览里的右键菜单开在鼠标那一点上（坐标口径和编辑区一致）"),
                  QStringLiteral("实际 (%1, %2)").arg(mx).arg(my));
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            for (int i = 0; i < 2; ++i)
                QCoreApplication::processEvents();
        }

        /*
         * 切回源码：确认这条路来回都通（别把预览开关弄成单向的）。
         * 同样只钉"真的换了状态"，不假设一定从"预览"出发 —— 上面那几次
         * 切换之后停在哪一侧取决于起点，钉死一侧会变成随偏好飘的断言。
         */
        {
            const bool before = uiState().value(QStringLiteral("markdownPreview")).toBool();
            QMetaObject::invokeMethod(qmlRoot, "toggleMarkdownPreview");
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            const bool after = uiState().value(QStringLiteral("markdownPreview")).toBool();
            tcheck(after != before, QStringLiteral("再切一次状态又换了一边（来回都通）"),
                  QStringLiteral("%1 -> %2").arg(before ? 1 : 0).arg(after ? 1 : 0));
        }

        /*
         * 收尾：把这份 .md 关掉。
         *
         * **先存盘**再关：刚打开的文件不算"已修改"，但保险起见存一次 ——
         * 万一它是脏的，closeAllTabs 会弹「保存 / 不保存」那块卡片等用户回答，
         * 自检里没有用户，整串就卡在那儿。
         */
        QMetaObject::invokeMethod(qmlRoot, "saveAll");
        for (int i = 0; i < 2; ++i)
            QCoreApplication::processEvents();
        QMetaObject::invokeMethod(qmlRoot, "closeAllTabs");
        for (int i = 0; i < 2; ++i)
            QCoreApplication::processEvents();
    } else {
        std::fputs("\n-- Markdown 预览的右键菜单：跳过（没传 Main.qml 根对象） --\n", stdout);
    }

    /* ------------------------------------------------------------------
     * 6. 对比绘制层（src/EditorViewItem.h 那组 setDiff*）
     *
     * 这一节钉的是"BC 那种对齐空行到底走不走得通"，也就是整套设计的地基：
     * 行注释（annotation）只许给那一行**加显示高度**，缓冲区 / 行数 / 行号 /
     * 保存点一个都不许动 —— 动了一次，对比页里就不能直接改字了。
     *
     * 所以每条都带数字打出来：先看尺子量不量得出来，再谈对不对。
     * ------------------------------------------------------------------ */
    std::fputs("\n-- 对比绘制层（行注释撑对齐空行） --\n", stdout);
    if (EditorViewItem *view = EditorViewItem::instance()) {
        /* 上面那一节关标签时把文档全收了，这里得先有一份能写的 */
        if (!view->hasDocument())
            view->newDocument();
        /*
         * 九行长（120 字）+ 一行短。长行用来量"底色铺不铺得开"，
         * 短行用来把这条**已知限度**钉在纸面上：底色只铺到文字结束，
         * 短行右边那一截不铺（Scintilla 没有"整行通宽且铺在字底下"那一格，
         * 见 EditorViewItem.cpp 里 beginDiff 那段实测记录）。
         */
        QString sample;
        const auto longRow = [](int n) {
            return QStringLiteral("L%1 ").arg(n, 2, 10, QLatin1Char('0'))
                   + QStringLiteral("内容").repeated(40);
        };
        for (int i = 1; i <= 9; ++i)
            sample += (i == 8 ? QStringLiteral("L08 短行\n") : longRow(i) + QLatin1Char('\n'));
        view->setText(sample);
        /* 把"刚写进去的这十行"定成保存点：下面那条"没弄脏"才是真在比前后 */
        view->setModified(false);
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const int h = view->textLineHeight();
        const int y3a = view->lineTopY(3);
        const int y4a = view->lineTopY(4);
        const int y5a = view->lineTopY(5);
        const int charsA = view->charCount();
        const int linesA = view->lineCount();
        const bool dirtyA = view->modified();

        view->beginDiff();
        /* 涂在**第 0 行**：光标开局就在那儿，这一行同时是"当前行"—— 最狠的一种叠法 */
        view->setDiffLineKind(0, QStringLiteral("mod"));
        view->setDiffLineKind(2, QStringLiteral("del"));
        view->setDiffLineKind(7, QStringLiteral("del"));   // 那一行是短行
        view->setDiffWordMarks(2, { 1, 2 });
        view->setDiffGap(3, 3);          // 第 4 行（0 基）底下撑 3 行高
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const int y3b = view->lineTopY(3);
        const int y4b = view->lineTopY(4);
        const int y5b = view->lineTopY(5);

        tout(QStringLiteral("行高 h=%1").arg(h));
        tout(QStringLiteral("撑带前  y3=%1  y4=%2  y5=%3   （间距 %4 / %5）")
                 .arg(y3a).arg(y4a).arg(y5a).arg(y4a - y3a).arg(y5a - y4a));
        tout(QStringLiteral("撑带后  y3=%1  y4=%2  y5=%3   （间距 %4 / %5）")
                 .arg(y3b).arg(y4b).arg(y5b).arg(y4b - y3b).arg(y5b - y4b));

        tcheck(h > 0, QStringLiteral("行高量得出来（后面所有像素判断的尺子）"),
               QStringLiteral("%1 px").arg(h));
        tcheck(y3b == y3a, QStringLiteral("带子**上面**那些行一动不动"),
               QStringLiteral("%1 -> %2").arg(y3a).arg(y3b));
        tcheck(y4b - y3b == 4 * h, QStringLiteral("带子正好多出 3 行高"),
               QStringLiteral("间距 %1，应为 %2").arg(y4b - y3b).arg(4 * h));
        tcheck(y5b - y4b == y4a - y3a, QStringLiteral("带子不改后面的行距"),
               QStringLiteral("%1 vs %2").arg(y5b - y4b).arg(y4a - y3a));

        tcheck(view->charCount() == charsA, QStringLiteral("正文缓冲区一个字节没变"),
               QStringLiteral("%1 -> %2").arg(charsA).arg(view->charCount()));
        tcheck(view->lineCount() == linesA, QStringLiteral("行数没变（行号还是 1..10）"),
               QStringLiteral("%1 -> %2").arg(linesA).arg(view->lineCount()));
        tcheck(view->modified() == dirtyA, QStringLiteral("保存点没被弄脏（不会冒出未保存的小圆点）"),
               dirtyA ? QStringLiteral("脏") : QStringLiteral("干净"));
        tcheck(view->diffGapAt(3) == 3, QStringLiteral("diffGapAt 读回来也是 3 行"),
               QStringLiteral("实际 %1").arg(view->diffGapAt(3)));
        /*
         * 命中测试：带子下面那一行的"上沿 + 半行高"那点，Scintilla 认不认得是第 4 行。
         * 认不出来的话，用户在对比页里点一下就会点到隔壁那行 —— 这是这条路的生死线。
         * x 取 100：三条边距（行号 / 折叠 / 分隔线）加起来不到这个数，落在正文里。
         */
        tcheck(view->lineAtPoint(100, y4b + h / 2) == 4,
               QStringLiteral("点带子下面那一行，落点还是它自己（命中测试算进了带子）"),
               QStringLiteral("量到第 %1 行").arg(view->lineAtPoint(100, y4b + h / 2)));
        tcheck(view->lineAtPoint(100, y3b + h / 2) == 3,
               QStringLiteral("带子**上面**那行的落点不受影响"),
               QStringLiteral("量到第 %1 行").arg(view->lineAtPoint(100, y3b + h / 2)));

        /*
         * 底色铺没铺开、字还看不看得见 —— 这两件事必须同时成立。
         * 整行扫一遍数像素：band = 底色像素，glyph = 既不是底色也不是正文底色的
         * （就是笔画）。只看一个点分不出"没铺"和"铺了但把字糊住了"。
         */
        const auto stats = [view](int line, const char *hex) {
            return view->diffRowStats(line, QString::fromLatin1(hex));
        };
        const auto cnt = [](const QVariantMap &m, const char *key) {
            return m.value(QLatin1String(key)).toInt();
        };
        const QVariantMap sDel = stats(2, "#3a2224");
        const QVariantMap sCaret = stats(0, "#3a3320");
        const QVariantMap sShort = stats(7, "#3a2224");
        const QVariantMap sPlain = stats(5, "#3a2224");
        tout(QStringLiteral("扫描区间 x %1..%2   长行 del：底色 %3 笔画 %4 底 %5")
                 .arg(cnt(sDel, "xFrom")).arg(cnt(sDel, "xTo"))
                 .arg(cnt(sDel, "band")).arg(cnt(sDel, "glyph")).arg(cnt(sDel, "paper")));
        tout(QStringLiteral("             当前行 mod：底色 %1 笔画 %2   "
                            "短行 del：底色 %3 笔画 %4   没涂那行：底色 %5")
                 .arg(cnt(sCaret, "band")).arg(cnt(sCaret, "glyph"))
                 .arg(cnt(sShort, "band")).arg(cnt(sShort, "glyph"))
                 .arg(cnt(sPlain, "band")));

        tcheck(cnt(sDel, "band") > 100 && cnt(sDel, "glyph") > 0,
               QStringLiteral("长行：底色铺开了一整条，笔画还在（字没被盖住）"),
               QStringLiteral("底色 %1 / 笔画 %2")
                   .arg(cnt(sDel, "band")).arg(cnt(sDel, "glyph")));
        tcheck(cnt(sCaret, "band") > 100 && cnt(sCaret, "glyph") > 0,
               QStringLiteral("光标停着那一行的底色不被当前行色盖掉，字也还在"),
               QStringLiteral("底色 %1 / 笔画 %2")
                   .arg(cnt(sCaret, "band")).arg(cnt(sCaret, "glyph")));
        tcheck(cnt(sPlain, "band") == 0,
               QStringLiteral("没涂的那一行一个底色像素都没有"));
        /* 这条钉的是**限度**不是缺陷：底色只铺到文字结束，短行铺不满一行。 */
        tcheck(cnt(sShort, "band") > 0 && cnt(sShort, "band") < cnt(sDel, "band") / 3,
               QStringLiteral("限度：短行的底色只到文字结束（远少于长行那一整条）"),
               QStringLiteral("短行底色 %1 / 长行底色 %2")
                   .arg(cnt(sShort, "band")).arg(cnt(sDel, "band")));

        /* 退出对比要把这一层擦干净：同一份文档切回普通标签不能带着色块 */
        view->endDiff();
        for (int i = 0; i < 2; ++i)
            QCoreApplication::processEvents();
        tcheck(cnt(stats(2, "#3a2224"), "band") == 0,
               QStringLiteral("endDiff 之后底色擦干净了"),
               QStringLiteral("还剩 %1 个底色像素")
                   .arg(cnt(stats(2, "#3a2224"), "band")));
        tcheck(view->diffGapAt(3) == 0, QStringLiteral("endDiff 之后带子也撤掉了"),
               QStringLiteral("还剩 %1 行").arg(view->diffGapAt(3)));
    } else {
        std::fputs("  skip  没有编辑器实例 / 没有当前文档，这一节跳过\n", stdout);
    }

    /* ------------------------------------------------------------------
     * 7. 对比页（qml/components/DiffPane.qml + Main.qml 那一节）
     *
     * 钉的是"两栏真的对上了没有"。上面第 6 节量的是单个编辑器里空白带撑得起来；
     * 这一节量的是**两份文档一起挂上去之后**，同一处差异在两栏的同一个高度上。
     * 底色、标记位这些在"逻辑对、画出来错开"的情况下照样能全绿，只有 y 量得出来。
     * ------------------------------------------------------------------ */
    if (qmlRoot) {
        std::fputs("\n-- 对比页（两栏对齐） --\n", stdout);

        auto *view = qmlRoot->property("view").value<QObject *>();
        const auto docIdOf = [view]() {
            int id = -1;
            if (view)
                QMetaObject::invokeMethod(view, "currentDocId", Q_RETURN_ARG(int, id));
            return id;
        };
        const auto openFile = [qmlRoot](const QString &path) {
            QMetaObject::invokeMethod(qmlRoot, "openTreeFile", Q_ARG(QVariant, QVariant(path)));
            for (int i = 0; i < 4; ++i)
                QCoreApplication::processEvents();
        };

        /*
         * 两份差"中间插一行 + 后面改一行"的文件，各 120 行。
         *
         * 三个讲究：
         *   * 插在**中间**不是开头 —— 开头那种这一版撑不出空白带
         *     （见 src/Diff.h 里 leadGap 那段），量不到对齐；
         *   * 改的那一行只加两个字符（L118 -> L118x）—— 相似度够高才会被判成
         *     一"处"mod、两边落在同一行上。改成完全不同的内容会退化成
         *     "删一行 + 加一行"，那本来就是两行，量不出对齐；
         *   * 120 行是为了让视口装不下 —— 短于二十行的文件根本滚不动，
         *     同步那两条就会假红。
         */
        QTemporaryDir diffDir;
        const QString pathA = diffDir.filePath(QStringLiteral("a.md"));
        const QString pathB = diffDir.filePath(QStringLiteral("b.md"));
        {
            QString a;
            QString b;
            for (int i = 1; i <= 120; ++i) {
                const QString line = QStringLiteral("L%1 content\n").arg(i);
                a += line;
                if (i == 10)
                    b += QStringLiteral("INSERTED\n");
                b += (i == 118) ? QStringLiteral("L118x content\n") : line;
            }
            QFile fa(pathA);
            if (fa.open(QIODevice::WriteOnly)) {
                fa.write(a.toUtf8());
                fa.close();
            }
            QFile fb(pathB);
            if (fb.open(QIODevice::WriteOnly)) {
                fb.write(b.toUtf8());
                fb.close();
            }
        }

        openFile(pathA);
        const int idA = docIdOf();
        openFile(pathB);
        const int idB = docIdOf();
        tcheck(idA >= 0 && idB >= 0 && idA != idB,
               QStringLiteral("两份文件各自成了一份文档"),
               QStringLiteral("docId %1 / %2").arg(idA).arg(idB));

        QMetaObject::invokeMethod(qmlRoot, "diffOpenForTest",
                                  Q_ARG(QVariant, idA), Q_ARG(QVariant, idB),
                                  Q_ARG(QVariant, QStringLiteral("a.md")),
                                  Q_ARG(QVariant, QStringLiteral("b.md")));
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();

        const auto diffState = [qmlRoot]() {
            QVariant r;
            QMetaObject::invokeMethod(qmlRoot, "diffState", Q_RETURN_ARG(QVariant, r));
            return r.toMap();
        };
        const QVariantMap st = diffState();
        const auto num = [](const QVariant &v, const char *key) {
            return v.toMap().value(QLatin1String(key)).toInt();
        };
        tout(QStringLiteral("mode=%1 tabs=%2 rows=%3 changes=%4 lead=%5/%6  左 %7 行 右 %8 行")
                 .arg(st.value(QStringLiteral("mode")).toBool() ? 1 : 0)
                 .arg(st.value(QStringLiteral("tabs")).toInt())
                 .arg(st.value(QStringLiteral("rows")).toInt())
                 .arg(st.value(QStringLiteral("changes")).toInt())
                 .arg(st.value(QStringLiteral("leadLeft")).toInt())
                 .arg(st.value(QStringLiteral("leadRight")).toInt())
                 .arg(num(st.value(QStringLiteral("left")), "lines"))
                 .arg(num(st.value(QStringLiteral("right")), "lines")));
        tout(QStringLiteral("概况：%1").arg(st.value(QStringLiteral("summary")).toString()));

        tcheck(st.value(QStringLiteral("mode")).toBool(), QStringLiteral("对比页开着"));
        tcheck(st.value(QStringLiteral("tabs")).toInt() == 1,
               QStringLiteral("标签栏上多了一格对比"));
        tcheck(num(st.value(QStringLiteral("left")), "lines") == 121
                   && num(st.value(QStringLiteral("right")), "lines") == 122,
               QStringLiteral("两栏各看自己那份，行数没被对比层改动"),
               QStringLiteral("左 %1 / 右 %2")
                   .arg(num(st.value(QStringLiteral("left")), "lines"))
                   .arg(num(st.value(QStringLiteral("right")), "lines")));
        tcheck(!num(st.value(QStringLiteral("left")), "dirty")
                   && !num(st.value(QStringLiteral("right")), "dirty"),
               QStringLiteral("挂了色块和空白带之后两份文档都还不算改过"));
        tcheck(st.value(QStringLiteral("changes")).toInt() == 2,
               QStringLiteral("认出两处差异（中间插一行 + 后面改一行）"),
               QStringLiteral("实际 %1").arg(st.value(QStringLiteral("changes")).toInt()));
        tcheck(st.value(QStringLiteral("leadLeft")).toInt() == 0
                   && st.value(QStringLiteral("leadRight")).toInt() == 0,
               QStringLiteral("差异不在开头，两边都不欠\"首行之上\"那段空"));

        /*
         * 对齐本身：第二处差异（第 118 行改了）在左栏是第 118 行、右栏是第 119 行，
         * 中间那次插入把左栏第 10 行下面撑了一行高，所以两边的 y 应该相等。
         */
        const auto changeYs = [qmlRoot](int index) {
            QVariant r;
            QMetaObject::invokeMethod(qmlRoot, "diffChangeYs", Q_RETURN_ARG(QVariant, r),
                                      Q_ARG(QVariant, index));
            return r.toMap();
        };
        const QVariantMap ys = changeYs(1);
        const int yL = ys.value(QStringLiteral("left")).toInt();
        const int yR = ys.value(QStringLiteral("right")).toInt();
        tout(QStringLiteral("第二处差异：左栏 y=%1  右栏 y=%2").arg(yL).arg(yR));
        tcheck(yL > 0 && yL == yR,
               QStringLiteral("同一处差异在两栏的同一个高度上（对齐真的生效）"),
               QStringLiteral("%1 vs %2").arg(yL).arg(yR));

        /*
         * 跳到第 2 处（两边都有行、算 mod 的那一处）：整块要涂成亮一档的同族色。
         * 原来这里是一个描边框，实测下边框看不见（那条线正好压在行界上，
         * 被下一行的背景糊掉了），改成亮一档的底色 —— 量的还是"底色 + 笔画"
         * 两个都要有，别又换成一种把字盖住的画法。
         */
        QVariant curPixel;
        QMetaObject::invokeMethod(qmlRoot, "diffGotoForTest", Q_RETURN_ARG(QVariant, curPixel),
                                  Q_ARG(QVariant, 1));
        const QVariantMap curMap = curPixel.toMap();
        tout(QStringLiteral("跳到第 2 处：亮一档底色 %1 像素、笔画 %2 像素、底 %3 像素")
                 .arg(num(curMap, "band")).arg(num(curMap, "glyph")).arg(num(curMap, "paper")));
        /*
         * 这一行是 "L118 content"，一共 12 个字符 —— 底色 + 笔画加起来就是
         * 那一小段文字的宽度，所以门槛按这个样本给（>20 个底色像素），
         * 不是第 6 节那种 120 字的长行。
         */
        tcheck(num(curMap, "band") > 20 && num(curMap, "glyph") > 0,
               QStringLiteral("当前这一处整块亮一档，而且字还看得见"),
               QStringLiteral("底色 %1 / 笔画 %2")
                   .arg(num(curMap, "band")).arg(num(curMap, "glyph")));

        /* 滚动同步：滚左边，右边要跟到同一显示行 */
        const auto scrollOf = [&diffState]() {
            return diffState().value(QStringLiteral("scroll")).toMap();
        };
        const int beforeRight = num(scrollOf(), "right");
        QMetaObject::invokeMethod(qmlRoot, "diffScrollLeftForTest", Q_ARG(QVariant, 3));
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const QVariantMap after = scrollOf();
        tcheck(num(after, "left") == 3,
               QStringLiteral("左栏滚到了第 3 显示行"),
               QStringLiteral("实际 %1").arg(num(after, "left")));
        tcheck(num(after, "right") != beforeRight,
               QStringLiteral("右栏跟着滚了（同步开着）"),
               QStringLiteral("%1 -> %2").arg(beforeRight).arg(num(after, "right")));

        /*
         * 合并：把第 0 处（右边多出来的那一行）用左边的内容换掉 = 删掉它。
         * 钉三件事：差异少了一处、右边那份**真的被改了**（未保存标记亮）、
         * 右边少一行。第二件最要紧 —— 改的是文档本身，不是只把色块抹掉。
         */
        const int changesBefore = st.value(QStringLiteral("changes")).toInt();
        QMetaObject::invokeMethod(qmlRoot, "diffMergeForTest", Q_ARG(QVariant, 1));
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
        const QVariantMap m1 = diffState();
        tout(QStringLiteral("合并 左->右 之后：差异 %1 -> %2  右边 %3 行  未保存标记 %4")
                 .arg(changesBefore)
                 .arg(m1.value(QStringLiteral("changes")).toInt())
                 .arg(num(m1.value(QStringLiteral("right")), "lines"))
                 .arg(m1.value(QStringLiteral("right")).toMap()
                          .value(QStringLiteral("dirty")).toBool() ? 1 : 0));
        tcheck(m1.value(QStringLiteral("changes")).toInt() == changesBefore - 1,
               QStringLiteral("左 → 右：合并掉一处差异"),
               QStringLiteral("%1 -> %2").arg(changesBefore)
                   .arg(m1.value(QStringLiteral("changes")).toInt()));
        tcheck(num(m1.value(QStringLiteral("right")), "lines") == 121,
               QStringLiteral("右边那份真的少了一行（不是只把色块抹掉）"),
               QStringLiteral("现在 %1 行").arg(num(m1.value(QStringLiteral("right")), "lines")));
        tcheck(m1.value(QStringLiteral("right")).toMap()
                   .value(QStringLiteral("dirty")).toBool(),
               QStringLiteral("被改过的那份文档冒出未保存标记"));

        /* 再来一次（这回两边都有行）：改过的那一行搬回左边，两栏就该完全一样 */
        QMetaObject::invokeMethod(qmlRoot, "diffMergeForTest", Q_ARG(QVariant, -1));
        for (int i = 0; i < 6; ++i)
            QCoreApplication::processEvents();
        const QVariantMap m2 = diffState();
        tcheck(m2.value(QStringLiteral("changes")).toInt() == 0,
               QStringLiteral("右 → 左：再合并一次，两处都清完"),
               m2.value(QStringLiteral("summary")).toString());

        /* 关掉会话：对比页收起，两栏身上那层画的东西要还干净 */
        QMetaObject::invokeMethod(qmlRoot, "closeDiffSession", Q_ARG(QVariant, 0));
        for (int i = 0; i < 4; ++i)
            QCoreApplication::processEvents();
        const QVariantMap closed = diffState();
        tcheck(!closed.value(QStringLiteral("mode")).toBool(),
               QStringLiteral("关掉会话之后退出对比页"));
        tcheck(closed.value(QStringLiteral("tabs")).toInt() == 0,
               QStringLiteral("对比标签跟着没了"));
        const QVariantMap cl = closed.value(QStringLiteral("left")).toMap();
        tcheck(num(cl, "gap") == 0,
               QStringLiteral("退出之后左栏第 1 行下面的空白带撤掉了"),
               QStringLiteral("还剩 %1 行").arg(num(cl, "gap")));
        tcheck(num(closed.value(QStringLiteral("left")), "lines") == 121,
               QStringLiteral("退出之后左栏还是那 121 行（文档没被动过）"));

        QMetaObject::invokeMethod(qmlRoot, "saveAll");
        QMetaObject::invokeMethod(qmlRoot, "closeAllTabs", Q_ARG(QVariant, QVariant()));
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
    }

    /* ------------------------------------------------------------------
     * 8. 提示框不许越过窗口右边缘（qml/components/AppToolTip.qml）
     *
     * 原来气泡的 x 是 (父项宽 - 气泡宽)/2 —— 居中。父项贴着窗口右边缘时
     * （标签栏最右边那个"源码 / 预览"开关）右半边就跑到窗口外，被切掉半截。
     * 现在 place() 按窗口边夹一次，这里量夹没夹住。
     * ------------------------------------------------------------------ */
    if (qmlRoot) {
        std::fputs("\n-- 提示框的窗口边界 --\n", stdout);
        QVariant tipVar;
        QMetaObject::invokeMethod(qmlRoot, "tipEdgeProbeState", Q_RETURN_ARG(QVariant, tipVar));
        const QVariantMap tip = tipVar.toMap();
        const double winW = tip.value(QStringLiteral("windowWidth")).toDouble();
        const double left = tip.value(QStringLiteral("tipLeft")).toDouble();
        const double right = tip.value(QStringLiteral("tipRight")).toDouble();
        const double btnRight = tip.value(QStringLiteral("buttonRight")).toDouble();
        const double btnLeft = tip.value(QStringLiteral("buttonLeft")).toDouble();
        tout(QStringLiteral("窗口宽 %1  按钮占 %2..%3  气泡宽 %4  气泡占 %5..%6")
                 .arg(winW).arg(btnLeft).arg(btnRight)
                 .arg(tip.value(QStringLiteral("tipWidth")).toDouble())
                 .arg(left).arg(right));
        tcheck(right <= winW && left >= 0.0,
               QStringLiteral("贴右边缘的气泡不出窗口"),
               QStringLiteral("%1..%2 对 0..%3").arg(left).arg(right).arg(winW));
        /*
         * 夹住之后还得**指着那个按钮**：整块被推到左边去等于换了个地方被切。
         * 口径取按钮的水平中心 —— 气泡贴边时被夹掉的最多就是边缘那几个像素，
         * 要求它连按钮最后一个像素都盖住反而是苛求（真做到反而要压到按钮外边）。
         */
        const double center = (btnLeft + btnRight) / 2.0;
        tcheck(left <= center && right >= center,
               QStringLiteral("夹回来之后气泡仍然指着那个按钮（按钮中心在气泡里）"),
               QStringLiteral("气泡 %1..%2 含按钮中心 %3").arg(left).arg(right).arg(center));
    }

    /* 自检改过的设置按原样放回去 */
    if (savedTool.isValid())
        settings.setValue(QStringLiteral("format/tool/plain"), savedTool);
    else
        settings.remove(QStringLiteral("format/tool/plain"));

    std::fputs("\n", stdout);
    tout(QStringLiteral("工具自检：通过 %1 项，失败 %2 项").arg(gToolPassed).arg(gToolFailed));
    return gToolFailed;
}

int SelfTest::toolsPassed() { return gToolPassed; }
int SelfTest::toolsFailed() { return gToolFailed; }

bool SelfTest::toolTestEnabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--tool-test") == 0)
            return true;
    }
    return false;
}
