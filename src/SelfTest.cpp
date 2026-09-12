#include "SelfTest.h"

#include "ClipboardStore.h"
#include "EditorViewItem.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QTextStream>
#include <QVariant>
#include <QWidget>
#include <cstdio>

namespace {

int gPassed = 0;
int gFailed = 0;

QTextStream &out() {
    static QTextStream stream(stdout);
    return stream;
}

void check(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++gPassed;
        out() << "  ok    " << what << Qt::endl;
    } else {
        ++gFailed;
        out() << "  FAIL  " << what;
        if (!detail.isEmpty())
            out() << "   [" << detail << "]";
        out() << Qt::endl;
    }
}

/*
 * Scintilla 的颜色是 0x00BBGGRR（BGR 打包），这里按同样的规则打包 / 还原，
 * 免得断言里写 #1e1f22 这种"看起来对"的值（见 EditorViewItem.cpp 的 scColor）。
 */
int packed(int r, int g, int b) { return (b << 16) | (g << 8) | r; }
int unpacked(int v) { return ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff); }
QString readFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray bytes = file.readAll();
    file.close();
    return QString::fromUtf8(bytes);
}

QByteArray readBytes(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    const QByteArray bytes = file.readAll();
    file.close();
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    return ok;
}

}  // namespace

bool SelfTest::enabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--self-test"))
            return true;
    }
    return false;
}

int SelfTest::run(QObject *qmlRoot, ClipboardStore *store) {
    EditorViewItem *view = EditorViewItem::instance();

    /*
     * stdout 不缓冲。
     *
     * 自检是"崩了也要知道崩在哪一步"的工具：走默认的全缓冲时，进程一崩，
     * 最后那几行还压在 CRT 缓冲里，日志里什么都看不到（实测过一次 c0000005，
     * 输出文件是空的，只能上调试器）。这里关掉缓冲，每行立刻落盘。
     * 注意：必须留在**第一条输出之前**（setvbuf 要在流被用过之前调用）。
     */
    setvbuf(stdout, nullptr, _IONBF, 0);

    out() << "SmartClip 自检" << Qt::endl;

    if (!view) {
        out() << "  FAIL  编辑器实例不存在（EditorViewItem::instance() 为空）" << Qt::endl;
        return 1;
    }
    if (!qmlRoot) {
        out() << "  FAIL  QML 根对象为空" << Qt::endl;
        return 1;
    }

    /* 通过 QML 的 dispatch 发命令：工具栏 / 菜单 / 快捷键最后都走这条路 */
    auto dispatch = [qmlRoot](const QString &name) {
        QMetaObject::invokeMethod(qmlRoot, "dispatch", Q_ARG(QVariant, name));
    };

    /* 读 QML 侧的绑定状态（见 Main.qml 的 uiState） */
    auto uiState = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "uiState", Q_RETURN_ARG(QVariant, result));
        return result.toMap();
    };

    /* 读 tab 右键菜单的条目清单（见 Main.qml 的 tabMenuActs，和弹出的是同一份构造） */
    auto tabMenuActs = [qmlRoot](int index) {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "tabMenuActs", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant(index)));
        return result.toList();
    };

    /* 读设置菜单的条目清单（见 Main.qml 的 settingsMenuActs，同上） */
    auto settingsMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "settingsMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    /* 读视图菜单的条目清单（见 Main.qml 的 viewMenuActs，同上） */
    auto viewMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "viewMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    QDir dir(QDir::tempPath() + QStringLiteral("/smartclip-selftest"));
    dir.removeRecursively();
    dir.mkpath(QStringLiteral("."));

    const QString srcPath = dir.filePath(QStringLiteral("sample.cpp"));
    const QString outPath = dir.filePath(QStringLiteral("out.cpp"));
    const QString bomPath = dir.filePath(QStringLiteral("bom.txt"));

    /* 正文故意带 CRLF 和中文，覆盖换行符检测与 UTF-8 */
    const QByteArray sample =
        "line one: alpha\r\n"
        "line two: NEEDLE here\r\n"
        "line three: alpha NEEDLE\r\n";
    check(writeFile(srcPath, sample), QStringLiteral("准备测试文件"));

    /* ---- 打开文件 ---- */
    const int opened = view->openFile(srcPath);
    check(opened >= 0, QStringLiteral("openFile() 打开文件"), view->lastError());
    check(view->hasDocument(), QStringLiteral("打开后有当前文档"));
    /*
     * 文件末尾是 CRLF，Scintilla 会把它算成一条空行，所以是 4 行 ——
     * 和所有主流编辑器一致（末尾留一个空行可编辑）。
     */
    check(view->lineCount() == 4, QStringLiteral("行数 = 4（末尾换行算一条空行）"),
          QStringLiteral("实际 %1").arg(view->lineCount()));
    check(view->charCount() == sample.size(),
          QStringLiteral("字符数 = 文件字节数（UTF-8）"),
          QStringLiteral("实际 %1 / 期望 %2").arg(view->charCount()).arg(sample.size()));
    check(view->eolMode() == QLatin1String("CRLF"),
          QStringLiteral("换行符识别为 CRLF"), view->eolMode());
    check(view->language() == QLatin1String("cpp"),
          QStringLiteral("按扩展名识别语言为 cpp"), view->language());
    check(!view->modified(), QStringLiteral("刚打开时没有修改标记"));
    check(view->encoding() == QLatin1String("UTF-8"),
          QStringLiteral("编码识别为 UTF-8"), view->encoding());

    /*
     * 装了语法高亮 lexer 之后，样式字号必须还是 uiFont() 那一份（Scintilla 里是
     * "点 ×100"），既不能变成负值，也不能跟正文/行号栏不一致。
     *
     * 踩过：给 lexer 的字体用 QFont::setPixelSize() 造，pointSizeF() 是 -1，
     * QScintilla 会把它 *100 发给 SCI_STYLESETSIZEFRACTIONAL → 字号变成 -100，
     * 表现就是"一切换语言，正文小到看不见"（见 EditorViewItem::uiFont）。
     */
    check(view->styleSize(0) == view->stylePointSize() && view->styleSize(0) > 0,
          QStringLiteral("cpp 高亮下样式 0 的字号跟正文一致"),
          QStringLiteral("实际 %1（应为 %2）").arg(view->styleSize(0)).arg(view->stylePointSize()));
    check(view->styleSize(1) == view->stylePointSize(),
          QStringLiteral("cpp 高亮下样式 1（注释）的字号跟正文一致"),
          QStringLiteral("实际 %1（应为 %2）").arg(view->styleSize(1)).arg(view->stylePointSize()));
    check(view->styleSize(33) == view->stylePointSize(),   // 33 = STYLE_LINENUMBER
          QStringLiteral("行号栏的字号跟正文一致"),
          QStringLiteral("实际 %1（应为 %2）").arg(view->styleSize(33)).arg(view->stylePointSize()));

    /*
     * "设置里写 12"必须就是"12 像素"，不是 12 点。
     *
     * 踩过：直接用 setPointSize(12) 给 QFont，96 DPI 下渲染出来是 16 像素 ——
     * 设置写着 12，字却比界面上别处的 12 大一圈。换算回像素来卡这一条。
     */

    /*
     * 量字号之前先把行高按回 1.0 倍。
     *
     * 行高倍数存在 QSettings 里，自检启动时 Main.qml 会把它恢复出来 ——
     * 用户上次设成 1.5 倍的话，下面"行高跟字号一个量级"那条就会红
     * （12px 的字量出 23px 的行高）。那条要钉的是"字号单位没被当成点"，
     * 得在没有额外行距的前提下量。用户那份设置在本节末尾还原回去。
     */
    const qreal savedLineHeightFactor = view->lineHeightFactor();
    if (!qFuzzyCompare(savedLineHeightFactor, 1.0)) {
        view->setLineHeightFactor(1.0);
        QCoreApplication::processEvents();
    }

    out() << "        （字号换算：设置 " << view->fontPixelSize() << " px → 实际 "
          << view->fontPixelSizeEffective() << " px；行高 " << view->textLineHeight()
          << " px）" << Qt::endl;
    check(qAbs(view->fontPixelSizeEffective() - double(view->fontPixelSize())) < 0.51,
          QStringLiteral("字号单位是像素（设置 12 = 渲染 12px）"),
          QStringLiteral("设置 %1 px / 实际 %2 px")
              .arg(view->fontPixelSize())
              .arg(view->fontPixelSizeEffective()));

    /*
     * 再卡一道不绕圈的：真渲染出来的行高得跟字号是一个量级。
     *
     * 上面那条"换算回像素"用的是同一份换算，等于自己证明自己；这条量的是
     * Scintilla 按当前字体算出来的行高 —— 12pt 的行高在 20px 上下，12px 的
     * 只有 15~17px。单位再被写错（把像素当点）能当场抓住。
     */
    check(view->textLineHeight() > 0 && view->textLineHeight() <= view->fontPixelSize() + 6,
          QStringLiteral("行高跟字号一个量级（没按点当成像素画大）"),
          QStringLiteral("行高 %1 px / 字号 %2 px")
              .arg(view->textLineHeight()).arg(view->fontPixelSize()));

    /*
     * 行高倍数（设置菜单 / 设置面板里的"行高"）。
     *
     * Scintilla 没有"把行高设成 N 像素"的消息，只有给每行加**额外上下空白**
     * （SCI_SETEXTRAASCENT / SCI_SETEXTRADESCENT，见 ViewStyle::Refresh：
     * lineHeight = maxAscent + maxDescent + extraAscent + extraDescent），
     * 所以按"自然行高 × 倍数"算差额再摊到上下两侧。这里量三件事：
     *   * 1.0 倍时实际行高就是自然行高（没偷偷加空）；
     *   * 1.5 倍确实高了一档，且等于自然行高 × 1.5（取整 ±1px）；
     *   * 调回 1.0 倍又回到自然行高（不是只能往松里走）。
     */
    {
        const int natural = view->naturalLineHeight();
        const int plain = view->lineHeight();
        check(natural > 0 && natural == plain,
              QStringLiteral("行高 1.0 倍 = 字体自带的自然行高（没额外加空）"),
              QStringLiteral("自然 %1 px / 实际 %2 px").arg(natural).arg(plain));

        view->setLineHeightFactor(1.5);
        QCoreApplication::processEvents();
        const int wide = view->lineHeight();
        out() << "        （行高：1.0 倍 " << natural << " px → 1.5 倍 " << wide
              << " px）" << Qt::endl;
        check(qAbs(wide - qRound(natural * 1.5)) <= 1,
              QStringLiteral("行高 1.5 倍按自然行高成比例拉开"),
              QStringLiteral("实际 %1 px（应为 %2 px）").arg(wide).arg(qRound(natural * 1.5)));

        view->setLineHeightFactor(1.0);
        QCoreApplication::processEvents();
        check(view->lineHeight() == natural,
              QStringLiteral("行高调回 1.0 倍后回到自然值（不用重开标签）"),
              QStringLiteral("实际 %1 px（应为 %2 px）").arg(view->lineHeight()).arg(natural));

        /*
         * 菜单里那一组"行高"（档位表在 js/EditorMenus.js 的 kLineHeightFactors）：
         * 档位数、档位边界、以及**点下去真的会改行高** —— 三样一起钉。
         * 条目清单由 Main.qml 的 settingsMenuActs 提供，和点"设置"弹出的那份
         * 是同一个调用，所以断言看到的就是菜单里能点的。
         */
        {
            QStringList factors;
            const QVariantList items = settingsMenuActs();
            for (const QVariant &item : items) {
                const QString act = item.toString();
                if (act.startsWith(QStringLiteral("lineHeight:")))
                    factors << act.mid(int(qstrlen("lineHeight:")));
            }
            check(factors.size() == 7,
                  QStringLiteral("设置菜单里有 7 档行高（含\"跟随字体\"）"),
                  QStringLiteral("实际 %1 档：%2")
                      .arg(factors.size()).arg(factors.join(QLatin1Char('/'))));
            check(factors.value(0) == QStringLiteral("1")
                      && factors.value(1) == QStringLiteral("1.15")
                      && factors.contains(QStringLiteral("1.5"))
                      && factors.contains(QStringLiteral("2.5")),
                  QStringLiteral("档位从 1.0 起、最松 2.5 倍（没有比 1.0 更紧的档）"),
                  factors.join(QLatin1Char('/')));

            /* 点 1.15 倍那一档，走的就是菜单条目的 act */
            dispatch(QStringLiteral("lineHeight:1.15"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 1.15) < 0.001,
                  QStringLiteral("dispatch(lineHeight:1.15) 改到编辑器上了"),
                  QStringLiteral("实际 %1（应为 1.15）").arg(view->lineHeightFactor()));
            check(view->lineHeight() > natural,
                  QStringLiteral("这一档确实比自然行高松"),
                  QStringLiteral("实际 %1 px / 自然 %2 px")
                      .arg(view->lineHeight()).arg(natural));

            /*
             * 设置面板上那两个按钮（"行高 − / 行高 +"）：走 lineHeightUp /
             * lineHeightDown，在档位表里前后走一格 —— 1.15 上一格是 1.3，
             * 再下一格又回到 1.15。
             */
            dispatch(QStringLiteral("lineHeightUp"));
            QCoreApplication::processEvents();
            const qreal up = view->lineHeightFactor();
            check(qAbs(up - 1.3) < 0.001,
                  QStringLiteral("行高 + 走到下一档（1.15 → 1.3）"),
                  QStringLiteral("实际 %1").arg(up));
            dispatch(QStringLiteral("lineHeightDown"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 1.15) < 0.001,
                  QStringLiteral("行高 − 走回上一档（1.3 → 1.15）"),
                  QStringLiteral("实际 %1").arg(view->lineHeightFactor()));

            /* 两头要夹住：最松一档再 +、最紧一档再 − 都不许跑出表外 */
            view->setLineHeightFactor(2.5);
            dispatch(QStringLiteral("lineHeightUp"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 2.5) < 0.001,
                  QStringLiteral("最松一档再按\"行高 +\"就停在原地（不越界）"),
                  QStringLiteral("实际 %1").arg(view->lineHeightFactor()));
            view->setLineHeightFactor(1.0);
            dispatch(QStringLiteral("lineHeightDown"));
            QCoreApplication::processEvents();
            check(qAbs(view->lineHeightFactor() - 1.0) < 0.001,
                  QStringLiteral("最紧一档（跟随字体）再按\"行高 −\"也停在原地"),
                  QStringLiteral("实际 %1").arg(view->lineHeightFactor()));
        }

        /* 自检不该把用户调好的行高改掉，最后统一还原 */
        view->setLineHeightFactor(savedLineHeightFactor);
        QCoreApplication::processEvents();
    }

    /*
     * 注释可以单独设字号（设置菜单里的"注释字号"）。
     *
     * 注释样式是 lexer 按 description 里的 "comment" 单独标出来的，所以能给它
     * 一套自己的字号 —— 这里设成 10px，注释样式(1) 应该变成 10px 的点数，
     * 而正文样式(0) 一个像素都不许动。
     */
    {
        view->setCommentFontPixelSize(10);
        QCoreApplication::processEvents();
        const int commentStyle = view->styleSize(1);
        const int bodyStyle = view->styleSize(0);
        const int wantComment = view->styleCommentPointSize();

        out() << "        （注释字号 10px → 样式 " << commentStyle << "（应为 " << wantComment
              << "）；正文样式 " << bodyStyle << "）" << Qt::endl;

        check(commentStyle == wantComment && bodyStyle == view->stylePointSize(),
              QStringLiteral("注释字号能单独设（正文不动）"),
              QStringLiteral("注释 %1 / 正文 %2").arg(commentStyle).arg(bodyStyle));
        check(commentStyle < bodyStyle,
              QStringLiteral("注释字号确实变小了（10px < 12px）"),
              QStringLiteral("注释 %1 / 正文 %2").arg(commentStyle).arg(bodyStyle));

        view->setCommentFontPixelSize(0);
        QCoreApplication::processEvents();
        check(view->styleSize(1) == view->stylePointSize(),
              QStringLiteral("注释字号设回 0 后跟着正文"),
              QStringLiteral("注释 %1 / 正文 %2")
                  .arg(view->styleSize(1)).arg(view->stylePointSize()));
    }

    /* 字体家族能改（"中英文分开设字体"做不到，但能整体换成中英都覆盖的等宽字体） */
    {
        const QString original = view->fontFamily();
        /*
         * 家族名必须用**英文**（NSimSun）：QScintilla 转发给 Scintilla 时走的是
         * QFont::family().toLatin1()，中文名会被压成 "???" —— 这条自检就是按
         * 这个坑写的（正文样式由 lexer 转发，行号栏样式是我们自己发 toUtf8）。
         */
        view->setFontFamily(QStringLiteral("NSimSun"));
        QCoreApplication::processEvents();
        check(view->styleFontName(0) == QStringLiteral("NSimSun")
              && view->styleFontName(33) == QStringLiteral("NSimSun"),   // 33 = STYLE_LINENUMBER
              QStringLiteral("字体家族改得动（正文和行号栏一起换）"),
              QStringLiteral("实际 '%1' / '%2'")
                  .arg(view->styleFontName(0), view->styleFontName(33)));

        view->setFontFamily(original);
        QCoreApplication::processEvents();
        check(view->styleFontName(0) == original,
              QStringLiteral("字体家族改回原样"), view->styleFontName(0));
    }

    /*
     * 行号栏不能变白。
     *
     * SC_MARGIN_NUMBER 的背景取的是 STYLE_LINENUMBER 的 paper，而装 lexer 时
     * QScintilla 的 detachLexer() 会 SCI_STYLECLEARALL 把它刷回默认（白底黑字）——
     * 实测过"一切换语言，行号栏变成一条白带"。所以装完 lexer 必须重刷。
     */
    check(view->styleBack(33) == packed(0x1e, 0x1f, 0x22),   // 33 = STYLE_LINENUMBER
          QStringLiteral("装 lexer 后行号栏底色仍是编辑区底色（不是白的）"),
          QStringLiteral("实际 #%1").arg(unpacked(view->styleBack(33)), 6, 16, QLatin1Char('0')));
    check(view->marginBack(0) == packed(0x1e, 0x1f, 0x22)
          && view->marginBack(1) == packed(0x1e, 0x1f, 0x22),
          QStringLiteral("边距背景也压成编辑区底色（行号栏 / 折叠栏）"),
          QStringLiteral("margin0=#%1 margin1=#%2")
              .arg(view->marginBack(0), 6, 16, QLatin1Char('0'))
              .arg(view->marginBack(1), 6, 16, QLatin1Char('0')));

    /* 折叠边距在（第 1 列有宽度，夹在行号和分隔线之间） */
    check(view->marginWidth(1) > 0,
          QStringLiteral("折叠边距已启用（第 1 列有宽度，在行号右边）"),
          QStringLiteral("实际 %1").arg(view->marginWidth(1)));

    /*
     * 折叠栏宽度 = 箭头宽 + 左右各 5px。
     *
     * 位图标记是画在边距正中的（PlatQt.cpp 的 DrawXPM），所以"左右各 5px"落到
     * 宽度上就是 图标边长 + 10；曾经 14px 的默认宽度（QScintilla 那个
     * defaultFoldMarginWidth）就是在这里被卡住的。
     */
    {
        const int expected = view->foldIconSize() + 10;
        check(qAbs(view->marginWidth(1) - expected) <= 1,
              QStringLiteral("折叠栏宽度 = 尖括号宽 + 左右各 5px"),
              QStringLiteral("图标 %1 → 期望 %2，实际 %3")
                  .arg(view->foldIconSize()).arg(expected).arg(view->marginWidth(1)));
    }

    /*
     * 尖括号的方向：折叠态向右 "›"、展开态向下 "⌄"。
     *
     * 位图标记的形状没法从 Scintilla 那边读回来，所以直接量自己画的那两张图：
     * 向右的竖着比横着长，向下的横着比竖着长。方框标记（原来那套 +/- 方块）
     * 在这里是正方的，这条能当场抓住"又退回方框了"。
     */
    {
        const QVariantList icon = view->foldIconPixelStats();
        const int cw = icon.value(0).toInt(), ch = icon.value(1).toInt();
        const int ow = icon.value(2).toInt(), oh = icon.value(3).toInt();
        out() << "        （折叠箭头墨迹：折叠态 " << cw << "x" << ch
              << "，展开态 " << ow << "x" << oh << "）" << Qt::endl;
        check(cw > 0 && ch > cw,
              QStringLiteral("折叠状态画成向右的尖括号 ›（竖着比横着长）"),
              QStringLiteral("实际 %1x%2").arg(cw).arg(ch));
        check(ow > oh,
              QStringLiteral("展开状态画成向下的尖括号 ⌄（横着比竖着长）"),
              QStringLiteral("实际 %1x%2").arg(ow).arg(oh));
    }

    /*
     * 紧贴正文左边那条分隔竖线（第 2 条边距，也是最后一条）。
     *
     * 宽度 1px、底色是**分隔色**而不是编辑区底色 —— 边距背景整列一次填满，
     * 所以它是一条从顶到底的竖线。放在最后一条边距上，正文左边缘就在它右边
     * （正文左边缘 = 各条边距宽之和 + 左留白），也就是线和正文之间不再有空档。
     */
    check(view->marginWidth(2) == 1 && view->marginBack(2) == packed(0x33, 0x38, 0x40),
          QStringLiteral("正文左边有 1px 分隔竖线（第 2 列，正文紧贴着它）"),
          QStringLiteral("宽 %1 / 底色 #%2")
              .arg(view->marginWidth(2))
              .arg(unpacked(view->marginBack(2)), 6, 16, QLatin1Char('0')));

    /*
     * 这两条竖线的开关和列号都存在 QSettings 里，自检启动时 Main.qml 会把用户
     * 上次设的那份恢复出来（实测：用户把参考线点成 120 字之后，"默认 80"那条
     * 断言就红了 —— 量到的是用户的设置，不是出厂值）。所以下面先把用户那份存
     * 起来，用一组确定的值跑断言，最后原样放回去：自检不改用户的设置。
     */
    const bool savedGutterLine = view->gutterLineVisible();
    const bool savedRulerVisible = view->rulerVisible();
    const int savedRulerColumn = view->rulerColumn();
    view->setGutterLineVisible(true);
    view->setRulerVisible(true);
    view->setRulerColumn(80);
    QCoreApplication::processEvents();

    dispatch(QStringLiteral("toggleGutterLine"));
    check(view->marginWidth(2) == 0,
          QStringLiteral("dispatch(toggleGutterLine) 把分隔线关掉"),
          QStringLiteral("实际宽 %1").arg(view->marginWidth(2)));
    dispatch(QStringLiteral("toggleGutterLine"));
    check(view->marginWidth(2) == 1 && view->gutterLineVisible(),
          QStringLiteral("再切一次分隔线回来"));

    /*
     * 字数参考线（"一行 80 字"那条竖线）。
     *
     * 两件事一起钉：Scintilla 那边的 edge 状态（模式 / 列号 / 颜色），以及
     * **画出来的像素位置** —— 抓图里扫那条线，跟"列号 × 空格宽 + 正文左边缘"
     * 对一下。只看列号的话，"消息发下去了但线没画出来"是查不到的。
     */
    {
        /*
         * 量像素得有文档：一个标签都没开时视图挂的是 scratch 占位文档、编辑区
         * 不显示，抓出来的图是空的。这里自己开一个；后面那些断言用的 before
         * 计数在更靠后的位置取，不受影响。
         */
        if (!view->hasDocument())
            dispatch(QStringLiteral("new"));

        /*
         * 分隔线"真的画出来了"这一条要用像素说话：它占的是第 1 条边距的那 1 像素，
         * 底色是分隔色，边距背景整列填满，所以抓图里应该数得到接近控件高度那么多个
         * 点；关掉开关就一个都不剩。
         */
        {
            const QVariantList on = view->marginPixelStats();
            dispatch(QStringLiteral("toggleGutterLine"));
            const QVariantList off = view->marginPixelStats();
            dispatch(QStringLiteral("toggleGutterLine"));
            out() << "        （分隔竖线像素：开着 " << on.value(3).toInt() << "（第 "
                  << on.value(10).toInt() << " 列起）/ 关掉 " << off.value(3).toInt()
                  << "；三条边距宽 " << on.value(7).toInt() << "+" << on.value(8).toInt()
                  << "+" << on.value(9).toInt()
                  << "；线到正文 " << on.value(11).toInt() << " px）" << Qt::endl;
            check(on.value(3).toInt() > 100 && off.value(3).toInt() == 0,
                  QStringLiteral("那条分隔线真的画出来了（关掉就一个像素都没有）"),
                  QStringLiteral("开着 %1 / 关掉 %2")
                      .arg(on.value(3).toInt()).arg(off.value(3).toInt()));

            /*
             * 线和正文之间不能有空档。
             *
             * 这是用户明确提过的那条：折叠栏（14px）原来夹在分隔线和正文之间，
             * 加上 12px 左留白，线和字之间空了 26px。现在折叠栏在线**左边**、
             * 左留白收到 2px，这个距离应该只剩个位数（字形的左侧留白也要算）。
             * 上界给 12 设备像素：26px 那个版本在 125% 缩放下量出来是 30 以上，
             * 这条能当场抓住"折叠栏又跑回线右边"。
             */
            check(on.value(11).toInt() >= 0 && on.value(11).toInt() <= 12,
                  QStringLiteral("分隔线和正文之间没有空档（折叠栏在线左边）"),
                  QStringLiteral("实测 %1 设备像素（-1 = 没扫到正文墨迹）")
                      .arg(on.value(11).toInt()));
        }

        /*
         * 菜单里得能点到：开关在"视图"菜单，列号档位在"设置"菜单。
         * 断言读的是和弹出来那份同一个构造（Main.qml 的 viewMenuActs /
         * settingsMenuActs），所以"菜单里真的有这一条"是被钉住的。
         */
        {
            QStringList acts;
            for (const QVariant &item : viewMenuActs())
                acts << item.toString();
            check(acts.contains(QStringLiteral("toggleGutterLine"))
                      && acts.contains(QStringLiteral("toggleRuler")),
                  QStringLiteral("视图菜单里有那两条竖线的开关"),
                  acts.join(QLatin1Char('/')));
        }
        {
            QStringList acts, cols;
            for (const QVariant &item : settingsMenuActs()) {
                const QString act = item.toString();
                acts << act;
                if (act.startsWith(QStringLiteral("rulerColumn:")))
                    cols << act.mid(int(qstrlen("rulerColumn:")));
            }
            check(cols.size() == 5 && cols.contains(QStringLiteral("80"))
                      && cols.contains(QStringLiteral("100")),
                  QStringLiteral("设置菜单里有 5 档参考线列号（含 80 与 100）"),
                  cols.join(QLatin1Char('/')));
            check(acts.contains(QStringLiteral("rulerColumnAsk")),
                  QStringLiteral("设置菜单里有\"自定义…\"（弹整数输入框）"));
        }

        check(view->rulerEdgeMode() == 1 && view->rulerEdgeColumn() == 80,
              QStringLiteral("列号设成 80 后 Scintilla 那边就是第 80 列（EDGE_LINE）"),
              QStringLiteral("模式 %1 / 列号 %2")
                  .arg(view->rulerEdgeMode()).arg(view->rulerEdgeColumn()));
        check(view->rulerEdgeColor() == packed(0x4b, 0x51, 0x5a),
              QStringLiteral("参考线用的是主题里的颜色（不是 Scintilla 默认）"),
              QStringLiteral("实际 #%1")
                  .arg(unpacked(view->rulerEdgeColor()), 6, 16, QLatin1Char('0')));

        QVariantList rulerPixels = view->rulerPixelStats();
        out() << "        （参考线：扫到 x=" << rulerPixels.value(0).toInt()
              << "，按 80 字算出来应为 x=" << rulerPixels.value(1).toInt() << "）"
              << Qt::endl;
        check(rulerPixels.value(0).toInt() >= 0
                  && qAbs(rulerPixels.value(0).toInt() - rulerPixels.value(1).toInt()) <= 3,
              QStringLiteral("那条线真的画在第 80 个字的位置上"),
              QStringLiteral("扫到 x=%1 / 应为 x=%2")
                  .arg(rulerPixels.value(0).toInt()).arg(rulerPixels.value(1).toInt()));

        /* 改列号：菜单里"100 字"那一档，act 就是 "rulerColumn:100" */
        dispatch(QStringLiteral("rulerColumn:100"));
        check(view->rulerColumn() == 100 && view->rulerEdgeColumn() == 100,
              QStringLiteral("dispatch(rulerColumn:100) 改列号"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        rulerPixels = view->rulerPixelStats();
        check(rulerPixels.value(0).toInt() >= 0
                  && qAbs(rulerPixels.value(0).toInt() - rulerPixels.value(1).toInt()) <= 3,
              QStringLiteral("列号改到 100 之后线跟着挪（位置仍然对得上）"),
              QStringLiteral("扫到 x=%1 / 应为 x=%2")
                  .arg(rulerPixels.value(0).toInt()).arg(rulerPixels.value(1).toInt()));

        /*
         * 越界值两道都堵：QML 的 dispatch 直接丢掉不合法的，C++ 的 setter 再夹一道
         * （设置文件是手改得动的，那边不夹的话 0 或者几十万这种值会顶到画面外）。
         */
        dispatch(QStringLiteral("rulerColumn:0"));
        check(view->rulerColumn() == 100,
              QStringLiteral("非法列号（0）被 dispatch 丢掉（要关这条线用开关，不用 0）"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        view->setRulerColumn(0);
        check(view->rulerColumn() == 1, QStringLiteral("C++ 侧把 0 夹到 1"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        view->setRulerColumn(99999);
        check(view->rulerColumn() == 2000, QStringLiteral("C++ 侧把超大值夹到 2000"),
              QStringLiteral("实际 %1").arg(view->rulerColumn()));
        view->setRulerColumn(80);

        /* 关掉之后抓图里要扫不到那条线，开回来又要有 */
        dispatch(QStringLiteral("toggleRuler"));
        check(!view->rulerVisible() && view->rulerEdgeMode() == 0,
              QStringLiteral("dispatch(toggleRuler) 关掉参考线（EDGE_NONE）"),
              QStringLiteral("模式 %1").arg(view->rulerEdgeMode()));
        check(view->rulerPixelStats().value(0).toInt() < 0,
              QStringLiteral("关掉之后抓图里扫不到那条线"),
              QStringLiteral("扫到 x=%1").arg(view->rulerPixelStats().value(0).toInt()));
        dispatch(QStringLiteral("toggleRuler"));
        check(view->rulerVisible() && view->rulerEdgeMode() == 1,
              QStringLiteral("再切一次参考线回来"));

        /* 自检不改用户的设置：把开头存的那份放回去 */
        view->setGutterLineVisible(savedGutterLine);
        view->setRulerVisible(savedRulerVisible);
        view->setRulerColumn(savedRulerColumn);
        QCoreApplication::processEvents();
        out() << "        （用户设置已还原：分隔线 "
              << (savedGutterLine ? "开" : "关") << " / 参考线 "
              << (savedRulerVisible ? "开" : "关") << " / 列号 " << savedRulerColumn
              << "）" << Qt::endl;
    }

    /*
     * 注：这里原本还有"光标行行号高亮"的断言（逐行边距样式 / 强调色样式），
     * 已随功能一起去掉 —— 数字边距不认逐行样式；改用文本边距那条路实测会让
     * QML 侧认不到编辑区对象（改一次字号就"失联"），也没留下。详见
     * EditorViewItem::applyMargins() 里的注释。
     */

    /*
     * 切到"纯文本"之后，正文底色必须还是深色。
     *
     * 踩过（用户报的"选 txt 之后字体全变成白底"）：QScintilla 的 detachLexer()
     * 先发 SCI_STYLERESETDEFAULT —— 那一步把 STYLE_DEFAULT 打回 Scintilla 的内置
     * 默认（白底黑字），紧接着的 SCI_STYLECLEARALL 再把它刷到所有样式号。
     * 装 lexer 的语言不怕：随后 themeLexer() 会把每个样式的底色重设一遍；
     * 纯文本没有 lexer，就没人再压回来 —— 整篇变白底。
     */
    {
        view->setLanguage(QStringLiteral("plain"));
        QCoreApplication::processEvents();
        const int back0 = view->styleBack(0);
        const int back5 = view->styleBack(5);
        const int back17 = view->styleBack(17);
        const int paper = packed(0x1e, 0x1f, 0x22);

        out() << "        （纯文本样式底色 0=#" << QString::number(back0, 16) << " 5=#"
              << QString::number(back5, 16) << " 17=#" << QString::number(back17, 16) << "）"
              << Qt::endl;
        check(back0 == paper && back5 == paper && back17 == paper,
              QStringLiteral("切到纯文本后正文底色仍是深色（不是白底）"),
              QStringLiteral("0=#%1 5=#%2 17=#%3")
                  .arg(back0, 6, 16, QLatin1Char('0'))
                  .arg(back5, 6, 16, QLatin1Char('0'))
                  .arg(back17, 6, 16, QLatin1Char('0')));

        /*
         * 再来一条看画面的：正文区里不许出现纯白像素。
         *
         * 样式表对了不等于画出来就对 —— 用户看到的是"字后面一块白"，所以直接
         * 抓控件图数 #ffffff 附近的像素（正文/底色的混色都在 250 以下）。
         */
        const int whitePx = view->whiteBackgroundPixels();
        out() << "        （纯文本正文区纯白像素 " << whitePx << "）" << Qt::endl;
        check(whitePx == 0, QStringLiteral("切到纯文本后画面里没有白色背景块"),
              QStringLiteral("%1 个纯白像素").arg(whitePx));

        view->setLanguage(QStringLiteral("cpp"));
        QCoreApplication::processEvents();
    }

    /*
     * 再确认一遍"渲染出来"的大小：同一篇正文，装着 lexer 和切成纯文本，行高
     * 得是一个量级。字号被带坏成负值的话，这里会直接塌成 0 或个位数。
     *
     * 允许 ±2px：行高是 Scintilla 按"那一行实际用到的字体"算的，lexer 会给
     * 关键字/预处理行套粗体、注释套斜体，同一字号下这些字形的行高本身就能
     * 差 1px（实测 cpp 15px / 纯文本 16px，属于字体 hinting 的正常抖动）。
     */
    {
        const int withLexer = view->textLineHeight();
        const int guidesWithLexer = view->marginPixelStats().value(12).toInt();
        view->setLanguage(QStringLiteral("plain"));
        QCoreApplication::processEvents();
        const int plain = view->textLineHeight();
        const int guidesPlain = view->marginPixelStats().value(12).toInt();
        view->setLanguage(QStringLiteral("cpp"));
        QCoreApplication::processEvents();

        out() << "        （cpp 高亮行高 " << withLexer << " px / 参考线 " << guidesWithLexer
              << " px ；纯文本行高 " << plain << " px / 参考线 " << guidesPlain
              << " px）" << Qt::endl;

        check(plain > 0 && withLexer > 0 && qAbs(withLexer - plain) <= 2,
              QStringLiteral("装/不装语法高亮时行高一致（没塌掉）"),
              QStringLiteral("%1 px vs %2 px").arg(withLexer).arg(plain));
    }

    /*
     * 光标行的文字必须画在当前行底色**之上**。
     *
     * 这是个纯渲染问题，属性值看着都对，只能抓图数像素：
     * 光标停在第 2 行和停在第 1 行时，整个编辑区的文字像素数应该几乎一样
     * （正文没变，只差一根光标线）。如果第 2 行的文字被当前行底色盖住，
     * 前者会明显少一截。
     *
     * 曾经踩过：SCI_SETCARETLINEBACKALPHA 传了 255。Scintilla 只有在
     * alpha == SC_ALPHA_NOALPHA(256) 时才把当前行底色当背景画在文字下面，
     * 其余值都是"画完文字再叠一层半透明色"，255 就等于把整行文字糊掉。
     */
    {
        /*
         * 当前行底色的 alpha 必须是 256（SC_ALPHA_NOALPHA），不能是 255。
         *
         * 写成 255 时 Scintilla 会把当前行底色叠在文字**上面**，那一行的字就
         * 全糊掉了 —— 表现就是"光标移到哪一行，哪一行的字看不见"。
         * 这个坑名字很像："不透明"是 SC_ALPHA_OPAQUE(255)，而这里要的是
         * "不要 alpha 通道" SC_ALPHA_NOALPHA(256)，两者只差 1。
         *
         * 注意只能这么查：把控件 grab() 成图去数文字像素**看不出**这个问题，
         * 实测抓图里根本没有当前行那层底色（alwaysVisible=1 也一样），
         * 得在真实窗口上截屏才量得到。见 EditorViewItem.cpp 里那段注释。
         */
        check(view->caretLineAlpha() == 256,
              QStringLiteral("当前行底色 alpha = 256（SC_ALPHA_NOALPHA，不是 255）"),
              QStringLiteral("实际 %1").arg(view->caretLineAlpha()));

        /*
         * 当前行底色**只铺到"文本区"右边**：Scintilla 把它当正文段的底色画
         * （EditView::DrawBackground），文本区 = 编辑器宽 - marginRight。
         * 所以右边距必须是 0，否则那层底色会在离卡片右边缘十几像素的地方
         * 断掉 —— 表现就是"当前行有背景，但背景右边空一块"（用户报的）。
         * 这条只能靠属性钉住：控件 grab() 出来的图里根本没有这层底色（见上面）。
         */
        check(view->paddingRight() == 0,
              QStringLiteral("当前行底色一直铺到编辑器右边缘（正文区右边距 = 0）"),
              QStringLiteral("实际 %1").arg(view->paddingRight()));
    }

    /* ---- 查找 ---- */
    const int hitLine = view->find(QStringLiteral("NEEDLE"), true, false, false, true);
    check(hitLine == 2, QStringLiteral("find() 命中第 2 行"),
          QStringLiteral("实际 %1").arg(hitLine));
    check(view->selectionLength() == 6, QStringLiteral("命中后选中 6 个字符"),
          QStringLiteral("实际 %1").arg(view->selectionLength()));

    const int matches =
        view->highlightMatches(QStringLiteral("NEEDLE"), true, false, false);
    check(matches == 2, QStringLiteral("highlightMatches() = 2 处"),
          QStringLiteral("实际 %1").arg(matches));
    view->clearHighlights();

    /* ---- 全部替换 ---- */
    const int replaced =
        view->replaceAll(QStringLiteral("NEEDLE"), QStringLiteral("PIN"), true, false, false);
    check(replaced == 2, QStringLiteral("replaceAll() 替换 2 处"),
          QStringLiteral("实际 %1").arg(replaced));
    check(view->modified(), QStringLiteral("替换后是已修改状态"));
    check(view->currentText().contains(QStringLiteral("PIN here")),
          QStringLiteral("正文里出现替换结果"));

    /* ---- 撤销 ---- */
    view->undo();
    check(view->currentText().contains(QStringLiteral("NEEDLE here")),
          QStringLiteral("undo() 撤销了全部替换"));

    /* ---- 注释切换 ---- */
    view->selectAll();
    view->toggleComment(QStringLiteral("//"));
    const QString commented = view->currentText();
    check(commented.count(QStringLiteral("//")) == 3,
          QStringLiteral("toggleComment() 给 3 行都加了注释"),
          QStringLiteral("实际 %1 个 //").arg(commented.count(QStringLiteral("//"))));
    view->selectAll();
    view->toggleComment(QStringLiteral("//"));
    check(!view->currentText().contains(QStringLiteral("//")),
          QStringLiteral("再切一次注释被去掉"));

    /* ---- 换行符转换 + 另存为 ---- */
    view->setEolMode(QStringLiteral("LF"));
    check(view->eolMode() == QLatin1String("LF"), QStringLiteral("换行符切到 LF"));
    view->setEncoding(QStringLiteral("UTF-8 BOM"));
    const bool saved = view->saveCurrentAs(outPath);
    check(saved, QStringLiteral("另存为成功"), view->lastError());
    check(!view->modified(), QStringLiteral("保存后修改标记清掉"));

    const QByteArray written = readBytes(outPath);
    check(written.startsWith("\xEF\xBB\xBF"), QStringLiteral("UTF-8 BOM 写进文件"));
    check(!written.contains('\r'), QStringLiteral("保存后没有 CR"));
    check(QString::fromUtf8(written.mid(3)).contains(QStringLiteral("NEEDLE here")),
          QStringLiteral("落盘内容正确"));
    check(view->filePath() == QFileInfo(outPath).absoluteFilePath(),
          QStringLiteral("当前文件路径已更新"));

    /* ---- 编码切换 ---- */
    view->setEncoding(QStringLiteral("UTF-8"));
    check(view->saveCurrent(), QStringLiteral("改回 UTF-8 后直接保存"));
    check(!readBytes(outPath).startsWith("\xEF\xBB\xBF"),
          QStringLiteral("重新保存后 BOM 消失"));

    /* ---- 写入失败要报错，不能静默 ---- */
    view->lastError();
    check(!view->saveCurrentAs(QStringLiteral("Z:/nope/deep/none.txt")),
          QStringLiteral("写入不存在的路径返回 false"));
    check(!view->lastError().isEmpty(), QStringLiteral("失败时 lastError 有内容"));

    /* ---- 多标签 ---- */
    const int before = view->documents().size();
    const int second = view->newDocument();
    check(view->documents().size() == before + 1, QStringLiteral("newDocument() 多出一个标签"));
    check(view->currentIndex() == second, QStringLiteral("新文档成为当前标签"));
    check(!view->modified(), QStringLiteral("新文档没有修改标记"));

    view->activateDocument(0);
    check(view->currentIndex() == 0, QStringLiteral("activateDocument() 切回第一个标签"));
    check(view->filePath() == QFileInfo(outPath).absoluteFilePath(),
          QStringLiteral("切回来文件路径跟着回来"));
    check(view->currentText().contains(QStringLiteral("NEEDLE here")),
          QStringLiteral("切回来正文跟着回来"));

    view->activateNextDocument();
    check(view->currentIndex() == 1, QStringLiteral("activateNextDocument() 到下一个标签"));
    view->closeDocument(1);
    check(view->documents().size() == before, QStringLiteral("closeDocument() 关掉一个标签"));
    check(view->hasDocument(), QStringLiteral("还剩一个标签，不是空状态"));
    check(view->filePath() == QFileInfo(outPath).absoluteFilePath(),
          QStringLiteral("关掉别的标签后当前文件不变"));

    /* ---- QML 命令分发（工具栏 / 菜单走的就是这里）---- */

    /* 界面绑定：菜单栏 / 状态栏有没有真的拿到编辑器 */
    {
        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("hasView")).toBool(),
              QStringLiteral("Main.view 拿到编辑器"));
        check(ui.value(QStringLiteral("topBarHasView")).toBool(),
              QStringLiteral("菜单栏 view 绑定生效"));
        check(ui.value(QStringLiteral("statusHasDoc")).toBool(),
              QStringLiteral("状态栏知道当前有文档"));
    }

    /*
     * 分隔线热区只能占中间那一行的间隙。
     *
     * 之前它写的是 y: 0 / height: 窗口高度，热区从最顶上一直拉到最底下 ——
     * 顶栏菜单和底部状态栏那两条上也压着它：鼠标停在菜单 / 状态文字上光标
     * 会变成 <->，在那里按住也能拖左树宽度（就是"分隔线溢出了上下两条栏"）。
     * 这几条断言把它钉在中间行的上下边界之间。
     */
    {
        const QVariantMap ui = uiState();
        const double top = ui.value(QStringLiteral("splitterTop")).toDouble();
        const double bottom = ui.value(QStringLiteral("splitterBottom")).toDouble();
        const double midTop = ui.value(QStringLiteral("midRowTop")).toDouble();
        const double midBottom = ui.value(QStringLiteral("midRowBottom")).toDouble();
        const double topBar = ui.value(QStringLiteral("topBarHeight")).toDouble();
        const double statusBar = ui.value(QStringLiteral("statusBarHeight")).toDouble();
        const double winHeight = ui.value(QStringLiteral("windowHeight")).toDouble();
        const QString geom = QStringLiteral("热区 %1..%2，中间行 %3..%4，顶栏 %5，底栏 %6，窗口高 %7")
                                 .arg(top).arg(bottom).arg(midTop).arg(midBottom)
                                 .arg(topBar).arg(statusBar).arg(winHeight);

        const double eps = 0.5;
        check(qAbs(top - midTop) < eps,
              QStringLiteral("分隔线热区上边界 = 顶栏下沿（没有盖住顶栏）"), geom);
        check(qAbs(bottom - midBottom) < eps,
              QStringLiteral("分隔线热区下边界 = 底栏上沿（没有盖住底栏）"), geom);
        check(top >= topBar - eps && bottom <= winHeight - statusBar + eps,
              QStringLiteral("分隔线热区整个落在顶栏和底栏之间"), geom);
        check(bottom > top,
              QStringLiteral("分隔线热区有实际高度（能拖得动）"), geom);
    }

    dispatch(QStringLiteral("find"));
    check(uiState().value(QStringLiteral("findOpened")).toBool(),
          QStringLiteral("dispatch(find) 打开查找栏"));
    dispatch(QStringLiteral("replace"));
    check(uiState().value(QStringLiteral("findReplaceVisible")).toBool(),
          QStringLiteral("dispatch(replace) 展开替换行"));

    dispatch(QStringLiteral("toggleWrap"));
    check(view->wrapEnabled(), QStringLiteral("dispatch(toggleWrap) 生效"));
    dispatch(QStringLiteral("toggleWrap"));
    check(!view->wrapEnabled(), QStringLiteral("再切一次自动换行关掉"));

    dispatch(QStringLiteral("toggleLineNumbers"));
    check(!view->lineNumbersVisible(), QStringLiteral("dispatch(toggleLineNumbers) 生效"));
    dispatch(QStringLiteral("toggleLineNumbers"));
    check(view->lineNumbersVisible(), QStringLiteral("行号切回来"));

    dispatch(QStringLiteral("toggleWhitespace"));
    check(view->whitespaceVisible(), QStringLiteral("dispatch(toggleWhitespace) 生效"));
    dispatch(QStringLiteral("toggleWhitespace"));

    /* 字号：默认 12，可通过“设置”菜单的命令改 */
    dispatch(QStringLiteral("fontSize:18"));
    check(view->fontPixelSize() == 18,
          QStringLiteral("dispatch(fontSize:18) 改编辑器字号"),
          QStringLiteral("实际 %1").arg(view->fontPixelSize()));
    dispatch(QStringLiteral("fontSize:12"));
    check(view->fontPixelSize() == 12,
          QStringLiteral("dispatch(fontSize:12) 回到默认字号"),
          QStringLiteral("实际 %1").arg(view->fontPixelSize()));

    dispatch(QStringLiteral("lang:python"));
    check(view->language() == QLatin1String("python"),
          QStringLiteral("dispatch(lang:python) 切语言"), view->language());

    /* 设置菜单里的"注释字号 / 字体"走的是同一条 dispatch */
    dispatch(QStringLiteral("commentFontSize:10"));
    check(view->commentFontPixelSize() == 10,
          QStringLiteral("dispatch(commentFontSize:10) 生效"),
          QStringLiteral("实际 %1").arg(view->commentFontPixelSize()));
    dispatch(QStringLiteral("commentFontSize:0"));
    check(view->commentFontPixelSize() == 0,
          QStringLiteral("dispatch(commentFontSize:0) 恢复跟随正文"));

    const QString familyBefore = view->fontFamily();
    dispatch(QStringLiteral("font:NSimSun"));
    check(view->fontFamily() == QStringLiteral("NSimSun"),
          QStringLiteral("dispatch(font:NSimSun) 生效"), view->fontFamily());
    dispatch(QStringLiteral("font:") + familyBefore);
    check(view->fontFamily() == familyBefore,
          QStringLiteral("dispatch(font:...) 换回原字体"), view->fontFamily());

    dispatch(QStringLiteral("zoomIn"));
    check(view->zoomPercent() > 100, QStringLiteral("dispatch(zoomIn) 放大"),
          QStringLiteral("实际 %1%").arg(view->zoomPercent()));
    dispatch(QStringLiteral("zoomReset"));
    check(view->zoomPercent() == 100, QStringLiteral("dispatch(zoomReset) 回到 100%"));

    dispatch(QStringLiteral("new"));
    check(view->documents().size() == before + 1,
          QStringLiteral("dispatch(new) 新建标签"));
    /*
     * 点了工具栏"新建"之后要能直接打字：键盘焦点必须落到原生编辑控件上。
     * requestEditorFocus() 是排到下一轮事件循环再落一次的（鼠标点击的收尾
     * 处理会把同步那次抢走），所以这里也等一轮再看。
     *
     * 先把窗口激活：Qt 里"某个子控件有焦点"的前提是**窗口本身是活动窗口**，
     * 自检跑在后台（或者刚才别的窗口抢了前）时，hasFocus() 一律是 false ——
     * 那是环境问题，不是这条断言的意图（实测就因此误报过一次）。
     */
    if (!QApplication::activeWindow()) {
        const QWidgetList tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (w->isVisible() && w->windowTitle().contains(QStringLiteral("SmartClip"))) {
                w->raise();
                w->activateWindow();
                break;
            }
        }
    }
    QCoreApplication::processEvents();
    check(view->hasEditorFocus(),
          QStringLiteral("新建之后键盘焦点在编辑区（可以直接打字）"),
          QApplication::activeWindow() ? QStringLiteral("窗口已激活")
                                       : QStringLiteral("窗口不是活动窗口"));

    dispatch(QStringLiteral("closeTab"));
    check(view->documents().size() == before,
          QStringLiteral("dispatch(closeTab) 关闭标签（新标签没改动，不该弹窗）"));

    /* ---- 只读模式 ---- */
    dispatch(QStringLiteral("toggleReadOnly"));
    check(view->readOnly(), QStringLiteral("dispatch(toggleReadOnly) 生效"));
    dispatch(QStringLiteral("toggleReadOnly"));
    check(!view->readOnly(), QStringLiteral("只读模式切回来"));

    /* ---- 剪贴板条目载入（左侧列表点条目走的就是这里）---- */
    const QVariantList items = store ? store->items(QString()) : QVariantList();
    if (!items.isEmpty()) {
        const QVariantMap first = items.first().toMap();
        const qint64 id = first.value(QStringLiteral("id")).toLongLong();
        const QString title = first.value(QStringLiteral("title")).toString();
        view->openClipboardItem(id, title);
        check(view->hasDocument(), QStringLiteral("openClipboardItem() 载入剪贴板条目"));
        check(view->displayName() == title || !title.isEmpty(),
              QStringLiteral("标签标题取自剪贴板条目"), view->displayName());
    } else {
        out() << "  --    剪贴板库为空，跳过条目载入检查" << Qt::endl;
    }

    /*
     * ============ 左边列表点条目：一个条目一条标签 ============
     *
     * 原来 openClipboardItem() 是"复用那条没改过内容的剪贴板标签"——点来点去
     * 始终是同一个标签在换内容，看着就是"不管点哪个文件都只有一个标签在变"。
     * 现在按条目 id 认标签：
     *
     *   没开过的条目 -> 新开一条；
     *   开过的条目   -> 切回它自己那条（正文在它那份 QsciDocument 里，不重灌）。
     *
     * 所以这里量三件事：点新条目会多一条标签；点开的条目不再多开；
     * 以及"切回来"是真的切换（标签下标回到原来那条、正文没被换掉）。
     * 找两条**正文不一样**的文本条目来做判定 —— 正文一样就分不出切到哪条了。
     */
    if (store) {
        auto straight = [](const QString &s) {
            QString copy = s;
            copy.remove(QLatin1Char('\r'));  // Scintilla 会把 CRLF 归一，比较时先抹平
            return copy;
        };

        const QVariantList all = store->items(QString());
        QVariantMap entryA, entryB;
        for (const QVariant &v : all) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("type")).toString() != QLatin1String("text"))
                continue;
            if (entryA.isEmpty()) {
                entryA = m;
                continue;
            }
            if (straight(store->contentOf(m.value(QStringLiteral("id")).toLongLong()))
                != straight(store->contentOf(entryA.value(QStringLiteral("id")).toLongLong()))) {
                entryB = m;
                break;
            }
        }

        if (entryA.isEmpty() || entryB.isEmpty()) {
            out() << "  --    剪贴板里没有两条正文不同的文本条目，跳过" << Qt::endl;
        } else {
            const qint64 idA = entryA.value(QStringLiteral("id")).toLongLong();
            const QString titleA = entryA.value(QStringLiteral("title")).toString();
            const qint64 idB = entryB.value(QStringLiteral("id")).toLongLong();
            const QString titleB = entryB.value(QStringLiteral("title")).toString();

            view->openClipboardItem(idA, titleA);
            const int afterA = view->documents().size();
            const int indexA = view->currentIndex();
            const QString textA = view->currentText();

            view->openClipboardItem(idB, titleB);
            check(view->documents().size() == afterA + 1,
                  QStringLiteral("点另一个条目会新开一条标签（不再共用一个标签换内容）"),
                  QStringLiteral("%1 -> %2 条").arg(afterA).arg(view->documents().size()));
            check(view->currentText() != textA,
                  QStringLiteral("新标签里装的是另一个条目的正文"),
                  QStringLiteral("长度 %1").arg(view->currentText().size()));

            const int indexB = view->currentIndex();

            /* 在这条标签上改一笔：下面看它会不会被下一次点击冲掉 */
            view->duplicateLine();
            const QString textBEdited = view->currentText();
            check(view->modified(), QStringLiteral("在标签里改一笔 -> 已修改状态"));

            view->openClipboardItem(idA, titleA);
            check(view->documents().size() == afterA + 1,
                  QStringLiteral("再点开过的条目不再新开标签（切回原来那条）"),
                  QStringLiteral("实际 %1 条").arg(view->documents().size()));
            check(view->currentIndex() == indexA, QStringLiteral("切回来的就是那个条目自己那条"),
                  QStringLiteral("下标 %1（应该是 %2）").arg(view->currentIndex()).arg(indexA));
            check(view->currentText() == textA,
                  QStringLiteral("那条的正文原样还在"));

            view->openClipboardItem(idB, titleB);
            check(view->documents().size() == afterA + 1,
                  QStringLiteral("点回改过的那条也不新开标签"));
            check(view->currentIndex() == indexB && view->currentText() == textBEdited,
                  QStringLiteral("改过的那条改动还在，没被重灌成原文"),
                  QStringLiteral("下标 %1 / 正文 %2 字符")
                      .arg(view->currentIndex()).arg(view->currentText().size()));

            /*
             * 收尾：把这两条关掉。
             *
             * 上面那条现在是"已修改"，C++ 的 closeDocument() 不问保存直接关
             * （问保存的是 QML 的 closeTab）——不留着它，最后那次
             * dispatch(closeAllTabs) 才不会被"要不要保存"的弹窗卡住。
             * 先关下标大的，小的那个下标才不会跟着挪。
             */
            view->closeDocument(qMax(indexA, indexB));
            view->closeDocument(qMin(indexA, indexB));
        }
    }

    /* 缩进参考线：默认开、颜色是压过的灰（不是正文色） */
    check(view->indentGuidesVisible(), QStringLiteral("缩进参考线默认开启"));
    check(view->styleFore(37) == packed(0x3e, 0x42, 0x47),   // 37 = STYLE_INDENTGUIDE
          QStringLiteral("缩进参考线的颜色是压过的灰（不是正文色）"),
          QStringLiteral("实际 #%1").arg(unpacked(view->styleFore(37)), 6, 16, QLatin1Char('0')));

    /* ---- 代码折叠：单独开一个带大括号的临时文件，别动前面那些断言的行号 ---- */
    {
        const QString foldPath = dir.filePath(QStringLiteral("foldtest.cpp"));
        const QByteArray foldSrc =
            "int main() {\n"
            "    if (1) {\n"
            "        return 0;\n"
            "    }\n"
            "    return 1;\n"
            "}\n";
        check(writeFile(foldPath, foldSrc), QStringLiteral("准备折叠测试文件"));
        view->openFile(foldPath);
        check(view->hasDocument(), QStringLiteral("打开折叠测试文件"));
        check(view->foldingEnabled(), QStringLiteral("代码折叠默认开着"));

        view->unfoldAll();
        check(view->lineVisible(3), QStringLiteral("展开状态下第 3 行可见"));

        view->foldAll();
        check(!view->lineVisible(3),
              QStringLiteral("foldAll() 之后第 3 行被折起来（不可见）"));

        view->unfoldAll();
        check(view->lineVisible(3), QStringLiteral("unfoldAll() 之后第 3 行又可见"));

        /*
         * 边距渲染的像素检查（看的是控件自己渲染出来的图）：
         *   行号栏要有数字、折叠栏要有折叠标记、**不能有纯白像素**
         *   （白带就是 SC_MARGIN_NUMBER 的背景被 lexer 的 STYLECLEARALL
         *     刷回默认白色造成的）。
         */
        view->gotoLine(1);
        QCoreApplication::processEvents();
        const QVariantList stats = view->marginPixelStats();
        const int numberInk = stats.value(0).toInt();
        const int foldInk = stats.value(1).toInt();
        const int whitePx = stats.value(2).toInt();

        out() << "        （行号墨点 " << numberInk << " / 折叠墨点 " << foldInk
              << " / 纯白 " << whitePx
              << " / 边距宽 " << stats.value(7).toInt() << "+" << stats.value(8).toInt()
              << "+" << stats.value(9).toInt() << "）" << Qt::endl;

        check(whitePx == 0, QStringLiteral("边距里没有纯白像素（行号栏不是白底）"),
              QStringLiteral("实际 %1 个白点").arg(whitePx));
        check(numberInk > 0, QStringLiteral("行号栏画出了数字"),
              QStringLiteral("墨点 %1").arg(numberInk));
        check(foldInk > 0, QStringLiteral("折叠栏画出了折叠标记"),
              QStringLiteral("墨点 %1").arg(foldInk));

        /* 缩进参考线：开着能画出来，关掉就一个像素都没有 */
        view->setIndentGuidesVisible(true);
        QCoreApplication::processEvents();
        const int guidesOn = view->marginPixelStats().value(12).toInt();
        view->setIndentGuidesVisible(false);
        QCoreApplication::processEvents();
        const int guidesOff = view->marginPixelStats().value(12).toInt();
        view->setIndentGuidesVisible(true);
        QCoreApplication::processEvents();

        check(guidesOn > 0, QStringLiteral("缩进参考线画出来了"),
              QStringLiteral("像素 %1").arg(guidesOn));
        check(guidesOff == 0, QStringLiteral("关掉缩进参考线后一个像素都没有"),
              QStringLiteral("像素 %1").arg(guidesOff));

        view->closeDocument(view->currentIndex());
    }

    /*
     * ============ 横向滚动条：内容没撑满就不该有 ============
     *
     * 用户报的："内容区只有几个字，但是横向有滚动条"。
     *
     * 根因在 Scintilla 判显隐的口径（third/qscintilla/src/ScintillaQt.cpp
     * 的 ModifyScrollBars）：
     *     hNewPage = GetTextRectangle().Width();          // 一页**文本**宽
     *     hMax     = scrollWidth > hNewPage ? scrollWidth - hNewPage : 0;
     * 横条是 AsNeeded 策略，hMax > 0 就露出来。而 GetTextRectangle() 是 viewport
     * 再扣掉行号/折叠那几条边距和左右留白之后的宽度，**比 viewport 窄几十像素**。
     * updateHorizontalScroll() 原来拿 viewport 宽当"放得下"的界：短内容时
     * scrollWidth = viewport 宽，仍比 hNewPage 大一截 -> hMax 恒 > 0 ->
     * 正文只有几个字也一直挂着横条，还能向右滚那几十像素（正好是边距 + 留白）。
     *
     * 这里钉三件事：
     *   1) 短内容：横条不出现（maximum = 0），scrollWidth 没超过一页文本宽；
     *   2) 长内容：横条出现（maximum > 0），能向右滚到底；
     *   3) 口径一致：自己按公式算的一页宽 == Scintilla 的 pageStep（hNewPage），
     *      免得以后两边又各算一份、慢慢走样。
     */
    {
        const QString shortPath = dir.filePath(QStringLiteral("hscroll-short.txt"));
        const QString longPath = dir.filePath(QStringLiteral("hscroll-long.txt"));

        /* 几个汉字 / 一个字都不换行的长行（400 字符，必然比编辑区宽） */
        check(writeFile(shortPath, QString::fromUtf8("换承载方式\n").toUtf8()),
              QStringLiteral("准备短内容文件"));
        check(writeFile(longPath,
                        QByteArray("LINE ") + QByteArray(390, 'x') + QByteArray("\n")),
              QStringLiteral("准备长行文件"));

        auto hState = [view]() { return view->horizontalScrollState(); };
        auto detail = [](const QVariantMap &s) {
            return QStringLiteral("可见 %1 / maximum %2 / pageStep %3 / 算出来 %4"
                                  " / viewport %5 / scrollWidth %6 / 内容 %7")
                .arg(s.value(QStringLiteral("visible")).toBool())
                .arg(s.value(QStringLiteral("maximum")).toInt())
                .arg(s.value(QStringLiteral("pageStep")).toInt())
                .arg(s.value(QStringLiteral("pageWidthComputed")).toInt())
                .arg(s.value(QStringLiteral("viewportWidth")).toInt())
                .arg(s.value(QStringLiteral("scrollWidth")).toInt())
                .arg(s.value(QStringLiteral("contentWidth")).toInt());
        };

        /* ---- 短内容 ---- */
        check(view->openFile(shortPath) >= 0, QStringLiteral("打开短内容文件"),
              view->lastError());
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap small = hState();
        out() << "        （短内容：" << detail(small) << "）" << Qt::endl;
        check(small.value(QStringLiteral("maximum")).toInt() == 0
              && !small.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("短内容：没有横向滚动条（maximum = 0）"), detail(small));
        check(small.value(QStringLiteral("scrollWidth")).toInt()
              <= small.value(QStringLiteral("pageStep")).toInt(),
              QStringLiteral("短内容：scrollWidth 没超过一页文本宽"), detail(small));
        check(small.value(QStringLiteral("contentWidth")).toInt() > 0,
              QStringLiteral("短内容：量到了内容宽（不是没量）"), detail(small));

        /*
         * 一页宽的两种算法必须一致：公式（viewport - 边距 - 左右留白）对
         * Scintilla 自己写进 pageStep 的那个值。差一点点就说明口径又开始分家了
         * —— 这正是原来那条 bug 的来源。
         */
        check(qAbs(small.value(QStringLiteral("pageStep")).toInt()
                   - small.value(QStringLiteral("pageWidthComputed")).toInt()) <= 1,
              QStringLiteral("一页文本宽：自己算的和 Scintilla 的一致"), detail(small));

        /* ---- 长内容 ---- */
        check(view->openFile(longPath) >= 0, QStringLiteral("打开长行文件"),
              view->lastError());
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap big = hState();
        out() << "        （长内容：" << detail(big) << "）" << Qt::endl;
        check(big.value(QStringLiteral("maximum")).toInt() > 0
              && big.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("长行：横向滚动条出现"), detail(big));
        check(big.value(QStringLiteral("contentWidth")).toInt()
              > big.value(QStringLiteral("pageStep")).toInt(),
              QStringLiteral("长行：量出来的内容宽确实超过了一页"), detail(big));

        /*
         * 自动换行开着时内容折起来，横条必须收回去；关掉换行又得立刻回来 ——
         * 切"换行"开关会走一趟 updateHorizontalScroll，这里量的是那趟有没有
         * 把 scrollWidth 算拧（换行时 hNewPage 是折行宽度，不是原来那个）。
         */
        view->setWrapEnabled(true);
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const QVariantMap wrapped = hState();
        check(!wrapped.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("自动换行：长行折起来，横条收回去"), detail(wrapped));

        view->setWrapEnabled(false);
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const QVariantMap unwrapped = hState();
        check(unwrapped.value(QStringLiteral("maximum")).toInt() > 0
              && unwrapped.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("关掉自动换行：长行又把横条要回来"), detail(unwrapped));

        /* ---- 再回到短内容：横条要收回去 ---- */
        view->closeDocument(view->currentIndex());
        check(view->filePath() == QFileInfo(shortPath).absoluteFilePath(),
              QStringLiteral("关掉长行文件后回到短内容文件"), view->filePath());
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap again = hState();
        check(again.value(QStringLiteral("maximum")).toInt() == 0
              && !again.value(QStringLiteral("visible")).toBool(),
              QStringLiteral("又切回短内容：横条收回去"), detail(again));

        dispatch(QStringLiteral("closeAllTabs"));
    }

    /*
     * ============ 正文卡片：两条滚动条都要贴着卡片边 ============
     *
     * 用户提的是两条滚动条的位置：
     *   1) 横向滚动条离卡片底边太远（原来编辑器四边都让开 10px，横条下面
     *      就空出一条）；
     *   2) 竖向滚动条离卡片右边太远（同理，右边也让开 10px）。
     *
     * 让开的理由本来只有一个：编辑器是**原生子控件**，自己的矩形角是直角，
     * 贴着卡片的角会把 contentArea（radius: 10）画出来的圆角盖成直角。
     * 但**实测（截图逐像素比对）2px 的余量就够了** —— 编辑器底色和卡片底色
     * 本来就是同一个（paperColor: root.editorBg），角上那一两个像素看不出来，
     * 2px 和 10px 画出来的圆角一模一样。所以右边 / 底边都收到 2px，滚动条跟着
     * 贴到卡片边上。
     *
     * 左边后来也收到 2px：编辑器最左边那一条就是行号栏，"序号贴紧左边"
     * 只能靠这个左边距（Scintilla 的 SCI_SETMARGINLEFT 落在**分隔竖线和正文**
     * 之间，跟行号位置无关）。正文离卡片左边缘的余量由行号栏 + 折叠栏的
     * 宽度顶着，不再靠这里。
     *
     * 这里钉住：三条边都只留一点点（>0 且 ≤4px，给坐标取整留余量）、
     * 编辑器整体不出卡片。改回 10px 或者改成 0 都会在这里红。
     */
    {
        /* 需要编辑器在场（visible）时量，所以先开一个文档 */
        check(view->newDocument() >= 0, QStringLiteral("卡片几何用例：新建文档"));
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap card = uiState().value(QStringLiteral("editorCard")).toMap();
        const double radius = card.value(QStringLiteral("radius")).toDouble();
        const double bottomGap = card.value(QStringLiteral("bottomGap")).toDouble();
        const double leftGap = card.value(QStringLiteral("leftGap")).toDouble();
        const double rightGap = card.value(QStringLiteral("rightGap")).toDouble();
        const double viewBottom = card.value(QStringLiteral("viewBottom")).toDouble();
        const double viewRight = card.value(QStringLiteral("viewRight")).toDouble();
        const double cardH = card.value(QStringLiteral("cardHeight")).toDouble();
        const double cardW = card.value(QStringLiteral("cardWidth")).toDouble();
        const QString geom =
            QStringLiteral("卡片 %1x%2 圆角 %3 / 编辑器 %4,%5 %6x%7 / 下留 %8 左留 %9 右留 %10")
                .arg(cardW).arg(cardH).arg(radius)
                .arg(card.value(QStringLiteral("viewX")).toDouble())
                .arg(card.value(QStringLiteral("viewY")).toDouble())
                .arg(card.value(QStringLiteral("viewWidth")).toDouble())
                .arg(card.value(QStringLiteral("viewHeight")).toDouble())
                .arg(bottomGap).arg(leftGap).arg(rightGap);

        out() << "        （" << geom << "）" << Qt::endl;

        check(bottomGap >= 1.0 && bottomGap <= 4.0,
              QStringLiteral("编辑器贴着卡片底边（横向滚动条不再浮在半空）"), geom);
        check(rightGap >= 1.0 && rightGap <= 4.0,
              QStringLiteral("编辑器贴着卡片右边（竖向滚动条不再浮在中间）"), geom);
        check(leftGap >= 1.0 && leftGap <= 4.0,
              QStringLiteral("编辑器贴着卡片左边（行号栏不再浮在中间）"), geom);
        check(viewBottom <= cardH + 0.5 && viewRight <= cardW + 0.5,
              QStringLiteral("编辑器没有溢出卡片（不压状态栏、不出画布）"), geom);

        dispatch(QStringLiteral("closeAllTabs"));
    }

    /* ---- 收尾 ---- */
    dispatch(QStringLiteral("closeAllTabs"));
    check(view->documents().isEmpty(), QStringLiteral("closeAllTabs 之后没有标签"));
    check(!view->hasDocument(), QStringLiteral("空状态：hasDocument = false"));

    /*
     * ============ tab 撑满容器：顶上那条横向滚动条 ============
     *
     * 标签多到装不下时，标签栏顶部要出现一条横向滚动条（见 EditorArea 的
     * ScrollBar.horizontal），而且：
     *
     *   * 没撑满时不出现；
     *   * **出现时也不许把标签栏撑高**（不占高度）：标签栏始终 35px、标签始终
     *     29px 高、标签上沿始终在 y=3 —— 那条横条是浮在标签原有的 3px 上边距
     *     里的，标签和下面的编辑区都不该挪一下；
     *   * 横条左右要让开容器圆角半径那么多，否则会把圆角啃成直角；
     *   * 横条整个在标签上沿之上，不盖住标签。
     *
     * 标签宽 132（短标题取最小值）、间距 3：9 个就撑满（1104px 的标签区）。
     */
    {
        /* 先开两个：没撑满，应该没有横条 */
        dispatch(QStringLiteral("new"));
        dispatch(QStringLiteral("new"));
        const QVariantMap small = uiState().value(QStringLiteral("tabBar")).toMap();
        check(!small.value(QStringLiteral("scrollShown")).toBool(),
              QStringLiteral("两个标签：没撑满，顶部没有滚动条"),
              QStringLiteral("%1 个标签 / 高 %2")
                  .arg(small.value(QStringLiteral("tabs")).toInt())
                  .arg(small.value(QStringLiteral("height")).toDouble()));
        check(qAbs(small.value(QStringLiteral("height")).toDouble() - 35.0) < 0.5,
              QStringLiteral("标签栏是 35px"),
              QStringLiteral("实际 %1").arg(small.value(QStringLiteral("height")).toDouble()));
        check(qAbs(small.value(QStringLiteral("stripHeight")).toDouble() - 29.0) < 0.5,
              QStringLiteral("两个标签时标签高度 29px"),
              QStringLiteral("实际 %1").arg(small.value(QStringLiteral("stripHeight")).toDouble()));
        const double smallStripTop = small.value(QStringLiteral("stripTop")).toDouble();

        /* 再开到 12 个：撑满了，横条出现 */
        for (int i = 0; i < 10; ++i)
            dispatch(QStringLiteral("new"));

        /*
         * 等 QML 把标签重新摆一遍再量。
         *
         * 上面那串 dispatch 是同步返回的：文档已经加进去了，但标签的宽度
         * （Row 的宽度 -> contentWidth）要等这一轮布局跑完才是新的，
         * 滚动条的比例也是跟着 visibleArea 才更新的。不等的话量到的是
         * 上一次布局的旧值（实测量到 278，是只有两个标签时的宽度）。
         */
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();

        const QVariantMap big = uiState().value(QStringLiteral("tabBar")).toMap();
        const double h = big.value(QStringLiteral("height")).toDouble();
        const double w = big.value(QStringLiteral("width")).toDouble();
        const double radius = big.value(QStringLiteral("cornerRadius")).toDouble();
        const double stripTop = big.value(QStringLiteral("stripTop")).toDouble();
        const double stripH = big.value(QStringLiteral("stripHeight")).toDouble();
        const double left = big.value(QStringLiteral("scrollLeft")).toDouble();
        const double right = big.value(QStringLiteral("scrollRight")).toDouble();
        const double barTop = big.value(QStringLiteral("scrollTop")).toDouble();
        const double barBottom = big.value(QStringLiteral("scrollBottom")).toDouble();
        const QString geom = QStringLiteral("栏 %1x%2 圆角 %3 / 横条 x %4..%5 y %6..%7 / 标签 %8..%9"
                                            " / size %10 pos %11 可见 %12 / Flickable %13 内容 %14")
                                 .arg(w).arg(h).arg(radius).arg(left).arg(right)
                                 .arg(barTop).arg(barBottom).arg(stripTop)
                                 .arg(stripTop + stripH)
                                 .arg(big.value(QStringLiteral("scrollSize")).toDouble())
                                 .arg(big.value(QStringLiteral("scrollPosition")).toDouble())
                                 .arg(big.value(QStringLiteral("scrollVisible")).toBool())
                                 .arg(big.value(QStringLiteral("flickWidth")).toDouble())
                                 .arg(big.value(QStringLiteral("flickContent")).toDouble());

        check(big.value(QStringLiteral("scrollShown")).toBool(),
              QStringLiteral("12 个标签：撑满了，顶部出现横向滚动条"),
              QStringLiteral("%1 个标签").arg(big.value(QStringLiteral("tabs")).toInt()));
        /*
         * 不占高度这条是重点：撑满之后标签栏高度、标签高度、标签上沿
         * 都必须和没撑满时一模一样。
         */
        check(qAbs(h - small.value(QStringLiteral("height")).toDouble()) < 0.5,
              QStringLiteral("横条出现时标签栏没被撑高（还是 35px）"), geom);
        check(qAbs(stripTop - smallStripTop) < 0.5,
              QStringLiteral("标签上沿没挪（横条浮在它上面那条 3px 里）"), geom);
        check(qAbs(stripH - 29.0) < 0.5,
              QStringLiteral("标签本身还是 29px 高"), geom);
        check(qAbs((barBottom - barTop) - 3.0) < 0.5,
              QStringLiteral("横条高 3px"), geom);
        check(left >= radius - 0.5 && (w - right) >= radius - 0.5,
              QStringLiteral("横条左右各让开容器圆角，没压在圆角上"), geom);
        check(barTop >= -0.5 && barBottom <= stripTop + 0.5,
              QStringLiteral("横条贴在容器顶边上、整个在标签上沿之上（不盖标签）"), geom);
        check(right > left, QStringLiteral("横条有实际宽度"), geom);
        check(barBottom <= h - 0.5 && right <= w - 0.5,
              QStringLiteral("横条整个在标签栏里面"), geom);

        /* 关掉多余的，回到一个：横条应该收回去（同样要等这一轮布局） */
        dispatch(QStringLiteral("closeAllTabs"));
        dispatch(QStringLiteral("new"));
        for (int i = 0; i < 3; ++i)
            QCoreApplication::processEvents();
        const QVariantMap again = uiState().value(QStringLiteral("tabBar")).toMap();
        check(!again.value(QStringLiteral("scrollShown")).toBool(),
              QStringLiteral("标签又少了：滚动条收回去，标签栏还是 35px"),
              QStringLiteral("高 %1").arg(again.value(QStringLiteral("height")).toDouble()));
        dispatch(QStringLiteral("closeAllTabs"));
    }

    /*
     * ================= 内容区 tab 的右键菜单 =================
     *
     * 两件事要钉住：
     *
     *  1) 菜单里的动作是按**被右键的那个标签**来的，不是当前激活的那个。
     *     右键一个没激活的标签时两者不是同一个，"关闭其他"要留下点中的那个、
     *     关掉其余的全部；动错标签就是这个功能最典型的 bug。
     *     这里用"文件标签 + 空白标签"两种标签来分辨：文件标签有 filePath、
     *     空白标签没有，关错了从 filePath 上一眼就能看出来。
     *
     *  2) 菜单左上角紧贴鼠标右键那一点（DropdownMenu.openAtPoint）。
     *     这条只能量：菜单 x/y 必须正好等于传进去的坐标。
     */
    check(view->openFile(srcPath) >= 0, QStringLiteral("tab 菜单用例：打开文件标签"));
    dispatch(QStringLiteral("new"));
    check(view->documents().size() == 2, QStringLiteral("tab 菜单用例：文件标签 + 空白标签"),
          QStringLiteral("实际 %1 个").arg(view->documents().size()));

    /* closeTab:<i>：关的是下标 0 那个（文件），当前标签是 1（空白） */
    dispatch(QStringLiteral("closeTab:0"));
    check(view->documents().size() == 1, QStringLiteral("closeTab:0 只关掉一个标签"),
          QStringLiteral("剩 %1 个").arg(view->documents().size()));
    check(view->filePath().isEmpty(),
          QStringLiteral("关掉的是下标 0 那个文件标签，活下来的是空白标签"),
          view->filePath());

    /* closeOthers:<i>：留下的是下标 1（文件），当前标签是下标 2 */
    check(view->openFile(srcPath) >= 0, QStringLiteral("tab 菜单用例：再打开文件标签"));
    dispatch(QStringLiteral("new"));
    check(view->documents().size() == 3 && view->currentIndex() == 2,
          QStringLiteral("tab 菜单用例：三个标签，当前的不是要留下的那个"),
          QStringLiteral("共 %1 个 / 当前下标 %2")
              .arg(view->documents().size()).arg(view->currentIndex()));

    dispatch(QStringLiteral("closeOthers:1"));
    check(view->documents().size() == 1, QStringLiteral("closeOthers:1 只留一个标签"),
          QStringLiteral("剩 %1 个").arg(view->documents().size()));
    check(view->filePath() == QFileInfo(srcPath).absoluteFilePath(),
          QStringLiteral("留下的是下标 1 那个文件标签（不是当前标签）"), view->filePath());

    /* 菜单条目：三条，下标跟着"被右键的那个标签"走（这里问的是下标 1） */
    {
        QString acts;
        const QVariantList list = tabMenuActs(1);
        for (const QVariant &a : list)
            acts += (acts.isEmpty() ? QString() : QStringLiteral(" | ")) + a.toString();
        check(acts == QStringLiteral("closeTab:1 | closeOthers:1 | closeAllTabs"),
              QStringLiteral("tab 菜单 = 关闭 / 关闭其他 / 关闭全部，下标是点中的那个"),
              acts);
    }

    /*
     * 左上角就落在鼠标那一点上。
     *
     * anchor 传 null：坐标直接按宿主窗口内容区算，自检不用真的去点某个标签
     * （标签当锚点时走的是同一行 mapToItem，和菜单栏那套 openFor 共用）。
     * 菜单开着不动它：和下面那组长菜单检查一样，进程随后就退出了。
     */
    const double probeX = 300.0;
    const double probeY = 120.0;
    QMetaObject::invokeMethod(qmlRoot, "openTabMenu",
                              Q_ARG(QVariant, QVariant(0)),
                              Q_ARG(QVariant, QVariant()),
                              Q_ARG(QVariant, QVariant(probeX)),
                              Q_ARG(QVariant, QVariant(probeY)));
    {
        const QVariantMap ui = uiState();
        const double mx = ui.value(QStringLiteral("menuX")).toDouble();
        const double my = ui.value(QStringLiteral("menuY")).toDouble();
        const double contentH = ui.value(QStringLiteral("menuContentHeight")).toDouble();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("openTabMenu 弹出菜单（tab 右键那条路）"));
        check(qAbs(mx - probeX) < 0.5 && qAbs(my - probeY) < 0.5,
              QStringLiteral("菜单左上角紧贴鼠标点（300,120）"),
              QStringLiteral("实际 (%1, %2)").arg(mx).arg(my));
        check(qAbs(contentH - 92.0) < 0.5,
              QStringLiteral("菜单里就是 tab 那三条（3×28 + 8 内边距）"),
              QStringLiteral("内容高 %1").arg(contentH));
    }

    /*
     * 长下拉菜单必须限高 + 可滚动。
     *
     * 语言菜单有 27 项（约 764px），不限高就会一路盖住左侧导航栏 ——
     * 这是界面上实际报过的问题。放在最后做：菜单开着直接退出进程。
     */
    dispatch(QStringLiteral("new"));

    /* 工具栏取消后，图标改由菜单条目承担：图标在左、快捷键在右 */
    dispatch(QStringLiteral("menu:文件"));
    {
        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("dispatch(menu:文件) 打开下拉菜单"));
        check(ui.value(QStringLiteral("menuHasIcons")).toBool(),
              QStringLiteral("文件菜单条目带图标（图标在左、快捷键在右）"));
    }

    dispatch(QStringLiteral("menu:语言"));
    {
        const QVariantMap ui = uiState();
        const double menuH = ui.value(QStringLiteral("menuHeight")).toDouble();
        const double contentH = ui.value(QStringLiteral("menuContentHeight")).toDouble();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("dispatch(menu:语言) 打开下拉菜单"));
        check(contentH > 700, QStringLiteral("语言菜单条目总高 > 700px（27 项）"),
              QStringLiteral("实际 %1").arg(contentH));
        check(menuH < contentH, QStringLiteral("长菜单被限高，不再整块铺下去"),
              QStringLiteral("画出来 %1 / 内容 %2").arg(menuH).arg(contentH));
        check(menuH <= 461, QStringLiteral("菜单高度夹在 maxMenuHeight 以内"),
              QStringLiteral("实际 %1").arg(menuH));
        check(ui.value(QStringLiteral("menuScrollable")).toBool(),
              QStringLiteral("长菜单标记为可滚动"));
    }

    out() << Qt::endl
          << "通过 " << gPassed << " 项，失败 " << gFailed << " 项" << Qt::endl;
    return gFailed == 0 ? 0 : 1;
}
