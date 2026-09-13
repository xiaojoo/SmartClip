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
#include <QRegularExpression>
#include <QScreen>
#include <QThread>
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

    /* 从干净的便签清单开始（自检不动用户真实的 notes.json，见下面各段用临时文件） */
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
                  QStringLiteral("窗口：调色板里有多个底色可选（菜单色板用它）"),
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
     * 5) 「⋯」菜单：贴着鼠标、颜色在里面、子面板会翻边
     * =================================================================== */
    {
        StickyNote *menuNote = probe;
        QObject *root = notes->windowRootForId(menuNote->id());

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
        noteCheck(open.value(QStringLiteral("swatchCount")).toInt() >= 8,
                  QStringLiteral("菜单：颜色那一条带整份调色板（≥8 格）"),
                  QStringLiteral("%1 格").arg(open.value(QStringLiteral("swatchCount")).toInt()));

        const QString labels = open.value(QStringLiteral("labels")).toString();
        for (const QString &want : {QStringLiteral("颜色"), QStringLiteral("透明度"),
                                    QStringLiteral("新建便签"), QStringLiteral("始终置顶"),
                                    QStringLiteral("锁定"), QStringLiteral("删除这块便签")}) {
            noteCheck(labels.contains(want),
                      QStringLiteral("菜单：有「%1」这一条").arg(want), labels);
        }

        /* 主栏的宽度（子面板是自己一块窗口，主栏窗口整场菜单就这么宽） */
        const double collapsedWidth = open.value(QStringLiteral("paneWidth")).toDouble();

        /* 展开颜色子面板：位置给得出来、和主栏并排、还在屏幕里 */
        noteCheck(notes->openMenuFlyout(menuNote->id(), QStringLiteral("color")),
                  QStringLiteral("菜单：颜色子面板打得开"));
        settle();
        const QVariantMap fly = notes->menuState(menuNote->id());
        const QRect menuFly = fly.value(QStringLiteral("screenRect")).toRect();
        const QRect subFly = fly.value(QStringLiteral("flyoutRect")).toRect();
        const QString side = fly.value(QStringLiteral("flyoutSide")).toString();

        noteCheck(fly.value(QStringLiteral("flyout")).toString() == QLatin1String("color"),
                  QStringLiteral("菜单：当前展开的是颜色面板"));
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
        noteCheck(fly.value(QStringLiteral("firstSwatch")).toString()
                      .startsWith(QLatin1String("#ffe9a8")),
                  QStringLiteral("菜单：色板第一格就是默认的便签黄"),
                  fly.value(QStringLiteral("firstSwatch")).toString());

        /*
         * 「子菜单闪一下就消失」那条毛病的钉子。
         *
         * 主栏和子面板之间有 6px 的缝：鼠标从"颜色"那一条往面板上挪，中途会
         * 离开那一行，而"进到面板上"那个 hover 事件在窗口刚改过尺寸时并不
         * 可靠 —— 只靠 hover 判断的话，面板会在这条缝上被收掉（用户看到的就是
         * 闪一下）。现在改成按**光标实际位置**判断，这里就把光标放进那条缝里，
         * 等过一个收合周期，面板必须还在。
         */
        {
            const QRect subFly2 = fly.value(QStringLiteral("flyoutRect")).toRect();
            const QRect menuFly2 = fly.value(QStringLiteral("screenRect")).toRect();
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
                              == QLatin1String("color"),
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
            notes->openMenuFlyout(menuNote->id(), QStringLiteral("color"));
            settle();

            const QVariantMap edge = notes->menuState(menuNote->id());
            const bool edgeOpen = edge.value(QStringLiteral("opened")).toBool()
                                  && edge.value(QStringLiteral("flyout")).toString()
                                         == QLatin1String("color");
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
         * 用户报的场景：把便签拖到右边，点「⋯」，右边那块子面板（颜色/透明度）
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
            notes->openMenuFlyout(menuNote->id(), QStringLiteral("color"));
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

            QMetaObject::invokeMethod(root, "handleMenuAct", Q_ARG(QVariant, QVariant("lock")));
            settle();
            s = notes->windowState(probe->id());
            noteCheck(s.value(QStringLiteral("locked")).toBool(),
                      QStringLiteral("菜单：勾上「锁定」之后便签鼠标穿透"));
            noteCheck(notes->lockedCount() >= 1,
                      QStringLiteral("菜单：锁定条数报得出来（菜单据此显示解锁）"));
            noteCheck(notes->unlockAll(),
                      QStringLiteral("菜单：「解锁所有便签」解得开（锁上之后点不到它）"));
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
     * 8) 托盘那三条（收进托盘也能用）
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
