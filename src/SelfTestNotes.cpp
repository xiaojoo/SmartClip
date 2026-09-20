#include "SelfTest.h"

#include "EditorController.h"
#include "NoteLinkModel.h"
#include "NoteThumbs.h"
#include "StickyNotes.h"
#include "StickyNoteStore.h"
#include "TrayIcon.h"

#include <QAction>
#include <QColor>
#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QMenu>
#include <QMetaObject>
#include <QQuickItem>
#include <QRegularExpression>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QVariant>
#include <QWindow>
#include <cstdio>

/*
 * 便签专用自检（`SmartClip.exe --note-test`，见 SelfTest.h 的 runNotes）。
 *
 * 这个文件和 SelfTest.cpp **分开**，是有意的：SelfTest.cpp 里有三千多行别的
 * 检查（编辑区 / 截图 / 设置面板…），其中截图那几节有自己的时序问题（偶发
 * 飘红，和便签无关）。只动便签的时候不需要把那些跑一遍 —— 那些检查会开全屏
 * 选区窗口、弹卡片，界面上看着乱跳，一遍也要跑十几秒。
 *
 * 这里只碰便签：建它、量它、改它、删它。不碰编辑区，不碰截图，不弹任何模态框。
 */

/*
 * 计数器和输出口（SelfTest.cpp 里那两个是同一个命名空间内的静态量，但它那边
 * 是文件作用域的，这里再用一份自己的 —— 两边不会同时跑）。
 */
namespace {

int gNotePassed = 0;
int gNoteFailed = 0;

void noteCheck(bool ok, const QString &what, const QString &detail = QString()) {
    if (ok) {
        ++gNotePassed;
        std::fputs("  ok    ", stdout);
    } else {
        ++gNoteFailed;
        std::fputs("  FAIL  ", stdout);
    }

    const QByteArray text = what.toUtf8();
    std::fputs(text.constData(), stdout);
    if (!detail.isEmpty()) {
        std::fputs("   [", stdout);
        std::fputs(detail.toUtf8().constData(), stdout);
        std::fputs("]", stdout);
    }
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

void noteOut(const QString &line) {
    std::fputs("        ", stdout);
    std::fputs(line.toUtf8().constData(), stdout);
    std::fputs("\n", stdout);
    std::fflush(stdout);
}

/* 让事件处理一轮：布局 / 原生窗口几何都是下一帧才落定的 */
void settle() {
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(10);
    }
}

/* 屏幕上看得见的顶层窗口尺寸（用来量"菜单真摆出来了""收起来不留残窗"） */
QStringList visibleTopLevels() {
    QStringList out;
    for (QWindow *w : QGuiApplication::topLevelWindows()) {
        if (w->isVisible())
            out << QStringLiteral("%1x%2@%3,%4")
                       .arg(w->width()).arg(w->height()).arg(w->x()).arg(w->y());
    }
    return out;
}

}  // namespace

bool SelfTest::noteTestEnabled(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QLatin1String("--note-test"))
            return true;
    }
    return false;
}

int SelfTest::notesPassed() { return gNotePassed; }
int SelfTest::notesFailed() { return gNoteFailed; }

int SelfTest::runNotes(ClipboardStore *store, TrayIcon *tray, EditorController *cmd,
                       StickyNotes *notes) {
    /* 每条检查立刻落盘：崩了也能看到崩在哪一条 */
    setvbuf(stdout, nullptr, _IONBF, 0);

    if (!notes) {
        noteOut(QStringLiteral("便签对象为空，没法测"));
        return 1;
    }

    QDir dir(QDir::tempPath() + QStringLiteral("/smartclip-notetest"));
    dir.removeRecursively();
    dir.mkpath(QStringLiteral("."));

    /*
     * **先把这份活 store 的落盘路径挪到临时目录**，再动清单。
     *
     * 下面那几行 deleteNote 删的是 `notes` 里此刻的东西 —— 而 `notes` 是启动时
     * 从用户真实 notes.json 读进来的那一份（见 StickyNoteStore::setFilePath 的
     * 注释：测试 / 自检本来就该指到别处，这一路一直忘了挪）。不挪的后果踩过：
     * 2026-09-20 跑了一次 `--note-test`（以及带这一段的主自检 `--self-test`），
     * 用户桌面上的便签被清空，找不回来。
     *
     * 路径一改，内存里这些条目照样被删（自检要的是干净起点），但**再也写不回
     * 用户那份文件**。
     */
    notes->store()->setFilePath(dir.filePath(QStringLiteral("notes.json")));

    /* 从干净的便签清单开始（真实文件已经在上一步被换到临时目录之外了） */
    for (StickyNote *stale : notes->store()->notes())
        notes->deleteNote(stale);
    settle();

    noteOut(QStringLiteral("便签自检（只测便签，不碰其它功能）"));

    /* 先报一下屏的可用区域：菜单"会不会跑到任务栏底下"全看这个数 */
    if (QScreen *s = QGuiApplication::primaryScreen()) {
        const QRect avail = s->availableGeometry();
        const QRect full = s->geometry();
        noteOut(QStringLiteral("（主屏：整块 %1,%2 %3x%4 / 可用 %5,%6 %7x%8）")
                    .arg(full.x()).arg(full.y()).arg(full.width()).arg(full.height())
                    .arg(avail.x()).arg(avail.y()).arg(avail.width()).arg(avail.height()));
    }

    /* =====================================================================
     * 1) 正文里的链接：认得出、认得准
     * =================================================================== */
    {
        const QString body = QStringLiteral(
            "看这个 https://example.com/a?b=1 吧，\n"
            "还有 www.qt.io 和 http://localhost:8080/x 这种不算。\n"
            "重复一遍 https://example.com/a?b=1 只算一条。\n"
            "别的协议 ftp://files.example.com/pub 也不算。\n"
            "最后一条 https://doc.qt.io/qt-6/ 。\n");

        const QList<NoteLink> links = NoteLinkModel::parse(body);
        QStringList urls;
        for (const NoteLink &link : links)
            urls << link.url;

        noteCheck(links.size() == 3,
                  QStringLiteral("链接：正文里认出 3 条（重复 / localhost / ftp 都不算）"),
                  urls.join(QStringLiteral(" | ")));
        noteCheck(!links.isEmpty()
                      && links.first().url == QLatin1String("https://example.com/a?b=1"),
                  QStringLiteral("链接：第一条就是正文里那个网址"));
        noteCheck(links.size() >= 2 && links.at(1).host == QLatin1String("qt.io"),
                  QStringLiteral("链接：www.qt.io 补成完整地址、域名取 qt.io"));
        noteCheck(links.size() >= 3
                      && links.at(2).url == QLatin1String("https://doc.qt.io/qt-6/"),
                  QStringLiteral("链接：结尾那个中文句号不算地址的一部分"));
        noteCheck(links.size() >= 3 && links.at(2).line == 4,
                  QStringLiteral("链接：记下了它在正文第 5 行（0 起算 = 4）"));
        noteCheck(links.size() >= 3 && links.at(2).column > 0 && links.at(2).length > 0,
                  QStringLiteral("链接：记下了列号和长度（点卡片能定位回正文）"));
    }

    /* =====================================================================
     * 2) 缩略图缓存：key 稳定、图取得到
     * =================================================================== */
    {
        NoteThumbs thumbs;
        const QString thumbDir = dir.filePath(QStringLiteral("thumbs"));
        thumbs.setCacheDir(thumbDir);
        thumbs.load();

        const QString keyA = NoteThumbs::cacheKeyFor(QStringLiteral("https://example.com/a"));
        const QString keyB = NoteThumbs::cacheKeyFor(QStringLiteral("https://example.com/b"));
        noteCheck(!keyA.isEmpty()
                      && keyA == NoteThumbs::cacheKeyFor(QStringLiteral("https://example.com/a")),
                  QStringLiteral("缩略图：同一个网址的缓存 key 稳定不变"));
        noteCheck(keyA != keyB, QStringLiteral("缩略图：不同网址的 key 不一样"));
        noteCheck(thumbs.imageForCacheKey(keyA).isNull(),
                  QStringLiteral("缩略图：还没抓过的时候没有图（界面画占位块）"));

        QImage fake(320, 200, QImage::Format_ARGB32);
        fake.fill(QColor(0x4c, 0x96, 0xd8));
        const QString fakeKey = NoteThumbs::cacheKeyFor(
            QStringLiteral("https://example.com/cached"));
        const QString fakePath = thumbDir + QLatin1Char('/') + fakeKey
                                 + QStringLiteral(".png");
        QDir().mkpath(thumbDir);
        noteCheck(fake.save(fakePath, "PNG"), QStringLiteral("缩略图：准备一张缓存图"));

        QFile index(thumbDir + QStringLiteral("/index.json"));
        const bool indexOpened = index.open(QIODevice::WriteOnly | QIODevice::Truncate);
        Q_UNUSED(indexOpened);
        index.write(QStringLiteral(
            "{\"version\":1,\"items\":{\"https://example.com/cached\":"
            "{\"title\":\"缓存的网页\",\"image\":\"%1\"}}}").arg(fakePath).toUtf8());
        index.close();

        NoteThumbs reloaded;
        reloaded.setCacheDir(thumbDir);
        reloaded.load();
        const QImage got = reloaded.imageForCacheKey(fakeKey);
        noteCheck(!got.isNull() && got.width() == 320,
                  QStringLiteral("缩略图：从缓存目录取得到图（第二次打开不再联网）"),
                  QStringLiteral("%1x%2").arg(got.width()).arg(got.height()));
        noteCheck(reloaded.titleFor(QStringLiteral("https://example.com/cached"))
                      == QStringLiteral("缓存的网页"),
                  QStringLiteral("缩略图：缓存里那个网页标题也读回来了"));
    }

    /* =====================================================================
     * 3) 落盘：notes.json 往返 + 坏文件不静默清空
     * =================================================================== */
    {
        const QString noteFile = dir.filePath(QStringLiteral("notes.json"));
        QString savedId;
        {
            StickyNoteStore temp;
            temp.setFilePath(noteFile);
            StickyNote *note = temp.create();
            savedId = note->id();
            note->setText(QStringLiteral("买牛奶\nhttps://example.com/a"));
            note->setColor(QColor(QStringLiteral("#3b4048")));
            note->setOpacityPercent(70);
            note->setGeometry(QRect(120, 140, 340, 300));
            temp.flush();
            noteCheck(QFileInfo::exists(noteFile),
                      QStringLiteral("落盘：notes.json 真的写出来了"), noteFile);
        }

        StickyNoteStore again;
        again.setFilePath(noteFile);
        noteCheck(again.load(), QStringLiteral("落盘：读得回来"));
        noteCheck(again.count() == 1, QStringLiteral("落盘：还是那一条"),
                  QStringLiteral("读到 %1 条").arg(again.count()));
        if (again.count() == 1) {
            StickyNote *note = again.notes().first();
            noteCheck(note->id() == savedId, QStringLiteral("落盘：id 原样回来"));
            noteCheck(note->text().contains(QStringLiteral("买牛奶")),
                      QStringLiteral("落盘：正文原样回来"));
            noteCheck(note->color() == QColor(QStringLiteral("#3b4048")),
                      QStringLiteral("落盘：底色原样回来"), note->color().name());
            noteCheck(note->opacityPercent() == 70,
                      QStringLiteral("落盘：透明度原样回来（菜单里那条会存下来）"),
                      QStringLiteral("%1%").arg(note->opacityPercent()));
            noteCheck(note->geometry() == QRect(120, 140, 340, 300),
                      QStringLiteral("落盘：位置和尺寸原样回来"));
            noteCheck(note->linkCount() == 1,
                      QStringLiteral("落盘：正文里的链接是读回来之后重新抽的"));
        }

        QFile bad(noteFile);
        const bool badOpened = bad.open(QIODevice::WriteOnly | QIODevice::Truncate);
        Q_UNUSED(badOpened);
        bad.write("{ 这不是 JSON");
        bad.close();
        StickyNoteStore broken;
        broken.setFilePath(noteFile);
        noteCheck(!broken.load() && QFileInfo::exists(noteFile + QStringLiteral(".bad")),
                  QStringLiteral("落盘：notes.json 坏了改名留一份 .bad，不静默清空"));
    }

    /* =====================================================================
     * 4) 便签窗口：置顶无边框、配色、正文、链接卡片
     * =================================================================== */
    StickyNote *probe = nullptr;
    {
        probe = notes->createNote();
        settle();
        noteCheck(probe != nullptr, QStringLiteral("窗口：新建了一块便签"));

        const QVariantMap state = notes->windowState(probe ? probe->id() : QString());
        noteCheck(state.value(QStringLiteral("qmlError")).toString().isEmpty(),
                  QStringLiteral("窗口：界面（StickyNoteWindow.qml）加载成功"),
                  state.value(QStringLiteral("qmlError")).toString());
        noteCheck(state.value(QStringLiteral("visible")).toBool(),
                  QStringLiteral("窗口：新建出来就露在桌面上"));
        noteCheck(state.value(QStringLiteral("frameless")).toBool()
                      && state.value(QStringLiteral("onTopFlag")).toBool(),
                  QStringLiteral("窗口：无边框 + 置顶（钉在桌面上）"));
        noteCheck(state.value(QStringLiteral("toolWindow")).toBool(),
                  QStringLiteral("窗口：是 Qt::Tool（不占任务栏）"));
        noteCheck(state.value(QStringLiteral("color")).toString()
                      == QLatin1String("#ffe9a8"),
                  QStringLiteral("窗口：默认底色是便签黄"));
        noteCheck(QColor(state.value(QStringLiteral("textColor")).toString()).lightness() < 100,
                  QStringLiteral("窗口：浅色纸上配深字"));
        noteCheck(state.value(QStringLiteral("headerHeight")).toDouble() > 10,
                  QStringLiteral("窗口：头部（拖动把手）有高度"));
        noteCheck(notes->palette().size() >= 8,
                  QStringLiteral("窗口：调色板里有多个底色可选（便签头上那个颜色弹窗用它）"),
                  QStringLiteral("%1 格").arg(notes->palette().size()));

        QObject *root = notes->windowRootForId(probe->id());
        noteCheck(root != nullptr, QStringLiteral("窗口：拿到 QML 根对象"));

        /*
         * 换底色：走界面上那条路 —— 菜单里点色块调的是
         * StickyNoteWindow::setNoteColor（在那个**窗口对象**上，不在 QML 根上），
         * 换完深浅纸的前景色要跟着变。
         */
        if (QObject *windowObj = notes->windowForId(probe->id())) {
            QMetaObject::invokeMethod(windowObj, "setNoteColor",
                                      Q_ARG(QString, QStringLiteral("#3b4048")));
            settle();
            const QVariantMap dark = notes->windowState(probe->id());
            noteCheck(dark.value(QStringLiteral("color")).toString()
                          == QLatin1String("#3b4048"),
                      QStringLiteral("配色：底色换成石墨色"),
                      dark.value(QStringLiteral("color")).toString());
            noteCheck(QColor(dark.value(QStringLiteral("textColor")).toString()).lightness() > 150,
                      QStringLiteral("配色：深色纸上自动配浅字"),
                      dark.value(QStringLiteral("textColor")).toString());
            noteCheck(!dark.value(QStringLiteral("shadeColor")).toString().isEmpty()
                          && dark.value(QStringLiteral("shadeColor")).toString()
                                 != dark.value(QStringLiteral("color")).toString(),
                      QStringLiteral("配色：头部 / 链接栏的底和便签纸拉开了层次"));

            /* 换回便签黄，后面的用例按常规配色量 */
            QMetaObject::invokeMethod(windowObj, "setNoteColor",
                                      Q_ARG(QString, QStringLiteral("#ffe9a8")));
            settle();
            noteCheck(notes->windowState(probe->id()).value(QStringLiteral("color")).toString()
                          == QLatin1String("#ffe9a8"),
                      QStringLiteral("配色：换回便签黄"));
        } else {
            noteCheck(false, QStringLiteral("配色：拿得到便签窗口对象"));
        }

        /* 在编辑区里敲字（走用户那条路：QML 的 TextEdit -> note） */
        if (QObject *windowObj = notes->windowForId(probe->id())) {
            const QString typed = QStringLiteral(
                "备忘：https://example.com/note\n再看 www.qt.io 这个。");
            QMetaObject::invokeMethod(windowObj, "typeText", Q_ARG(QString, typed));
            settle();

            const QVariantMap after = notes->windowState(probe->id());
            noteCheck(after.value(QStringLiteral("text")).toString() == typed,
                      QStringLiteral("正文：在编辑区里敲的字进了数据"));
            noteCheck(after.value(QStringLiteral("linkCount")).toInt() == 2,
                      QStringLiteral("正文：两个网址被认出来了"),
                      QStringLiteral("%1 条").arg(after.value(QStringLiteral("linkCount")).toInt()));
            noteCheck(after.value(QStringLiteral("cardCount")).toInt() == 2,
                      QStringLiteral("正文：界面上摆出了两张链接卡片（缩略图）"),
                      QStringLiteral("%1 张").arg(after.value(QStringLiteral("cardCount")).toInt()));
        }
    }

    /* =====================================================================
     * 5) 右键菜单：贴着鼠标、子面板会翻边（颜色/锁定已经搬到便签头上，
     *    「更多颜色」那条色板也删了，见下面那两条）
     * =================================================================== */
    {
        StickyNote *menuNote = probe;
        QObject *root = notes->windowRootForId(menuNote->id());

        /*
         * 先把真实光标挪开再去 hook 打开菜单。
         *
         * 菜单"鼠标挪开就收"是看门狗按光标位置判的（见 NoteMenu 的 watchHover）：
         * hook 打开时如果真实光标**正好压在这块菜单上**，看门狗会把"进来过"
         * 记成真，之后光标一离开（后面的用例都会挪它）菜单就被收掉 ——
         * 于是"鼠标经过那条缝"那条会偶发飘红。这里的量全是按 hook 给的点算的，
         * 真实光标在屏幕哪个角落都无所谓。
         */
        if (QScreen *menuScreen = QGuiApplication::primaryScreen()) {
            const QRect a = menuScreen->availableGeometry();
            QCursor::setPos(a.x() + 20, a.bottom() - 20);
            settle();
        }

        QVariant opened;
        QMetaObject::invokeMethod(root, "openNoteMenu", Q_RETURN_ARG(QVariant, opened),
                                  Q_ARG(QVariant, QVariant(1500)),
                                  Q_ARG(QVariant, QVariant(900)));
        settle();
        noteCheck(opened.toBool() && root, QStringLiteral("菜单：openNoteMenu() 打得开"));

        const QVariantMap open = notes->menuState(menuNote->id());
        const QRect menuOpen = open.value(QStringLiteral("screenRect")).toRect();
        noteCheck(qAbs(menuOpen.x() - 1500) <= 2 && qAbs(menuOpen.y() - 900) <= 2,
                  QStringLiteral("菜单：左上角就在鼠标那一点（不是贴按钮）"),
                  QStringLiteral("菜单 %1,%2 / 鼠标 1500,900")
                      .arg(menuOpen.x()).arg(menuOpen.y()));
        noteCheck(open.value(QStringLiteral("opened")).toBool(),
                  QStringLiteral("菜单：弹出来了"));
        noteCheck(open.value(QStringLiteral("flyout")).toString().isEmpty(),
                  QStringLiteral("菜单：刚打开时右边没有子面板"));
        /*
         * 「鼠标挪开就收」这条已经换成「在别处按一下就收」（用户要求）。
         * 这里钉住前半段：光标一直待在菜单外面、让 160ms 那拍看门狗至少跑两轮，
         * 菜单必须还开着。
         *
         * 后半段（按一下就收）没法在无头自检里钉 —— 那要真按一次鼠标，而合成
         * 点击会落到光标所在处**别人的**窗口上（踩过：连点左树弹出重命名卡片、
         * 回车就把文件改了）。那半段只能人工验。
         */
        {
            QElapsedTimer sinceParked;
            sinceParked.start();
            while (sinceParked.elapsed() < 400)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
            const QVariantMap still = notes->menuState(menuNote->id());
            noteCheck(still.value(QStringLiteral("opened")).toBool(),
                      QStringLiteral("菜单：光标一直挪在外面也不自己收（改成点别处才收）"),
                      QStringLiteral("400ms 之后 opened=%1")
                          .arg(still.value(QStringLiteral("opened")).toBool()));
        }
        /*
         * 颜色弹窗：便签头上那个**色块按钮**点开的调色板（用户要求"颜色放到外面
         * 来、做成弹窗的形式"）。这里走的就是按钮那条路（root.openPalette），
         * 量三件事：弹窗真开了、整份调色板都在里面、第一格就是默认的便签黄。
         */
        {
            QObject *palette = root ? root->findChild<QObject *>(QStringLiteral("notePalette"))
                                    : nullptr;
            noteCheck(palette != nullptr,
                      QStringLiteral("颜色弹窗：便签窗口里找得到它（按名字）"));
            QVariant openedPalette;
            QMetaObject::invokeMethod(root, "openPalette", Q_RETURN_ARG(QVariant, openedPalette));
            settle();
            QVariant swatch0;
            QMetaObject::invokeMethod(root, "paletteSwatchAt", Q_RETURN_ARG(QVariant, swatch0),
                                      Q_ARG(QVariant, QVariant(0)));
            const int swatchCount = palette ? palette->property("swatches").toList().size() : -1;
            noteCheck(palette && palette->property("opened").toBool() && swatchCount >= 24,
                      QStringLiteral("颜色弹窗：点色块按钮弹出的就是整份调色板（≥24 格）"),
                      QStringLiteral("%1 格 / 第一格 %2").arg(swatchCount).arg(swatch0.toString()));
            noteCheck(swatch0.toString().startsWith(QLatin1String("#ffe9a8")),
                      QStringLiteral("颜色弹窗：色板第一格就是默认的便签黄"),
                      swatch0.toString());

            /* 按一格 = 真的换底色（走的是弹窗里那条 onClicked 同一条路） */
            noteCheck(root->findChild<QObject *>(QStringLiteral("noteColorButton")) != nullptr,
                      QStringLiteral("便签头：找得到颜色按钮"));
            const QString colorBefore =
                notes->windowState(menuNote->id()).value(QStringLiteral("color")).toString();
            QVariant pickedOther;
            QMetaObject::invokeMethod(root, "pickPaletteSwatch", Q_RETURN_ARG(QVariant, pickedOther),
                                      Q_ARG(QVariant, QVariant(3)));
            settle();
            const QString colorAfter =
                notes->windowState(menuNote->id()).value(QStringLiteral("color")).toString();
            noteCheck(pickedOther.toBool() && colorAfter != colorBefore,
                      QStringLiteral("颜色弹窗：按一格真的换了底色"),
                      QStringLiteral("%1 -> %2").arg(colorBefore, colorAfter));
            noteCheck(palette && !palette->property("opened").toBool(),
                      QStringLiteral("颜色弹窗：选完那一格自己收起来"));
            /* 换回原来的底色，后面的用例不跟着变 */
            QMetaObject::invokeMethod(root, "setBackground", Q_ARG(QVariant, QVariant(colorBefore)));
            settle();

            QMetaObject::invokeMethod(root, "closePalette");
            settle();
            noteCheck(palette && !palette->property("opened").toBool(),
                      QStringLiteral("颜色弹窗：收得掉"));

            /*
             * 按钮那一下是"开 / 收"：判据是弹窗**关的那一刻**光标还在不在按钮里
             * （见 StickyNoteWindow 的 palettePopup.closedByButton）—— 按在按钮上
             * 关掉的这一下该算"收"，不该又把它开回来（不然按钮永远关不掉弹窗）。
             * 界面上 colorHit.onClicked 走的就是 paletteButtonClicked()。
             */
            palette->setProperty("closedByButton", true);
            QVariant clickedAfterButtonClose;
            QMetaObject::invokeMethod(root, "paletteButtonClicked",
                                      Q_RETURN_ARG(QVariant, clickedAfterButtonClose));
            settle();
            noteCheck(!clickedAfterButtonClose.toBool() && !palette->property("opened").toBool()
                          && !palette->property("closedByButton").toBool(),
                      QStringLiteral("颜色弹窗：按在按钮上收掉之后，这一下不会再把它开回来"));
            /* 标记用掉之后，再点按钮就是"开" */
            QVariant clickedOpen;
            QMetaObject::invokeMethod(root, "paletteButtonClicked",
                                      Q_RETURN_ARG(QVariant, clickedOpen));
            settle();
            noteCheck(clickedOpen.toBool() && palette->property("opened").toBool(),
                      QStringLiteral("颜色弹窗：按钮那一下真的把弹窗开起来"));
            QMetaObject::invokeMethod(root, "closePalette");
            settle();
        }

        const QString labels = open.value(QStringLiteral("labels")).toString();
        for (const QString &want : {QStringLiteral("透明度"),
                                    QStringLiteral("新建便签"), QStringLiteral("始终置顶"),
                                    QStringLiteral("删除这块便签")}) {
            noteCheck(labels.contains(want),
                      QStringLiteral("菜单：有「%1」这一条").arg(want), labels);
        }
        /* 锁定那条搬到便签头上了，菜单里不该再有它 */
        noteCheck(!labels.contains(QStringLiteral("锁定")),
                  QStringLiteral("菜单：锁定已经从菜单里搬走（在便签头上）"), labels);
        /* 「正文里的链接」那一栏用户也不要了（正文底下的链接卡片照样能点） */
        noteCheck(!labels.contains(QStringLiteral("正文里的链接")),
                  QStringLiteral("菜单：没有「正文里的链接」那一栏了"), labels);
        /*
         * 「更多颜色」那条也删了（用户要求："取消更多颜色这行，调色板不要了删除"，
         * 后来又说"一起删干净"）。
         *
         * 它原来是鼠标停上去飞出一块色板（8 列 48 格）+ 最下面一条「更多颜色…」
         * （开 Qt 取色框）。现在**条目、色板、以及 C++ 那侧那套取色框
         * （pickColor / colorDialogFor / captureColorDialog）全都删了**：换底色的
         * 入口只剩便签头上那个颜色弹窗（见上面那一节），菜单里不该再留着它。
         *
         * 下面那条顺手钉住"再也飞不出来"：openMenuFlyout 会把 QML 的结果透传
         * 出来，菜单里没有挂着 color 的条目时它返回 false。
         */
        noteCheck(!labels.contains(QStringLiteral("更多颜色")),
                  QStringLiteral("菜单：没有「更多颜色」那一行了"), labels);
        noteCheck(!notes->openMenuFlyout(menuNote->id(), QStringLiteral("color")),
                  QStringLiteral("菜单：色板那一栏没了，再也飞不出面板（颜色交给便签头上的弹窗）"));

        /* 主栏的宽度（子面板是自己一块窗口，主栏窗口整场菜单就这么宽） */
        const double collapsedWidth = open.value(QStringLiteral("paneWidth")).toDouble();

        /* 展开透明度子面板：位置给得出来、和主栏并排、还在屏幕里
           （子面板这套机制现在只剩透明度和组合在用） */
        noteCheck(notes->openMenuFlyout(menuNote->id(), QStringLiteral("opacity")),
                  QStringLiteral("菜单：透明度子面板打得开"));
        settle();
        const QVariantMap fly = notes->menuState(menuNote->id());
        const QRect menuFly = fly.value(QStringLiteral("screenRect")).toRect();
        const QRect subFly = fly.value(QStringLiteral("flyoutRect")).toRect();
        const QString side = fly.value(QStringLiteral("flyoutSide")).toString();

        noteCheck(fly.value(QStringLiteral("flyout")).toString() == QLatin1String("opacity"),
                  QStringLiteral("菜单：当前展开的是透明度面板"));
        /*
         * 主栏那块窗口的几何整场菜单里不许变 —— 这是"不闪、不伸缩"的前提：
         * 子面板翻到左边时，只要主栏窗口跟着挪，Windows 就会把旧画面按新位置
         * 合成一下（闪），或者内容比窗口慢一帧（一伸一缩地抖）。
         */
        noteCheck(qAbs(fly.value(QStringLiteral("width")).toDouble()
                       - open.value(QStringLiteral("width")).toDouble()) < 0.5
                      && qAbs(fly.value(QStringLiteral("height")).toDouble()
                              - open.value(QStringLiteral("height")).toDouble()) < 0.5
                      && qAbs(fly.value(QStringLiteral("x")).toDouble()
                              - open.value(QStringLiteral("x")).toDouble()) < 0.5,
                  QStringLiteral("菜单：展开子面板时主栏窗口一动不动（不闪、不伸缩）"),
                  QStringLiteral("%1x%2@%3 -> %4x%5@%6")
                      .arg(open.value(QStringLiteral("width")).toDouble())
                      .arg(open.value(QStringLiteral("height")).toDouble())
                      .arg(open.value(QStringLiteral("x")).toDouble())
                      .arg(fly.value(QStringLiteral("width")).toDouble())
                      .arg(fly.value(QStringLiteral("height")).toDouble())
                      .arg(fly.value(QStringLiteral("x")).toDouble()));
        noteCheck(side == QLatin1String("right"),
                  QStringLiteral("菜单：右边装得下时子面板放右边"), side);
        noteCheck(subFly.x() >= menuFly.x() + collapsedWidth,
                  QStringLiteral("菜单：子面板和主栏并排（不叠在主栏上）"),
                  QStringLiteral("主栏右缘 %1 / 面板左缘 %2")
                      .arg(menuFly.x() + collapsedWidth).arg(subFly.x()));

        /*
         * 「子菜单闪一下就消失」那条毛病的钉子。
         *
         * 主栏和子面板之间有 6px 的缝：鼠标从"透明度"那一条往面板上挪，中途会
         * 离开那一行，而"进到面板上"那个 hover 事件在窗口刚改过尺寸时并不
         * 可靠 —— 只靠 hover 判断的话，面板会在这条缝上被收掉（用户看到的就是
         * 闪一下）。现在改成按**光标实际位置**判断，这里就把光标放进那条缝里，
         * 等过一个收合周期，面板必须还在。
         */
        {
            const QRect subFly2 = fly.value(QStringLiteral("flyoutRect")).toRect();
            const QRect menuFly2 = fly.value(QStringLiteral("screenRect")).toRect();
            noteOut(QStringLiteral("TRACE 缝检查开始 flyout=%1")
                        .arg(notes->menuState(menuNote->id())
                                 .value(QStringLiteral("flyout")).toString()));
            noteOut(QStringLiteral("（主栏 %1,%2 / 面板 %3,%4 %5 宽 / side=%6）")
                        .arg(menuFly2.x()).arg(menuFly2.y())
                        .arg(subFly2.x()).arg(subFly2.y()).arg(subFly2.width()).arg(side));
            if (side == QLatin1String("right") && subFly2.width() > 0) {
                const int gapX = menuFly2.x() + int(collapsedWidth) + 1;   /* 缝里 */
                const int gapY = subFly2.y() + 8;
                noteOut(QStringLiteral("（把光标放进主栏和面板之间那条缝：%1,%2）")
                            .arg(gapX).arg(gapY));
                const QPoint keep = QCursor::pos();
                QCursor::setPos(gapX, gapY);
                /* 等过 NoteMenu 那个 hoverWatch 的 160ms（收/不收都是它拍板的） */
                QElapsedTimer waited;
                waited.start();
                while (waited.elapsed() < 400) {
                    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                    QThread::msleep(10);
                }
                const QVariantMap still = notes->menuState(menuNote->id());
                noteCheck(still.value(QStringLiteral("flyout")).toString()
                              == QLatin1String("opacity"),
                          QStringLiteral("菜单：鼠标经过那条缝时子面板不会闪掉"),
                          QStringLiteral("flyout=%1")
                              .arg(still.value(QStringLiteral("flyout")).toString()));
                QCursor::setPos(keep);
                settle();
            }
        }

        /* 贴右缘：右边塞不下 -> 子面板翻到左边，而且两块都还在屏幕里 */
        QScreen *screen = QGuiApplication::primaryScreen();
        if (screen) {
            const QRect area = screen->availableGeometry();
            /*
             * 开菜单之前先把光标挪走。
             *
             * 上一条检查故意把光标留在了"主栏和面板之间那条缝"里（1709,985），
             * 这儿换到屏幕右缘开菜单时，光标会落在**要么主栏、要么面板**上 ——
             * 哪个都可能把菜单收掉（点开之后那一瞬间的 hover / 失焦判断）。
             * 这是自检自己的竞态，不是功能问题；挪到屏幕左下角（那儿没东西）
             * 就干净了。
             */
            const QPoint away = QCursor::pos();
            QCursor::setPos(area.x() + 20, area.bottom() - 20);
            settle();
            QMetaObject::invokeMethod(root, "openNoteMenu",
                                      Q_ARG(QVariant, QVariant(area.right() - 2)),
                                      Q_ARG(QVariant, QVariant(area.y() + 300)));
            settle();
            notes->openMenuFlyout(menuNote->id(), QStringLiteral("opacity"));
            settle();

            const QVariantMap edge = notes->menuState(menuNote->id());
            const bool edgeOpen = edge.value(QStringLiteral("opened")).toBool()
                                  && edge.value(QStringLiteral("flyout")).toString()
                                         == QLatin1String("opacity");
            const QRect menuEdge = edge.value(QStringLiteral("screenRect")).toRect();
            const QRect subEdge = edge.value(QStringLiteral("flyoutRect")).toRect();
            const QString edgeSide = edge.value(QStringLiteral("flyoutSide")).toString();

            /* 菜单没开成（光标位置把它带关了）就跳过这一组，别报假红 */
            if (!edgeOpen) {
                noteOut(QStringLiteral("（贴右缘那组没开成菜单，跳过：opened=%1 flyout=%2）")
                            .arg(edge.value(QStringLiteral("opened")).toBool())
                            .arg(edge.value(QStringLiteral("flyout")).toString()));
            } else {
                noteCheck(edgeSide == QLatin1String("left"),
                          QStringLiteral("菜单：右边塞不下时子面板翻到左边"), edgeSide);
                /*
                 * 翻到左边 = 挂在**主栏那块窗口**的左边、中间隔一条 paneGap。
                 *
                 * 这里原来比的是 `subEdge.right() <= menuEdge.x() + 2` —— 那时候
                 * 面板和主栏挤在一个窗口里，窗口左缘就是面板左缘。现在面板是
                 * **自己一块窗口**（见 NoteMenu 的 flyoutWindow），"主栏左缘"就是
                 * 主栏窗口的左缘（menuEdge.x()），比的是"面板右缘紧挨着它"。
                 */
                const int mainPaneLeft = menuEdge.x() + menuEdge.width() - int(collapsedWidth);
                noteCheck(subEdge.right() <= mainPaneLeft + 2,
                          QStringLiteral("菜单：翻到左边的子面板确实在主栏左侧"),
                          QStringLiteral("面板右缘 %1 / 主栏左缘 %2")
                              .arg(subEdge.right()).arg(mainPaneLeft));
                noteCheck(mainPaneLeft - subEdge.right() <= 12,
                          QStringLiteral("菜单：翻到左边的子面板紧挨着主栏（中间那条缝）"),
                          QStringLiteral("缝 %1px").arg(mainPaneLeft - subEdge.right()));
                noteCheck(area.contains(menuEdge) && area.contains(subEdge),
                          QStringLiteral("菜单：翻边之后两块都还在屏幕里"),
                          QStringLiteral("主栏 %1,%2 %3x%4 / 面板 %5,%6 %7x%8")
                              .arg(menuEdge.x()).arg(menuEdge.y())
                              .arg(menuEdge.width()).arg(menuEdge.height())
                              .arg(subEdge.x()).arg(subEdge.y())
                              .arg(subEdge.width()).arg(subEdge.height()));
            }
            QCursor::setPos(away);
            settle();
        }

        /*
         * 贴屏幕**下沿**点开：菜单不能探到屏幕外面去（任务栏底下就点不到了）。
         * 这一条钉的是"纵向夹取"——上面那几条量的都是横向。
         */
        if (screen && root) {
            const QRect area = screen->availableGeometry();
            QMetaObject::invokeMethod(root, "openNoteMenu",
                                      Q_ARG(QVariant, QVariant(area.x() + 600)),
                                      Q_ARG(QVariant, QVariant(area.bottom() - 4)));
            settle();
            const QVariantMap bottom = notes->menuState(menuNote->id());
            const QRect menuBottom = bottom.value(QStringLiteral("screenRect")).toRect();
            noteCheck(area.contains(menuBottom),
                      QStringLiteral("菜单：贴屏幕下沿点开时，整块还在屏幕里（不会钻到任务栏底下）"),
                      QStringLiteral("菜单 %1,%2 %3x%4 / 可用区 %5,%6 %7x%8")
                          .arg(menuBottom.x()).arg(menuBottom.y())
                          .arg(menuBottom.width()).arg(menuBottom.height())
                          .arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height()));
        }

        /*
         * 便签贴近屏幕右边时：子面板不能挂在"被便签挡住"的位置上。
         *
         * 用户报的场景：把便签拖到右边，点「⋯」，右边那块子面板（透明度/链接）
         * 要么出屏、要么被便签压住 —— 看着就是"便签把菜单盖住了"。
         * 规矩是：子面板优先挂右边，但右边**被便签挡**或者**出屏**时就挂左边，
         * 而主栏永远贴着鼠标那一点（不许为了塞下自己把主栏搬走、压住便签）。
         */
        if (screen && root) {
            const QRect area = screen->availableGeometry();
            /* 光标先挪开：免得它正好落在新开的菜单上把菜单带关（见上一条的说明） */
            const QPoint away = QCursor::pos();
            QCursor::setPos(area.x() + 20, area.bottom() - 20);
            settle();
            /* 把便签摆到靠近右沿，但左边留出 208（主栏宽）+ 缝隙 */
            const int noteX = area.x() + area.width() - 330 - 30;
            if (QObject *windowObj = notes->windowForId(menuNote->id())) {
                QMetaObject::invokeMethod(windowObj, "setPlacement",
                                          Q_ARG(QRect, QRect(noteX, area.y() + 60, 330, 300)));
            }
            settle();

            /* 点在便签左边一点：主栏不压便签，右边那块会被便签挡住 -> 应翻到左边 */
            const int clickX = noteX - 6;
            const int clickY = area.y() + 60;
            QMetaObject::invokeMethod(root, "openNoteMenu",
                                      Q_ARG(QVariant, QVariant(clickX)),
                                      Q_ARG(QVariant, QVariant(clickY)));
            settle();
            notes->openMenuFlyout(menuNote->id(), QStringLiteral("opacity"));
            settle();

            const QVariantMap near = notes->menuState(menuNote->id());
            const QRect menuNear = near.value(QStringLiteral("screenRect")).toRect();
            const QRect subNear = near.value(QStringLiteral("flyoutRect")).toRect();
            const QRect noteRect = near.value(QStringLiteral("noteRect")).toRect();
            const QString nearSide = near.value(QStringLiteral("flyoutSide")).toString();
            noteOut(QStringLiteral("（便签 %1,%2 %3x%4 / 鼠标 %5,%6 / 主栏 %7,%8 / 面板 %9,%10 %11x%12 side=%13）")
                        .arg(noteRect.x()).arg(noteRect.y())
                        .arg(noteRect.width()).arg(noteRect.height())
                        .arg(clickX).arg(clickY)
                        .arg(menuNear.x() + int(near.value(QStringLiteral("width")).toDouble())
                             - 208 - 0).arg(menuNear.y())
                        .arg(subNear.x()).arg(subNear.y())
                        .arg(subNear.width()).arg(subNear.height()).arg(nearSide));

            noteCheck(!subNear.intersects(noteRect) || nearSide == QLatin1String("right"),
                      QStringLiteral("便签贴右边时：子面板不会被便签压住（右边被挡就挂左边）"),
                      QStringLiteral("面板 %1,%2 %3x%4 / 便签 %5,%6 %7x%8 / side=%9")
                          .arg(subNear.x()).arg(subNear.y())
                          .arg(subNear.width()).arg(subNear.height())
                          .arg(noteRect.x()).arg(noteRect.y())
                          .arg(noteRect.width()).arg(noteRect.height()).arg(nearSide));
            noteCheck(nearSide == QLatin1String("left"),
                      QStringLiteral("便签贴右边时：右边那块被便签占着，子面板挂到左边"),
                      nearSide);
            noteCheck(area.contains(subNear),
                      QStringLiteral("便签贴右边时：子面板整块还在屏幕里"),
                      QStringLiteral("面板 %1,%2 %3x%4")
                          .arg(subNear.x()).arg(subNear.y())
                          .arg(subNear.width()).arg(subNear.height()));

            /* 收工：便签摆回原位，别影响后面几节 */
            if (QObject *windowObj = notes->windowForId(menuNote->id())) {
                QMetaObject::invokeMethod(windowObj, "setPlacement",
                                          Q_ARG(QRect, QRect(area.x() + 200, area.y() + 200,
                                                             330, 300)));
            }
            QMetaObject::invokeMethod(root, "closeNoteMenu");
            QCursor::setPos(away);
            settle();
        }

        /* 菜单不接焦点：它得能被收掉，而且不留残窗 */
        QMetaObject::invokeMethod(root, "closeNoteMenu");
        settle();
        const QVariantMap shut = notes->menuState(menuNote->id());
        noteCheck(!shut.value(QStringLiteral("opened")).toBool(),
                  QStringLiteral("菜单：收得掉"));
        const QStringList tops = visibleTopLevels();
        bool leftover = false;
        for (const QString &t : tops) {
            const int w = t.section(QLatin1Char('x'), 0, 0).toInt();
            if (w == int(collapsedWidth)
                || w == int(shut.value(QStringLiteral("width")).toDouble()))
                leftover = true;
        }
        noteCheck(!leftover, QStringLiteral("菜单：收起来之后屏幕上没有残留的菜单窗口"),
                  tops.join(QStringLiteral("，")));
    }

    /* =====================================================================
     * 6) 透明度 / 锁定 / 置顶 / 收起（菜单里那几条）
     * =================================================================== */
    {
        QObject *root = notes->windowRootForId(probe->id());
        if (root) {
            QMetaObject::invokeMethod(root, "handleMenuAct",
                                      Q_ARG(QVariant, QVariant("opacity:70")));
            settle();
            QVariantMap s = notes->windowState(probe->id());
            noteCheck(s.value(QStringLiteral("opacityPercent")).toInt() == 70,
                      QStringLiteral("菜单：选 70% 透明度之后便签真的半透明"),
                      QStringLiteral("%1%").arg(s.value(QStringLiteral("opacityPercent")).toInt()));
            noteCheck(probe->opacity() < 1.0,
                      QStringLiteral("菜单：透明度进了数据（会跟着 notes.json 存）"));

            /*
             * 锁定：走**便签头上那个锁按钮**那条路（root.toggleLock）。
             *
             * 这里钉住用户报的那条："这个锁定是单向的，只能锁定不能解锁" ——
             * 原来锁定是**窗口级鼠标穿透**（Qt::WindowTransparentForInput），
             * 连这个按钮自己都一起穿透了，锁上就再也点不着。现在窗口照收事件、
             * 由界面按 locked 关掉正文/拖动/改大小这些交互，头部那排按钮留着。
             */
            QObject *lockBtn = root->findChild<QObject *>(QStringLiteral("noteLockButton"));
            noteCheck(lockBtn != nullptr, QStringLiteral("便签头：找得到锁定按钮"));

            QVariant lockedByButton;
            QMetaObject::invokeMethod(root, "toggleLock", Q_RETURN_ARG(QVariant, lockedByButton));
            settle();
            s = notes->windowState(probe->id());
            QObject *bodyItem = root->findChild<QObject *>(QStringLiteral("noteBody"));
            noteCheck(lockedByButton.toBool() && s.value(QStringLiteral("locked")).toBool(),
                      QStringLiteral("便签头：点锁定按钮就锁上了"));
            noteCheck(lockBtn && lockBtn->property("on").toBool(),
                      QStringLiteral("便签头：锁上之后按钮自己也显示成「已锁」"),
                      QStringLiteral("on=%1").arg(lockBtn && lockBtn->property("on").toBool() ? 1 : 0));
            noteCheck(bodyItem && !bodyItem->property("enabled").toBool(),
                      QStringLiteral("便签头：锁上之后正文不再响应鼠标（不是窗口级穿透）"));

            QVariant unlockedByButton;
            QMetaObject::invokeMethod(root, "toggleLock", Q_RETURN_ARG(QVariant, unlockedByButton));
            settle();
            s = notes->windowState(probe->id());
            noteCheck(unlockedByButton.toBool() && !s.value(QStringLiteral("locked")).toBool(),
                      QStringLiteral("便签头：再点一次就解锁了（锁是双向的）"));
            noteCheck(bodyItem && bodyItem->property("enabled").toBool(),
                      QStringLiteral("便签头：解锁之后正文又能点了"));

            /*
             * 两个图标一样大（用户要求）。原来头上有三个按钮（颜色 / 锁定 / ⋯），
             * "⋯ 不要了"之后只剩两个 —— 这里按实际存在的那几个量。
             */
            {
                QObject *colorBtn = root->findChild<QObject *>(QStringLiteral("noteColorButton"));
                const bool sameW = colorBtn && lockBtn
                                   && colorBtn->property("implicitWidth").toReal()
                                          == lockBtn->property("implicitWidth").toReal();
                const bool sameH = colorBtn && lockBtn
                                   && colorBtn->property("implicitHeight").toReal()
                                          == lockBtn->property("implicitHeight").toReal();
                noteCheck(sameW && sameH,
                          QStringLiteral("便签头：颜色 / 锁定 两个按钮一样大"),
                          QStringLiteral("颜色 %1x%2 / 锁定 %3x%4")
                              .arg(colorBtn ? colorBtn->property("implicitWidth").toReal() : -1)
                              .arg(colorBtn ? colorBtn->property("implicitHeight").toReal() : -1)
                              .arg(lockBtn ? lockBtn->property("implicitWidth").toReal() : -1)
                              .arg(lockBtn ? lockBtn->property("implicitHeight").toReal() : -1));
                /* 「⋯」那个按钮已经撤了（菜单改成右键弹） */
                noteCheck(!root->findChild<QObject *>(QStringLiteral("noteMenuButton"))
                              && !root->findChild<QObject *>(QStringLiteral("noteMenuIcon")),
                          QStringLiteral("便签头：没有「⋯」按钮了（菜单走右键）"));
            }

            /*
             * 两个图标**画出来**也得一样大（用户报的："图标不一样大"）。
             *
             * 只比画布尺寸不够：画布本来就都是 14×14，可挂锁只画了 7 个单位宽、
             * T 恤画了 13.6 —— 摆在旁边就是小一号。所以这里抓一张窗口图，在图标
             * 的矩形里找和纸色差得多的像素，量出**墨迹包围盒**，比高度（宽度天然
             * 差得多：挂锁比 T 恤窄）。
             */
            {
                auto *iconWin = qobject_cast<QWidget *>(notes->windowForId(probe->id()));
                auto *rootItem = qobject_cast<QQuickItem *>(root);
                const QImage shot = iconWin ? iconWin->grab().toImage() : QImage();
                const qreal dpr = iconWin ? iconWin->devicePixelRatioF() : 1.0;
                QStringList inkHeights;
                bool inkOk = !shot.isNull() && rootItem != nullptr;
                for (const QString &name : {QStringLiteral("noteColorIcon"),
                                            QStringLiteral("noteLockIcon")}) {
                    auto *icon = qobject_cast<QQuickItem *>(
                        root->findChild<QObject *>(name));
                    if (!icon) {
                        inkOk = false;
                        inkHeights << QStringLiteral("?");
                        continue;
                    }
                    /* 图标在便签里的位置（换算到窗口像素） */
                    const QPointF at = icon->mapToItem(rootItem, QPointF(0, 0));
                    const QRect r(qRound(at.x() * dpr), qRound(at.y() * dpr),
                                  qRound(icon->width() * dpr), qRound(icon->height() * dpr));
                    const QRect clamped = r.intersected(shot.rect());
                    if (clamped.isEmpty()) {
                        inkOk = false;
                        inkHeights << QStringLiteral("空");
                        continue;
                    }
                    /*
                     * 这块里**出现最多的那一档亮度**当纸色（图标只占一小块，
                     * 剩下的都是纸），跟它差 40 以上的算墨迹 —— 便签纸有深有浅
                     * （浅纸上墨是暗的、深纸上墨是亮的），只看"比纸暗"会在深色
                     * 便签上把整块都算成墨迹。
                     */
                    QHash<int, int> lumHist;
                    int bestCount = -1;
                    int paperLum = 0;
                    for (int y = clamped.top(); y <= clamped.bottom(); ++y) {
                        for (int x = clamped.left(); x <= clamped.right(); ++x) {
                            const int bucket = (qGray(shot.pixel(x, y)) / 8) * 8;
                            const int count = ++lumHist[bucket];
                            if (count > bestCount) {
                                bestCount = count;
                                paperLum = bucket;
                            }
                        }
                    }
                    QRect ink;
                    for (int y = clamped.top(); y <= clamped.bottom(); ++y) {
                        for (int x = clamped.left(); x <= clamped.right(); ++x) {
                            if (qAbs(qGray(shot.pixel(x, y)) - paperLum) < 40)
                                continue;
                            ink = ink.isNull() ? QRect(x, y, 1, 1) : ink.united(QRect(x, y, 1, 1));
                        }
                    }
                    if (ink.isNull()) {
                        inkOk = false;
                        inkHeights << QStringLiteral("没画");
                        continue;
                    }
                    inkHeights << QString::number(ink.height());
                }
                int minH = 1000;
                int maxH = 0;
                for (const QString &h : std::as_const(inkHeights)) {
                    bool ok = false;
                    const int v = h.toInt(&ok);
                    if (!ok) {
                        inkOk = false;
                        continue;
                    }
                    minH = qMin(minH, v);
                    maxH = qMax(maxH, v);
                }
                /* 两个图标墨迹高度差不超过 2 个像素（画法在 16 的框里，容一点圆角） */
                noteCheck(inkOk && maxH - minH <= 2 && minH >= 8,
                          QStringLiteral("便签头：两个图标画出来一样大（量墨迹高度）"),
                          QStringLiteral("颜色 %1 / 锁定 %2")
                              .arg(inkHeights.value(0), inkHeights.value(1)));
            }

            /*
             * 菜单的入口是**在便签上点右键**（用户要求："右键点便签的任意位置
             * （正文 / 头部 / 标签条）也弹这个菜单"，并且"⋯ 不要了"）。
             *
             * 量的是那个 TapHandler：收哪几个键（Qt.RightButton = 2）、锁定时
             * 让不让位。真点一下右键是界面上那一下，自检里没法合成完整的
             * "按下+抬起+没怎么动"，所以这里量它的配置。
             */
            if (QObject *rightClick = root->findChild<QObject *>(QStringLiteral("noteRightClick"))) {
                noteCheck(rightClick->property("acceptedButtons").toInt() == Qt::RightButton,
                          QStringLiteral("便签：右键那个入口只认右键"),
                          QStringLiteral("acceptedButtons=%1")
                              .arg(rightClick->property("acceptedButtons").toInt()));
            } else {
                noteCheck(false, QStringLiteral("便签：找得到右键弹菜单的入口"));
            }

            /* 锁上之后「解锁所有便签」（托盘/菜单那条）照样管用 */
            QMetaObject::invokeMethod(root, "toggleLock");
            settle();
            noteCheck(notes->lockedCount() >= 1,
                      QStringLiteral("锁定：锁定条数报得出来（菜单据此显示解锁）"));
            noteCheck(notes->unlockAll(),
                      QStringLiteral("菜单：「解锁所有便签」解得开"));
            settle();
            s = notes->windowState(probe->id());
            noteCheck(!s.value(QStringLiteral("locked")).toBool(),
                      QStringLiteral("菜单：解锁之后又能点了"));

            QMetaObject::invokeMethod(root, "handleMenuAct", Q_ARG(QVariant, QVariant("pin")));
            settle();
            s = notes->windowState(probe->id());
            noteCheck(!s.value(QStringLiteral("staysOnTop")).toBool()
                          && !s.value(QStringLiteral("onTopFlag")).toBool(),
                      QStringLiteral("菜单：取消「始终置顶」之后窗口标志位也摘了"));
        }
    }

    /* =====================================================================
     * 7) 默认位置 + 一键排列
     * =================================================================== */
    {
        StickyNote *fresh = notes->createNote();
        settle();
        const QVariantMap one = notes->windowState(fresh ? fresh->id() : QString());
        const QRect first(one.value(QStringLiteral("x")).toInt(),
                          one.value(QStringLiteral("y")).toInt(),
                          one.value(QStringLiteral("w")).toInt(),
                          one.value(QStringLiteral("h")).toInt());
        noteCheck(first.y() <= 40, QStringLiteral("摆放：新便签默认贴在屏幕顶上"),
                  QStringLiteral("y=%1").arg(first.y()));
        noteCheck(first.x() + first.width() / 2
                      > (QGuiApplication::primaryScreen()
                             ? QGuiApplication::primaryScreen()->availableGeometry().center().x()
                             : 960),
                  QStringLiteral("摆放：新便签默认在屏幕右半边（右上角起）"),
                  QStringLiteral("x=%1").arg(first.x()));

        StickyNote *second = notes->createNote();
        settle();
        const QVariantMap two = notes->windowState(second ? second->id() : QString());
        const QRect next(two.value(QStringLiteral("x")).toInt(),
                         two.value(QStringLiteral("y")).toInt(),
                         two.value(QStringLiteral("w")).toInt(),
                         two.value(QStringLiteral("h")).toInt());
        noteCheck(!next.intersects(first),
                  QStringLiteral("摆放：连着开两块不会叠在一起"),
                  QStringLiteral("(%1,%2) vs (%3,%4)")
                      .arg(next.x()).arg(next.y()).arg(first.x()).arg(first.y()));

        noteCheck(notes->arrangeAll(), QStringLiteral("排列：一键排列生效"));
        settle();

        QList<QRect> rects;
        for (StickyNote *note : notes->store()->notes()) {
            if (!note || !note->visible())
                continue;
            const QVariantMap s = notes->windowState(note->id());
            rects.append(QRect(s.value(QStringLiteral("x")).toInt(),
                               s.value(QStringLiteral("y")).toInt(),
                               s.value(QStringLiteral("w")).toInt(),
                               s.value(QStringLiteral("h")).toInt()));
        }
        int overlaps = 0;
        for (int i = 0; i < rects.size(); ++i) {
            for (int j = i + 1; j < rects.size(); ++j) {
                if (rects.at(i).intersects(rects.at(j)))
                    ++overlaps;
            }
        }
        noteCheck(rects.size() >= 3 && overlaps == 0,
                  QStringLiteral("排列：几块便签排成网格、互不重叠"),
                  QStringLiteral("%1 块 / %2 处重叠").arg(rects.size()).arg(overlaps));

        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect area = screen->availableGeometry();
            bool inScreen = true;
            for (const QRect &r : rects) {
                if (!area.contains(r))
                    inScreen = false;
            }
            noteCheck(inScreen, QStringLiteral("排列：排完都在屏幕里"));

            /*
             * 每一行**从右往左填**（用户要的"排列从右开始"）：离右沿"整数个格子"
             * 的位置才允许 —— 也就是第 0 格贴着工作区右沿，之后逐格往左退一个
             * 步长。左对齐的话 x 会从 area.x() 起数，这里的余数就对不上了。
             */
            const int gap = 14;   /* kArrangeGap */
            const int rightEdge = area.x() + area.width();
            bool flushRight = true;
            int leftMost = rects.isEmpty() ? 0 : rects.first().x();
            for (const QRect &r : rects) {
                if (r.width() <= 0)
                    continue;
                const int offset = rightEdge - (r.x() + r.width());
                const int step = r.width() + gap;
                const int column = offset >= 0 ? offset / step : -1;
                const int expectedX = column >= 0 ? rightEdge - r.width() - column * step : -1;
                if (column < 0 || qAbs(r.x() - expectedX) > 1)
                    flushRight = false;
                leftMost = qMin(leftMost, r.x());
            }
            noteCheck(flushRight,
                      QStringLiteral("排列：每一行贴右沿开始（从右往左填）"),
                      QStringLiteral("右沿 %1 / 最左 %2").arg(rightEdge).arg(leftMost));
        }

        /* 收起 / 再叫回来：数据都留着 */
        notes->hideAll();
        settle();
        noteCheck(notes->visibleCount() == 0,
                  QStringLiteral("收起：全部收起来之后桌面上一块都没有"));
        noteCheck(notes->count() >= 3, QStringLiteral("收起：只是藏了，数据没删"));
        notes->showAll();
        settle();
        noteCheck(notes->visibleCount() == notes->count(),
                  QStringLiteral("叫回：再叫回来还是那几块"),
                  QStringLiteral("%1 块").arg(notes->visibleCount()));
    }

    /* =====================================================================
     * 8) 组合 + 排列成摞
     *
     * "组合" = 把几块便签归到一摞里（拖一块的头部到另一块身上松手，或者菜单
     * 里选"与「便签 N」组合"）；"排列成摞" = 把一摞摆成露头 + 依次错开的样子。
     * 这一节两件都量：归堆、摆法、整摞一起搬、点下面那张纸抽到最上面、拆开、
     * 活过重启。
     * =================================================================== */
    {
        const QString groupFile = dir.filePath(QStringLiteral("group-notes.json"));
        notes->store()->setFilePath(groupFile);

        auto windowStateOf = [notes](StickyNote *note, const QString &key) {
            return notes->windowState(note ? note->id() : QString()).value(key);
        };
        auto noteRect = [&windowStateOf](StickyNote *note) {
            return QRect(windowStateOf(note, QStringLiteral("x")).toInt(),
                         windowStateOf(note, QStringLiteral("y")).toInt(),
                         windowStateOf(note, QStringLiteral("w")).toInt(),
                         windowStateOf(note, QStringLiteral("h")).toInt());
        };

        /* 三块新便签，先各自摆开（不然本来就是"一摞"叠着的，看不出重排效果） */
        StickyNote *a = notes->createNote();
        StickyNote *b = notes->createNote();
        StickyNote *c = notes->createNote();
        settle();

        auto placeNote = [notes](StickyNote *note, const QRect &rect) {
            if (QObject *windowObj = notes->windowForId(note ? note->id() : QString()))
                QMetaObject::invokeMethod(windowObj, "setPlacement", Q_ARG(QRect, rect));
        };
        placeNote(a, QRect(300, 300, 330, 300));
        placeNote(b, QRect(420, 360, 330, 300));
        placeNote(c, QRect(540, 420, 330, 300));
        settle();

        /* ---- 组合 ---- */
        /*
         * 便签窗口里 handleMenuAct 是**QML 根对象**上的函数（不在 C++ 窗口
         * 对象上），菜单里那几条命令最后都落到它身上。这个 lambda 让下面的
         * 用例走的就是用户点菜单那条路。
         */
        auto menuAct = [notes](StickyNote *note, const QString &act) {
            QObject *root = notes->windowRootForId(note ? note->id() : QString());
            if (!root)
                return false;
            /*
             * QML 里的函数在元对象系统里一律返回 QVariant，Q_RETURN_ARG(bool, …)
             * 会被拒（"return type mismatch … cannot convert from QVariant to bool"），
             * 所以这里按 QVariant 接、再自己转（和 revealLink 那边一个写法）。
             */
            QVariant ok;
            QMetaObject::invokeMethod(root, "handleMenuAct", Q_RETURN_ARG(QVariant, ok),
                                      Q_ARG(QVariant, QVariant(act)));
            return ok.toBool();
        };

        /*
         * 一次凑三块。
         *
         * 菜单里"与「便签 3」组合"一次只带**一条**，所以这里直接走那条命令最后
         * 落到的地方：groupWith(这一块, [另两块]) —— 和菜单点是同一个函数，
         * 只是名单长一点。
         *
         * 菜单那一条本身在下面那一节单独量（那一栏打得开、点一条能成组）；
         * 鼠标那条路（拖头部叠上去）在更下面单独量。
         */
        QList<StickyNote *> mates;
        if (b)
            mates << b;
        if (c)
            mates << c;
        const bool grouped = notes->groupWith(a, mates);
        settle();
        noteCheck(grouped, QStringLiteral("组合：能把三块便签凑成一摞"));

        const QVariantMap groupState = notes->groupState(a ? a->id() : QString());
        noteCheck(groupState.value(QStringLiteral("count")).toInt() == 3,
                  QStringLiteral("组合：这一摞里就是那三块"),
                  QStringLiteral("%1 块").arg(groupState.value(QStringLiteral("count")).toInt()));
        noteCheck(!groupState.value(QStringLiteral("groupId")).toString().isEmpty()
                      && a && a->groupId() == groupState.value(QStringLiteral("groupId")).toString()
                      && b && b->groupId() == a->groupId()
                      && c && c->groupId() == a->groupId(),
                  QStringLiteral("组合：组 id 贴到了组里每一条身上（跟着 notes.json 存）"));

        const QStringList order = groupState.value(QStringLiteral("members")).toStringList();
        noteCheck(!order.isEmpty() && a && order.first() == a->id(),
                  QStringLiteral("组合：点的那一块在最上面（它排第一）"));
        noteCheck(groupState.value(QStringLiteral("activeId")).toString()
                      == (a ? a->id() : QString()),
                  QStringLiteral("组合：报得出来「露头那张纸」是哪一块"));

        /* ---- 一摞只占一格：露头那块在原来的位置，其余几块对齐到同一格 ---- */
        const QRect aRect = noteRect(a);
        const QList<StickyNote *> members{a, b, c};
        QList<QRect> rects;
        for (StickyNote *member : members)
            rects.append(noteRect(member));

        noteCheck(rects.at(0).topLeft() == QPoint(300, 300),
                  QStringLiteral("组合：露头那块留在原地（一摞围着它摆）"),
                  QStringLiteral("%1,%2").arg(rects.at(0).x()).arg(rects.at(0).y()));
        /*
         * 同组其余几块**对齐到同一格**（用户要求："组合的卡片只显示一张卡片，
         * 这一张卡片切换"）：它们藏起来等着被换上来，位置就在露头那块那一格，
         * 换标签才是原地换纸。
         */
        noteCheck(rects.at(1).topLeft() == rects.at(0).topLeft()
                      && rects.at(2).topLeft() == rects.at(0).topLeft(),
                  QStringLiteral("组合：其余几块都对齐到露头那一格（桌面上只占一格）"),
                  QStringLiteral("%1,%2 / %3,%4 / %5,%6")
                      .arg(rects.at(0).x()).arg(rects.at(0).y())
                      .arg(rects.at(1).x()).arg(rects.at(1).y())
                      .arg(rects.at(2).x()).arg(rects.at(2).y()));
        noteCheck(rects.at(0).size() == rects.at(1).size()
                      && rects.at(1).size() == rects.at(2).size(),
                  QStringLiteral("组合：一摞里的纸一样大"));
        /* 露头那一块是**完整露着**的，其余几块收起来只留标签 */
        noteCheck(notes->windowState(a->id()).value(QStringLiteral("visible")).toBool()
                      && !notes->windowState(b->id()).value(QStringLiteral("visible")).toBool()
                      && !notes->windowState(c->id()).value(QStringLiteral("visible")).toBool(),
                  QStringLiteral("组合：一摞里只有露头那一张纸露着（其余收成标签）"));

        bool inScreen = true;
        if (QScreen *screen = QGuiApplication::primaryScreen()) {
            const QRect area = screen->availableGeometry();
            for (const QRect &r : rects) {
                if (!area.contains(r))
                    inScreen = false;
            }
        }
        noteCheck(inScreen, QStringLiteral("组合：整摞（那一格）都在屏幕里"));

        /* ---- QML 侧也看得出状态（菜单里两条文案按它挑） ---- */
        if (QObject *root = notes->windowRootForId(a ? a->id() : QString())) {
            noteCheck(root->property("inGroup").toBool()
                          && root->property("groupActive").toBool()
                          && root->property("groupSize").toInt() == 3,
                      QStringLiteral("组合：界面读得到「在组里 / 是不是露头 / 组里几块」"));
        } else {
            noteCheck(false, QStringLiteral("组合：拿得到便签的 QML 根对象"));
        }

        /* ---- 组合过的不能再凑一摞 ---- */
        const bool again = menuAct(a, QStringLiteral("group:%1").arg(b ? b->id() : QString()));
        noteCheck(!again, QStringLiteral("组合：已经组合过的便签不能再凑一摞（一次只做一层）"));

        /* ---- 整摞一起搬 ---- */
        /*
         * 拖的是**露头那块**（层叠顺序里排第一的；前面抽过主角之后不一定是 a）。
         * 走的是真实那条路：按下头部（beginDrag 记下基准）之后窗口被搬动，
         * moveEvent 里整摞按同一份位移跟着走（见 beginGroupDrag）。
         *
         * 用 QWidget::move 而不是 setPlacement：后者是"程序把便签摆到某处"
         * （带 m_placing 闸门，不当成拖动），那正是拖窗口的反面。
         */
        const QRect frontRectBefore = noteRect(notes->groupState(a->id())
                                                   .value(QStringLiteral("activeId"))
                                                   .toString()
                                               == (b ? b->id() : QString())
                                               ? b
                                               : a);
        StickyNote *draggedNote = (a && noteRect(a).topLeft() == frontRectBefore.topLeft()) ? a : b;
        StickyNote *otherNote1 = (draggedNote == a) ? b : a;
        const QRect draggedBefore = noteRect(draggedNote);
        const QRect other1Before = noteRect(otherNote1);
        const QRect other2Before = noteRect(c);
        const QPoint delta(64, 48);
        bool moved = false;
        bool refused = false;
        if (auto *front = qobject_cast<QWidget *>(notes->windowForId(draggedNote->id()))) {
            QMetaObject::invokeMethod(front, "beginDrag");
            /*
             * 搬的是**窗口**（QWidget::move 只认窗口坐标），而 noteRect 量的是
             * **卡片**：窗口左边还挂着一条标签条（见 frameRectFor），所以这里
             * 要把卡片的起点换算成窗口的起点再搬。
             */
            const QPoint frameOffset(front->geometry().left() - draggedBefore.left(), 0);
            front->move(draggedBefore.topLeft() + frameOffset + delta);
            settle();
            moved = noteRect(otherNote1).topLeft() == other1Before.topLeft() + delta
                    && noteRect(c).topLeft() == other2Before.topLeft() + delta;
            QMetaObject::invokeMethod(front, "promoteInGroup", Q_RETURN_ARG(bool, refused));
        }
        noteCheck(moved, QStringLiteral("拖动：拖露头那块，整摞按同一份位移跟着走"),
                  QStringLiteral("被拖 %1,%2 / 另一块 %3,%4 期望 %5,%6")
                      .arg(draggedBefore.x()).arg(draggedBefore.y())
                      .arg(noteRect(otherNote1).x()).arg(noteRect(otherNote1).y())
                      .arg(other1Before.x() + delta.x()).arg(other1Before.y() + delta.y()));
        noteCheck(!refused, QStringLiteral("拖动：本来就在最上面的那块不用再抽一次"));

        /* ---- 换标签：点另一块，它换上来、原来那张收回去（原地换纸） ---- */
        /*
         * 一摞在桌面上只有一张纸：其余几块是**藏着的窗口**（点不着），换哪一块
         * 只能走左边那排标签 —— 也就是 switchGroupTab（界面点色块那条路）。
         */
        const QRect stackOrigin = noteRect(notes->groupState(a->id())
                                               .value(QStringLiteral("activeId"))
                                               .toString()
                                           == (b ? b->id() : QString())
                                           ? b
                                           : a);
        const bool switchedToB = notes->switchGroupTab(b->id());
        settle();
        /*
         * "谁露头"看 groupState 的 activeId（= notes.json 里记的那一块），
         * **不看 members 的第一条**：members 是层叠顺序那套留下的（第一块曾是
         * "最上面那张"），现在一摞平铺在同一格、谁露着由 active 说了算。
         */
        const QVariantMap afterSwitch = notes->groupState(b ? b->id() : QString());
        noteCheck(switchedToB
                      && afterSwitch.value(QStringLiteral("activeId")).toString()
                             == (b ? b->id() : QString()),
                  QStringLiteral("换标签：点另一块，它换上来（露头那张纸换人）"),
                  QStringLiteral("露头 %1")
                      .arg(afterSwitch.value(QStringLiteral("activeId")).toString().left(4)));
        noteCheck(noteRect(b).topLeft() == stackOrigin.topLeft()
                      && noteRect(a).topLeft() == stackOrigin.topLeft()
                      && noteRect(c).topLeft() == stackOrigin.topLeft(),
                  QStringLiteral("换标签：原地换纸（一摞那一格没挪窝）"),
                  QStringLiteral("b %1,%2 / 那一格 %3,%4")
                      .arg(noteRect(b).x()).arg(noteRect(b).y())
                      .arg(stackOrigin.x()).arg(stackOrigin.y()));
        noteCheck(notes->windowState(b->id()).value(QStringLiteral("visible")).toBool()
                      && !notes->windowState(a->id()).value(QStringLiteral("visible")).toBool()
                      && !notes->windowState(c->id()).value(QStringLiteral("visible")).toBool(),
                  QStringLiteral("换标签：换上来那块露着、原来那张收起来"));

        /* ---- 抽过主角之后再拖一次：整摞还是刚性的（不许跳一格） ---- */
        {
            /*
             * 这一条是**回归钉子**：抽一块上来之后，"露头那块"（z 序最上面）
             * 和"占着整摞左上角那块"不再是同一块了。层叠偏移要是按组内顺序
             * 当级数算，这时候拖一次整摞会整体平移一格（鼠标一动、纸跳一下）。
             *
             * 拖的还是露头那块（b），量另外两块是不是跟着走了同一份位移。
             */
            const QList<QRect> before2 = {noteRect(a), noteRect(b), noteRect(c)};
            const QPoint delta2(40, 24);
            bool moved2 = false;
            if (auto *front = qobject_cast<QWidget *>(notes->windowForId(b->id()))) {
                QMetaObject::invokeMethod(front, "beginDrag");
                /* 同上面那一条：卡片起点 -> 窗口起点（窗口左边还有一条标签条） */
                const QPoint frameOffset(front->geometry().left() - before2.at(1).left(), 0);
                front->move(before2.at(1).topLeft() + frameOffset + delta2);
                settle();
                moved2 = noteRect(a).topLeft() == before2.at(0).topLeft() + delta2
                         && noteRect(c).topLeft() == before2.at(2).topLeft() + delta2;
            }
            noteCheck(moved2,
                      QStringLiteral("拖动：换过露头那块之后再拖，整摞仍然按同一份位移走"),
                      QStringLiteral("c %1,%2 -> %3,%4 / 期望 %5,%6")
                          .arg(before2.at(2).x()).arg(before2.at(2).y())
                          .arg(noteRect(c).x()).arg(noteRect(c).y())
                          .arg(before2.at(2).x() + delta2.x())
                          .arg(before2.at(2).y() + delta2.y()));
        }

        /* ---- 层叠顺序要活过重启 ---- */
        notes->store()->flush();
        const QString activeBefore = notes->groupState(b->id()).value(QStringLiteral("activeId")).toString();
        {
            StickyNoteStore probeStore;
            probeStore.setFilePath(groupFile);
            noteCheck(probeStore.load()
                          && probeStore.count() == notes->count(),
                      QStringLiteral("落盘：组合那一摞写进了 notes.json"));
            noteCheck(probeStore.groupActiveId(activeBefore.isEmpty()
                                                   ? QString()
                                                   : b->groupId())
                          == activeBefore,
                      QStringLiteral("落盘：连「哪一块在最上面」都记下来了（重启后摆回原样）"),
                      probeStore.groupActiveId(b->groupId()));
        }
        {
            /* 从文件重建一遍：三块还是属于同一个组、组里的先后也回来了 */
            StickyNoteStore reload;
            reload.setFilePath(groupFile);
            reload.load();
            QStringList ids;
            QString reloadGroup;
            for (StickyNote *note : reload.notes()) {
                if (!note || note->groupId().isEmpty())
                    continue;
                reloadGroup = note->groupId();
                ids << note->id();
            }
            noteCheck(ids.size() == 3 && !reloadGroup.isEmpty(),
                      QStringLiteral("落盘：读回来还是那三块在一摞里"),
                      QStringLiteral("%1 块").arg(ids.size()));
        }

        /* ---- 菜单里那一条：组合着的便签给的是「拆分组合」 ---- */
        if (QObject *root = notes->windowRootForId(a->id())) {
            QMetaObject::invokeMethod(root, "openNoteMenu",
                                      Q_ARG(QVariant, QVariant(1400)),
                                      Q_ARG(QVariant, QVariant(700)));
            settle();
            const QString groupedLabels = notes->menuState(a->id())
                                              .value(QStringLiteral("labels")).toString();
            noteCheck(groupedLabels.contains(QStringLiteral("拆分组合")),
                      QStringLiteral("菜单：组合着的便签给的是「拆分组合」"), groupedLabels);
            noteCheck(!groupedLabels.contains(QStringLiteral("与…组合")),
                      QStringLiteral("菜单：组合着的便签不再给「与…组合」那一栏"));
            /*
             * 那一栏根本没有，所以也**飞不出来**：openMenuFlyout 现在会把 QML
             * 的结果透传出来（以前它一律 return true，这条量不出来）。
             */
            noteCheck(!notes->openMenuFlyout(a->id(), QStringLiteral("group"))
                          && notes->menuState(a->id()).value(QStringLiteral("flyout")).toString()
                                 .isEmpty(),
                      QStringLiteral("菜单：组合着的便签没有「与…组合」那一栏（面板也飞不出来）"));
            QMetaObject::invokeMethod(root, "closeNoteMenu");
            settle();
        }

        /* ---- 解散：摊开成几块单独的卡片 ---- */
        const QList<QRect> beforeUngroup = {noteRect(a), noteRect(b), noteRect(c)};
        const bool ungrouped = menuAct(b, QStringLiteral("ungroup"));
        settle();
        noteCheck(ungrouped, QStringLiteral("解散：菜单那条路能把这一摞散了"));
        noteCheck(a->groupId().isEmpty() && b->groupId().isEmpty() && c->groupId().isEmpty(),
                  QStringLiteral("解散：三块的组 id 都摘干净了"));
        noteCheck(noteRect(b) == beforeUngroup.at(1),
                  QStringLiteral("解散：点的那一块留在原地（不跳回原来摆开的地方）"));
        /*
         * 其余几块**当场摊开**（用户明确要求："拆分组合点击后，展开成单独的
         * 卡片"）。不摊开的话它们严丝合缝叠在同一格上，屏幕上还是只有一张纸，
         * 而且压在最上面的正好是刚从"收起来的那张纸"状态出来的那块 —— 露出来
         * 的是一张空白卡片（用户截了图）。
         */
        noteCheck(noteRect(a) != beforeUngroup.at(0) && noteRect(c) != beforeUngroup.at(2)
                      && !noteRect(a).intersects(noteRect(b))
                      && !noteRect(a).intersects(noteRect(c))
                      && !noteRect(b).intersects(noteRect(c)),
                  QStringLiteral("解散：三块摊开成单独的卡片（互不重叠）"),
                  QStringLiteral("a %1,%2 / b %3,%4 / c %5,%6")
                      .arg(noteRect(a).x()).arg(noteRect(a).y())
                      .arg(noteRect(b).x()).arg(noteRect(b).y())
                      .arg(noteRect(c).x()).arg(noteRect(c).y()));
        noteCheck(notes->windowState(a->id()).value(QStringLiteral("visible")).toBool()
                      && notes->windowState(b->id()).value(QStringLiteral("visible")).toBool()
                      && notes->windowState(c->id()).value(QStringLiteral("visible")).toBool(),
                  QStringLiteral("解散：收起来的那几块都放回桌面上"));
        /*
         * 拆完每一块都得画回**完整卡片**：collapsed 要是停在"收起来的那张纸"
         * 那个状态，屏幕上就是一张空白卡片（用户报的就是这个）。
         */
        {
            const QList<StickyNote *> after{a, b, c};
            QStringList stillCollapsed;
            for (StickyNote *probe : std::as_const(after)) {
                QObject *root = notes->windowRootForId(probe ? probe->id() : QString());
                if (!root || root->property("collapsed").toBool())
                    stillCollapsed << (probe ? probe->id().left(4) : QStringLiteral("?"));
            }
            noteCheck(stillCollapsed.isEmpty(),
                      QStringLiteral("解散：三块都画回完整卡片（没有谁停在「收起来」状态）"),
                      stillCollapsed.join(QStringLiteral(",")));
        }
        noteCheck(notes->groupState(a->id()).value(QStringLiteral("count")).toInt() == 0,
                  QStringLiteral("解散：这一摞没了（组合状态报空）"));

        /*
         * ---- 「与…组合」那一栏整个删掉了（用户："这个选项不要了"）----
         *
         * 飞出面板从此只剩「透明度」一种：原来这一段还钉着"面板露着的时候换
         * 一栏（组合 -> 透明度 -> 组合）"，那条路在界面上再也走不到了（只有一
         * 种面板，无从切换），跟着删。这里改钉两件还在的事：
         *   1) 「与…组合」的面板**打不开**了（菜单里没有挂着这个 kind 的条目）；
         *   2) 透明度那块面板露着的时候不许被改几何（同一条 DWM 重放规矩）。
         * 归到一摞改走菜单里新那条「全部叠成一摞」，也从 fire() 这条路点一次。
         */
        if (QObject *root = notes->windowRootForId(a->id())) {
            QMetaObject::invokeMethod(root, "openNoteMenu",
                                      Q_ARG(QVariant, QVariant(1400)),
                                      Q_ARG(QVariant, QVariant(700)));
            settle();
            noteCheck(!notes->openMenuFlyout(a->id(), QStringLiteral("group")),
                      QStringLiteral("菜单：「与…组合」那一栏没有了（面板打不开）"));

            const int shifts0
                = notes->menuState(a->id()).value(QStringLiteral("flyoutShifts")).toInt();
            noteCheck(notes->openMenuFlyout(a->id(), QStringLiteral("opacity")),
                      QStringLiteral("菜单：「透明度」那一栏照常打得开"));
            settle();
            const QVariantMap flyoutState = notes->menuState(a->id());
            noteCheck(flyoutState.value(QStringLiteral("flyout")).toString()
                          == QLatin1String("opacity")
                          && flyoutState.value(QStringLiteral("entryCount")).toInt() > 0,
                      QStringLiteral("菜单：透明度面板真的摆出了条目"),
                      QStringLiteral("flyout=%1").arg(
                          flyoutState.value(QStringLiteral("flyout")).toString()));
            noteCheck(flyoutState.value(QStringLiteral("flyoutShifts")).toInt() == shifts0,
                      QStringLiteral("菜单：面板窗口露着的时候从来没被改过几何"),
                      QStringLiteral("露着改了几何 %1 次（要 %2）")
                          .arg(flyoutState.value(QStringLiteral("flyoutShifts")).toInt())
                          .arg(shifts0));

            QObject *menu = root->findChild<QObject *>(QStringLiteral("noteMenu"));
            if (menu) {
                /* 和用户在菜单上点同一条：fire("stack") -> Notes.stackAll(这一块) */
                QMetaObject::invokeMethod(menu, "fire", Q_ARG(QVariant, QVariant("stack")));
                settle();
            }
            noteCheck(menu && a && !a->groupId().isEmpty(),
                      QStringLiteral("菜单：点「全部叠成一摞」就真的凑成了一摞"),
                      QStringLiteral("组=%1").arg(a ? a->groupId() : QString()));
            settle();
        }
        const QStringList menuOrder =
            notes->groupState(a->id()).value(QStringLiteral("members")).toStringList();
        noteCheck(menuOrder.size() >= 2 && a && !menuOrder.isEmpty()
                      && menuOrder.first() == a->id(),
                  QStringLiteral("菜单：成组那块在最上面（菜单是挂在它身上打开的）"),
                  QStringLiteral("%1 块").arg(menuOrder.size()));

        /* =================================================================
         * 组合的**鼠标那条路**：拖一块便签的头部到另一块身上松手
         * ---------------------------------------------------------------
         * 界面上真跑起来是：按下头部（beginDrag）-> beginNoteDrag，拖动期间
         * dragPollTimer 每拍算一次落点（谁的头被压着谁就亮）-> 松手时
         * finishNoteDrag -> dropNoteOn。
         *
         * 自检没法真按住鼠标把窗口拖过去（光标是模拟的，被拖的窗口不会跟着
         * 走）—— 所以"拖过去"这一步用**真的把窗口挪过去**代替（QWidget::move，
         * 就是用户在拖它），光标也摆到落点那一块的头部上；之后走的全是真实
         * 链路：落点判定、亮灯、松手、组合。
         * =============================================================== */
        {
            StickyNote *d = notes->createNote();
            StickyNote *e = notes->createNote();
            settle();
            placeNote(d, QRect(300, 800, 330, 300));
            placeNote(e, QRect(900, 800, 330, 300));
            settle();

            const QRect eRect = noteRect(e);
            const QPoint eHead(eRect.x() + eRect.width() / 2, eRect.y() + 10);
            const QPoint eBody(eRect.x() + eRect.width() / 2, eRect.y() + eRect.height() - 20);

            bool targetOk = false;
            bool bodyNotTarget = false;
            bool geoSeen = false;     /* 落点那三个数留一份，失败详情里要打 */
            bool markSeen = false;
            QPoint dropAt;
            if (auto *dragWin = qobject_cast<QWidget *>(notes->windowForId(d->id()))) {
                /*
                 * 窗口左边还挂着一条标签条（见 frameRectFor）：QWidget::move 走
                 * 的是**窗口**坐标，而"落在哪儿"要按**卡片**算 —— 这里先量一次
                 * 两者的差，后面每次搬窗口都把它补上。
                 */
                const QPoint frameOffset(dragWin->geometry().left() - noteRect(d).left(), 0);
                /*
                 * 先把它挪到"没压着 e 的头部"的地方，再把光标钉在 e 的头部上，
                 * 然后按下头部开始拖。
                 *
                 * 为什么先挪：真拖的时候窗口是**跟着光标走**的（光标压在 e 的
                 * 头部上时，被拖的 d 已经在 e 旁边，不会盖住那一点）。自检里
                 * 光标是直接摆过去的、窗口不会跟，得手工把这件事补上 —— 不然
                 * 光标那一点落在**自己身上**，落点判定当然找不到目标。
                 */
                dragWin->move(eRect.right() + 40, eRect.bottom() + 40);
                settle();
                /*
                 * 松手的位置：自检里窗口不跟光标走，所以这里手工选一个落点
                 * ——"拖到哪儿"量的是**卡片**（用户看的是那张纸）。
                 *
                 * 搬的是**窗口**坐标：卡片要落在 droppedCard，窗口就在再往左
                 * 一条标签条的地方（见 frameRectFor）。
                 */
                const QRect droppedCard(noteRect(d).topLeft() + QPoint(-60, 10) + frameOffset,
                                        noteRect(d).size());
                QCursor::setPos(eHead);
                settle();

                QMetaObject::invokeMethod(dragWin, "beginDrag");
                dragWin->move(droppedCard.topLeft() - frameOffset);
                settle();
                /* 松手时被拖那块在哪（只用来打诊断信息，见下面那条断言） */
                dropAt = noteRect(d).topLeft();
                /*
                 * ---- 落点预览（"底下那一块亮起来"）----
                 *
                 * 界面上这一拍由 dragPollTimer（60ms）算，这里**直接调那一拍**
                 * （updateDropTarget 就是定时器回调里的第一句，不是另开一条路）。
                 *
                 * 关键是这中间**一次 settle() 都不能有**：那个定时器还负责判定
                 * "用户松手了"（光标连续两拍没动 + 左键没按着 -> finishNoteDrag，
                 * 见 StickyNotes 的构造函数）。自检没真按鼠标、光标又是钉着的，
                 * 一跑事件循环它就自己把这次拖动收掉、把 m_dragWindow 清掉 ——
                 * 再来读落点预览永远是 0。这条一直偶发红就是这个（三次运行的
                 * "几何判定 / 界面标记"三个数每次都不一样，也是它）。
                 *
                 * 所以：光标钉稳 -> 调那一拍 -> 立刻读三个数，中间不给事件循环
                 * 机会。光标钉几次是因为极少数情况下一次 setPos 没落到位。
                 */
                for (int attempt = 0; attempt < 5 && QCursor::pos() != eHead; ++attempt)
                    QCursor::setPos(eHead);
                QMetaObject::invokeMethod(notes, "updateDropTarget");
                geoSeen = notes->dropPreviewAt(e, eHead);
                markSeen = notes->windowState(e->id())
                               .value(QStringLiteral("dropPreview")).toBool();
                const bool bodySays = notes->dropPreviewAt(e, eBody);
                targetOk = geoSeen && markSeen;
                bodyNotTarget = !bodySays;
                noteOut(QStringLiteral("（落点：几何判定 %1 / 界面标记 %2 / 身子那条 %3）")
                            .arg(geoSeen ? 1 : 0).arg(markSeen ? 1 : 0).arg(bodySays ? 1 : 0));
                /* 松手：finishNoteDrag 是**总管**上的（见 StickyNotes.h） */
                QMetaObject::invokeMethod(notes, "finishNoteDrag");
                settle();
            }
            noteCheck(targetOk,
                      QStringLiteral("组合（鼠标）：拖到另一块头部上时，那一块亮起「可以放这儿」"),
                      QStringLiteral("几何判定 %1 / 界面标记 %2")
                          .arg(geoSeen ? 1 : 0).arg(markSeen ? 1 : 0));
            noteCheck(bodyNotTarget,
                      QStringLiteral("组合（鼠标）：压在身子上不算落点（只有头部那条算）"));
            noteCheck(d->groupId() == e->groupId() && !d->groupId().isEmpty(),
                      QStringLiteral("组合（鼠标）：松手就把两块组合成一摞"),
                      QStringLiteral("d=%1 e=%2").arg(d->groupId(), e->groupId()));
            noteCheck(notes->groupState(d->id()).value(QStringLiteral("count")).toInt() == 2,
                      QStringLiteral("组合（鼠标）：这一摞里正好两块"));
            /*
             * 放下之后这一摞归谁：**落点那一块留在原地露着**，被拖的那块并进来
             * 收起来（见 dropNoteOn 里"为什么不是被拖的那块排第一"那段 —— 反过来
             * 用户看到的是"原来那几张不见了"）。早先这两条断言写的是旧设计
             * （被拖的那块露头、留在松手的位置），所以拖动一旦成功它们必红。
             */
            const QStringList dragOrder =
                notes->groupState(d->id()).value(QStringLiteral("members")).toStringList();
            noteCheck(!dragOrder.isEmpty() && dragOrder.first() == e->id(),
                      QStringLiteral("组合（鼠标）：落点那一块在最上面（它留在原地露着）"),
                      QStringLiteral("露头 %1 / 落点 %2")
                          .arg(dragOrder.isEmpty() ? QStringLiteral("?") : dragOrder.first().left(4),
                               e->id().left(4)));
            noteCheck(noteRect(e).topLeft() == eRect.topLeft()
                          && noteRect(d).topLeft() == eRect.topLeft()
                          && noteRect(d).size() == noteRect(e).size(),
                      QStringLiteral("组合（鼠标）：整摞落在落点那一格（被拖的那块对齐过去）"),
                      QStringLiteral("e %1,%2 / d %3,%4 / 期望 %5,%6（松手时 d 在 %7,%8）")
                          .arg(noteRect(e).x()).arg(noteRect(e).y())
                          .arg(noteRect(d).x()).arg(noteRect(d).y())
                          .arg(eRect.x()).arg(eRect.y())
                          .arg(dropAt.x()).arg(dropAt.y()));
            noteCheck(notes->windowState(e->id()).value(QStringLiteral("visible")).toBool()
                          && !notes->windowState(d->id()).value(QStringLiteral("visible")).toBool(),
                      QStringLiteral("组合（鼠标）：落点那块露着、被拖的那块收成色块"));

            /* 顺手量一下"拖到空处不会乱组"：光标落在空桌面上 */
            const QString dGroup = d->groupId();
            StickyNote *f = notes->createNote();
            settle();
            placeNote(f, QRect(1500, 800, 330, 300));
            settle();
            QCursor::setPos(1800, 1900);   /* 空桌面 */
            settle();
            bool alone = false;
            if (QObject *windowObj = notes->windowForId(f->id())) {
                QMetaObject::invokeMethod(windowObj, "beginDrag");
                settle();
                /*
                 * 空桌面：这一块（f）身上不该有落点 —— 光标 1800,1900 离那些
                 * 便签都远着。注意 d 和 e 这会儿已经在同一摞里了，**同一个组
                 * 的不算落点**，所以拿它们来量必须选另一摞的目标。
                 */
                alone = notes->dropPreviewAt(f, QPoint(1800, 1900)) == false;
            }
            QMetaObject::invokeMethod(notes, "finishNoteDrag");
            settle();
            noteCheck(alone && f->groupId().isEmpty() && d->groupId() == dGroup,
                      QStringLiteral("组合（鼠标）：拖到空处就只是挪个位置，不会乱组"));

            /* 收尾：光标挪开，别影响后面几节 */
            QCursor::setPos(1800, 1900);
            notes->deleteNote(d);
            notes->deleteNote(e);
            notes->deleteNote(f);
            settle();
        }

        /* =================================================================
         * 左边那条标签条：**只有组合的便签才有**，一摞只看一张纸
         * ---------------------------------------------------------------
         * 用户明确要求："这个左边的 tab 不是每个卡片都有，只有组合的才有，
         * 而且组合的卡片只显示一张卡片。这一张卡片切换。"
         *
         * 所以这一节量四件事：
         *   1) 没组合过的便签**没有**标签条（左边干干净净、窗口 = 卡片宽）；
         *   2) 组合之后才长出来，色块 = 摞里的每一块（颜色取各自的底色）；
         *   3) 摞里只摆**一张纸**（其余几块窗口收起来，位置对齐到同一格）；
         *   4) 点一个色块 = 换那一张纸上来，色块先后一动不动。
         * =============================================================== */
        {
            StickyNote *t1 = notes->createNote();
            StickyNote *t2 = notes->createNote();
            StickyNote *t3 = notes->createNote();
            settle();
            placeNote(t1, QRect(300, 300, 330, 300));
            placeNote(t2, QRect(700, 300, 330, 300));
            placeNote(t3, QRect(1100, 300, 330, 300));
            settle();

            /* 各自给一个不一样的底色，标签条上那排色块才分得出来 */
            t1->setColor(QColor(QStringLiteral("#a8e6a1")));
            t2->setColor(QColor(QStringLiteral("#f7b6d2")));
            t3->setColor(QColor(QStringLiteral("#c9b6f7")));
            settle();

            QObject *root2 = notes->windowRootForId(t2->id());
            auto *t2Window = qobject_cast<StickyNoteWindow *>(notes->windowForId(t2->id()));

            /* ---- 1) 没组合过：没有标签条 ---- */
            noteCheck(root2 && t1->groupId().isEmpty() && t2->groupId().isEmpty()
                          && t3->groupId().isEmpty()
                          && root2->property("tabCount").toInt() == 0
                          && root2->property("hasTabStrip").toBool() == false,
                      QStringLiteral("标签条：没组合过的便签没有标签条（左边干干净净）"),
                      QStringLiteral("tabCount=%1")
                          .arg(root2 ? root2->property("tabCount").toInt() : -1));
            if (t2Window) {
                noteCheck(t2Window->width() == noteRect(t2).width(),
                          QStringLiteral("标签条：没标签条时窗口就是那张纸（不多一条宽）"),
                          QStringLiteral("窗口 %1 / 卡片 %2")
                              .arg(t2Window->width()).arg(noteRect(t2).width()));
            } else {
                noteCheck(false, QStringLiteral("标签条：拿得到便签窗口对象（量窗口宽度）"));
            }

            /* ---- 2) 组合之后：标签条长出来，色块 = 摞里的三块 ---- */
            QList<StickyNote *> trio{t2, t3};
            noteCheck(notes->groupWith(t1, trio), QStringLiteral("标签条：三块先归成一摞"));
            settle();

            QObject *root1 = notes->windowRootForId(t1->id());
            noteCheck(root1 && root1->property("tabCount").toInt() == 3,
                      QStringLiteral("标签条：组合之后露头那块画出 3 个色块"),
                      QStringLiteral("tabCount=%1")
                          .arg(root1 ? root1->property("tabCount").toInt() : -1));
            const QVariantList tabs = root1 ? root1->property("groupTabs").toList() : QVariantList();
            QStringList tabColors;
            bool selectedIsT1 = false;
            for (const QVariant &entry : tabs) {
                const QVariantMap tab = entry.toMap();
                tabColors << tab.value(QStringLiteral("color")).toString();
                if (tab.value(QStringLiteral("id")).toString() == t1->id()
                    && tab.value(QStringLiteral("selected")).toBool()) {
                    selectedIsT1 = true;
                }
            }
            bool colorsOk = tabs.size() == 3 && selectedIsT1
                            && tabColors.contains(QStringLiteral("#a8e6a1"))
                            && tabColors.contains(QStringLiteral("#f7b6d2"))
                            && tabColors.contains(QStringLiteral("#c9b6f7"));
            noteCheck(colorsOk,
                      QStringLiteral("标签条：每个色块用的是那一块便签自己的底色（选中圈在露头那块上）"),
                      tabColors.join(QStringLiteral(" ")));

            /*
             * ---- 3) 组合之后只摆一张纸 ----
             *
             * 露头那块（t1）是完整的纸；t2/t3 的窗口收起来、位置对齐到同一格
             * —— 用户在桌面上看到的只有一张卡片 + 左边那排标签。
             */
            const QRect stackCard = noteRect(t1);
            noteCheck(noteRect(t2).topLeft() == stackCard.topLeft()
                          && noteRect(t3).topLeft() == stackCard.topLeft(),
                      QStringLiteral("组合：一摞里其余几块都对齐到同一格（只显示一张卡片）"),
                      QStringLiteral("t1 %1,%2 / t2 %3,%4 / t3 %5,%6")
                          .arg(stackCard.x()).arg(stackCard.y())
                          .arg(noteRect(t2).x()).arg(noteRect(t2).y())
                          .arg(noteRect(t3).x()).arg(noteRect(t3).y()));
            noteCheck(notes->windowState(t1->id()).value(QStringLiteral("visible")).toBool()
                          && !notes->windowState(t2->id()).value(QStringLiteral("visible")).toBool()
                          && !notes->windowState(t3->id()).value(QStringLiteral("visible")).toBool(),
                      QStringLiteral("组合：桌面上只摆露头那一张（其余几块收成标签）"));
            noteCheck(root1 && root1->property("collapsed").toBool() == false
                          && root1->property("chipStripDrawn").toBool() == true,
                      QStringLiteral("标签条：露头那块自己画那排色块（内容照常显示）"));
            QObject *rootCollapsed = notes->windowRootForId(t2->id());
            noteCheck(rootCollapsed && rootCollapsed->property("collapsed").toBool() == true
                          && rootCollapsed->property("chipStripDrawn").toBool() == false,
                      QStringLiteral("标签条：收起来的那几块自己不再画一排色块（不叠成一串）"));

            /* ---- 4) 点一个色块：换那一张纸上来，色块先后不动 ---- */
            QStringList tabIdsBefore;
            for (const QVariant &entry : tabs)
                tabIdsBefore << entry.toMap().value(QStringLiteral("id")).toString();

            const QRect stackBefore = noteRect(t1);
            const bool switched = notes->switchGroupTab(t2->id());
            settle();
            noteCheck(switched && noteRect(t2) == stackBefore,
                      QStringLiteral("标签条：换标签是原地换纸（整摞不挪窝）"),
                      QStringLiteral("%1,%2 / 期望 %3,%4")
                          .arg(noteRect(t2).x()).arg(noteRect(t2).y())
                          .arg(stackBefore.x()).arg(stackBefore.y()));
            noteCheck(notes->groupState(t2->id()).value(QStringLiteral("activeId")).toString()
                          == t2->id()
                          && notes->windowState(t2->id()).value(QStringLiteral("visible")).toBool()
                          && !notes->windowState(t1->id()).value(QStringLiteral("visible")).toBool()
                          && !notes->windowState(t3->id()).value(QStringLiteral("visible")).toBool(),
                      QStringLiteral("标签条：点第二个色块，那一块换上来、原来那张收回去"));
            QObject *rootAfter = notes->windowRootForId(t2->id());
            const QVariantList tabsAfter = rootAfter ? rootAfter->property("groupTabs").toList()
                                                     : QVariantList();
            QStringList tabIdsAfter;
            bool selectedIsT2 = false;
            for (const QVariant &entry : tabsAfter) {
                const QVariantMap tab = entry.toMap();
                tabIdsAfter << tab.value(QStringLiteral("id")).toString();
                if (tab.value(QStringLiteral("id")).toString() == t2->id()
                    && tab.value(QStringLiteral("selected")).toBool()) {
                    selectedIsT2 = true;
                }
            }
            noteCheck(tabIdsAfter == tabIdsBefore && selectedIsT2,
                      QStringLiteral("标签条：换纸只挪选中圈，这排色块的先后一动不动"),
                      tabIdsAfter.join(QStringLiteral(",")));

            /* 窗口 = 卡片 + 左边那条标签条（纸一点没挪，多出来那条全在窗口左边） */
            if (t2Window) {
                noteCheck(t2Window->width() - noteRect(t2).width()
                              == StickyNoteWindow::tabStripWidth(),
                          QStringLiteral("标签条：窗口 = 卡片 + 左边那一条（卡片位置尺寸都没变）"),
                          QStringLiteral("窗口 %1 / 卡片 %2 / 标签条 %3")
                              .arg(t2Window->width()).arg(noteRect(t2).width())
                              .arg(StickyNoteWindow::tabStripWidth()));
            }
            /*
             * "色块在卡片**外面**"这件事本身（用户要的观感）：
             *   * 便签纸（paper）从色块右边才开始 —— 色块整块落在纸外面；
             *   * 窗口根那一层是**透的**（纸色只画在 paper 上），所以色块底下
             *     露出来的是桌面，不是一块纸色。
             *
             * 只量 tabStripWidth 是不够的：那是"打算让出多宽"，纸要是仍然刷满
             * 整个窗口，色块看着就是画在卡片里（用户截图报的正是这个）。
             */
            const qreal chipRight = rootAfter ? rootAfter->property("chipRight").toReal() : -1;
            const qreal paperLeft = rootAfter ? rootAfter->property("paperLeft").toReal() : -1;
            noteCheck(rootAfter && chipRight > 0 && chipRight < paperLeft,
                      QStringLiteral("标签条：色块整块落在便签纸外面（切换 tab 在卡片外边）"),
                      QStringLiteral("chipRight=%1 paperLeft=%2")
                          .arg(chipRight).arg(paperLeft));
            const QColor rootFill = rootAfter ? rootAfter->property("color").value<QColor>()
                                              : QColor();
            noteCheck(rootAfter && rootFill.alpha() == 0,
                      QStringLiteral("标签条：窗口根那一层是透的（纸色只画在纸那一层）"),
                      QStringLiteral("rootColor=%1")
                          .arg(rootFill.name(QColor::HexArgb)));

            /*
             * ---- 收起露头那一块：换一张纸露着，而且**数据也得跟着对** ----
             *
             * show() 只改窗口、不改数据。repairGroup 那条"这一摞至少留一块露着"
             * 的兜底把同组一块叫出来时，要是不把它置回 visible，界面上摆着一张
             * 纸、清单里却写着 false：下次启动它就不见了，拆分组合 / 换标签这些
             * 按 note->visible() 挑人的地方也全会认错（用户那份清单里六条全是
             * visible=false、桌面上却还摆着卡片，就是这么来的）。
             */
            {
                const QString frontBefore = notes->groupState(t1->id())
                                                .value(QStringLiteral("activeId")).toString();
                StickyNote *frontNote = nullptr;
                const QList<StickyNote *> everyone{t1, t2, t3};
                for (StickyNote *probe : std::as_const(everyone)) {
                    if (probe && probe->id() == frontBefore)
                        frontNote = probe;
                }
                noteCheck(frontNote && menuAct(frontNote, QStringLiteral("hide")),
                          QStringLiteral("收起露头那块：菜单那条路收得掉"));
                settle();
                const QString shownAfter = notes->groupState(t1->id())
                                               .value(QStringLiteral("activeId")).toString();
                StickyNote *shownNote = nullptr;
                for (StickyNote *probe : std::as_const(everyone)) {
                    if (probe && probe->id() == shownAfter)
                        shownNote = probe;
                }
                QObject *shownRoot2 = notes->windowRootForId(shownAfter);
                noteCheck(!shownAfter.isEmpty() && shownAfter != frontBefore && shownRoot2
                              && shownRoot2->property("collapsed").toBool() == false,
                          QStringLiteral("收起露头那块：换一张纸露着（不是一张空白卡片）"),
                          QStringLiteral("露头 %1").arg(shownAfter.left(4)));
                noteCheck(shownNote && shownNote->property("visible").toBool()
                              && notes->groupState(t1->id()).value(QStringLiteral("count")).toInt() == 3,
                          QStringLiteral("收起露头那块：换上来那块的数据也置回「摆着」了"),
                          QStringLiteral("visible=%1")
                              .arg(shownNote && shownNote->property("visible").toBool() ? 1 : 0));
            }

            /*
             * ---- 收起全部 / 显示全部：一摞还是"一张纸 + 左边那排色块" ----
             *
             * "显示全部便签"不能把同组那几块都摆出来：它们叠在同一格上，而
             * **后 show 出来的那块压在最上面**，它又是"收起来的那张纸"
             * （content 不画）—— 屏幕上就是一张空白卡片（用户报的就是这个）。
             */
            notes->hideAll();
            settle();
            noteCheck(notes->visibleCount() == 0,
                      QStringLiteral("显示全部：先收起全部，桌面上干净了"),
                      QStringLiteral("%1 块摆着").arg(notes->visibleCount()));
            notes->showAll();
            settle();
            /*
             * 一摞只摆一张纸，而且摆的得是**露头那张**（这一节前面点过标签，
             * 露头的已经是 t2 了 —— 所以这里按 activeId 量，不写死是哪一块）。
             */
            {
                const QString activeId = notes->groupState(t1->id())
                                             .value(QStringLiteral("activeId")).toString();
                const QList<StickyNote *> trioProbe{t1, t2, t3};
                int shownCount = 0;
                bool onlyFrontShown = true;
                for (StickyNote *probe : std::as_const(trioProbe)) {
                    const bool shown = probe && windowStateOf(probe, QStringLiteral("visible")).toBool();
                    if (shown)
                        ++shownCount;
                    if (probe && shown != (probe->id() == activeId))
                        onlyFrontShown = false;
                }
                noteCheck(shownCount == 1 && onlyFrontShown,
                          QStringLiteral("显示全部：一摞还是只摆露头那一张（其余仍收成色块）"),
                          QStringLiteral("摆着 %1 块 / 露头 %2")
                              .arg(shownCount).arg(activeId.left(4)));
            }

            /*
             * 收起其他便签：别的便签收掉、别的**摞**也得整个收掉 —— 只收露头
             * 那一块的话，兜底会把同组那张收起来的纸拉出来，一摞照样占着桌面。
             */
            auto *t1Window = qobject_cast<StickyNoteWindow *>(notes->windowForId(t1->id()));
            noteCheck(t1Window && notes->hideOthers(t1Window),
                      QStringLiteral("收起其他：点的那一块留着，其余都收掉"));
            settle();
            noteCheck(windowStateOf(t1, QStringLiteral("visible")).toBool()
                          && notes->visibleCount() == 1,
                      QStringLiteral("收起其他：桌面上只剩点的那一块"),
                      QStringLiteral("%1 块摆着").arg(notes->visibleCount()));
            /* 叫回来，后面几节还要用这几块 */
            notes->showAll();
            settle();

            /* ---- 拆开：标签条收掉，收起来的那几块都放出来 ---- */
            /*
             * 走**菜单对象上那条真路**（NoteMenu.fire）：用户在菜单里点「拆分
             * 组合」落到的是它，和便签根上的 handleMenuAct 不是同一个入口。
             *
             * 这里踩过一个大坑：fire 里把参数写成了 C++ 窗口的 noteData（那个
             * 属性根本不存在），C++ 收到空指针直接 return false —— 菜单里点
             * 「拆分组合」从来没有任何反应，还不报错。所以这一条必须从 fire
             * 进去，只调 notes->ungroup 是量不到它的。
             */
            {
                QObject *menuRoot = notes->windowRootForId(t2->id());
                QObject *menuObj = menuRoot
                                       ? menuRoot->findChild<QObject *>(QStringLiteral("noteMenu"))
                                       : nullptr;
                noteCheck(menuObj != nullptr,
                          QStringLiteral("菜单：按名字找得到便签菜单对象（NoteMenu）"));
                if (menuRoot && menuObj) {
                    QMetaObject::invokeMethod(menuRoot, "openNoteMenu",
                                              Q_ARG(QVariant, QVariant(1400)),
                                              Q_ARG(QVariant, QVariant(700)));
                    settle();
                    QVariant fired;
                    QMetaObject::invokeMethod(menuObj, "fire", Q_RETURN_ARG(QVariant, fired),
                                              Q_ARG(QVariant, QVariant(QStringLiteral("ungroup"))));
                    settle();
                }
                noteCheck(t1->groupId().isEmpty() && t2->groupId().isEmpty()
                              && t3->groupId().isEmpty(),
                          QStringLiteral("菜单：真点一次「拆分组合」把这一摞拆了（fire 那条路）"),
                          QStringLiteral("%1/%2/%3")
                              .arg(t1->groupId().left(4), t2->groupId().left(4),
                                   t3->groupId().left(4)));
            }
            QObject *rootUngrouped = notes->windowRootForId(t2->id());
            noteCheck(rootUngrouped && rootUngrouped->property("hasTabStrip").toBool() == false
                          && rootUngrouped->property("tabCount").toInt() == 0,
                      QStringLiteral("标签条：拆开之后标签条收掉（又变成普通便签）"));
            noteCheck(notes->windowState(t1->id()).value(QStringLiteral("visible")).toBool()
                          && notes->windowState(t3->id()).value(QStringLiteral("visible")).toBool(),
                      QStringLiteral("标签条：拆开之后收起来的那几块都放出来了"));
            /*
             * 拆完每一块都得画回**完整卡片**（collapsed 要是停在"收起来的那张
             * 纸"那个状态，屏幕上就是一张空白卡片 —— 用户报的就是这个），而且
             * 得摊开、不许还叠在同一格上。
             */
            {
                const QList<StickyNote *> apart{t1, t2, t3};
                QStringList stillCollapsed;
                for (StickyNote *probe : std::as_const(apart)) {
                    QObject *root = notes->windowRootForId(probe ? probe->id() : QString());
                    if (!root || root->property("collapsed").toBool())
                        stillCollapsed << (probe ? probe->id().left(4) : QStringLiteral("?"));
                }
                noteCheck(stillCollapsed.isEmpty(),
                          QStringLiteral("标签条：拆开之后每一块都画回完整卡片（没有空白卡片）"),
                          stillCollapsed.join(QStringLiteral(",")));
                noteCheck(noteRect(t1) != noteRect(t2) && noteRect(t2) != noteRect(t3)
                              && noteRect(t1) != noteRect(t3),
                          QStringLiteral("标签条：拆开之后三块摊开（不再叠在同一格上）"),
                          QStringLiteral("t1 %1,%2 / t2 %3,%4 / t3 %5,%6")
                              .arg(noteRect(t1).x()).arg(noteRect(t1).y())
                              .arg(noteRect(t2).x()).arg(noteRect(t2).y())
                              .arg(noteRect(t3).x()).arg(noteRect(t3).y()));
            }

            notes->deleteNote(t1);
            notes->deleteNote(t2);
            notes->deleteNote(t3);
            settle();
        }

        /* =================================================================
         * 组合**一块一块地加**（组合四个）—— 标签条得跟着长到四个
         * ---------------------------------------------------------------
         * 用户报的 bug：四块便签一次次凑成一摞之后，左边只有 3 个色块，再点
         * 一下色块又少一个（"组合四个后只能显示三个 tab，再点一下就变成第三
         * 个了"）。
         *
         * 为什么会这样（这一节就是那个 bug 的回归钉子）：一摞里**收起来的
         * 那几块**（已经变成色块的那几张）在重排时不算"露着的"，而组的成员
         * 清单原来是按"露着的几块"写回去的（见 StickyNotes::applyGroupLayout）
         * —— 每加一块就把上一块从清单里挤掉一个：色块少一个还算轻的，重的那
         * 半是下一次"合进来"的名单也从这份清单里取（groupWith / dropNoteOn
         * 都走 groupMembers），被挤掉的那块从此**既露不出来也点不到**，组
         * id 却还留在它身上（重启之后它变成桌上一块单独的便签，用户看到的就是
         * "四块里少了一块"）。
         *
         * 所以这里逐块加、每加一块量一次"这一摞还是那几块"，最后再换一遍
         * 标签 —— 换标签会重新读一遍标签条，短板最容易在那儿露出来。
         * =============================================================== */
        {
            StickyNote *q1 = notes->createNote();
            StickyNote *q2 = notes->createNote();
            StickyNote *q3 = notes->createNote();
            StickyNote *q4 = notes->createNote();
            settle();
            placeNote(q1, QRect(300, 300, 330, 300));
            placeNote(q2, QRect(640, 300, 330, 300));
            placeNote(q3, QRect(980, 300, 330, 300));
            placeNote(q4, QRect(1320, 300, 330, 300));
            settle();

            /* 四块不一样的底色：色块少一个、或者色块张冠李戴都看得出来 */
            q1->setColor(QColor(QStringLiteral("#ffe9a8")));
            q2->setColor(QColor(QStringLiteral("#f7b6d2")));
            q3->setColor(QColor(QStringLiteral("#a8e6a1")));
            q4->setColor(QColor(QStringLiteral("#c9b6f7")));
            settle();

            /* 标签条上那排色块的 id（顺序也算：点第几个换哪张纸全看它） */
            auto tabIdsOf = [notes](StickyNote *note) {
                QStringList out;
                QObject *root = notes->windowRootForId(note ? note->id() : QString());
                const QVariantList tabs = root ? root->property("groupTabs").toList()
                                               : QVariantList();
                for (const QVariant &entry : tabs)
                    out << entry.toMap().value(QStringLiteral("id")).toString();
                return out;
            };
            auto tabCountOf = [notes](StickyNote *note) {
                QObject *root = notes->windowRootForId(note ? note->id() : QString());
                return root ? root->property("tabCount").toInt() : -1;
            };

            /* 一次加一块：和用户拖一块到那一摞身上是同一条路，只是名单短 */
            QList<StickyNote *> one;
            one << q2;
            noteCheck(notes->groupWith(q1, one),
                      QStringLiteral("组合四个：第 2 块加进来了"));
            settle();
            one.clear();
            one << q3;
            noteCheck(notes->groupWith(q1, one),
                      QStringLiteral("组合四个：第 3 块加进来了"));
            settle();
            /*
             * 第 4 块走**鼠标那条路**：拖它的头部丢到 q1 身上（dropNoteOn）。
             * 用户组合就是一块一块丢上去的，而这条路自己算一份名单（不是
             * groupWith 那份），漏人也是从这儿漏 —— 所以两条路都得量。
             */
            noteCheck(notes->dropNoteOn(q4, q1),
                      QStringLiteral("组合四个：第 4 块拖到那一摞身上加进来了"));
            settle();

            /*
             * 四块都得在这一摞里：组 id 贴到每一条身上（被挤掉的那块会留着上
             * 一摞的组 id）、组里报得出来 4 块、标签条画 4 个色块。
             */
            const QString groupId = q1->groupId();
            noteCheck(!groupId.isEmpty() && q2->groupId() == groupId && q3->groupId() == groupId
                          && q4->groupId() == groupId,
                      QStringLiteral("组合四个：四块的组 id 是同一个（没有谁被挤出去）"),
                      QStringLiteral("%1/%2/%3/%4")
                          .arg(q1->groupId().left(4), q2->groupId().left(4),
                               q3->groupId().left(4), q4->groupId().left(4)));
            noteCheck(notes->groupState(q1->id()).value(QStringLiteral("count")).toInt() == 4,
                      QStringLiteral("组合四个：这一摞里报得出来 4 块"),
                      QStringLiteral("%1 块")
                          .arg(notes->groupState(q1->id()).value(QStringLiteral("count")).toInt()));
            noteCheck(tabCountOf(q1) == 4,
                      QStringLiteral("组合四个：露头那块画出 4 个色块"),
                      QStringLiteral("tabCount=%1").arg(tabCountOf(q1)));

            /* 换一次标签：这一步会重新读一遍标签条，短板就在这儿露出来 */
            noteCheck(notes->switchGroupTab(q2->id()),
                      QStringLiteral("组合四个：能换到第二块去"));
            settle();
            noteCheck(tabCountOf(q2) == 4
                          && notes->groupState(q2->id()).value(QStringLiteral("count")).toInt() == 4,
                      QStringLiteral("组合四个：换过标签之后这排还是 4 个色块"),
                      QStringLiteral("tabCount=%1 / 组里 %2 块")
                          .arg(tabCountOf(q2))
                          .arg(notes->groupState(q2->id())
                                   .value(QStringLiteral("count"))
                                   .toInt()));
            const QStringList idsNow = tabIdsOf(q2);
            noteCheck(idsNow.size() == 4 && idsNow.contains(q1->id()) && idsNow.contains(q2->id())
                          && idsNow.contains(q3->id()) && idsNow.contains(q4->id()),
                      QStringLiteral("组合四个：这排色块正好是那四块（一个不少、也不张冠李戴）"),
                      idsNow.join(QStringLiteral(",")));

            /* 四个色块每一个都换得上纸：谁被挤出去，谁就在这儿点不动 */
            const QList<StickyNote *> four{q1, q2, q3, q4};
            QStringList notSwitched;
            for (StickyNote *probe : four) {
                if (!probe)
                    continue;
                if (notes->groupState(probe->id()).value(QStringLiteral("active")).toBool())
                    continue;   /* 已经露着的那块点它自己返回 false，是设计如此 */
                if (!notes->switchGroupTab(probe->id()))
                    notSwitched << probe->id().left(4);
                settle();
            }
            noteCheck(notSwitched.isEmpty(),
                      QStringLiteral("组合四个：四个色块每一个都换得上纸（没有点不动的）"),
                      notSwitched.join(QStringLiteral(",")));

            notes->deleteNote(q1);
            notes->deleteNote(q2);
            notes->deleteNote(q3);
            notes->deleteNote(q4);
            settle();
        }

        /* =================================================================
         * 一键「全部叠成一摞」：不用拖，摆着的每一块并成一摞，整摞吸附到
         * 工作区右上角；选中的那个色块往外伸一截
         * ---------------------------------------------------------------
         * 用户给的参考图（partThreeGif.gif 第 22 帧）：一张纸 + 左边一列标签，
         * 选中的那块比其余的宽一截 —— 靠**位移差**表示"现在看的是这一张"。
         * 他要的是"所有的重叠在一起，不用拖动了，位置移动到右上角"，而
         * 「向左平铺排列」（arrangeAll）是另一件事，两条都留着。
         *
         * 这里量的四件事，一件都不能靠常量糊过去：
         *   1) 名单 = 摆着的每一块，**连着原来那一摞里藏着的成员**（s4）；
         *   2) 四块的卡片矩形一模一样（真重叠，不是错开）；
         *   3) 整摞贴着工作区右上角；
         *   4) 选中那块色块的左沿比其余的更靠左 —— 量的是 grab() 出来的
         *      像素，不是 QML 里那两个属性（属性对不上不代表画不出来，反过来也一样）。
         * =============================================================== */
        {
            StickyNote *s1 = notes->createNote();
            StickyNote *s2 = notes->createNote();
            StickyNote *s3 = notes->createNote();
            StickyNote *s4 = notes->createNote();
            settle();
            placeNote(s1, QRect(200, 200, 330, 300));
            placeNote(s2, QRect(600, 420, 330, 300));
            placeNote(s3, QRect(1000, 200, 330, 300));
            placeNote(s4, QRect(100, 900, 330, 300));
            settle();

            /* s3/s4 先自己成一摞：s4 就此收成色块藏起来，一键叠摞得把它一起并进来 */
            QList<StickyNote *> pair;
            pair << s4;
            noteCheck(notes->groupWith(s3, pair),
                      QStringLiteral("一键叠摞：s3/s4 先自己成一摞（s4 收成色块）"));
            settle();

            /*
             * 这一摞的名单是"**桌面上摆着的每一块**"，不止这四块 —— 前面几节
             * 留下的 a/b/c 那几块照样摆着，一起会被叠进来；反过来，单独收起
             * （不属于任何一摞）的那块不进。所以期望值要现算：
             * 摆着的 + 本来藏在某一摞里的（跟着自己那一摞并进来）。
             */
            int expected = 0;
            for (StickyNote *s : notes->store()->notes()) {
                if (s->visible() || !s->groupId().isEmpty())
                    ++expected;
            }
            noteCheck(notes->stackAll(s1),
                      QStringLiteral("一键叠摞：点一下就把摆着的每一块并成一摞（全程没拖）"));
            settle();

            const QString stackId = s1->groupId();
            noteCheck(!stackId.isEmpty() && s2->groupId() == stackId
                          && s3->groupId() == stackId && s4->groupId() == stackId,
                      QStringLiteral("一键叠摞：四块（含原来那摞里藏着的 s4）同一组 id"),
                      QStringLiteral("%1/%2/%3/%4")
                          .arg(s1->groupId(), s2->groupId(), s3->groupId(), s4->groupId()));
            noteCheck(notes->groupState(s1->id()).value(QStringLiteral("count")).toInt()
                          == expected,
                      QStringLiteral("一键叠摞：摞里的块数 = 该进来的每一块（摆着的 + 藏在一摞里的）"),
                      QStringLiteral("摞里 %1 块 / 该进 %2 块")
                          .arg(notes->groupState(s1->id())
                                   .value(QStringLiteral("count"))
                                   .toInt())
                          .arg(expected));
            QObject *stackRoot = notes->windowRootForId(s1->id());
            noteCheck(stackRoot && stackRoot->property("tabCount").toInt() == expected,
                      QStringLiteral("一键叠摞：露头那块每块画一个色块（一个不多一个不少）"),
                      QStringLiteral("tabCount=%1 / 该进 %2 块")
                          .arg(stackRoot ? stackRoot->property("tabCount").toInt() : -1)
                          .arg(expected));

            /* 只露一张纸：其余三块的窗口都收起来 */
            int stillShown = 0;
            for (StickyNote *s : {s1, s2, s3, s4}) {
                if (QObject *obj = notes->windowForId(s->id())) {
                    if (auto *w = qobject_cast<QWidget *>(obj); w && w->isVisible())
                        ++stillShown;
                }
            }
            noteCheck(stillShown == 1,
                      QStringLiteral("一键叠摞：桌面上只剩一张纸（其余收成色块）"),
                      QStringLiteral("还露着 %1 块").arg(stillShown));

            /* 重叠在一起：四块的卡片矩形分毫不差 */
            const QRect card = noteRect(s1);
            auto rectText = [](const QRect &r) {
                return QStringLiteral("%1x%2@%3,%4").arg(r.width()).arg(r.height())
                    .arg(r.x()).arg(r.y());
            };
            noteCheck(card == noteRect(s2) && card == noteRect(s3) && card == noteRect(s4),
                      QStringLiteral("一键叠摞：四块叠在同一格（不是错开摆）"),
                      QStringLiteral("%1 / %2 / %3 / %4")
                          .arg(rectText(card), rectText(noteRect(s2)), rectText(noteRect(s3)),
                               rectText(noteRect(s4))));

            /* 吸附到工作区右上角：卡片右沿贴工作区右沿、上沿贴工作区上沿 */
            QScreen *screen = QGuiApplication::screenAt(card.center());
            if (!screen)
                screen = QGuiApplication::primaryScreen();
            const QRect area = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
            noteCheck(card.x() + card.width() == area.right() + 1 && card.y() == area.top(),
                      QStringLiteral("一键叠摞：整摞贴着工作区右上角"),
                      QStringLiteral("卡片 %1x%2@%3,%4 / 工作区右 %5 上 %6")
                          .arg(card.width()).arg(card.height()).arg(card.x()).arg(card.y())
                          .arg(area.right() + 1).arg(area.top()));

            /*
             * 选中的那块往外伸一截：grab() 真实渲染，按每一行的**第一个不透明
             * 像素**量左沿。色块行在窗口里的高 = chipSize(28)、间隔 4，整列从
             * noteMargin(10) 开始 —— 这三个数在界面里，这里只按行取中点。
             */
            const QVariantList stackTabs
                = stackRoot ? stackRoot->property("groupTabs").toList() : QVariantList();
            int selIndex = -1;
            for (int i = 0; i < stackTabs.size(); ++i) {
                if (stackTabs.at(i).toMap().value(QStringLiteral("selected")).toBool())
                    selIndex = i;
            }
            auto *frontWidget = qobject_cast<QWidget *>(notes->windowForId(s1->id()));
            const QImage chipShot = frontWidget ? frontWidget->grab().toImage()
                                                      .convertToFormat(QImage::Format_ARGB32)
                                                : QImage();
            auto chipLeft = [&chipShot](int index) {
                if (index < 0 || chipShot.isNull())
                    return -1;
                const int y = 10 + index * 32 + 14;
                if (y >= chipShot.height())
                    return -1;
                for (int x = 0; x < chipShot.width(); ++x) {
                    if (qAlpha(chipShot.pixel(x, y)) > 40)
                        return x;
                }
                return -1;
            };
            const int otherIndex = selIndex == 0 ? 1 : 0;
            const int selLeft = chipLeft(selIndex);
            const int otherLeft = chipLeft(otherIndex);
            noteCheck(selIndex >= 0 && selLeft >= 0 && otherLeft > selLeft,
                      QStringLiteral("标签条：选中的那块往外伸一截（量像素左沿，不是常量）"),
                      QStringLiteral("选中第 %1 块 左沿=%2 / 其余=%3")
                          .arg(selIndex).arg(selLeft).arg(otherLeft));

            for (StickyNote *s : {s1, s2, s3, s4})
                notes->deleteNote(s);
            settle();
        }

        /* 收工：把这几块删掉，别影响后面几节 */
        notes->deleteNote(a);
        notes->deleteNote(b);
        notes->deleteNote(c);
        settle();
    }

    /* =====================================================================
     * 9) 托盘那三条（收进托盘也能用）
     * =================================================================== */
    if (tray) {
        QMenu *menu = tray->menu();
        const QList<QAction *> acts = menu ? menu->actions() : QList<QAction *>();
        QAction *noteAct = nullptr;
        bool hasArrange = false;
        bool hasToggle = false;
        for (QAction *a : acts) {
            if (!a)
                continue;
            if (a->text().contains(QStringLiteral("新建便签")))
                noteAct = a;
            if (a->text().contains(QStringLiteral("排列便签")))
                hasArrange = true;
            if (a->text().contains(QStringLiteral("便签"))
                && (a->text().contains(QStringLiteral("显示"))
                    || a->text().contains(QStringLiteral("收起"))))
                hasToggle = true;
        }
        noteCheck(noteAct != nullptr && hasArrange && hasToggle,
                  QStringLiteral("托盘：菜单里有新建 / 显示（收起）全部 / 排列便签"),
                  QStringLiteral("菜单 %1 条").arg(acts.size()));

        const int before = notes->count();
        if (noteAct) {
            noteAct->trigger();
            settle();
            noteCheck(notes->count() == before + 1,
                      QStringLiteral("托盘：点「新建便签」真的开出一块"),
                      QStringLiteral("现在 %1 条").arg(notes->count()));
        }
    } else {
        noteOut(QStringLiteral("（没有托盘对象，跳过托盘那两条）"));
    }

    /* =====================================================================
     * 9) 系统级热键（收进托盘也能按）
     * =================================================================== */
    if (cmd) {
        noteCheck(cmd->globalHotkeyActiveFor(QStringLiteral("note")),
                  QStringLiteral("热键：Ctrl+Alt+N 注册成了系统级热键"),
                  cmd->shortcutFor(QStringLiteral("note")));
        const int before = notes->count();
        cmd->activateCommand(QStringLiteral("note"));
        settle();
        noteCheck(notes->count() == before + 1,
                  QStringLiteral("热键：那条路真的开出一块便签"),
                  QStringLiteral("现在 %1 条").arg(notes->count()));
    } else {
        noteOut(QStringLiteral("（没有命令中枢，跳过热键那条）"));
    }

    /*
     * 临时探针（查"从菜单删一张"闪的那一帧；量完删）：
     * SMARTCLIP_NOTES_DELETE_PROBE=1 才跑。走的是和他手点**完全同一条路**：
     * openNoteMenu → noteMenu.fire("delete") → StickyNoteWindow::deleteNote
     * → singleShot(0) → StickyNotes::deleteNote。之前那版探针直接调的是
     * manager->deleteNote()，跳过了菜单和那个 singleShot —— 所以量不出来。
     * 每一步之间睡 600~900ms，好让 144fps 的录制把"删之前 / 那一下 / 之后"分开。
     */
    if (qEnvironmentVariableIsSet("SMARTCLIP_NOTES_DELETE_PROBE")) {
        noteOut(QStringLiteral("【探针】菜单删除逐帧：建 4 块 → 叠成一摞 → 连删 3 次"));
        StickyNote *first = notes->createNote();
        for (int i = 0; i < 3; ++i)
            notes->createNote();
        settle();
        QThread::msleep(500);
        notes->stackAll(first);
        settle();
        /* 只留这一摞的组 id（字符串）：下面每轮删掉的那块，它的裸指针当场就悬了 */
        const QString probeGroup = first ? first->groupId() : QString();
        QThread::msleep(900);

        /* 删到散伙为止：9→8→…→2→1，最后那一刀会让整条标签条消失 */
        for (int round = 0; round < 8; ++round) {
            StickyNoteWindow *face = notes->activeInGroup(probeGroup);
            const QString faceId = face && face->note() ? face->note()->id() : QString();
            QObject *root = notes->windowRootForId(faceId);
            const QVariantMap geo = notes->windowState(faceId);
            noteOut(QStringLiteral("【探针】第 %1 轮：露着的那块 id=%2 卡片=%3x%4@%5,%6 摞里 %7 块")
                        .arg(round + 1)
                        .arg(faceId.right(4))
                        .arg(geo.value(QStringLiteral("w")).toInt())
                        .arg(geo.value(QStringLiteral("h")).toInt())
                        .arg(geo.value(QStringLiteral("x")).toInt())
                        .arg(geo.value(QStringLiteral("y")).toInt())
                        .arg(notes->groupMembers(probeGroup).size()));
            std::fflush(stdout);
            if (!root) {
                noteOut(QStringLiteral("【探针】拿不到 QML 根，停"));
                break;
            }
            /*
             * 标记：**要被删的那块先涂红**。录屏里红 = "这一摞此刻露着的纸"，
             * 红消失之后到下一张纸出现之前那几帧是什么颜色，就是用户看到的闪。
             * 分析只盯纸面左上角一小块（菜单弹在右下，盖不到那里）。
             */
            face->note()->setColor(QColor(QStringLiteral("#d81b1b")));
            settle();
            QThread::msleep(400);
            QMetaObject::invokeMethod(root, "openNoteMenu",
                                      Q_ARG(QVariant, geo.value(QStringLiteral("x")).toInt() + 90),
                                      Q_ARG(QVariant, geo.value(QStringLiteral("y")).toInt() + 90));
            settle();
            QThread::msleep(600);
            QObject *menu = root->findChild<QObject *>(QStringLiteral("noteMenu"));
            if (menu)
                QMetaObject::invokeMethod(menu, "fire", Q_ARG(QVariant, QVariant("delete")));
            settle();
            noteOut(QStringLiteral("【探针】  ·那一拍之后：还剩 %1 块").arg(notes->count()));
            std::fflush(stdout);
            QThread::msleep(900);
        }
        QThread::msleep(900);
    }

    /* =====================================================================
     * 收工：删干净，桌面上不留残窗
     * =================================================================== */
    {
        for (StickyNote *note : notes->store()->notes())
            notes->deleteNote(note);
        settle();
        noteCheck(notes->count() == 0 && notes->visibleCount() == 0,
                  QStringLiteral("收工：便签删干净了，桌面不留残窗"),
                  QStringLiteral("还剩 %1 条 / %2 块").arg(notes->count())
                      .arg(notes->visibleCount()));
    }

    noteOut(QString());
    noteOut(QStringLiteral("便签自检：通过 %1 项，失败 %2 项")
                .arg(gNotePassed).arg(gNoteFailed));
    return gNoteFailed;
}
