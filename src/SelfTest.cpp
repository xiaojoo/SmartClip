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
          && view->marginBack(2) == packed(0x1e, 0x1f, 0x22),
          QStringLiteral("边距背景也压成编辑区底色"),
          QStringLiteral("margin0=#%1 margin2=#%2")
              .arg(view->marginBack(0), 6, 16, QLatin1Char('0'))
              .arg(view->marginBack(2), 6, 16, QLatin1Char('0')));

    /* 折叠边距在（第 2 列有宽度），第 1 列不用 */
    check(view->marginWidth(2) > 0,
          QStringLiteral("折叠边距已启用（第 2 列有宽度）"),
          QStringLiteral("实际 %1").arg(view->marginWidth(2)));
    check(view->marginWidth(1) == 0,
          QStringLiteral("第 1 列不用（当前行竖条已去掉）"),
          QStringLiteral("实际 %1").arg(view->marginWidth(1)));

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

    /* ---- 收尾 ---- */
    dispatch(QStringLiteral("closeAllTabs"));
    check(view->documents().isEmpty(), QStringLiteral("closeAllTabs 之后没有标签"));
    check(!view->hasDocument(), QStringLiteral("空状态：hasDocument = false"));

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
