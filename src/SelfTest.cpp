#include "SelfTest.h"

#include "ClipboardStore.h"
#include "EditorViewItem.h"
#include "Screenshot.h"
#include <QMessageBox>
#include <QQmlEngine>
#include <QWidget>
#include <QWindow>

#include "WindowHelper.h"

#include "EditorController.h"
#include "TrayIcon.h"

#include <QApplication>
#include <QClipboard>
#include <QAction>
#include <QIcon>
#include <QColor>
#include <QMenu>
#include <QDate>
#include <QDateTime>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QImage>
#include <QPalette>
#include <QPoint>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QTextStream>
#include <QThread>
#include <QVariant>
#include <QWidget>
#include <QWindow>
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

int SelfTest::run(QObject *qmlRoot, ClipboardStore *store, Screenshot *shot, TrayIcon *tray, EditorController *cmd) {
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

    /* 读左侧项目树的状态（见 Main.qml 的 treeState） */
    auto treeState = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "treeState", Q_RETURN_ARG(QVariant, result));
        return result.toMap();
    };

    /*
     * 等布局算完。
     *
     * 布局是**下一帧**才做的：dispatch 改完标志位立刻读 folderTree.width，
     * 拿到的还是上一帧那个槽位宽度（实测"收起面板"后读出 300）。
     * 所以量几何之前先把事件跑一轮 —— 只给标志位做断言是量不出这种毛病的。
     */
    auto settle = []() {
        for (int i = 0; i < 5; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            QThread::msleep(10);
        }
    };

    /* 读左树"更多"菜单的条目清单（见 Main.qml 的 treeMenuActs，和弹出的是同一份构造） */
    auto treeMenuActs = [qmlRoot]() {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "treeMenuActs", Q_RETURN_ARG(QVariant, result));
        return result.toList();
    };

    /*
     * 读左树某一类行的**右键**菜单条目（见 Main.qml 的 treeRowMenuActs）。
     * kind 传 "file" / "folder"：找树里第一个这一类行，用它构造菜单。
     */
    auto treeRowMenuActs = [qmlRoot](const QString &kind) {
        QVariant result;
        QMetaObject::invokeMethod(qmlRoot, "treeRowMenuActs", Q_RETURN_ARG(QVariant, result),
                                  Q_ARG(QVariant, QVariant(kind)));
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
        /*
         * 三条竖线一个颜色：分隔线（边距底色）、字数参考线（edge）、缩进参考线
         * （STYLE_INDENTGUIDE 前景色）—— 用户要的"这些竖线跟行号右边那条一样"。
         */
        check(view->rulerEdgeColor() == packed(0x33, 0x38, 0x40)
                  && view->marginBack(2) == packed(0x33, 0x38, 0x40)
                  && view->styleFore(37) == packed(0x33, 0x38, 0x40),   // 37 = STYLE_INDENTGUIDE
              QStringLiteral("分隔线 / 字数参考线 / 缩进参考线是同一个颜色"),
              QStringLiteral("参考线 #%1 / 分隔线 #%2 / 缩进 #%3")
                  .arg(unpacked(view->rulerEdgeColor()), 6, 16, QLatin1Char('0'))
                  .arg(unpacked(view->marginBack(2)), 6, 16, QLatin1Char('0'))
                  .arg(unpacked(view->styleFore(37)), 6, 16, QLatin1Char('0')));

        QVariantList rulerPixels = view->rulerPixelStats();
        out() << "        （参考线：扫到 x=" << rulerPixels.value(0).toInt()
              << "，按 80 字算出来应为 x=" << rulerPixels.value(1).toInt()
              << "，离底边 " << rulerPixels.value(2).toInt() << " px）"
              << Qt::endl;
        check(rulerPixels.value(0).toInt() >= 0
                  && qAbs(rulerPixels.value(0).toInt() - rulerPixels.value(1).toInt()) <= 3,
              QStringLiteral("那条线真的画在第 80 个字的位置上"),
              QStringLiteral("扫到 x=%1 / 应为 x=%2")
                  .arg(rulerPixels.value(0).toInt()).arg(rulerPixels.value(1).toInt()));
        /*
         * 两条竖线都要一直画到控件底边 —— 用户报过"有时候没撑满纵向屏幕"
         * （横条把正文区截短了，线就断在横条上沿）。这一条没有横条，本该是 0。
         */
        check(rulerPixels.value(2).toInt() >= 0 && rulerPixels.value(2).toInt() <= 3,
              QStringLiteral("参考线一直画到编辑区底边"),
              QStringLiteral("离底边 %1 px").arg(rulerPixels.value(2).toInt()));
        {
            const QVariantList m = view->marginPixelStats();
            check(m.value(13).toInt() >= 0 && m.value(13).toInt() <= 3,
                  QStringLiteral("分隔竖线也一直画到编辑区底边"),
                  QStringLiteral("离底边 %1 px").arg(m.value(13).toInt()));
        }

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

    /*
     * 提示框是深底浅字。
     *
     * 分两头：
     *   * main.cpp 设的应用调色板 —— 管**原生（QWidget）**那侧的提示框；
     *   * QML 那侧真正的提示框走 AppToolTip 组件（自己画的 Popup）——
     *     样式 Fusion 那套取的是平台主题的浅色调色板（#FFFFE1），应用调色板
     *     和 QML 调色板都压不住它，所以那边只能自己画。这里量的是组件报出来的
     *     配色，以及"这份调色板确实也到了 QML"。
     */
    {
        const QColor cppBase = QApplication::palette().color(QPalette::ToolTipBase);
        const QColor cppText = QApplication::palette().color(QPalette::ToolTipText);
        const QVariantMap ui = uiState();
        const QColor qmlBase = ui.value(QStringLiteral("toolTipBase")).value<QColor>();
        const QColor qmlText = ui.value(QStringLiteral("toolTipText")).value<QColor>();
        const QColor tipBg = ui.value(QStringLiteral("tipBackground")).value<QColor>();
        const QColor tipFg = ui.value(QStringLiteral("tipTextColor")).value<QColor>();
        const QString tip =
            QStringLiteral("调色板 底 %1 字 %2（QML 看到 %3 / %4）/ 提示框组件 底 %5 字 %6 "
                           "圆角 %7 延迟 %8ms")
                .arg(cppBase.name(), cppText.name(), qmlBase.name(), qmlText.name(),
                     tipBg.name(), tipFg.name())
                .arg(ui.value(QStringLiteral("tipRadius")).toInt())
                .arg(ui.value(QStringLiteral("tipDelay")).toInt());
        out() << "        （提示框：" << tip << "）" << Qt::endl;

        check(cppBase.lightness() < 90 && cppText.lightness() > 150,
              QStringLiteral("原生提示框用的是深底浅字"), tip);
        check(qmlBase == cppBase && qmlText == cppText,
              QStringLiteral("这份调色板也传到了 QML 那侧"), tip);
        check(tipBg.lightness() < 90 && tipFg.lightness() > 150,
              QStringLiteral("QML 提示框（AppToolTip）是深底浅字"), tip);
        check(tipBg != QColor(0xff, 0xff, 0xe1),
              QStringLiteral("QML 提示框不再是系统那种浅黄底（#FFFFE1）"), tip);
        check(ui.value(QStringLiteral("tipRadius")).toInt() >= 3,
              QStringLiteral("提示框是圆角的"), tip);
        check(ui.value(QStringLiteral("tipDelay")).toInt() == 420,
              QStringLiteral("悬停 420ms 才弹（和原来附加属性的 delay 一致）"), tip);
    }

    dispatch(QStringLiteral("replace"));
    check(uiState().value(QStringLiteral("findReplaceVisible")).toBool(),
          QStringLiteral("dispatch(replace) 展开替换行"));

    /*
     * 查找 / 替换栏的外观（照样例改的那三条）：
     *   * 两个输入框一样长；
     *   * 圆角面板，左右各留出间隙（不再是从左铺到右的长条）；
     *   * 面板是圆的。
     * 布局是 QML 算的，所以量的是 FindBar 报上来的实际宽度（见 uiState）。
     */
    {
        /*
         * 先让 QML 重新布局一次：宽度是布局算出来的，dispatch 之后立刻读还是 0
         * （实测第一次读就是 0/0）。跑两轮事件循环，网格布局收敛后再量。
         */
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        const QVariantMap ui = uiState();
        const double findW = ui.value(QStringLiteral("findFieldWidth")).toDouble();
        const double replW = ui.value(QStringLiteral("findReplaceFieldWidth")).toDouble();
        const double gapL = ui.value(QStringLiteral("findPanelLeftGap")).toDouble();
        const double gapR = ui.value(QStringLiteral("findPanelRightGap")).toDouble();
        const double radius = ui.value(QStringLiteral("findPanelRadius")).toDouble();
        const double barH = ui.value(QStringLiteral("findBarHeight")).toDouble();
        const QString geom = QStringLiteral("栏高 %1 / 查找框 %2 / 替换框 %3 / 左 %4 右 %5 / 圆角 %6")
                                 .arg(barH).arg(findW).arg(replW).arg(gapL).arg(gapR).arg(radius);

        out() << "        （查找栏：" << geom << "）" << Qt::endl;
        /* 高度为 0 说明 implicitHeight 那个绑定炸了（踩过：绑到已删掉的 id），
           这时候整个栏什么都不画，但别的断言照样能过，所以单独钉一条 */
        check(barH >= 60, QStringLiteral("展开替换行后查找栏有高度（不是 0）"), geom);
        check(findW > 100 && qAbs(findW - replW) <= 1,
              QStringLiteral("查找框和替换框一样长"), geom);
        check(gapL >= 6 && gapR >= 6 && qAbs(gapL - gapR) <= 1,
              QStringLiteral("面板左右各有间隙（不再顶到卡片两边）"), geom);
        check(radius >= 6, QStringLiteral("面板是圆角的"), geom);
    }

    /*
     * 自检要一个已知的起点：**自动换行是会被界面记住的**
     * （Connections onWrapChanged -> Cmd.remember("wrap", …)）。
     *
     * 用户上一次开着自动换行退出的话，这个开关下次启动就是开的 —— 于是
     * 下面"切一下应该变开 / 再切一下应该关掉"，以及后面横向滚动条那一整组
     * （换行开着时长行会折起来，横条本来就不该有）全都不成立。
     * 实测：只把设置里的 wrap 改成 1，同一个可执行文件这一组 6 条全红，
     * 看着像功能坏了，其实只是起点不一样 —— 所以这里先把起点钉死，
     * 到收尾再还原成用户自己那个值。
     */
    const bool wrapAtStart = view->wrapEnabled();
    view->setWrapEnabled(false);

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

    /*
     * ================= 剪贴板内容：落成 md 文件 + 元数据 =================
     *
     * 内容不再进数据库了：复制进来的东西写进
     *     <保存目录>/<日期>/<时分秒>.md
     * 库里只剩元数据（文件清单 + 每条的时间 / 类型 / 标题 / 摘要 / 去重哈希）。
     * 这一段走的就是采集那条链路 —— ClipboardManager 调的 captureText / captureImage。
     *
     * **跑在临时保存目录上**：先把保存位置换到临时目录，全部跑完再换回来
     * （见下面"新建 / 保存 / 改名 / 删除"那一段的收尾）。
     * 不能拿用户真实的目录来跑：采集是"往当天那份 md 里追加"落地的，往用户那份
     * 文件里写一段、再整份删掉，就把用户自己的东西一起删了。
     */
    QString savedRoot;
    if (store) {
        savedRoot = store->rootPath();
        const QString tempRoot = dir.filePath(QStringLiteral("clipstore"));
        check(store->setRootPath(tempRoot), QStringLiteral("保存位置可改（自检用临时目录）"));
        check(store->rootPath() == QDir::cleanPath(tempRoot),
              QStringLiteral("改完读回来还是那个目录"), store->rootPath());

        /* ---- 文本：写进当天的 md ---- */
        const QString marker =
            QStringLiteral("自检内容 %1").arg(QDateTime::currentMSecsSinceEpoch());
        check(store->captureText(marker), QStringLiteral("采集文本：写进了当天的 md"));
        check(!store->captureText(marker),
              QStringLiteral("同一段文本不会写第二遍（按内容去重）"));

        const QString dateKey = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
        const QDir day(store->rootPath() + QLatin1Char('/') + dateKey);
        check(day.exists(), QStringLiteral("日期目录按 2026-09-13 这种名字建"), day.path());

        /*
         * 在目录里**按内容**找文件。
         *
         * 不按文件名找：自检里几条内容往往落在同一秒里，名字会带 -2 / -3 后缀，
         * 而排序上 "073100-2.md" 反而排在 "073100.md" 前面（'-' < '.'），
         * 拿"最后一个"当"最新那个"是不成立的。
         */
        auto fileContaining = [](const QDir &folder, const QString &needle) {
            const QStringList names =
                folder.entryList({ QStringLiteral("*.md") }, QDir::Files, QDir::Name);
            for (const QString &name : names) {
                if (readFile(folder.absoluteFilePath(name)).contains(needle))
                    return folder.absoluteFilePath(name);
            }
            return QString();
        };

        const QString firstFile = fileContaining(day, marker);
        check(!firstFile.isEmpty(), QStringLiteral("当天的目录里有一份 md 装着刚采集的内容"));
        if (!firstFile.isEmpty()) {
            const QString name = QFileInfo(firstFile).fileName();
            check(QRegularExpression(QStringLiteral("^\\d{6}(-\\d+)?\\.md$")).match(name).hasMatch(),
                  QStringLiteral("文件名是时分秒（73100.md 这种）"), name);
            const QString body = readFile(firstFile);
            check(body.contains(QStringLiteral("## ")),
                  QStringLiteral("每条内容前面是 \"## 时分秒\" 的分段行"));
            check(body.startsWith(QStringLiteral("# ") + dateKey),
                  QStringLiteral("文件开头是当天的日期标题"));
        }

        /* ---- 图片：PNG 落到 assets/，md 里写相对引用 ---- */
        QImage shot(24, 24, QImage::Format_ARGB32);
        shot.fill(QColor(0x4c, 0x96, 0xd8));
        check(store->captureImage(shot), QStringLiteral("采集图片：PNG 落到当天目录的 assets/"));
        {
            const QStringList pngs = QDir(day.absoluteFilePath(QStringLiteral("assets")))
                                         .entryList({ QStringLiteral("*.png") }, QDir::Files);
            check(pngs.size() == 1, QStringLiteral("assets/ 里正好一张 PNG"),
                  QStringLiteral("实际 %1 张").arg(pngs.size()));
            const QString imageFile = fileContaining(day, QStringLiteral("](assets/"));
            check(!imageFile.isEmpty()
                      && readFile(imageFile).contains(QStringLiteral("![图片](assets/")),
                  QStringLiteral("md 里用相对路径引用了那张图"), imageFile);
        }

        /* ---- 20K：写满就另起一份 ---- */
        const QString big = QStringLiteral("自检大块内容 ") + QString(21 * 1024, QLatin1Char('y'));
        check(store->captureText(big), QStringLiteral("超过 20K 的正文也能落盘（自己占一份）"));
        const QString afterBig =
            QStringLiteral("自检大块之后的第二条 %1").arg(QDateTime::currentMSecsSinceEpoch());
        check(store->captureText(afterBig), QStringLiteral("紧接着的那一条也写进去了"));

        const QString bigFile = fileContaining(day, big);
        const QString nextFile = fileContaining(day, afterBig);
        check(!bigFile.isEmpty() && !nextFile.isEmpty(),
              QStringLiteral("两份内容都找得到自己的文件"));
        check(bigFile != nextFile,
              QStringLiteral("上一份超过 20K 之后，下一条落在了**新的一份**里"),
              QStringLiteral("%1 / %2").arg(QFileInfo(bigFile).fileName(),
                                            QFileInfo(nextFile).fileName()));
        check(!readFile(bigFile).contains(marker),
              QStringLiteral("超大的那一份是另起的：里面没有更早那条内容"));
        check(QFileInfo(bigFile).size() > 20 * 1024,
              QStringLiteral("单条内容本身就超过 20K 时照写（不截断）"),
              QStringLiteral("%1 字节").arg(QFileInfo(bigFile).size()));

        /* ---- 元数据：有记录，正文不进库 ---- */
        check(store->entryCount() >= 4, QStringLiteral("元数据里有条目记录（只记元数据）"),
              QStringLiteral("%1 条").arg(store->entryCount()));
        check(store->fileCount() >= 3, QStringLiteral("元数据里有文件记录"),
              QStringLiteral("%1 份").arg(store->fileCount()));

        const QVariantList clipTree = store->tree(QString(), true);
        check(!clipTree.isEmpty(), QStringLiteral("左树数据能从元数据建出来"));
        if (!clipTree.isEmpty()) {
            const QVariantMap top = clipTree.first().toMap();
            check(top.value(QStringLiteral("kind")).toString() == QLatin1String("date"),
                  QStringLiteral("树的第一层是日期文件夹"));
            check(top.value(QStringLiteral("label")).toString() == dateKey,
                  QStringLiteral("日期文件夹的名字就是这一天"),
                  top.value(QStringLiteral("label")).toString());
            check(top.value(QStringLiteral("files")).toInt() >= 3,
                  QStringLiteral("日期文件夹下面挂着那几份 md"),
                  QStringLiteral("%1 份").arg(top.value(QStringLiteral("files")).toInt()));
        }

        /* 搜索：按条目摘要找得到（库里存的是摘要，不是正文） */
        check(!store->tree(marker, true).isEmpty(),
              QStringLiteral("按内容搜得到：只留命中的文件"));
        check(store->tree(QStringLiteral("绝对不存在的关键词 zzz"), true).isEmpty(),
              QStringLiteral("搜不到的词 -> 空树"));

        /*
         * ---- 程序自己往剪贴板里写的内容不算采集对象 ----
         *
         * 编辑器里 Ctrl+C、菜单里的"复制全文"、左树回填剪贴板……都会触发
         * QClipboard::dataChanged，不挡掉的话刚复制的东西立刻又被采集一遍。
         * 机制就是这一个标记：置上 -> 下一次采集跳过 -> 用完就清。
         */
        store->markOwnCopy();
        check(store->takeSkipNextCapture(),
              QStringLiteral("自己人写的剪贴板变化：下一次采集会跳过"));
        check(!store->takeSkipNextCapture(),
              QStringLiteral("这个标记只生效一次（不会把后面真正的外部复制吃掉）"));
    }

    /* 缩进参考线：默认开、颜色和另外两条竖线一样（不是正文色、不是 Scintilla 默认） */
    check(view->indentGuidesVisible(), QStringLiteral("缩进参考线默认开启"));
    check(view->styleFore(37) == packed(0x33, 0x38, 0x40),   // 37 = STYLE_INDENTGUIDE
          QStringLiteral("缩进参考线是压过的灰，和行号右边那条竖线一个颜色"),
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

        /*
         * 横条出现之后，正文区就短了一截（横条那 12px）。两条竖线必须**补到控件
         * 底边**，不能断在横条上沿 —— 用户报的就是这个："有时候没撑满纵向屏幕"。
         * 补线是编辑控件上那块透明小控件画的（见 EditorViewItem::updateBottomLines）。
         */
        {
            QCoreApplication::processEvents();
            const QVariantList ruler = view->rulerPixelStats();
            const QVariantList margin = view->marginPixelStats();
            out() << "        （有横条时：参考线离底边 " << ruler.value(2).toInt()
                  << " px，分隔线离底边 " << margin.value(13).toInt() << " px）" << Qt::endl;
            check(ruler.value(2).toInt() >= 0 && ruler.value(2).toInt() <= 3,
                  QStringLiteral("有横条时参考线仍然画到编辑区底边"),
                  QStringLiteral("离底边 %1 px").arg(ruler.value(2).toInt()));
            check(margin.value(13).toInt() >= 0 && margin.value(13).toInt() <= 3,
                  QStringLiteral("有横条时分隔竖线也画到编辑区底边"),
                  QStringLiteral("离底边 %1 px").arg(margin.value(13).toInt()));
            /*
             * 补线控件是贴在那一条上的，它自己不能画背景、也不能接鼠标事件
             * （否则等于把滚动条糊住 / 点不动）。滚轮、拖横条都得照常能用。
             */
            const QVariantList bl = view->bottomLinesState();
            check(bl.value(0).toBool() && bl.value(1).toBool() && bl.value(2).toBool()
                      && !bl.value(3).toBool() && bl.value(4).toInt() == 2
                      && bl.value(5).toInt() >= 8,
                  QStringLiteral("补线控件在工作：鼠标穿透、不画背景、补两条线"),
                  QStringLiteral("可见 %1 / 穿透 %2 / 不画背景 %3 / 自动填背景 %4 "
                                 "/ 线数 %5 / 高 %6")
                      .arg(bl.value(0).toBool()).arg(bl.value(1).toBool())
                      .arg(bl.value(2).toBool()).arg(bl.value(3).toBool())
                      .arg(bl.value(4).toInt()).arg(bl.value(5).toInt()));
        }
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

        /*
         * ---- 滚动条的右键菜单：换成应用自己那套深色菜单 ----
         *
         * Qt 自带的 QScrollBar 右键菜单是浅色底 + 英文条目（"Scroll here /
         * Left edge / Page left / …"），跟界面里其它菜单完全不是一个样子。
         * 现在这条右键在 EditorViewItem::eventFilter 里被**吃掉**，改发信号让
         * Main.qml 弹同一套 DropdownMenu（条目见 js/EditorMenus.js 的 scrollBarMenu）。
         *
         * 钉三件事：
         *   1) 菜单里就是那七条，横向纵向各一组（scroll:h:* / scroll:v:*）；
         *   2) 事件真的被吃掉、弹出来的是我们那套菜单（原生那个没机会出现）；
         *   3) 点下去真的会滚 —— 拿横条的 value 量"右边缘 / 左边缘"。
         */
        {
            auto scrollActs = [qmlRoot](bool horizontal) {
                QVariant result;
                QMetaObject::invokeMethod(qmlRoot, "scrollMenuActs",
                                          Q_RETURN_ARG(QVariant, result),
                                          Q_ARG(QVariant, QVariant(horizontal)));
                return result.toList();
            };

            QStringList hActs, vActs;
            for (const QVariant &a : scrollActs(true))
                hActs << a.toString();
            for (const QVariant &a : scrollActs(false))
                vActs << a.toString();
            const QStringList expectedH = { QStringLiteral("scroll:h:here"),
                                            QStringLiteral("scroll:h:edgeStart"),
                                            QStringLiteral("scroll:h:edgeEnd"),
                                            QStringLiteral("scroll:h:pageBack"),
                                            QStringLiteral("scroll:h:pageForward"),
                                            QStringLiteral("scroll:h:lineBack"),
                                            QStringLiteral("scroll:h:lineForward") };
            check(hActs == expectedH,
                  QStringLiteral("横向滚动条右键 = 七条（和 Qt 原来那套语义一一对应）"),
                  hActs.join(QLatin1Char('/')));
            check(vActs.size() == 7
                      && vActs.contains(QStringLiteral("scroll:v:pageForward"))
                      && vActs.contains(QStringLiteral("scroll:v:edgeEnd")),
                  QStringLiteral("纵向那一组是 v 轴的动作"), vActs.join(QLatin1Char('/')));

            /* 事件被吃掉 + 弹的是我们那套菜单 */
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            /*
             * 返回值用**精确类型**接：QMetaObject::invokeMethod 对不上返回类型
             * 就直接失败、方法根本不会被调用（QVariant 接 bool 就是这样，
             * 踩过一次：表现为"探针一行都没打、handled 一直是 false"）。
             */
            bool handled = false;
            QMetaObject::invokeMethod(view, "triggerScrollBarContextMenu",
                                      Q_RETURN_ARG(bool, handled),
                                      Q_ARG(bool, true), Q_ARG(int, -1));
            settle();
            check(handled,
                  QStringLiteral("滚动条的右键事件被我们吃掉（Qt 那个浅色英文菜单不会弹）"));
            {
                const QVariantMap menu = uiState();
                check(menu.value(QStringLiteral("menuOpened")).toBool(),
                      QStringLiteral("滚动条右键弹出的是应用自己的菜单"));
                check(menu.value(QStringLiteral("menuHasIcons")).toBool(),
                      QStringLiteral("滚动条菜单也带图标（和编辑菜单一套观感）"));
            }
            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            settle();

            /* 点下去真的会滚 */
            const QVariantMap atStart = hState();
            check(atStart.value(QStringLiteral("value")).toInt() == 0,
                  QStringLiteral("动作之前横条在最左边"),
                  QStringLiteral("value %1").arg(atStart.value(QStringLiteral("value")).toInt()));

            dispatch(QStringLiteral("scroll:h:edgeEnd"));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            const QVariantMap atEnd = hState();
            check(atEnd.value(QStringLiteral("maximum")).toInt() > 0
                      && atEnd.value(QStringLiteral("value")).toInt()
                             == atEnd.value(QStringLiteral("maximum")).toInt(),
                  QStringLiteral("菜单里\"右边缘\"把横条滚到底"),
                  QStringLiteral("value %1 / maximum %2")
                      .arg(atEnd.value(QStringLiteral("value")).toInt())
                      .arg(atEnd.value(QStringLiteral("maximum")).toInt()));

            dispatch(QStringLiteral("scroll:h:edgeStart"));
            for (int i = 0; i < 3; ++i)
                QCoreApplication::processEvents();
            check(hState().value(QStringLiteral("value")).toInt() == 0,
                  QStringLiteral("菜单里\"左边缘\"滚回最左边"));
        }

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
     * ================= 左侧项目树标题栏那排按钮 =================
     *
     * 标题栏现在摆着七件事：新建 md / 刷新 / 定位 / 全部折叠 / 全部展开 / 更多 /
     * 收起面板。这里钉三件事：
     *
     *  1) 七个按钮真的摆在标题栏里（个数是从标题栏那排 RowLayout 里数出来的，
     *     不是写死的常量，见 FolderTree.toolbarButtonCount）；
     *  2) 全部折叠 / 全部展开真的把日期文件夹收拢 / 铺开（量的是 treeRows
     *     的行数，不是只看那个布尔量）；
     *  3) 收起面板把**布局槽位**收成 0，再点一次原样回来 ——
     *     只改标志位、宽度没跟着走，从界面上是一眼能看出来的。
     *
     * 菜单那几条走 treeMenuActs()（和"更多"弹出的是同一份构造）。
     */
    {
        QString acts;
        for (const QVariant &a : treeMenuActs())
            acts += (acts.isEmpty() ? QString() : QStringLiteral(" | ")) + a.toString();
        check(acts == QStringLiteral("treeNew | refresh | treeLocate | treeExpandAll"
                                     " | treeCollapseAll | treeSortNewest | treeSortOldest"
                                     " | treeImportFolder | treeOpenRoot | treeChooseRoot"
                                     " | treeHide"),
              QStringLiteral("左树\"更多\"菜单 = 新建 / 刷新 / 定位 / 全展开 / 全折叠 / "
                             "排序 / 导入文件夹 / 保存位置 / 收起面板"),
              acts);

        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("treeToolbarButtons")).toInt() == 7,
              QStringLiteral("标题栏摆着七个工具按钮（新建 / 刷新 / 定位 / 全折 / 全展 / 更多 / 收起）"),
              QStringLiteral("实际 %1 个")
                  .arg(ui.value(QStringLiteral("treeToolbarButtons")).toInt()));

        const QVariantMap before = treeState();
        const int folders = before.value(QStringLiteral("folderCount")).toInt();
        /*
         * 日期文件夹的个数由**磁盘上有哪些日期目录**决定（自检跑在临时保存
         * 目录上，所以只有今天一个），导入的目录算在同一个计数里。
         */
        check(folders >= 1, QStringLiteral("左树上至少有一个日期文件夹"),
              QStringLiteral("实际 %1 个").arg(folders));

        dispatch(QStringLiteral("treeCollapseAll"));
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("openFolders")).toInt() == 0,
                  QStringLiteral("全部折叠：日期文件夹都收起来了"));
            /*
             * 折叠干净之后，剩下的就是"最外层那几行"。
             *
             * 这里不能拿 folderCount 比：导入的目录能往下套（chat/frontend），
             * 那些子目录折叠时本来就不占行 —— 用户设置里挂着导入目录时，
             * 老写法会数出"行 2 / 文件夹 3"这种假红。
             */
            check(s.value(QStringLiteral("rows")).toInt()
                      == before.value(QStringLiteral("topLevelRows")).toInt(),
                  QStringLiteral("折叠后树里只剩最外层那几行"),
                  QStringLiteral("实际 %1 行 / 最外层 %2 行")
                      .arg(s.value(QStringLiteral("rows")).toInt())
                      .arg(before.value(QStringLiteral("topLevelRows")).toInt()));
        }

        dispatch(QStringLiteral("treeExpandAll"));
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("openFolders")).toInt() == folders,
                  QStringLiteral("全部展开：日期文件夹都开了"));
            check(s.value(QStringLiteral("rows")).toInt() >= folders,
                  QStringLiteral("展开后行数不少于文件夹数"),
                  QStringLiteral("实际 %1 行").arg(s.value(QStringLiteral("rows")).toInt()));
            /* 展开之后，当天的那些 md 应该真的作为文件行出现在树里 */
            check(s.value(QStringLiteral("rows")).toInt() > folders,
                  QStringLiteral("展开后有文件行（日期文件夹下面挂着 md）"),
                  QStringLiteral("%1 行 / %2 个文件夹")
                      .arg(s.value(QStringLiteral("rows")).toInt()).arg(folders));
        }

        /*
         * 左树右键菜单：文件一条路、文件夹一条路（见 js/EditorMenus.js 的
         * fileContextMenu / folderContextMenu）。这里钉"菜单里有这几条"
         * 以及"每条都带着那个文件 / 文件夹的路径" —— 动作名是
         * fileOpen:<路径> 这种前缀形式，dispatch 按前缀切。
         */
        {
            QStringList fileActs;
            for (const QVariant &a : treeRowMenuActs(QStringLiteral("file")))
                fileActs << a.toString();
            check(fileActs.size() == 4,
                  QStringLiteral("文件右键菜单四条（打开 / 重命名 / 删除 / 在文件夹中显示）"),
                  QStringLiteral("实际 %1 条").arg(fileActs.size()));
            check(fileActs.value(0).startsWith(QStringLiteral("fileOpen:"))
                      && fileActs.value(1).startsWith(QStringLiteral("fileRename:"))
                      && fileActs.value(2).startsWith(QStringLiteral("fileDelete:"))
                      && fileActs.value(3).startsWith(QStringLiteral("fileReveal:")),
                  QStringLiteral("四条各带自己的动作前缀"),
                  fileActs.join(QLatin1Char('/')));
            {
                /* 菜单里带的那条路径得是磁盘上真有的那份 md */
                const QString acted = fileActs.value(0).mid(int(qstrlen("fileOpen:")));
                check(!acted.isEmpty() && QFileInfo::exists(acted),
                      QStringLiteral("菜单里带的就是磁盘上那份文件"), acted);
            }

            QStringList folderActs;
            for (const QVariant &a : treeRowMenuActs(QStringLiteral("folder")))
                folderActs << a.toString();
            check(folderActs.size() == 3
                      && folderActs.value(0) == QLatin1String("treeNew")
                      && folderActs.value(1) == QLatin1String("refresh")
                      && folderActs.value(2).startsWith(QStringLiteral("folderReveal:")),
                  QStringLiteral("文件夹右键菜单三条（新建 / 刷新 / 在文件夹中显示）"),
                  folderActs.join(QLatin1Char('/')));

            /*
             * 右键菜单指着的那一行要有灰黑底。
             *
             * 蓝底（rowHighlight）是"这份文件开在编辑器里"，和"菜单要动哪一行"
             * 是两件事：右键一个没打开的文件时，光看菜单看不出动的是谁，所以
             * 那一行单独画一层灰黑底。这里走的就是界面上那条路
             * （openTreeRowMenuFor -> openTreeRowMenu），量的也是委托自己
             * 报上来的 rowContext。
             */
            QVariant ctxPath;
            QMetaObject::invokeMethod(qmlRoot, "openTreeRowMenuFor", Q_RETURN_ARG(QVariant, ctxPath),
                                      Q_ARG(QVariant, QVariant(QStringLiteral("file"))));
            settle();
            {
                const QVariantMap s = treeState();
                const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
                check(!ctxPath.toString().isEmpty()
                          && s.value(QStringLiteral("contextPath")).toString() == ctxPath.toString(),
                      QStringLiteral("右键：菜单指着的那一行记下来了"),
                      QStringLiteral("菜单行 %1 / 树上报 %2")
                          .arg(ctxPath.toString())
                          .arg(s.value(QStringLiteral("contextPath")).toString()));
                check(hl.value(QStringLiteral("context")).toInt() == 1,
                      QStringLiteral("右键：那一行画上了灰黑底"),
                      QStringLiteral("带着灰黑底的行 %1")
                          .arg(hl.value(QStringLiteral("context")).toInt()));
            }

            QMetaObject::invokeMethod(qmlRoot, "closeMenu");
            settle();
            {
                const QVariantMap s = treeState();
                const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
                check(s.value(QStringLiteral("contextPath")).toString().isEmpty()
                          && hl.value(QStringLiteral("context")).toInt() == 0,
                      QStringLiteral("右键：菜单收起后灰黑底也撤掉"),
                      QStringLiteral("菜单行 %1 / 灰黑底 %2")
                          .arg(s.value(QStringLiteral("contextPath")).toString())
                          .arg(hl.value(QStringLiteral("context")).toInt()));
            }

            /*
             * 一级（日期文件夹）和二级（文件）的图标要落在同一列上。
             *
             * 文件夹行比文件行多一格展开箭头，那一格文件行不占宽 —— 少补
             * 16-14=2px 的话两级的图标就是歪的（用户报的"没对齐"）。量的是
             * 委托自己报出来的坐标（TreeDelegate.iconCellX），不是把缩进公式
             * 在 C++ 这侧再算一遍。
             */
            {
                const QVariantMap cols =
                    treeState().value(QStringLiteral("iconColumns")).toMap();
                const double folderX = cols.value(QStringLiteral("folder")).toDouble();
                const double fileX = cols.value(QStringLiteral("file")).toDouble();
                check(folderX > 0 && fileX > 0 && qAbs(folderX - fileX) < 0.5,
                      QStringLiteral("左树：一级 / 二级图标左边对齐"),
                      QStringLiteral("文件夹图标 x=%1 / 文件图标 x=%2").arg(folderX).arg(fileX));
            }

            /*
             * 导入的文件夹要**原样**列出来：什么后缀都收（src 里的 .cpp/.h）、
             * 隐藏目录（.idea）要进去、一个文件都没有的空目录也得有一行。
             *
             * 用户报的就是这个：导入 H:\test 之后，TetrisGame\src 整块不见了
             * （上一版只收 md / markdown / txt），.idea 那个空目录也没有节点
             * （树是按文件拼的，没文件的目录出不来）。这里照那个形状造一份：
             *
             *     imported-project/
             *         README.md
             *         src/main.cpp
             *         .idea/            <- 空目录
             *         shot.png          <- 二进制，不许当文本解析
             */
            {
                const QString projectDir = dir.filePath(QStringLiteral("imported-project"));
                QDir().mkpath(projectDir + QStringLiteral("/src"));
                QDir().mkpath(projectDir + QStringLiteral("/.idea"));
                /* 依赖目录：只该留一行，里面的东西不进去扫（见 ClipboardStore::scanFolder） */
                QDir().mkpath(projectDir + QStringLiteral("/node_modules/dep"));
                auto writeFile = [](const QString &path, const QByteArray &bytes) {
                    QFile f(path);
                    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                        f.write(bytes);
                };
                writeFile(projectDir + QStringLiteral("/README.md"), "## 07:31:00\nhello\n");
                writeFile(projectDir + QStringLiteral("/src/main.cpp"), "int main() {}\n");
                writeFile(projectDir + QStringLiteral("/node_modules/dep/index.js"),
                          "module.exports = 1;\n");
                /* 带 NUL 的假图片：looksLikeText 该把它当二进制 */
                writeFile(projectDir + QStringLiteral("/shot.png"),
                          QByteArray("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16));

                check(store->addImportedFolder(projectDir),
                      QStringLiteral("导入用例：整个项目目录挂到左树上"));
                settle();

                /* 直接在"树"那份数据上找 —— QML 画的左树就是它 */
                std::function<bool(const QVariantList &, const QString &)> hasLabel =
                    [&](const QVariantList &list, const QString &label) -> bool {
                    for (const QVariant &v : std::as_const(list)) {
                        const QVariantMap m = v.toMap();
                        if (m.value(QStringLiteral("label")).toString() == label)
                            return true;
                        if (hasLabel(m.value(QStringLiteral("children")).toList(), label))
                            return true;
                    }
                    return false;
                };
                const QVariantList nodes = store->tree(QString(), true);
                check(hasLabel(nodes, QStringLiteral("src")),
                      QStringLiteral("导入：子目录 src 在树上（里面的 .cpp 也算数）"));
                check(hasLabel(nodes, QStringLiteral("main.cpp")),
                      QStringLiteral("导入：src 里的 .cpp 文件在树上"));
                check(hasLabel(nodes, QStringLiteral(".idea")),
                      QStringLiteral("导入：空目录 .idea 也在树上"));
                check(hasLabel(nodes, QStringLiteral("shot.png")),
                      QStringLiteral("导入：png 这类二进制也在树上"));

                /*
                 * 依赖目录（node_modules…）：**留一行，但不进去扫**。
                 *
                 * 这是"导入大文件夹卡死"的正解：H:\chat 三万四千个文件，三万三千
                 * 个在 node_modules 里，全过一遍库就是十几秒的卡死（实测）。主流
                 * 编辑器也是这么办的（排除依赖 / 构建目录）。那一行标成"未索引"，
                 * 不冒充"0 个文件"。
                 */
                {
                    std::function<QVariantMap(const QVariantList &, const QString &)> findLabel =
                        [&](const QVariantList &list, const QString &label) -> QVariantMap {
                        for (const QVariant &v : std::as_const(list)) {
                            const QVariantMap m = v.toMap();
                            if (m.value(QStringLiteral("label")).toString() == label)
                                return m;
                            const QVariantMap nested =
                                findLabel(m.value(QStringLiteral("children")).toList(), label);
                            if (!nested.isEmpty())
                                return nested;
                        }
                        return {};
                    };
                    const QVariantMap deps = findLabel(nodes, QStringLiteral("node_modules"));
                    check(!deps.isEmpty(),
                          QStringLiteral("导入：依赖目录 node_modules 留了一行"));
                    check(deps.value(QStringLiteral("skipped")).toBool(),
                          QStringLiteral("导入：那一行标着「未索引」（没进去扫）"),
                          QStringLiteral("skipped=%1")
                              .arg(deps.value(QStringLiteral("skipped")).toString()));
                    /*
                     * 判据只看这一棵子树：用户设置里可能还挂着别的导入目录，
                     * 树里别处出现同名的 index.js 不算数（第一版就这么误报了）。
                     */
                    check(deps.value(QStringLiteral("children")).toList().isEmpty()
                              && deps.value(QStringLiteral("files")).toInt() == 0,
                          QStringLiteral("导入：依赖目录里的文件一个都没列（不扫进去）"),
                          QStringLiteral("子节点 %1 / 文件 %2")
                              .arg(deps.value(QStringLiteral("children")).toList().size())
                              .arg(deps.value(QStringLiteral("files")).toInt()));
                }

                /* 二进制不当文本读：条数必须是 0，不能从乱码里数出几段来 */
                {
                    QVariant entries = -1;
                    for (const QVariant &v : std::as_const(nodes)) {
                        const QVariantMap root0 = v.toMap();
                        if (root0.value(QStringLiteral("label")).toString()
                            != QLatin1String("imported-project"))
                            continue;
                        for (const QVariant &c : root0.value(QStringLiteral("children")).toList()) {
                            const QVariantMap m = c.toMap();
                            if (m.value(QStringLiteral("label")).toString()
                                == QLatin1String("shot.png"))
                                entries = m.value(QStringLiteral("entries"));
                        }
                    }
                    check(entries.toInt() == 0,
                          QStringLiteral("导入：png 按二进制处理（不解析内容，条数 0）"),
                          QStringLiteral("条数 %1").arg(entries.toInt()));
                }

                /* 编辑器也不许把二进制当文本打开（灌进去就是乱码，存回去就毁了） */
                check(view->openFile(projectDir + QStringLiteral("/shot.png")) < 0,
                      QStringLiteral("二进制文件编辑器不开（拒绝而不是灌乱码）"),
                      view->lastError());

                /* 收尾：别把用户自己的导入列表改了 */
                check(store->removeImportedFolder(projectDir),
                      QStringLiteral("导入用例：收尾把目录移除"));
                settle();
                QDir(projectDir).removeRecursively();
            }
        }

        /*
         * 设置面板的"存储"栏。
         *
         * 那一栏里全是绑定（保存位置 / 文件数 / 内容条数 / 导入的文件夹），
         * 打开它等于把这些绑定真算一遍 —— QML 侧的绑定错误只有算过才暴露
         * （"Sequence length out of range" 就是这么抓出来的）。
         */
        dispatch(QStringLiteral("storage"));
        settle();
        {
            const QVariantMap panel = uiState();
            check(panel.value(QStringLiteral("settingsOpened")).toBool()
                      && panel.value(QStringLiteral("settingsSection")).toString()
                             == QLatin1String("storage"),
                  QStringLiteral("dispatch(storage) 打开设置面板的\"存储\"栏"),
                  panel.value(QStringLiteral("settingsSection")).toString());
            check(panel.value(QStringLiteral("storageRoot")).toString() == store->rootPath(),
                  QStringLiteral("\"存储\"栏显示的就是当前保存位置"),
                  panel.value(QStringLiteral("storageRoot")).toString());
            check(panel.value(QStringLiteral("storageEntries")).toInt() == store->entryCount(),
                  QStringLiteral("\"存储\"栏的内容条数和元数据一致"),
                  QStringLiteral("面板 %1 / 元数据 %2")
                      .arg(panel.value(QStringLiteral("storageEntries")).toInt())
                      .arg(store->entryCount()));
        }
        QMetaObject::invokeMethod(qmlRoot, "closeSettings");
        settle();

        /*
         * 设置面板这块窗口本身的两条要求。
         *
         *  1) 点面板外面的空白处不许自己收起来 —— 面板里那几个按钮弹的是
         *     **系统**文件对话框，用户去点那个对话框，按"点外面就收"的老规矩
         *     面板会先一步没掉（而它本该一直开着）。关它只能靠标题栏的 ✕ / Esc。
         *  2) 会话框出现时面板得让开一条路：这块窗口是 Qt 按 Popup.Window 建的，
         *     flags 里带着 WindowStaysOnTopHint（下面报出来那一行），Windows 上
         *     非置顶窗口永远盖不住置顶窗口，所以对话框只能出现在面板下面 ——
         *     见 Main.qml 的 withSettingsPanelAway。
         */
        dispatch(QStringLiteral("storage"));
        settle();
        {
            QWindow *panelWin = nullptr;
            for (QWindow *w : QGuiApplication::topLevelWindows()) {
                if (w->isVisible() && w->width() > 700 && w->width() < 900 && w->height() > 400)
                    panelWin = w;
            }
            check(panelWin != nullptr, QStringLiteral("设置面板：那块窗口开出来了"));
            if (panelWin) {
                /* 只报一声不断言：这是 Qt 建窗口的规矩，不是我们的设定 */
                out() << "        （设置面板窗口 flags = 0x"
                      << QString::number(int(panelWin->flags()), 16) << "）" << Qt::endl;
            }
            check(!uiState().value(QStringLiteral("settingsClosesOnOutside")).toBool(),
                  QStringLiteral("设置面板：点面板外面的空白处不会自己收起来"));

            /*
             * 面板里那几个按钮要弹系统文件夹选择框 —— 弹之前面板必须先让开，
             * 弹完还要原样回来（栏目、位置都不变）。
             */
            QVariant away;
            QMetaObject::invokeMethod(qmlRoot, "probeSettingsAway", Q_RETURN_ARG(QVariant, away));
            settle();
            check(away.toString() == QLatin1String("away"),
                  QStringLiteral("设置面板：弹系统对话框之前自己让开了"),
                  away.toString());
            check(uiState().value(QStringLiteral("settingsOpened")).toBool()
                      && uiState().value(QStringLiteral("settingsSection")).toString()
                             == QLatin1String("storage"),
                  QStringLiteral("设置面板：对话框关掉之后原栏目放回来"),
                  uiState().value(QStringLiteral("settingsSection")).toString());
        }
        QMetaObject::invokeMethod(qmlRoot, "closeSettings");
        settle();

        dispatch(QStringLiteral("treeHide"));
        settle();
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("hidden")).toBool(),
                  QStringLiteral("收起面板：标志位置上了"));
            check(s.value(QStringLiteral("panelWidth")).toDouble() < 0.5,
                  QStringLiteral("收起面板：布局里的槽位宽度归 0"),
                  QStringLiteral("实际 %1").arg(s.value(QStringLiteral("panelWidth")).toDouble()));
        }

        dispatch(QStringLiteral("treeHide"));
        settle();
        {
            const QVariantMap s = treeState();
            check(!s.value(QStringLiteral("hidden")).toBool()
                  && s.value(QStringLiteral("panelWidth")).toDouble() > 100.0,
                  QStringLiteral("再点一次面板回来（宽度还是收起前那个）"),
                  QStringLiteral("实际 %1").arg(s.value(QStringLiteral("panelWidth")).toDouble()));
        }
    }

    /*
     * ================= 新建 md / Ctrl+S 写回文件 / 改名 / 删除 =================
     *
     * 走路：Store.createFile（左侧树 "+" 那条路）
     *       -> 编辑器标签 -> 改一笔 -> saveCurrent() -> 磁盘上那份 md 跟着变；
     *       再走一遍重命名 / 删除（左树右键菜单那两条）。
     *
     * 同样跑在临时保存目录上（保存位置在上一段换成临时目录的），收尾时换回来。
     */
    if (store) {
        const QString path = store->createFile(QStringLiteral("自检新建的内容"));
        check(!path.isEmpty(), QStringLiteral("新建文件：在今天的目录里建出一份 md"), path);
        check(QFileInfo::exists(path), QStringLiteral("新建的文件真的在磁盘上"));
        check(QFileInfo(path).fileName().contains(QRegularExpression(QStringLiteral("^\\d{6}"))),
              QStringLiteral("新建的文件名也是时分秒"), QFileInfo(path).fileName());

        view->openFile(path);
        check(view->hasDocument(), QStringLiteral("新建的 md 能打开成标签"));
        check(QFileInfo(view->filePath()).absoluteFilePath() == QFileInfo(path).absoluteFilePath(),
              QStringLiteral("标签认得这份文件的路径"), view->filePath());
        check(view->currentText().contains(QStringLiteral("自检新建的内容")),
              QStringLiteral("新建时给的那段内容是文件正文的一部分"));

        /* 改一笔：复制一行（新建出来的文件第一行是 "# 日期" 标题，复制的是它） */
        const QString beforeEdit = readFile(path);
        view->duplicateLine();
        check(view->modified(), QStringLiteral("改一笔 -> 已修改状态"));

        check(view->saveCurrent(), QStringLiteral("md 标签 Ctrl+S 写回文件"), view->lastError());
        check(!view->modified(), QStringLiteral("写回之后修改标记清掉"));
        {
            /*
             * 判据是"磁盘上那份 = 编辑器里的正文"，不是"某段文字出现几次"：
             * 文件格式（日期标题 + "## 时分秒" 分段）以后可能变，这条断言不该跟着变。
             * Scintilla 会把 CRLF 归一，比较时先把 \r 抹平。
             */
            auto straight = [](QString s) {
                s.remove(QLatin1Char('\r'));
                return s;
            };
            const QString disk = readFile(path);
            check(disk.length() > beforeEdit.length(),
                  QStringLiteral("改的那一笔真的落到了磁盘上（文件变长了）"),
                  QStringLiteral("%1 -> %2 字符").arg(beforeEdit.length()).arg(disk.length()));
            check(straight(disk) == straight(view->currentText()),
                  QStringLiteral("磁盘上那份文件 = 编辑器里的正文"),
                  QStringLiteral("文件 %1 字符 / 编辑器 %2 字符")
                      .arg(disk.size()).arg(view->currentText().size()));
            check(disk.contains(QStringLiteral("自检新建的内容")),
                  QStringLiteral("新建时给的那段内容还在文件里"));
        }

        /* ---- 左树定位当前文件 ---- */
        dispatch(QStringLiteral("treeCollapseAll"));
        check(treeState().value(QStringLiteral("openFolders")).toInt() == 0,
              QStringLiteral("定位用例：先全部折叠，看它会不会自己展开"));

        dispatch(QStringLiteral("treeLocate"));
        settle();
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("currentPath")).toString()
                      == QFileInfo(path).absoluteFilePath(),
                  QStringLiteral("定位用例：当前标签认得出是哪一份文件（按路径认）"),
                  s.value(QStringLiteral("currentPath")).toString());
            check(s.value(QStringLiteral("selectedPath")).toString()
                      == QFileInfo(path).absoluteFilePath(),
                  QStringLiteral("定位用例：左树里选中的就是当前标签那一份"),
                  s.value(QStringLiteral("selectedPath")).toString());
            check(s.value(QStringLiteral("openFolders")).toInt() >= 1,
                  QStringLiteral("定位用例：它所在的那个日期目录被展开了"));

            /*
             * 蓝底只给选中的文件：日期文件夹那一级不亮。
             *
             * 量的是委托自己报的 rowHighlight（见 FolderTree.highlightCounts），
             * 不是把 QML 里那个表达式在 C++ 这侧再算一遍。
             */
            const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
            check(hl.value(QStringLiteral("folders")).toInt() == 0,
                  QStringLiteral("日期文件夹不亮蓝底"),
                  QStringLiteral("亮着的文件夹行 %1")
                      .arg(hl.value(QStringLiteral("folders")).toInt()));
            check(hl.value(QStringLiteral("files")).toInt() == 1,
                  QStringLiteral("蓝底只留在选中的那一份文件上"),
                  QStringLiteral("亮着的文件行 %1")
                      .arg(hl.value(QStringLiteral("files")).toInt()));
        }

        /*
         * 关掉标签之后，左树那一行的蓝底也要跟着撤掉。
         *
         * 用户报的：右边标签关光了、编辑区回到欢迎页，左边还蓝着一行，看着
         * 像那份文件还开着。判据取两处：selectedPath 清空 + 委托自己报的亮行
         * 数归零（不把 QML 里那个表达式在 C++ 这侧再算一遍）。
         */
        dispatch(QStringLiteral("closeTab"));
        settle();
        {
            const QVariantMap s = treeState();
            check(s.value(QStringLiteral("selectedPath")).toString().isEmpty(),
                  QStringLiteral("关掉标签后左树不再选中它"),
                  QStringLiteral("还选着 %1").arg(s.value(QStringLiteral("selectedPath")).toString()));
            const QVariantMap hl = s.value(QStringLiteral("highlighted")).toMap();
            check(hl.value(QStringLiteral("files")).toInt() == 0,
                  QStringLiteral("关掉标签后那一行的蓝底也撤掉"),
                  QStringLiteral("亮着的文件行 %1")
                      .arg(hl.value(QStringLiteral("files")).toInt()));
        }

        /* 下面那条用例要在标签开着的前提下改名，所以重新打开它 */
        check(view->openFile(path) >= 0, QStringLiteral("重新打开它，接着测重命名"));
        settle();

        /* ---- 重命名：磁盘上的文件和开着的标签一起改 ---- */
        const QString newName = QStringLiteral("selfcheck-renamed.md");
        const QString renamed = QFileInfo(path).absolutePath() + QLatin1Char('/') + newName;
        check(store->renameFile(path, newName), QStringLiteral("重命名：文件改名成功"));
        check(QFileInfo::exists(renamed), QStringLiteral("改完名字的文件在磁盘上"), renamed);
        check(!QFileInfo::exists(path), QStringLiteral("老名字那份已经不在了"));
        check(view->updateDocumentPath(path, renamed),
              QStringLiteral("打开着的标签跟着换路径（不换的话下一次 Ctrl+S 会写回老名字）"));
        check(QFileInfo(view->filePath()).absoluteFilePath() == QFileInfo(renamed).absoluteFilePath(),
              QStringLiteral("标签上的路径就是新名字"), view->filePath());
        check(!store->renameFile(QStringLiteral("这个文件不存在.md"), QStringLiteral("x")),
              QStringLiteral("重命名不存在的文件会失败（不会悄悄新建一份）"));

        /* ---- 删除：文件从磁盘上消失，元数据也跟着清 ---- */
        const int entriesBefore = store->entryCount();
        view->closeDocument(view->indexOfPath(renamed));
        check(store->deleteFile(renamed), QStringLiteral("删除：文件真的被删掉了"));
        check(!QFileInfo::exists(renamed), QStringLiteral("磁盘上那份已经不在了"));
        check(store->entryCount() < entriesBefore,
              QStringLiteral("删掉文件之后它的条目元数据也清了"),
              QStringLiteral("%1 -> %2").arg(entriesBefore).arg(store->entryCount()));
        check(!store->deleteFile(renamed),
              QStringLiteral("再删一次会失败（文件已经不在了）"));

        /*
         * 不在左树管得着的范围里的标签（未命名空白文档）没什么可定位的：
         * 准星按钮该是灰的。
         */
        dispatch(QStringLiteral("new"));
        {
            const QVariantMap s = treeState();
            check(!s.value(QStringLiteral("locateEnabled")).toBool()
                      && s.value(QStringLiteral("currentPath")).toString().isEmpty(),
                  QStringLiteral("定位按钮：未命名空白标签时置灰（没有可定位的文件）"));
        }
        dispatch(QStringLiteral("closeTab"));

        /*
         * 收尾：保存位置换回用户原来那个。
         *
         * setRootPath 会顺带重扫一遍，所以元数据也跟着回到真实目录上
         * （临时目录那些记录会在"扫不到的文件"那一步被清掉）。
         */
        check(store->setRootPath(savedRoot), QStringLiteral("收尾：保存位置换回原目录"));
        check(store->rootPath() == QDir::cleanPath(savedRoot),
              QStringLiteral("收尾：读回来就是用户原来那个目录"), store->rootPath());
    }

    /*
     * ================= 子菜单（视图 -> 语言 / 编码 / 换行符） =================
     *
     * 这三条要在主菜单**右边**再展开一栏（条目就在 js/EditorMenus.js 里挂着
     * submenu: true + items 的那几条）。要钉三件事：
     *
     *  1) 真的多出一栏，而且那一栏在主栏右边（量位置，不是量"展开了"这个标志）；
     *  2) 长列表（语言 27 项）照样限高 + 可滚动；
     *  3) 两条入口都能通：菜单栏那条（dispatch("menu:视图") 开主菜单）
     *     和条目自己的 items。
     *
     * 悬停那一下 C++ 点不出来，所以走 Main.openSubmenuFor —— 它和界面用的是
     * 同一份 Menus.menuItems("视图") 构造、同一个 ddMenu.openSubmenu()。
     *
     * 先开一个空白标签：语言 / 编码 两张表是按**当前文档**算出来的
     * （没文档时 languageItems() 直接返回空表，子菜单也就没什么可展开的）。
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

    /*
     * "文件"菜单里既要能打开文件，也要能打开文件夹（= 把目录挂到左树上，
     * 就是本程序里"打开一个项目"的意思）—— 上一版只有"打开…"，想开目录
     * 得绕到左边树的"更多"里去找。这里核对的就是界面上那份菜单本身。
     */
    {
        QVariant acts;
        QMetaObject::invokeMethod(qmlRoot, "topMenuActs", Q_RETURN_ARG(QVariant, acts),
                                  Q_ARG(QVariant, QVariant(QStringLiteral("文件"))));
        QStringList names;
        for (const QVariant &a : acts.toList())
            names << a.toString();
        check(names.contains(QStringLiteral("open"))
                  && names.contains(QStringLiteral("treeImportFolder")),
              QStringLiteral("\"文件\"菜单里既能打开文件、也能打开文件夹"),
              names.join(QLatin1Char('/')));
    }

    dispatch(QStringLiteral("menu:视图"));
    /*
     * 等一帧：菜单条目的 y 是 Column（positioner）算的，下一帧才摆到位 ——
     * 同一轮事件里读，每条都还是 y = 0，子菜单就会"对齐到第一行"。
     */
    settle();
    {
        const QVariantMap ui = uiState();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("dispatch(menu:视图) 打开\"视图\"菜单"));
        check(!ui.value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("刚打开时右边还没有子菜单那一栏"));
    }

    {
        QVariant opened;
        QMetaObject::invokeMethod(qmlRoot, "openSubmenuFor", Q_RETURN_ARG(QVariant, opened),
                                  Q_ARG(QVariant, QVariant(QStringLiteral("menu:语言"))));
        check(opened.toBool(), QStringLiteral("openSubmenuFor(menu:语言) 展开了子菜单"));

        const QVariantMap ui = uiState();
        const double subH = ui.value(QStringLiteral("submenuHeight")).toDouble();
        const double subContentH = ui.value(QStringLiteral("submenuContentHeight")).toDouble();
        const double paneW = ui.value(QStringLiteral("menuPaneWidth")).toDouble();
        const double inset = ui.value(QStringLiteral("submenuInset")).toDouble();
        const double totalW = ui.value(QStringLiteral("menuTotalWidth")).toDouble();
        const double totalH = ui.value(QStringLiteral("menuTotalHeight")).toDouble();
        const double mainH = ui.value(QStringLiteral("menuHeight")).toDouble();
        const double subTop = ui.value(QStringLiteral("submenuTop")).toDouble();
        const double subRowY = ui.value(QStringLiteral("submenuRowY")).toDouble();
        const double gap = ui.value(QStringLiteral("menuPaneGap")).toDouble();

        check(ui.value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("语言子菜单那一块真的画出来了"));
        check(qAbs(inset - (paneW + gap)) < 0.5,
              QStringLiteral("子菜单在主菜单右边，中间留着一条缝"),
              QStringLiteral("子栏 x %1 / 主栏宽 %2 / 缝 %3").arg(inset).arg(paneW).arg(gap));
        check(gap >= 3.0, QStringLiteral("那条缝够看得见两块面板各自的圆角"),
              QStringLiteral("缝 %1").arg(gap));
        check(qAbs(totalW - (paneW * 2 + gap)) < 0.5,
              QStringLiteral("弹窗宽度 = 两块面板 + 中间那条缝"),
              QStringLiteral("弹窗宽 %1").arg(totalW));
        /*
         * 顶边对齐"父级那一条所在的行"，不是贴到菜单顶上 ——
         * "语言"在"视图"菜单里靠下（y 远大于 0），所以这两条一量就能分清。
         */
        check(qAbs(subTop - subRowY) < 0.5,
              QStringLiteral("子菜单顶边 = 父级那一条所在的行（没被夹到菜单顶上）"),
              QStringLiteral("子栏顶边 %1 / 那一行 %2").arg(subTop).arg(subRowY));
        check(subRowY > 100.0, QStringLiteral("对的是菜单靠下的那一条（不是第一行）"),
              QStringLiteral("行 y %1").arg(subRowY));
        check(totalH > mainH + 0.5,
              QStringLiteral("弹窗往下长高，把从中间那一行伸出来的子栏装下"),
              QStringLiteral("弹窗高 %1 / 主栏高 %2").arg(totalH).arg(mainH));
        check(subContentH > 700, QStringLiteral("语言子菜单条目总高 > 700px（27 项）"),
              QStringLiteral("实际 %1").arg(subContentH));
        check(subH < subContentH, QStringLiteral("长子菜单被限高，不再整块铺下去"),
              QStringLiteral("画出来 %1 / 内容 %2").arg(subH).arg(subContentH));
        check(subH <= 461, QStringLiteral("子菜单高度夹在 maxMenuHeight 以内"),
              QStringLiteral("实际 %1").arg(subH));
        check(ui.value(QStringLiteral("submenuScrollable")).toBool(),
              QStringLiteral("长子菜单标记为可滚动"));

        /*
         * 鼠标往右挪进子菜单：进的是子菜单里第一条，**不能**把子菜单收掉。
         *
         * 这一步原来错了 —— 委托把"普通条目"的收子菜单逻辑也用在子菜单自己的
         * 条目上，于是鼠标刚移进去面板就没了（用户报的"还没移上去就消失了"）。
         * 这里走的是 DropdownMenu.hoverEntry()，和真实悬停同一份判断。
         */
        QVariant stillOpen;
        QMetaObject::invokeMethod(qmlRoot, "hoverSubmenuEntry", Q_RETURN_ARG(QVariant, stillOpen),
                                  Q_ARG(QVariant, QVariant(0)));
        check(stillOpen.toBool() && uiState().value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("鼠标移进子菜单里第一条，子菜单不会自己收掉"));
    }

    /* 子菜单里的条目照样能点：选一门语言，菜单自己也收掉 */
    dispatch(QStringLiteral("lang:python"));
    check(view->language() == QLatin1String("python"),
          QStringLiteral("子菜单里的条目（lang:python）能执行"));

    /*
     * ================= 编辑区的右键菜单 =================
     *
     * 编辑器里按右键弹的必须是 **QML 那套菜单**（和菜单栏"编辑"同一份构造），
     * 不是 Scintilla 自带的 QtWidgets 菜单 —— 后者英文、观感也和界面不搭。
     *
     * C++ 侧点不出右键，所以走 Main.openEditorContextMenu（编辑器那个
     * contextMenuRequested 信号最终就是调它）。要钉两件事：
     *   1) 菜单真的弹在了右键那一点上；
     *   2) 弹出的是"编辑"菜单那一份（条目总高对得上）。
     */
    const double editProbeX = 620.0;
    const double editProbeY = 260.0;
    QMetaObject::invokeMethod(qmlRoot, "openEditorContextMenu",
                              Q_ARG(QVariant, QVariant(editProbeX)),
                              Q_ARG(QVariant, QVariant(editProbeY)));
    {
        const QVariantMap ui = uiState();
        const double mx = ui.value(QStringLiteral("menuX")).toDouble();
        const double my = ui.value(QStringLiteral("menuY")).toDouble();
        const double contentH = ui.value(QStringLiteral("menuContentHeight")).toDouble();
        check(ui.value(QStringLiteral("menuOpened")).toBool(),
              QStringLiteral("编辑区右键弹出 QML 菜单"));
        check(qAbs(mx - editProbeX) < 0.5 && qAbs(my - editProbeY) < 0.5,
              QStringLiteral("右键菜单左上角紧贴鼠标点（620,260）"),
              QStringLiteral("实际 (%1, %2)").arg(mx).arg(my));
        /* 12 条命令 + 4 条分隔线：12*28 + 4*9 + 上下各 4px 内缩 = 380 */
        check(qAbs(contentH - 380.0) < 0.5,
              QStringLiteral("弹的就是\"编辑\"菜单那一份（12 项 + 4 分隔线）"),
              QStringLiteral("内容高 %1").arg(contentH));
        check(ui.value(QStringLiteral("menuHasIcons")).toBool(),
              QStringLiteral("右键菜单条目也带图标（和菜单栏一致）"));
    }
    QMetaObject::invokeMethod(qmlRoot, "closeMenu");

    /*
     * 长下拉菜单必须限高 + 可滚动（老路子：直接把语言列表当整个菜单弹出来）。
     *
     * dispatch("menu:语言") 这条仍然可用 —— 它不管子菜单那套，直接把列表
     * 当主菜单摆出来。放在最后做：菜单开着直接退出进程。
     */
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
        check(!ui.value(QStringLiteral("submenuOpened")).toBool(),
              QStringLiteral("当主菜单弹出来时右边不留上一个子菜单"));
    }

    /*
     * 收尾：把菜单关掉再退出。
     *
     * 弹窗是独立原生窗口（见 DropdownMenu.qml 开头），留着它在进程退出时拆，
     * 偶尔会踩到拆除顺序的竞态 —— 实测有过一次 0xC0000005（访问冲突），
     * 报出来的却是"自检崩了"，而检查项其实一条没挂。
     * 在事件循环还活着的时候正常 close()，这个假故障就没了。
     */
    QMetaObject::invokeMethod(qmlRoot, "closeMenu");

    /* 还原用户自己的自动换行设置（起点在"dispatch(toggleWrap)"那一处钉过） */
    view->setWrapEnabled(wrapAtStart);

    /*
     * ============ 截图（抓屏 -> 选区 -> 加文字 -> 合成 / 贴图） ============
     *
     * 为什么走选区窗口 QML 上那几个 test* 函数，而不是合成键鼠去点：
     * 选区窗口是**独立的原生置顶窗口**，和编辑区那个 QScintilla 一样，
     * 系统级的合成鼠标事件进不到里面（见文件头）。所以这里调的是
     * CaptureOverlay.qml 里那几个函数 —— 它们和界面上的操作是**同一批**：
     *
     *   testSelect -> 拖框那一步（同一个 sel）
     *   testAddText -> 文字工具点一下（同一个 addText）
     *   textsData -> 工具条上"复制 / 保存 / 贴图"要传的那份数据
     *
     * 要钉的几件事：真的抓到屏了、选区窗口铺满整块屏、框出来的选区尺寸对得上、
     * 文字真的画进了最终图（不是只有预览里有）、三条出口都能出图。
     */
    if (shot) {
        /* 自检会往剪贴板里放图，先记着原来的文字，收尾放回去 */
        const QString oldClipboard = QGuiApplication::clipboard()->text();

        /*
         * 预热必须**真的把窗口在幕外映射并画过**（见 Screenshot::prewarm）。
         * 否则第一次 show() 时 DWM 手上是空的，会和 Qt 的首帧赛跑 ——
         * 抢在前面就是"第一次按快捷键偶发闪一下整屏"。
         */
        check(shot->overlayWarmed(),
              QStringLiteral("截图：选区窗口启动时已在幕外画过一帧（第一次抓屏不带空窗帧）"));

        /*
         * ============ 复现：启动后**第一次**抓屏，马上按 Esc 取消 ============
         *
         * 用户报的就是这一下：程序起来之后第一次按截图快捷键，屏幕上先亮起
         * 一整块全屏选区（要再按一次 Esc 才关得掉）。关键是"第一次" —— 所以
         * 这段必须放在**任何别的抓屏之前**：上面那些预热检查没有动过窗口，
         * 此刻的选区窗口正是启动预热留下的那份状态（见 Screenshot::prewarm），
         * 和用户冷启动后第一次按键时一模一样。
         *
         * 走 QML 里和 Esc Shortcut **一字不差**的代码（captureCancelDuringPending），
         * 同时从 C++ 这边每 3ms 采一次窗口可见性 —— "闪一下"这种一闪而过的
         * 残留只有这么采样才抓得住。取消时刻试 0/5/15/25ms（都在延时里）和
         * 60ms（延时之后，窗口本来就该开着，取消就该关掉它）。
         */
        {
            const int cancelAt[] = { 0, 5, 15, 25, 60 };
            for (int at : cancelAt) {
                QObject *root = shot->overlayRoot();
                if (!root)
                    break;
                QVariant probe;
                QMetaObject::invokeMethod(root, "captureCancelDuringPending",
                                          Q_RETURN_ARG(QVariant, probe),
                                          Q_ARG(QVariant, QVariant(at)));
                const QVariantMap probeMap = probe.toMap();

                int visibleFrames = 0;
                long long lastVisibleMs = -1;
                QElapsedTimer watch;
                watch.start();
                while (watch.elapsed() < 1200) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 3);
                    QThread::msleep(3);
                    if (shot->overlayVisible()) {
                        ++visibleFrames;
                        lastVisibleMs = watch.elapsed();
                    }
                }
                const int qmlFrames = probeMap.value(QStringLiteral("frames")).toInt();

                /*
                 * 延时里取消（at < 30）：窗口一帧都不该露。
                 * 延时之后取消（at >= 30）：窗口先开出来了，取消必须把它关掉。
                 */
                if (at < 30) {
                    check(visibleFrames == 0 && qmlFrames == 0,
                          QStringLiteral("截图：第一次抓屏、%1ms 时按 Esc 取消 -> 窗口一帧都没露")
                              .arg(at),
                          QStringLiteral("C++ 数到 %1 帧 / QML 数到 %2 帧")
                              .arg(visibleFrames).arg(qmlFrames));
                } else {
                    check(!shot->overlayVisible(),
                          QStringLiteral("截图：第一次抓屏、%1ms 时按 Esc（窗口已开）-> 收工后是关着的")
                              .arg(at),
                          QStringLiteral("最后可见于 %1ms").arg(lastVisibleMs));
                }

                /* 复位，别把上一次的窗口状态带进下一次 */
                shot->cancelCapture();
                for (int i = 0; i < 20; ++i) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                    QThread::msleep(5);
                }
            }
        }

        shot->beginCapture();
        /*
         * 抓屏是**延时**的（要先等主窗口藏起来那一帧重画完，见
         * Screenshot::beginCapture），所以这里等它真正进入截图状态。
         *
         * 判据用 active()、不是 overlayRoot()：选区窗口现在是预建复用的
         * （Screenshot::prewarm），窗口对象一开始就在，拿它当"出来了吗"
         * 会立刻返回、后面全成时序赌运气。
         */
        for (int i = 0; i < 60 && !shot->active(); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(25);
        }

        QObject *overlay = shot->overlayRoot();
        check(overlay != nullptr && shot->overlayVisible(),
              QStringLiteral("截图：抓屏之后选区窗口显示出来了"));
        check(shot->active(), QStringLiteral("截图：处于截图状态（active = true）"));

        if (overlay) {
            settle();

            /*
             * "撤销"按钮的可用状态。
             *
             * 钉这个是因为踩过：history 是 JS 数组，原地 push 不触发绑定，
             * 按钮的 enabled 一直停在启动时那次求值上 —— 画了再多标注，
             * "撤销"也一直是灰的、点不动。所以"还没有标注 -> 不可用，
             * 落一条之后 -> 可用"这两头都得量一下。
             */
            check(!overlay->property("undoAvailable").toBool(),
                  QStringLiteral("截图：刚开出来（还没标注）时「撤销」是置灰的"));

            const double overlayW = overlay->property("width").toDouble();
            const double overlayH = overlay->property("height").toDouble();
            check(overlayW > 640 && overlayH > 480,
                  QStringLiteral("截图：选区窗口铺满整块屏（不是一个小窗）"),
                  QStringLiteral("窗口 %1x%2").arg(overlayW).arg(overlayH));

            const QImage frozen =
                shot->imageForId(QStringLiteral("full%1").arg(shot->serial()));
            const double dpr = frozen.devicePixelRatio() > 0 ? frozen.devicePixelRatio() : 1.0;
            check(!frozen.isNull()
                      && frozen.width() >= qRound(overlayW * dpr) - 1
                      && frozen.height() >= qRound(overlayH * dpr) - 1,
                  QStringLiteral("截图：冻结图是按设备像素抓下来的（不小于窗口尺寸 × DPR）"),
                  QStringLiteral("图 %1x%2（dpr %3）/ 窗口 %4x%5")
                      .arg(frozen.width()).arg(frozen.height()).arg(dpr)
                      .arg(overlayW).arg(overlayH));

            /*
             * 复位时选区必须**按调用方给的矩形**，不能按控件尺寸猜。
             *
             * 这是"闪一下方框轮廓"的根：选区窗口预建复用（Screenshot::prewarm），
             * setGeometry 之后 QQuickWidget 的布局是延迟生效的 —— 复位那一刻读
             * width/height 拿到的还是预热时的旧尺寸（640x480 那种），于是头一两帧
             * 按一个小方框画出来，等 resize 事件到了才变成整屏。所以改成由 C++
             * 把屏幕矩形直接传进来（见 CaptureOverlay::resetForCapture 的说明）。
             */
            {
                QMetaObject::invokeMethod(overlay, "resetForCapture",
                                          Q_ARG(QVariant, QVariant::fromValue(QRectF(11, 22, 333, 155))));
                /* 等布局落定再读（setGeometry 之后 QQuickWidget 的布局是延迟生效的） */
                settle();
                const QRectF got = overlay->property("sel").toRectF();
                check(qAbs(got.x() - 11) < 1.5 && qAbs(got.y() - 22) < 1.5
                          && qAbs(got.width() - 333) < 1.5 && qAbs(got.height() - 155) < 1.5,
                      QStringLiteral("截图：复位时选区按调用方给的矩形（不按控件尺寸猜）"),
                      QStringLiteral("得到 %1×%2 @%3,%4")
                          .arg(got.width()).arg(got.height()).arg(got.x()).arg(got.y()));
            }

            /* 框一块 + 在框里落一条文字（走界面上那同一批函数） */
            const double selX = 120.0, selY = 90.0, selW = 420.0, selH = 260.0;

            /*
             * 浮动工具条的位置：还没框选（选区是整屏）时在**右上角**，框选之后
             * 贴着选区下沿。
             *
             * 这两条都量，是因为"右上角"这个位置被来回改过：曾经为了消灭"一进
             * 截图先闪在右上角"改成过"贴鼠标"，用户明确要求改回右上角，所以这里
             * 把**两边**都钉住，以后谁再动它都会红。
             *
             * 期望值按产品里同一套夹取算：工具条比选区宽时右边顶出屏幕，x 会被
             * 夹回窗口里（bar 的 x = max(8, min(ctrlW - bw - 8, 选区右下 - bw))）。
             */
            {
                QMetaObject::invokeMethod(
                    overlay, "resetForCapture",
                    Q_ARG(QVariant, QVariant::fromValue(QRectF(0, 0, 1500, 900))));
                settle();

                /* 工具条是可视项（QQuickItem），C++ 的 findChild 够不着 —— 让根对象报 */
                QVariant barStateVar;
                QMetaObject::invokeMethod(overlay, "barState",
                                          Q_RETURN_ARG(QVariant, barStateVar));
                const QVariantMap bs = barStateVar.toMap();
                const double bw = bs.value(QStringLiteral("width")).toDouble();
                const double bh = bs.value(QStringLiteral("height")).toDouble();
                check(!bs.isEmpty() && bh > 20 && bs.contains(QStringLiteral("atScreensRight")),
                      QStringLiteral("截图：读得到浮动工具条的状态（barState）"),
                      QStringLiteral("工具条 %1,%2（%3×%4）")
                          .arg(bs.value(QStringLiteral("x")).toDouble())
                          .arg(bs.value(QStringLiteral("y")).toDouble())
                          .arg(bw).arg(bh));
                check(bs.value(QStringLiteral("atScreensRight")).toBool(),
                      QStringLiteral("截图：还没框选时工具条按整屏选区定位（右上角）"));

                /*
                 * 复位之后**不许**马上画选区边框（见 CaptureOverlay.qml 的
                 * overlayReady）。
                 *
                 * 用户报的："取消截图后全屏出现了一下绿色边框"。抓屏 + 4K 底图上传
                 * 要几十毫秒，这期间窗口可能已经 show() 出来了，而框选默认是整屏
                 * —— 那圈"全屏选区"的边框就会先亮一下。所以复位时先关掉，等窗口
                 * 稳住了由 settleOverlay() 打开。
                 */
                check(!bs.value(QStringLiteral("ready")).toBool()
                          && !bs.value(QStringLiteral("borderVisible")).toBool(),
                      QStringLiteral("截图：刚复位时先把选区边框压住（取消时不会闪一下全屏边框）"));

                QMetaObject::invokeMethod(overlay, "settleOverlay");
                settle();
                QMetaObject::invokeMethod(overlay, "barState",
                                          Q_RETURN_ARG(QVariant, barStateVar));
                const QVariantMap bsReady = barStateVar.toMap();
                check(bsReady.value(QStringLiteral("ready")).toBool()
                          && bsReady.value(QStringLiteral("borderVisible")).toBool(),
                      QStringLiteral("截图：窗口稳住之后（settleOverlay）选区边框才出来"));

                const double ctrlW = overlay->property("width").toDouble();
                const double wantRightX = qMax(8.0, qMin(ctrlW - bw - 8.0, 1500.0 - bw));
                check(qAbs(bs.value(QStringLiteral("x")).toDouble() - wantRightX) < 8,
                      QStringLiteral("截图：一进截图工具条在右沿（剪掉夹取那部分）"),
                      QStringLiteral("工具条 x=%1 / 期望 %2（控件宽 %3，条宽 %4）")
                          .arg(bs.value(QStringLiteral("x")).toDouble())
                          .arg(wantRightX).arg(ctrlW).arg(bw));

                /* 框选之后贴到选区下沿 */
                QMetaObject::invokeMethod(overlay, "testSelect", Q_ARG(QVariant, selX),
                                          Q_ARG(QVariant, selY), Q_ARG(QVariant, selW),
                                          Q_ARG(QVariant, selH));
                settle();
                QMetaObject::invokeMethod(overlay, "barState",
                                          Q_RETURN_ARG(QVariant, barStateVar));
                const QVariantMap bs2 = barStateVar.toMap();
                const double wantX = qMax(8.0, qMin(ctrlW - bw - 8.0, selX + selW - bw));
                check(qAbs(bs2.value(QStringLiteral("x")).toDouble() - wantX) < 8
                          && qAbs(bs2.value(QStringLiteral("y")).toDouble()
                                  - (selY + selH + 10)) < 8,
                      QStringLiteral("截图：框选之后工具条贴着选区下沿"),
                      QStringLiteral("工具条 %1,%2 / 期望 %3,%4")
                          .arg(bs2.value(QStringLiteral("x")).toDouble())
                          .arg(bs2.value(QStringLiteral("y")).toDouble())
                          .arg(wantX).arg(selY + selH + 10));

                /* 复位回整屏、清掉这次试出来的选区，别带进后面的检查 */
                QMetaObject::invokeMethod(
                    overlay, "resetForCapture",
                    Q_ARG(QVariant, QVariant::fromValue(QRectF(0, 0, 1500, 900))));
                settle();
            }

            QMetaObject::invokeMethod(overlay, "testSelect", Q_ARG(QVariant, selX),
                                      Q_ARG(QVariant, selY), Q_ARG(QVariant, selW),
                                      Q_ARG(QVariant, selH));
            QMetaObject::invokeMethod(overlay, "testTextTool", Q_ARG(QVariant, true));

            QVariant added;
            QMetaObject::invokeMethod(overlay, "testAddText", Q_RETURN_ARG(QVariant, added),
                                      Q_ARG(QVariant, selX + 40), Q_ARG(QVariant, selY + 60),
                                      Q_ARG(QVariant, QStringLiteral("自检文字")));
            check(added.toBool(), QStringLiteral("截图：文字工具在选区里落下一段文字"));
            /* 落了一条之后"撤销"必须变成可点的（不能一直是灰的） */
            check(overlay->property("undoAvailable").toBool(),
                  QStringLiteral("截图：落一条标注之后「撤销」变成可点（不再是灰的）"));

            QVariant textsVar;
            QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, textsVar));
            const QVariantList texts = textsVar.toList();
            check(texts.size() == 1
                      && texts.first().toMap().value(QStringLiteral("text")).toString()
                             == QStringLiteral("自检文字"),
                  QStringLiteral("截图：标注数据（x/y/文字/字号/颜色）齐了"),
                  QStringLiteral("条目数 %1").arg(texts.size()));

            /*
             * QML 与 C++ 之间那份数据的**字段契约**：合成要用到框宽 / 框高 /
             * 旋转角（见 Screenshot::compose）。哪天 QML 那边改了字段名而
             * C++ 没跟着改，合成出来的位置就是错的，而且不报错 —— 钉一下。
             */
            {
                const QVariantMap first = texts.isEmpty() ? QVariantMap()
                                                          : texts.first().toMap();
                check(first.contains(QStringLiteral("w"))
                          && first.value(QStringLiteral("w")).toDouble() > 0
                          && first.contains(QStringLiteral("h"))
                          && first.value(QStringLiteral("h")).toDouble() > 0
                          && first.contains(QStringLiteral("rot")),
                      QStringLiteral("截图：标注带着框宽 / 框高 / 旋转角（合成的三个输入）"),
                      QStringLiteral("w=%1 h=%2 rot=%3")
                          .arg(first.value(QStringLiteral("w")).toDouble())
                          .arg(first.value(QStringLiteral("h")).toDouble())
                          .arg(first.value(QStringLiteral("rot")).toDouble()));
            }

            /*
             * 文字工具是"按住左键拖出文本框"：拖出 200 × 90，框就该是这个尺寸
             * （走的是界面上同一个 resizeBox），反向拖落出来的矩形要一样。
             */
            {
                QVariant boxed;
                QMetaObject::invokeMethod(overlay, "testBoxDrag", Q_RETURN_ARG(QVariant, boxed),
                                          Q_ARG(QVariant, selX + 200), Q_ARG(QVariant, selY + 120),
                                          Q_ARG(QVariant, selX + 400), Q_ARG(QVariant, selY + 210));
                QVariant boxTexts;
                QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, boxTexts));
                const QVariantMap drawn = boxTexts.toList().isEmpty()
                                              ? QVariantMap()
                                              : boxTexts.toList().last().toMap();
                check(boxed.toBool()
                          && qAbs(drawn.value(QStringLiteral("w")).toDouble() - 200.0) < 1.5
                          && qAbs(drawn.value(QStringLiteral("h")).toDouble() - 90.0) < 1.5,
                      QStringLiteral("截图：文字工具拖出来的框就是拖的那个尺寸（200 × 90）"),
                      QStringLiteral("w=%1 h=%2")
                          .arg(drawn.value(QStringLiteral("w")).toDouble())
                          .arg(drawn.value(QStringLiteral("h")).toDouble()));

                QVariant back;
                QMetaObject::invokeMethod(overlay, "testBoxDrag", Q_RETURN_ARG(QVariant, back),
                                          Q_ARG(QVariant, selX + 400), Q_ARG(QVariant, selY + 210),
                                          Q_ARG(QVariant, selX + 200), Q_ARG(QVariant, selY + 120));
                QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, boxTexts));
                const QVariantMap backDrawn = boxTexts.toList().isEmpty()
                                                  ? QVariantMap()
                                                  : boxTexts.toList().last().toMap();
                check(back.toBool()
                          && qAbs(backDrawn.value(QStringLiteral("x")).toDouble() - (selX + 200)) < 1.5
                          && qAbs(backDrawn.value(QStringLiteral("y")).toDouble() - (selY + 120)) < 1.5
                          && qAbs(backDrawn.value(QStringLiteral("w")).toDouble() - 200.0) < 1.5,
                      QStringLiteral("截图：反向拖（右下往左上）落出来的框位置一样"),
                      QStringLiteral("x=%1 y=%2 w=%3")
                          .arg(backDrawn.value(QStringLiteral("x")).toDouble())
                          .arg(backDrawn.value(QStringLiteral("y")).toDouble())
                          .arg(backDrawn.value(QStringLiteral("w")).toDouble()));
            }

            /*
             * 三条边手柄（左 / 右 / 下）：走的是界面上同一个 dragEdge。
             * 靶子就是上面拖出来的那个 200 × 90 的空框。
             */
            {
                QVariant all;
                QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, all));
                const int boxIndex = all.toList().size() - 1;
                auto boxState = [&]() {
                    QVariant now;
                    QMetaObject::invokeMethod(overlay, "textsData", Q_RETURN_ARG(QVariant, now));
                    const QVariantList list = now.toList();
                    return (boxIndex >= 0 && boxIndex < list.size()) ? list.at(boxIndex).toMap()
                                                                     : QVariantMap();
                };
                auto dragEdge = [&](const QString &edge, double dx, double dy) {
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testEdgeDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex), Q_ARG(QVariant, edge),
                                              Q_ARG(QVariant, dx), Q_ARG(QVariant, dy));
                    return ok.toBool();
                };
                auto num = [](const QVariantMap &m, const char *key) {
                    return m.value(QString::fromLatin1(key)).toDouble();
                };

                dragEdge(QStringLiteral("right"), 100.0, 0.0);
                QVariantMap st = boxState();
                check(qAbs(num(st, "w") - 300.0) < 1.5
                          && qAbs(num(st, "x") - (selX + 200)) < 1.5,
                      QStringLiteral("截图：拖右边 → 框变宽、左边不动"),
                      QStringLiteral("x=%1 w=%2").arg(num(st, "x")).arg(num(st, "w")));

                dragEdge(QStringLiteral("left"), -80.0, 0.0);
                st = boxState();
                check(qAbs(num(st, "w") - 380.0) < 1.5
                          && qAbs(num(st, "x") - (selX + 120)) < 1.5,
                      QStringLiteral("截图：拖左边 → 框变宽、左上角跟着往左移"),
                      QStringLiteral("x=%1 w=%2").arg(num(st, "x")).arg(num(st, "w")));

                dragEdge(QStringLiteral("bottom"), 0.0, 60.0);
                st = boxState();
                check(qAbs(num(st, "h") - 150.0) < 1.5
                          && qAbs(num(st, "y") - (selY + 120)) < 1.5,
                      QStringLiteral("截图：拖下边 → 框变高、上边不动"),
                      QStringLiteral("y=%1 h=%2").arg(num(st, "y")).arg(num(st, "h")));

                /* 第四条边：上边往下拖 60 → 变矮，且上边界跟着往下走 */
                dragEdge(QStringLiteral("top"), 0.0, 60.0);
                st = boxState();
                check(qAbs(num(st, "h") - 90.0) < 1.5
                          && qAbs(num(st, "y") - (selY + 180)) < 1.5,
                      QStringLiteral("截图：拖上边 → 框变矮、上边界跟着走（下边不动）"),
                      QStringLiteral("y=%1 h=%2").arg(num(st, "y")).arg(num(st, "h")));

                /* 左下角：整体放大 —— 字号和框一起按比例长 */
                {
                    const double wBefore = num(boxState(), "w");
                    const double hBefore = num(boxState(), "h");
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testScaleDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex),
                                              Q_ARG(QVariant, -60.0), Q_ARG(QVariant, 60.0));
                    st = boxState();
                    check(ok.toBool() && num(st, "size") > 16.0
                              && num(st, "w") > wBefore + 100.0
                              && num(st, "h") > hBefore + 20.0,
                          QStringLiteral("截图：拖左下角 → 整体放大（字号和框一起长）"),
                          QStringLiteral("size %1 / w %2→%3 / h %4→%5")
                              .arg(num(st, "size")).arg(wBefore).arg(num(st, "w"))
                              .arg(hBefore).arg(num(st, "h")));
                }

                /* 左下角（现在的语义）：拖动整个框 —— tx/ty 各挪 dx/dy，宽高不变 */
                {
                    const QVariantMap before = boxState();
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testMoveDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex),
                                              Q_ARG(QVariant, 70.0), Q_ARG(QVariant, 40.0));
                    st = boxState();
                    check(ok.toBool()
                              && qAbs(num(st, "x") - (num(before, "x") + 70.0)) < 1.5
                              && qAbs(num(st, "y") - (num(before, "y") + 40.0)) < 1.5
                              && qAbs(num(st, "w") - num(before, "w")) < 1.5
                              && qAbs(num(st, "h") - num(before, "h")) < 1.5,
                          QStringLiteral("截图：左下角手柄拖动整个框（位置走、宽高不动）"),
                          QStringLiteral("(%1,%2)→(%3,%4) w %5→%6")
                              .arg(num(before, "x")).arg(num(before, "y"))
                              .arg(num(st, "x")).arg(num(st, "y"))
                              .arg(num(before, "w")).arg(num(st, "w")));
                }

                /* 右下角：宽高分开拖 */
                {
                    const double wBefore = num(boxState(), "w");
                    const double hBefore = num(boxState(), "h");
                    QVariant ok;
                    QMetaObject::invokeMethod(overlay, "testFreeDrag", Q_RETURN_ARG(QVariant, ok),
                                              Q_ARG(QVariant, boxIndex),
                                              Q_ARG(QVariant, 50.0), Q_ARG(QVariant, 30.0));
                    st = boxState();
                    check(ok.toBool() && qAbs(num(st, "w") - (wBefore + 50.0)) < 1.5
                              && qAbs(num(st, "h") - (hBefore + 30.0)) < 1.5,
                          QStringLiteral("截图：拖右下角 → 宽高分别 +50 / +30"),
                          QStringLiteral("w %1→%2 / h %3→%4")
                              .arg(wBefore).arg(num(st, "w"))
                              .arg(hBefore).arg(num(st, "h")));
                }

                /* 宽度拖到比最小还窄：夹在 minBoxW，不会拖成一条线 */
                dragEdge(QStringLiteral("right"), -5000.0, 0.0);
                st = boxState();
                check(num(st, "w") >= 39.0 && num(st, "w") <= 41.0,
                      QStringLiteral("截图：宽度拖过头 → 夹在最小框宽（40）"),
                      QStringLiteral("w=%1").arg(num(st, "w")));
            }

            const QRectF sel = overlay->property("sel").toRectF();
            check(qAbs(sel.width() - selW) < 1.5 && qAbs(sel.height() - selH) < 1.5,
                  QStringLiteral("截图：框出来的选区就是刚才那一块（420 × 260）"),
                  QStringLiteral("实际 %1 × %2").arg(sel.width()).arg(sel.height()));

            /*
             * 第一条出口：存 png。存两张 —— 带标注的和不带标注的 ——
             * 比出"文字真的画进图里了"，而不是只有预览里有。
             */
            const QString plainPath = dir.filePath(QStringLiteral("shot-plain.png"));
            const QString markPath = dir.filePath(QStringLiteral("shot-marked.png"));
            check(shot->saveResult(plainPath, sel, QVariantList()),
                  QStringLiteral("截图：合成并保存 png（不带标注）"));
            check(shot->saveResult(markPath, sel, texts),
                  QStringLiteral("截图：合成并保存 png（带标注）"));

            const QImage plain(plainPath);
            const QImage marked(markPath);
            const int wantW = qRound(selW * dpr);
            const int wantH = qRound(selH * dpr);
            check(!plain.isNull() && qAbs(plain.width() - wantW) <= 1
                      && qAbs(plain.height() - wantH) <= 1,
                  QStringLiteral("截图：存出来的图正好是选区那一块（设备像素）"),
                  QStringLiteral("图 %1x%2 / 期望 %3x%4")
                      .arg(plain.width()).arg(plain.height()).arg(wantW).arg(wantH));

            /*
             * 数标注色（#ff3b30）的像素：带标注的那张必须明显多出来。
             * 只看文本落点那一小块，桌面背景里正好有一片红色的概率很低，
             * 而且这里比的是同一个位置"有无标注"的差，稳。
             */
            auto reddish = [](const QImage &img) {
                int count = 0;
                for (int y = 0; y < img.height(); ++y) {
                    for (int x = 0; x < img.width(); ++x) {
                        const QColor c = img.pixelColor(x, y);
                        if (c.red() > 170 && c.green() < 120 && c.blue() < 120)
                            ++count;
                    }
                }
                return count;
            };
            const int plainRed = reddish(plain);
            const int markedRed = reddish(marked);
            check(markedRed > plainRed + 30,
                  QStringLiteral("截图：文字真的画进了最终图（标注色像素明显多出来）"),
                  QStringLiteral("带标注 %1 / 不带 %2").arg(markedRed).arg(plainRed));

            /*
             * 旋转那条路也要能出图：把同一条标注转 30° 再合成一张，
             * 尺寸不变、内容必须和没转的那张不一样（转了要是还一样，
             * 说明 compose() 根本没理会 rot）。
             */
            {
                QVariantList rotated = texts;
                QVariantMap first = rotated.first().toMap();
                first.insert(QStringLiteral("rot"), 30.0);
                rotated[0] = first;

                const QString rotPath = dir.filePath(QStringLiteral("shot-rotated.png"));
                check(shot->saveResult(rotPath, sel, rotated),
                      QStringLiteral("截图：带旋转角的标注也能合成出图"));
                const QImage rotatedImage(rotPath);
                check(rotatedImage.size() == marked.size()
                          && rotatedImage != marked,
                      QStringLiteral("截图：转 30° 之后成品图跟着变了（旋转真的生效）"),
                      QStringLiteral("尺寸 %1x%2 / 原图 %3x%4")
                          .arg(rotatedImage.width()).arg(rotatedImage.height())
                          .arg(marked.width()).arg(marked.height()));
            }

            /*
             * 箭头 / 铅笔：走界面上那套"起笔 -> 落笔"（testAddShape 调的
             * 就是 commitShape），要钉三件事：
             *   1) 数据里有它、起终点 / 点列对得上；
             *   2) 两种工具各画一笔都在；
             *   3) 合成出来的成品图**跟着变**（不是只有预览里画了）。
             */
            {
                QVariant ok;
                QMetaObject::invokeMethod(overlay, "testAddShape", Q_RETURN_ARG(QVariant, ok),
                                          Q_ARG(QVariant, QStringLiteral("arrow")),
                                          Q_ARG(QVariant, selX + 40), Q_ARG(QVariant, selY + 180),
                                          Q_ARG(QVariant, selX + 200), Q_ARG(QVariant, selY + 230));

                QVariant shapesVar;
                QMetaObject::invokeMethod(overlay, "shapesData", Q_RETURN_ARG(QVariant, shapesVar));
                const QVariantList arrows = shapesVar.toList();
                const QVariantMap arrow = arrows.isEmpty() ? QVariantMap()
                                                           : arrows.first().toMap();
                check(ok.toBool() && arrows.size() == 1
                          && arrow.value(QStringLiteral("kind")).toString() == QLatin1String("arrow")
                          && qAbs(arrow.value(QStringLiteral("x1")).toDouble() - (selX + 40)) < 1.5
                          && qAbs(arrow.value(QStringLiteral("y2")).toDouble() - (selY + 230)) < 1.5,
                      QStringLiteral("截图：拖出来的箭头带着起终点（kind=arrow）"),
                      QStringLiteral("条目 %1 / 终点 (%2,%3)")
                          .arg(arrows.size())
                          .arg(arrow.value(QStringLiteral("x2")).toDouble())
                          .arg(arrow.value(QStringLiteral("y2")).toDouble()));

                QMetaObject::invokeMethod(overlay, "testAddShape", Q_RETURN_ARG(QVariant, ok),
                                          Q_ARG(QVariant, QStringLiteral("pencil")),
                                          Q_ARG(QVariant, selX + 60), Q_ARG(QVariant, selY + 200),
                                          Q_ARG(QVariant, selX + 240), Q_ARG(QVariant, selY + 160));
                QMetaObject::invokeMethod(overlay, "shapesData", Q_RETURN_ARG(QVariant, shapesVar));
                const QVariantList shapes = shapesVar.toList();
                const QVariantMap pencil = shapes.isEmpty() ? QVariantMap()
                                                            : shapes.last().toMap();
                check(ok.toBool() && shapes.size() == 2
                          && pencil.value(QStringLiteral("kind")).toString()
                                 == QLatin1String("pencil")
                          && pencil.value(QStringLiteral("pts")).toList().size() == 6,
                      QStringLiteral("截图：铅笔那一笔带着点列（x,y 成对）"),
                      QStringLiteral("条目 %1 / 点数 %2")
                          .arg(shapes.size())
                          .arg(pencil.value(QStringLiteral("pts")).toList().size()));

                /*
                 * 三个工具都要能从"按下"开始整条走通（不只是 commitShape）——
                 * 加"方框"时就是这里漏了：数据 / 合成都支持 rect，可按下时的
                 * 分派还写着 arrow||pencil，于是方框工具下拖出来的是"改选区"。
                 */
                {
                    const double bx = selX + 20;
                    const double by = selY + 200;
                    QVariant drew;
                    QMetaObject::invokeMethod(overlay, "testDrawWith", Q_RETURN_ARG(QVariant, drew),
                                              Q_ARG(QVariant, QStringLiteral("rect")),
                                              Q_ARG(QVariant, bx), Q_ARG(QVariant, by),
                                              Q_ARG(QVariant, bx + 60), Q_ARG(QVariant, by + 40));
                    check(drew.toBool(), QStringLiteral("截图：方框工具从按下到松手整条路走得通"));

                    /* 而且**不该**顺手把选区改掉（那正是漏掉分派时的症状） */
                    const QRectF afterDraw = overlay->property("sel").toRectF();
                    check(qAbs(afterDraw.width() - selW) < 1.5
                              && qAbs(afterDraw.height() - selH) < 1.5,
                          QStringLiteral("截图：用形状工具画一笔不会改掉选区"),
                          QStringLiteral("选区 %1 × %2").arg(afterDraw.width())
                              .arg(afterDraw.height()));
                }

                QVariant all;
                QMetaObject::invokeMethod(overlay, "annotationsData", Q_RETURN_ARG(QVariant, all));

                /* 方框：和箭头同一套"按下 + 松开"，只有描边 */
                QMetaObject::invokeMethod(overlay, "testAddShape", Q_RETURN_ARG(QVariant, ok),
                                          Q_ARG(QVariant, QStringLiteral("rect")),
                                          Q_ARG(QVariant, selX + 260), Q_ARG(QVariant, selY + 40),
                                          Q_ARG(QVariant, selX + 380), Q_ARG(QVariant, selY + 150));
                QMetaObject::invokeMethod(overlay, "shapesData", Q_RETURN_ARG(QVariant, shapesVar));
                const QVariantList withRect = shapesVar.toList();
                /* 按 kind + 角点找那一条（列表里这会儿不止一个形状） */
                QVariantMap rect;
                for (const QVariant &v : withRect) {
                    const QVariantMap m = v.toMap();
                    if (m.value(QStringLiteral("kind")).toString() == QLatin1String("rect")
                        && qAbs(m.value(QStringLiteral("x1")).toDouble() - (selX + 260)) < 1.5)
                        rect = m;
                }
                check(ok.toBool() && !rect.isEmpty()
                          && qAbs(rect.value(QStringLiteral("y2")).toDouble() - (selY + 150)) < 1.5,
                      QStringLiteral("截图：方框带着两个角点（kind=rect）"),
                      QStringLiteral("条目 %1 / 角点 (%2,%3)-(%4,%5)")
                          .arg(withRect.size())
                          .arg(rect.value(QStringLiteral("x1")).toDouble())
                          .arg(rect.value(QStringLiteral("y1")).toDouble())
                          .arg(rect.value(QStringLiteral("x2")).toDouble())
                          .arg(rect.value(QStringLiteral("y2")).toDouble()));

                QMetaObject::invokeMethod(overlay, "annotationsData", Q_RETURN_ARG(QVariant, all));
                const QString shapePath = dir.filePath(QStringLiteral("shot-shapes.png"));
                check(shot->saveResult(shapePath, sel, all.toList()),
                      QStringLiteral("截图：箭头 + 铅笔 + 方框 + 文字一起合成出图"));
                const QImage withShapes(shapePath);
                check(withShapes.size() == marked.size() && withShapes != marked,
                      QStringLiteral("截图：箭头 / 铅笔 / 方框真的画进了成品图（不是只在预览里）"));
            }

            /* 第二条出口：剪贴板 */
            check(shot->copyResult(sel, texts), QStringLiteral("截图：复制到剪贴板"));
            const QImage clip = QGuiApplication::clipboard()->image();
            check(!clip.isNull() && clip.size() == marked.size(),
                  QStringLiteral("截图：剪贴板里那张图和存出来的是同一张"),
                  QStringLiteral("剪贴板 %1x%2 / 文件 %3x%4")
                      .arg(clip.width()).arg(clip.height())
                      .arg(marked.width()).arg(marked.height()));

            /* 第三条出口：固定到桌面（贴图窗口） */
            QWidget *overlayWidget = nullptr;
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (w->windowTitle() == QStringLiteral("SmartClip 截图"))
                    overlayWidget = w;
            }
            shot->pinResult(sel, texts);
            QWidget *pin = nullptr;
            for (QWidget *w : QApplication::topLevelWidgets()) {
                if (w->windowTitle() == QStringLiteral("SmartClip 贴图"))
                    pin = w;
            }
            check(shot->pinnedCount() == 1 && pin != nullptr,
                  QStringLiteral("截图：固定到桌面开出了一个贴图窗口"));
            if (pin) {
                check(qAbs(pin->width() - qRound(selW)) <= 1
                          && qAbs(pin->height() - qRound(selH)) <= 1,
                      QStringLiteral("截图：贴图窗口就是选区那么大"),
                      QStringLiteral("窗口 %1x%2").arg(pin->width()).arg(pin->height()));
                check(pin->windowFlags().testFlag(Qt::WindowStaysOnTopHint)
                          && pin->windowFlags().testFlag(Qt::FramelessWindowHint),
                      QStringLiteral("截图：贴图窗口是置顶 + 无边框（钉在桌面上）"));
                /* 就钉在选区原来那块地方（截图时框的是哪儿，贴出来在哪儿） */
                if (overlayWidget) {
                    const QPoint want = overlayWidget->pos() + QPoint(qRound(selX), qRound(selY));
                    check((pin->pos() - want).manhattanLength() <= 2,
                          QStringLiteral("截图：贴图窗口钉在选区原来的位置上"),
                          QStringLiteral("实际 %1,%2 / 期望 %3,%4")
                              .arg(pin->pos().x()).arg(pin->pos().y())
                              .arg(want.x()).arg(want.y()));
                }
            }
            shot->closeAllPins();
            check(shot->pinnedCount() == 0, QStringLiteral("截图：贴图窗口关得掉"));
        }

        shot->endCapture();
        /*
         * 收工之后选区必须复位成整屏 —— 否则下次打开会带出上一次的框选轮廓
         * （用户报过："下次使用的时候会调出上次的截图框轮廓"）。
         * 这条钉住状态那一半；另一半是窗口表面残留，靠 Screenshot::endCapture 里
         * "强制渲染 + repaint" 解决 —— 那个只能实测（实测：残留帧 3 -> 0）。
         */
        if (QObject *root = shot->overlayRoot()) {
            const QRectF leftover = root->property("sel").toRectF();
            check(leftover.width() >= 1000 && leftover.height() >= 600,
                  QStringLiteral("截图：收工之后选区复位成整屏（下次不会带出上次的框）"),
                  QStringLiteral("残留选区 %1×%2").arg(leftover.width()).arg(leftover.height()));
        }

        check(!shot->overlayVisible() && !shot->active(),
              QStringLiteral("截图：收工之后选区窗口收起来、状态复位"
                             "（窗口留着复用，见 Screenshot::prewarm）"));

        QGuiApplication::clipboard()->setText(oldClipboard);
    }

    /*
     * 托盘右键菜单：用户要的是"托盘右键里能选截图"。
     *
     * 托盘图标本身点不出来（Windows 会把它塞进"隐藏的图标"浮出区，位置还随
     * 图标数量变），但**菜单对象**能直接拿 —— 所以验的是：菜单里确实有
     * 「截图…」这一条，而且触发它真的能开出选区窗口。
     */
    if (tray && shot) {
        QMenu *menu = tray->menu();
        const QList<QAction *> acts = menu ? menu->actions() : QList<QAction *>();
        QAction *shotAct = nullptr;
        for (QAction *a : acts) {
            if (a && a->text().contains(QStringLiteral("截图")))
                shotAct = a;
        }
        check(menu != nullptr && shotAct != nullptr,
              QStringLiteral("托盘：右键菜单里有「截图…」这一条"),
              QStringLiteral("菜单条目 %1 条").arg(acts.size()));

        /*
         * 退出相关的两条得在。原来这里是"退出…"+ 一个"完全退出 / 收进托盘"的选择框，
         * 用户实测那个框会在用截图快捷键的场景里挡住程序（模态，看着像卡死），
         * 要求去掉；现在改成菜单里两个明确条目，这条把菜单结构钉住。
         */
        {
            bool hide = false;
            bool quit = false;
            for (QAction *a : acts) {
                if (!a)
                    continue;
                if (a->text().contains(QStringLiteral("收进托盘")))
                    hide = true;
                if (a->text().contains(QStringLiteral("退出")))
                    quit = true;
            }
            check(hide && quit,
                  QStringLiteral("托盘：菜单里有「收进托盘」和「退出 SmartClip」两条"
                                 "（不再弹模态选择框）"));
        }

        /*
         * 图标资源。原来用的是 QIcon::fromTheme("edit-paste")，Windows 上没有
         * 图标主题、返回空图标，托盘上是一块空白；现在指向随包的 SVG，
         * 这条把资源路径钉住（前缀被改过就会红）。
         */
        check(!QIcon(QStringLiteral(":/icons/image.svg")).isNull(),
              QStringLiteral("托盘：图标资源 :/icons/image.svg 能加载（不是空白图标）"));

        /*
         * 白底。全局调色板是深色的，QMenu 默认跟着走 —— 但用户要白底，
         * 所以这是一处"特意覆盖"，很容易被以后某次改动顺手带回去
         * （谁把样式表删了、谁动了全局调色板都会）。直接把菜单画出来数像素。
         */
        if (menu) {
            menu->adjustSize();
            const QImage shot = menu->grab().toImage();
            /* 顺手留一张渲染图，人眼也能看一眼（临时目录里，和其它自检产物一起） */
            shot.save(dir.filePath(QStringLiteral("tray-menu.png")));
            int light = 0;
            int total = 0;
            for (int y = 0; y < shot.height(); y += 2) {
                for (int x = 0; x < shot.width(); x += 2) {
                    const QColor c = shot.pixelColor(x, y);
                    ++total;
                    if (c.red() > 200 && c.green() > 200 && c.blue() > 200)
                        ++light;
                }
            }
            const int pct = total > 0 ? light * 100 / total : 0;
            check(pct >= 60,
                  QStringLiteral("托盘：右键菜单是白底（不是跟界面走的深色）"),
                  QStringLiteral("亮像素 %1% / 菜单 %2x%3")
                      .arg(pct).arg(shot.width()).arg(shot.height()));
        }

        if (shotAct) {
            shotAct->trigger();
            for (int i = 0; i < 60 && !shot->active(); ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(25);
            }
            check(shot->active(),
                  QStringLiteral("托盘：点菜单里的「截图…」真的开出了选区窗口"));
            shot->endCapture();
        }
    }

    /*
     * 全局截图热键。
     *
     * 用户报的是"最小化之后截图快捷键不能用"—— 程序内那条 QAction 的上下文是
     * WindowShortcut，窗口没激活就不响。修法是额外注册一个系统级热键
     * （RegisterHotKey，见 EditorController::applyGlobalHotkey），这里验两件事：
     * 注册上了没有，以及"收到热键 -> 发命令 -> 开出截图"这条链走不走得通。
     */
    if (cmd && shot) {
        check(cmd->globalHotkeyActive(),
              QStringLiteral("截图键注册成了系统级热键（窗口没激活也能按）"),
              QStringLiteral("注册失败通常是组合键被别的程序占了"));

        /* 系统那边来的就是 WM_HOTKEY，回调里做的正是这一句 */
        cmd->activateCommand(QStringLiteral("shot"));
        for (int i = 0; i < 60 && !shot->active(); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            QThread::msleep(25);
        }
        check(shot->active(),
              QStringLiteral("全局热键那条路：命令一发，选区窗口就开出来了"));
        shot->endCapture();

        /*
         * 取消截图（Esc / 双击）必须落在"抓屏延时"里也算数。
         *
         * 用户报的就是这个：按了截图快捷键、马上按 Esc 取消，屏幕先亮起一整块
         * 全屏选区、要再按一次 Esc 才关得掉 —— 约 30ms 的延时（见
         * Screenshot::beginCapture）里用户已经取消了，可那会儿 endCapture
         * 什么都关不掉（没有 active 状态可收），延时到点 grabAndShow() 照样
         * 把选区窗口铺出来。
         *
         * 这里钉两层：取消之后窗口**一直**藏着（要等过延时，否则测不出"照样
         * 铺出来"那一半），以及收工时窗口表面是干净的全屏选区、没有虚线框
         * 残留（残留的就是用户看到的那块"全屏框"）。
         */
        {
            shot->beginCapture();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            shot->cancelCapture();     /* 界面那边 Esc / 双击叫的就是这个 */

            check(!shot->active() && !shot->overlayVisible(),
                  QStringLiteral("截图：抓屏延时里按取消，选区窗口立刻收起来"));

            /* 等过那段延时：窗口不许再冒出来 */
            QElapsedTimer waited;
            waited.start();
            while (waited.elapsed() < 400) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            check(!shot->active() && !shot->overlayVisible(),
                  QStringLiteral("截图：取消之后延时到点也不会冒出全屏选区（取消要算数）"),
                  QStringLiteral("active=%1 / 窗口可见=%2")
                      .arg(shot->active() ? 1 : 0).arg(shot->overlayVisible() ? 1 : 0));
        }
    }

    /*
     * 关闭键那个问句：一块**独立的小卡片**，底下的界面原封不动。
     *
     * 这条检查的来历：用户在"闪一下"上折腾了三轮 —— 先是被模态框挡住（像卡死），
     * 后来铺透明遮罩（看不见，但铺满整窗，鼠标全被吃掉，编辑区点不动），
     * 最后要求"底下什么也别做"。现在的实现是 QML 侧一块 Popup.Window 卡片
     * （见 qml/components/AskCard.qml），结构上钉三点：没有模态、没有遮罩
     * （就是没有铺满整窗的窗口）、卡片本身比主窗口小得多。
     */
    {
        if (qmlRoot) {
            /*
             * 直接调界面上的入口，不走 C++ 侧那个 WindowHelper 单例 ——
             * 用 engine->singletonInstance 拿到的实例和 QML 里用的不是同一个
             * （改成本对象只负责发信号之后就露馅了：那边发信号，界面这边没反应）。
             * 真机上 ✕ → 信号 → 界面那条桥是好的（点 ✕ 会弹出卡片，已实测）。
             */
            QMetaObject::invokeMethod(qmlRoot, "openQuitAsk");
            for (int i = 0; i < 40; ++i) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
                QThread::msleep(15);
            }

            check(!QApplication::activeModalWidget(),
                  QStringLiteral("窗口：退出问句是非模态的（不会把程序挡住）"));

            /*
             * 卡片是 Popup.Window 开出来的 **QQuickWindow**，不是 QWidget ——
             * 得看 QWindow 列表，`topLevelWidgets()` 里根本找不到它（第一版就栽在这）。
             */
            int small = 0;
            int big = 0;
            QStringList seen;
            const auto tops = QGuiApplication::topLevelWindows();
            for (QWindow *w : tops) {
                if (!w->isVisible())
                    continue;
                seen << QStringLiteral("%1x%2").arg(w->width()).arg(w->height());
                if (w->width() < 800 && w->height() < 400)
                    ++small;      /* 卡片 */
                else
                    ++big;        /* 主窗口那种大窗口 */
            }
            check(small == 1 && big <= 1,
                  QStringLiteral("窗口：问句是一块小卡片，没有铺满整窗的遮罩"),
                  QStringLiteral("小窗 %1 / 大窗 %2；可见顶层窗口：%3")
                      .arg(small).arg(big).arg(seen.join(QStringLiteral("，"))));

            QMetaObject::invokeMethod(qmlRoot, "closeQuitAsk");
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            bool stillThere = false;
            for (QWindow *w : QGuiApplication::topLevelWindows()) {
                if (w->isVisible() && w->width() < 800 && w->height() < 400)
                    stillThere = true;
            }
            check(!stillThere, QStringLiteral("窗口：问句收得掉（不留残窗）"));
        }
    }

    /*
     * 「有未保存的改动」那张卡片：和退出问句同一个组件（AskCard.qml），
     * 三个按钮 保存 / 不保存 / 取消。
     *
     * 这条检查钉的是"问完之前不许关"。卡片是异步的（非模态原生小窗，
     * 见 AskCard.qml 开头），原来 Cmd.confirmSave() 那种"同步返回一个数字"
     * 的写法换成了 Main.qml 里的关闭队列（requestCloseTabs / answerSaveAsk）
     * —— 这里最容易出的错就是没等回答就把标签关了，那等于替用户选了"不保存"，
     * 未保存的改动会无声无息地没掉。
     */
    {
        const QString askPath = dir.filePath(QStringLiteral("ask-save.txt"));
        QFile askFile(askPath);
        if (askFile.open(QIODevice::WriteOnly)) {
            askFile.write("hello\n");
            askFile.close();
        }

        check(view->openFile(askPath) >= 0, QStringLiteral("未保存问句用例：打开一个文件"));
        const int before = view->documents().size();
        /*
         * 真改一笔（复制一行）。
         *
         * 不能用 view->setModified(true)：那个 setter 是**只能清标记**的
         * （Scintilla 没有反向的 SCI_SETMODIFY，见 EditorViewItem::setModified），
         * 传 true 是空操作 —— 第一版就栽在这，于是"没改动"那条路把标签直接关了，
         * 卡片压根没弹。
         */
        view->duplicateLine();
        check(view->modified(), QStringLiteral("未保存问句用例：改一笔 -> 已修改"));
        dispatch(QStringLiteral("closeTab"));
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            QThread::msleep(15);
        }

        check(uiState().value(QStringLiteral("saveAskOpened")).toBool(),
              QStringLiteral("未保存问句：有改动，卡片弹出来了"));
        check(view->documents().size() == before,
              QStringLiteral("未保存问句：还没回答，标签不许关"),
              QStringLiteral("还剩 %1 个").arg(view->documents().size()));

        /*
         * 按"不保存"（下标 1）：这一刻才真的关掉。
         *
         * 点的是卡片上的按钮那条路（Main.qml 的 clickSaveAsk -> AskCard.answer
         * -> answered -> answerSaveAsk），不是直接调 answerSaveAsk —— 后者
         * 卡片还开着，"答完就收"就验不到了。
         */
        QMetaObject::invokeMethod(qmlRoot, "clickSaveAsk", Q_ARG(QVariant, QVariant(1)));
        for (int i = 0; i < 40; ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            QThread::msleep(15);
        }
        check(view->documents().size() == before - 1,
              QStringLiteral("未保存问句：答了「不保存」才关掉"),
              QStringLiteral("还剩 %1 个").arg(view->documents().size()));
        check(!uiState().value(QStringLiteral("saveAskOpened")).toBool(),
              QStringLiteral("未保存问句：答完卡片收掉了"));

        QFile::remove(askPath);
    }

    /*
     * 滚动条：两条轨道都得是正文底色，不许露出白带。
     *
     * 这条的来历：轨道原来写的是 background: transparent —— 那是"这一层不画"，
     * 露出来的是底下那一层。滑块的深灰一直是对的（说明样式表确实生效了），
     * 轨道却在有的机器上是一条 12px 的 #f2f2f2 白带（用户报的"这个滚动条白色
     * 背景去掉"）。现在轨道色写死成正文底色，这里抓两条滚动条**自己渲染出来的
     * 图**数一遍近白像素 —— 属性值看着都对、画出来不对，只能这么钉。
     */
    {
        const QString oldClipboard = QGuiApplication::clipboard()->text();
        const bool oldWrap = view->wrapEnabled();   /* 收尾要放回去 */

        check(view->newDocument() >= 0, QStringLiteral("滚动条用例：新建长文档"));
        view->setWrapEnabled(false);        /* 不换行，长行才会顶出横向滚动条 */
        QString big;
        for (int i = 1; i <= 200; ++i)
            big += QStringLiteral("line %1 : ").arg(i)
                   + QString(220, QLatin1Char('x')) + QLatin1Char('\n');
        QGuiApplication::clipboard()->setText(big);
        view->paste();
        settle();

        const QVariantList bars = view->scrollBarPixelStats();
        const int vWhite = bars.value(0).toInt();
        const int hWhite = bars.value(1).toInt();
        const bool vShown = bars.value(2).toBool();
        const bool hShown = bars.value(3).toBool();
        const uint vTrack = bars.value(4).toUInt();
        const uint hTrack = bars.value(5).toUInt();
        /* 底色 #1e1f22 按 QColor 的 RGB 顺序（和 packed() 的 BGR 不一样） */
        const uint paperRgb = 0x1E1F22u;

        out() << "        （竖条 白点 " << vWhite << " / 轨道 #"
              << QString::number(vTrack, 16) << " ；横条 白点 " << hWhite
              << " / 轨道 #" << QString::number(hTrack, 16) << "）" << Qt::endl;

        check(vShown && hShown,
              QStringLiteral("滚动条用例：长内容 + 长行时两条滚动条都在"),
              QStringLiteral("竖 %1 / 横 %2").arg(vShown).arg(hShown));
        check(vWhite == 0 && hWhite == 0,
              QStringLiteral("滚动条轨道里没有白底（露白带就是这条红）"),
              QStringLiteral("竖条 %1 个白点 / 横条 %2 个").arg(vWhite).arg(hWhite));
        check(vTrack == paperRgb && hTrack == paperRgb,
              QStringLiteral("滚动条轨道色就是正文底色"),
              QStringLiteral("竖 #%1 / 横 #%2（要 #1e1f22）")
                  .arg(vTrack, 6, 16, QLatin1Char('0'))
                  .arg(hTrack, 6, 16, QLatin1Char('0')));

        QGuiApplication::clipboard()->setText(oldClipboard);
        view->closeDocument(view->currentIndex());
        view->setWrapEnabled(oldWrap);
        settle();
    }

    out() << Qt::endl
          << "通过 " << gPassed << " 项，失败 " << gFailed << " 项" << Qt::endl;
    return gFailed == 0 ? 0 : 1;
}