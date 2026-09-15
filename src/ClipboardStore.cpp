#include "ClipboardStore.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHashFunctions>
#include <QImage>
#include <QRegularExpression>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <map>
#include <utility>

namespace {

/*
 * 剪贴板内容那一层的目录名（根目录下面再套一层，见 ClipboardStore::contentRoot）。
 *
 * 单独提出来是因为它出现在好几个地方（建目录 / 扫描 / 清理旧结构），
 * 写散了改一处漏一处。
 */
const QString kContentDirName = QStringLiteral("剪贴板");

/* 我们自己写进 md 的段首标记："## 07:31:00" */
const QString kEntryMark = QStringLiteral("## ");
/* 日期目录名：2026-09-13 */
const QRegularExpression &dateDirPattern() {
    static const QRegularExpression re(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}$"));
    return re;
}

QString readAllText(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(file.readAll());
}

/*
 * 这个文件按"文本"处理吗（决定要不要解析里面的 "## 时分秒" 分段）。
 *
 * 判据是**开头有没有 NUL 字节**：源码 / 配置 / 日志都不会有，png、exe、
 * 压缩包这类几乎一定有。导入的文件夹现在是原样列出来的（见 scanFolder），
 * 里面什么后缀都可能有 —— 挨个当文本读进来解析，一个 .exe 就能把一次重扫
 * 拖垮，解析出来的"条数"也是乱码里数出来的。
 *
 * 顺带卡一道大小：超过 kMaxParseBytes 的文件也不解析（分段是给剪贴板内容
 * 用的，那种文件最多几十 KB）。
 */
constexpr qint64 kMaxParseBytes = 4 * 1024 * 1024;

/*
 * 只列名字、不读内容的目录：依赖包 / 构建产物 / 版本库内部。
 *
 * 主流编辑器都是这个路子（VS Code 的 files.exclude、PyCharm 的 Excluded
 * Directories）：node_modules 里几万个文件，读进来既没意义又慢 —— 用户导入
 * H:\chat（3.4 万个文件，其中 3.3 万在 node_modules 里）时卡死就是它。
 *
 * 树里照样看得到这些目录和文件（上一版要的"遍历出所有内容"没变），
 * 只是不解析内容：条数记 0，也不进内容搜索（按文件名还是搜得到，
 * 见 searchFiles 里那条 path LIKE）。
 */
bool contentSkipped(const QString &path) {
    static const QSet<QString> kSkip = {
        QStringLiteral("node_modules"), QStringLiteral(".pnpm"), QStringLiteral("bower_components"),
        QStringLiteral(".git"),         QStringLiteral(".svn"),  QStringLiteral(".hg"),
        QStringLiteral("target"),       QStringLiteral("dist"),  QStringLiteral("out"),
        QStringLiteral("build"),        QStringLiteral("bin"),   QStringLiteral("obj"),
        QStringLiteral(".venv"),        QStringLiteral("venv"),  QStringLiteral("__pycache__"),
        QStringLiteral(".gradle"),      QStringLiteral(".npm-cache"), QStringLiteral(".cache"),
        QStringLiteral(".next"),        QStringLiteral(".nuxt"), QStringLiteral(".vite"),
        QStringLiteral("coverage")
    };
    const QStringList parts =
        QDir::fromNativeSeparators(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &part : std::as_const(parts)) {
        if (kSkip.contains(part))
            return true;
    }
    return false;
}

/*
 * 读一个文本文件：返回是不是文本，内容写进 out。
 *
 * 只开一次文件：先前是 looksLikeText() 读 4KB 探一遍、readAllText() 再整个
 * 读一遍，同样一个文件两遍 I/O。二进制只读开头那 4KB 就返回，不把整个
 * 大文件拖进内存。
 */
bool readTextFile(const QFileInfo &info, QString *out) {
    out->clear();
    if (!info.isFile() || info.size() > kMaxParseBytes)
        return false;
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray head = file.read(4096);
    if (head.contains('\0'))
        return false;                   /* 二进制：剩下的不读了 */
    const QByteArray rest = file.readAll();
    *out = QString::fromUtf8(head + rest);
    return true;
}

}  // namespace

ClipboardStore::ClipboardStore(QObject *parent) : QObject(parent) {}

QString ClipboardStore::defaultRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
           + QStringLiteral("/SmartClip");
}

QString ClipboardStore::settingsKey(const QString &key) {
    return QStringLiteral("clip/") + key;
}

/*
 * 清掉"加剪贴板那一层"之前的旧目录结构。
 *
 * 旧布局是把日期目录直接摆在**根目录**下（`<root>/2026-09-16/…`），现在内容都在
 * `<root>/剪贴板/` 里了，根目录下那些日期目录已经没人读（重扫只看 contentRoot），
 * 留着就是一堆孤儿。用户明确说"历史数据不要了，清理掉"。
 *
 * **只删根目录下名字严格是 `yyyy-MM-dd` 的目录**，别的一律不碰：
 *   * 用户的保存根目录可能是他自己挑的（甚至指向网盘某个夹），不能横扫；
 *   * 判据用和重扫同一个 dateDirPattern，认得出的才是我们以前建的。
 *
 * 幂等：清完之后根目录下就没有日期目录了，再跑什么都不做。
 */
void ClipboardStore::pruneLegacyLayout() {
    QDir root(m_rootPath);
    if (!root.exists())
        return;

    const QFileInfoList dirs =
        root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    int removed = 0;
    for (const QFileInfo &dir : dirs) {
        if (!dateDirPattern().match(dir.fileName()).hasMatch())
            continue;
        if (QDir(dir.absoluteFilePath()).removeRecursively())
            ++removed;
    }
    if (removed > 0) {
        qInfo("剪贴板：清掉了 %d 个旧布局的日期目录（内容现在在 %s 下）",
              removed, qUtf8Printable(contentRoot()));
    }
}

QString ClipboardStore::dateDir(const QString &dateKey) const {
    return contentRoot() + QLatin1Char('/') + dateKey;
}

QString ClipboardStore::contentRoot() const {
    return m_rootPath + QLatin1Char('/') + kContentDirName;
}

bool ClipboardStore::open() {
    QSettings settings;
    m_rootPath = settings.value(settingsKey(QStringLiteral("root")), defaultRoot()).toString();
    if (m_rootPath.trimmed().isEmpty())
        m_rootPath = defaultRoot();
    m_rootPath = QDir::cleanPath(m_rootPath);
    m_imported = settings.value(settingsKey(QStringLiteral("imported"))).toStringList();

    /* 保存目录先建出来：用户把设置里的路径指到一个还不存在的目录也要能用 */
    QDir().mkpath(m_rootPath);
    /*
     * 内容那一层也先建出来（<root>/剪贴板）—— 采集时反正会 mkpath，但重扫 /
     * 建树那几条路都从这层往下列，先建出来省得每处都判一次存不存在。
     * （旧布局的清理在 rescan() 里，open 底下会调它。）
     */
    QDir().mkpath(contentRoot());

    /*
     * 元数据库还是放在应用数据目录（%APPDATA%/SmartClip/SmartClip/smartclip.db）——
     * 它是"这台机器上这份安装的索引"，和内容放哪儿无关：内容目录可以指到网盘，
     * 索引跟着本机走反而更合适（换目录不用重建）。
     */
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    m_db.setDatabaseName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/smartclip.db"));
    if (!m_db.open())
        return false;

    /*
     * 落盘策略放宽一点。
     *
     * 这个库是**缓存**不是唯一副本：内容全在 md 文件里，库坏了大不了重扫一遍
     * 磁盘重建（见文件头）。默认的 journal + FULL 是每写一条就 fsync 一次 ——
     * 导入一个几万文件的项目时，那点落盘开销比解析本身还贵。WAL 让读写不
     * 互相阻塞，synchronous=NORMAL 省掉每条语句的 fsync，实测导入快一个量级。
     */
    QSqlQuery query(m_db);
    query.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
    query.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));

    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS clip_files ("
            "path TEXT PRIMARY KEY, folder TEXT NOT NULL, date_key TEXT NOT NULL, "
            "size INTEGER NOT NULL, mtime TEXT NOT NULL, entries INTEGER NOT NULL, "
            "imported INTEGER NOT NULL DEFAULT 0)")))
        return false;
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS clip_entries ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT, file_path TEXT NOT NULL, "
            "entry_time TEXT NOT NULL, type TEXT NOT NULL, title TEXT NOT NULL, "
            "preview TEXT NOT NULL, bytes INTEGER NOT NULL, hash TEXT, asset TEXT)")))
        return false;
    /* 当前正在追加的那个文件（每天一个） */
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS clip_state ("
            "date_key TEXT PRIMARY KEY, path TEXT NOT NULL)")))
        return false;
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_entries_file "
                              "ON clip_entries(file_path)"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_entries_hash "
                              "ON clip_entries(hash)"));

    migrateFromLegacy();
    rescan();
    return true;
}

/* ------------------------------------------------------------------ */
/* 上一版的正文表                                                        */
/* ------------------------------------------------------------------ */

void ClipboardStore::migrateFromLegacy() {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT name FROM sqlite_master WHERE type = 'table' AND name = 'clipboard_items'"));
    if (!query.exec() || !query.next())
        return;  /* 没有旧表：不是从上一版升上来的，什么都不用做 */

    query.exec(QStringLiteral("DROP TABLE IF EXISTS clipboard_items"));
    /* 删表不会把文件缩小（空页留在库里），顺手 VACUUM 一次把旧正文占的那几兆还回去 */
    query.exec(QStringLiteral("VACUUM"));

    /*
     * 旧库里图片条目的 content 存的是 AppData/images/xxx.png 的路径，
     * 条目一没这批图就没人认领了 —— 一起删掉，不然它们会一直占着几百兆。
     * （只在保存目录没被指到那里时才删，见下。）
     */
    const QString legacyImages =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
        + QStringLiteral("/images");
    const QString clean = QDir::cleanPath(legacyImages);
    if (!m_rootPath.startsWith(clean))
        QDir(clean).removeRecursively();
}

/* ------------------------------------------------------------------ */
/* 采集                                                                */
/* ------------------------------------------------------------------ */

QString ClipboardStore::titleFor(const QString &text) {
    const QString title = text.simplified();
    return title.left(80).isEmpty() ? tr("新建条目") : title.left(80);
}

QString ClipboardStore::previewFor(const QString &text) {
    /*
     * 摘要：单行 + 限长，给搜索和左树用。
     *
     * 顺序很讲究：先截断再 simplified()。反过来的话，一条 66 万字符的正文
     * 要先被 simplified() 全扫一遍，虽然只是几十毫秒，但每条都要付这个成本。
     */
    constexpr int kProbe = 400;
    constexpr int kPreview = 200;

    const QString head = text.left(kProbe).simplified();
    if (head.size() > kPreview)
        return head.left(kPreview) + QStringLiteral("…");
    return head;
}

bool ClipboardStore::captureText(const QString &text) {
    const QString clean = text.trimmed();
    if (clean.isEmpty())
        return false;

    /* 同一段文本不再存第二遍（老版本的 hash 唯一约束是同一个意思） */
    const QString hash = QString::number(qHash(clean));
    if (hashExists(hash))
        return false;

    return appendEntry(QDateTime::currentDateTime(), QStringLiteral("text"),
                       titleFor(clean), clean, hash, QString());
}

bool ClipboardStore::captureImage(const QImage &image) {
    if (image.isNull())
        return false;

    /*
     * 按像素内容去重（和文本一个道理）。
     *
     * 不只是省一次写盘：Windows 上"复制一张图"往往会连着发两次剪贴板通知
     * （CF_BITMAP、CF_DIB 各一次），不去重的话同一张图会存成两份 PNG、
     * 在当天的 md 里出现两段一模一样的引用。
     */
    const QString hash = QString::number(
        qHashBits(image.constBits(), size_t(image.sizeInBytes()), 0));
    if (hashExists(hash))
        return false;

    const QDateTime now = QDateTime::currentDateTime();
    const QString dateKey = now.toString(QStringLiteral("yyyy-MM-dd"));
    const QString assets = dateDir(dateKey) + QStringLiteral("/assets");
    if (!QDir().mkpath(assets))
        return false;

    const QString name = now.toString(QStringLiteral("yyyyMMdd-HHmmsszzz")) + QStringLiteral(".png");
    const QString path = assets + QLatin1Char('/') + name;
    if (!image.save(path, "PNG")) {
        QFile::remove(path);   /* 写到一半的残缺文件不留 */
        return false;
    }

    /* md 里写相对路径：整个日期目录搬走也还指向得到 */
    const QString payload = QStringLiteral("![图片](assets/%1)").arg(name);
    return appendEntry(now, QStringLiteral("image"),
                       tr("图片 %1").arg(now.toString(QStringLiteral("HH:mm:ss"))),
                       payload, hash, path);
}

QString ClipboardStore::captureTarget(const QString &dateKey, const QString &dir) const {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT path FROM clip_state WHERE date_key = ?"));
    query.addBindValue(dateKey);
    if (!query.exec() || !query.next())
        return QString();

    const QString path = query.value(0).toString();
    const QFileInfo info(path);
    /* 文件被删了 / 写满了 / 已经不是这个目录里的了（换过保存目录）-> 这一轮另起一个 */
    if (!info.exists() || info.size() >= kFileLimit)
        return QString();
    if (QDir::cleanPath(info.absolutePath()) != QDir::cleanPath(dir))
        return QString();
    return path;
}

QString ClipboardStore::newFilePath(const QString &dir, const QDateTime &now) const {
    const QString base = now.toString(QStringLiteral("HHmmss"));
    QString candidate = QStringLiteral("%1/%2.md").arg(dir, base);
    int n = 2;
    while (QFileInfo::exists(candidate))
        candidate = QStringLiteral("%1/%2-%3.md").arg(dir, base).arg(n++);
    return candidate;
}

bool ClipboardStore::appendEntry(const QDateTime &now, const QString &type, const QString &title,
                                 const QString &payload, const QString &hash,
                                 const QString &assetPath) {
    const QString dateKey = now.toString(QStringLiteral("yyyy-MM-dd"));
    const QString dir = dateDir(dateKey);
    if (!QDir().mkpath(dir))
        return false;

    const QByteArray block =
        QStringLiteral("## %1\n\n%2\n\n").arg(now.toString(QStringLiteral("HH:mm:ss")), payload)
            .toUtf8();

    QString target = captureTarget(dateKey, dir);
    /* 这一段写进去会超 20K -> 另起一个文件（下一条自然落到新文件上） */
    if (!target.isEmpty() && QFileInfo(target).size() + block.size() > kFileLimit)
        target.clear();

    if (target.isEmpty()) {
        target = newFilePath(dir, now);
        QFile created(target);
        if (!created.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        created.write(QStringLiteral("# %1\n\n").arg(dateKey).toUtf8());
        created.close();

        QSqlQuery state(m_db);
        state.prepare(QStringLiteral(
            "INSERT INTO clip_state(date_key, path) VALUES(?, ?) "
            "ON CONFLICT(date_key) DO UPDATE SET path = excluded.path"));
        state.addBindValue(dateKey);
        state.addBindValue(target);
        state.exec();
    }

    QFile file(target);
    if (!file.open(QIODevice::Append))
        return false;
    const qint64 written = file.write(block);
    file.close();
    if (written != block.size())
        return false;

    recordEntry(target, now, type, title, previewFor(payload), block.size(), hash, assetPath);
    refreshFileRow(target, dateKey, false);
    recount();
    emit changed();
    return true;
}

/* ------------------------------------------------------------------ */
/* 元数据                                                              */
/* ------------------------------------------------------------------ */

bool ClipboardStore::hashExists(const QString &hash) const {
    if (hash.isEmpty())
        return false;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("SELECT 1 FROM clip_entries WHERE hash = ? LIMIT 1"));
    query.addBindValue(hash);
    return query.exec() && query.next();
}

void ClipboardStore::recordEntry(const QString &filePath, const QDateTime &when,
                                 const QString &type, const QString &title,
                                 const QString &preview, qint64 bytes, const QString &hash,
                                 const QString &assetPath) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO clip_entries(file_path, entry_time, type, title, preview, bytes, hash, asset) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    query.addBindValue(filePath);
    query.addBindValue(when.toString(Qt::ISODate));
    query.addBindValue(type);
    query.addBindValue(title);
    query.addBindValue(preview);
    query.addBindValue(bytes);
    query.addBindValue(hash.isEmpty() ? QVariant() : QVariant(hash));
    query.addBindValue(assetPath);
    query.exec();
}

/*
 * 写 / 更新 clip_files 里那一行。
 *
 * entries 传 >= 0 就是**已知条数**（刚插完几条自己数着），不再去 SELECT
 * COUNT(*)：扫一个新导入的目录时每个文件都要来一条 COUNT，三万个文件就是
 * 三万条查询，白等。传 -1（默认）才按老办法问数据库 —— 只有"追加内容"那条
 * 路（appendEntry）需要，它调用一次，无所谓。
 */
void ClipboardStore::refreshFileRow(const QString &filePath, const QString &dateKey, bool imported,
                                    int entries) {
    const QFileInfo info(filePath);

    if (entries < 0) {
        QSqlQuery count(m_db);
        count.prepare(QStringLiteral("SELECT COUNT(*) FROM clip_entries WHERE file_path = ?"));
        count.addBindValue(filePath);
        entries = (count.exec() && count.next()) ? count.value(0).toInt() : 0;
    }

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "INSERT INTO clip_files(path, folder, date_key, size, mtime, entries, imported) "
        "VALUES(?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET folder = excluded.folder, date_key = excluded.date_key, "
        "size = excluded.size, mtime = excluded.mtime, entries = excluded.entries, "
        "imported = excluded.imported"));
    query.addBindValue(filePath);
    query.addBindValue(info.absolutePath());
    query.addBindValue(dateKey);
    query.addBindValue(info.size());
    query.addBindValue(info.lastModified().toString(Qt::ISODate));
    query.addBindValue(entries);
    query.addBindValue(imported ? 1 : 0);
    query.exec();
}

void ClipboardStore::forgetFile(const QString &filePath) {
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("DELETE FROM clip_entries WHERE file_path = ?"));
    query.addBindValue(filePath);
    query.exec();

    query.prepare(QStringLiteral("DELETE FROM clip_files WHERE path = ?"));
    query.addBindValue(filePath);
    query.exec();
}

void ClipboardStore::recount() {
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM clip_files")) && query.next())
        m_fileCount = query.value(0).toInt();
    if (query.exec(QStringLiteral("SELECT COUNT(*) FROM clip_entries")) && query.next())
        m_entryCount = query.value(0).toInt();
}

/* ------------------------------------------------------------------ */
/* 扫盘 / 解析                                                          */
/* ------------------------------------------------------------------ */

void ClipboardStore::rescan() {
    QSet<QString> seen;
    m_importedDirs.clear();

    /*
     * 先清旧布局（日期目录原来直接摆在根目录下，现在内容都在 <root>/剪贴板/ 里）。
     *
     * 放在 rescan 里而不是 open 里：换保存位置（setRootPath）走的也是 rescan ——
     * 用户切到一个还留着旧布局的目录时，那批孤儿目录也该顺手清掉。幂等，重复调
     * 没代价（清完根目录下就没有日期目录了）。
     */
    pruneLegacyLayout();

    /*
     * 整趟扫盘包在**一个事务**里。
     *
     * 每个文件要写好几条 SQL（删旧条目、插条目、更新文件行）；SQLite 默认
     * 每条语句自己一个事务，导入一个几万文件的项目就是十几万次提交 ——
     * 用户报的"导入大文件夹慢得离谱"主要就是它。包成一个事务之后只有
     * 最后一次提交要等落盘。
     */
    const bool batched = m_db.transaction();

    /*
     * 上次扫盘的结果整表读进内存（路径 -> 大小 / 修改时间 / 是否导入）。
     *
     * 先前是**每个文件**一条 SELECT 去查缓存：一个三万多文件的项目就是三万多次
     * prepare + exec，光这一趟实测十几秒 —— 用户报的"导入大文件夹慢得离谱"
     * 主要就是它。这张表本来就只有几万行，一次读进来在内存里比快得多。
     */
    QHash<QString, FileCacheEntry> cached;
    {
        QSqlQuery all(m_db);
        if (all.exec(QStringLiteral("SELECT path, size, mtime, imported FROM clip_files"))) {
            while (all.next()) {
                FileCacheEntry entry;
                entry.size = all.value(1).toLongLong();
                entry.mtime = all.value(2).toString();
                entry.imported = all.value(3).toBool();
                cached.insert(all.value(0).toString(), entry);
            }
        }
    }

    QDir root(contentRoot());
    if (root.exists()) {
        const QFileInfoList dirs =
            root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &dir : dirs) {
            /* 这层里只认 "2026-09-13" 这种日期目录，别把别的文件夹也索引进来 */
            if (!dateDirPattern().match(dir.fileName()).hasMatch())
                continue;
            scanFolder(dir.absoluteFilePath(), false, seen, 0, &cached);
        }
    }

    for (const QString &imported : std::as_const(m_imported))
        scanFolder(QDir::cleanPath(imported), true, seen, 0, &cached);

    /*
     * 缓存里有、这次没扫到的：文件被删掉或改名了，元数据跟着清。
     * 判据直接用刚才读进内存的那张表，不用再查一遍库。
     */
    for (auto it = cached.constBegin(); it != cached.constEnd(); ++it) {
        if (!seen.contains(it.key()))
            forgetFile(it.key());
    }

    if (batched)
        m_db.commit();

    recount();
    emit changed();
}

void ClipboardStore::scanFolder(const QString &dir, bool imported, QSet<QString> &seen, int depth,
                                QHash<QString, FileCacheEntry> *cache) {
    QDir folder(dir);
    if (!folder.exists())
        return;

    /*
     * 导入的文件夹**原样**列出来：什么后缀都收（cpp / h / xml / png…），
     * 隐藏目录（.idea 这种点开头的）也进去 —— 用户要的是"打开一个项目"，
     * 上一版只收 md / markdown / txt，于是 H:\test\TetrisGame\src 整块不见了
     * （里面的 .cpp/.h 一个都不匹配）。
     *
     * 自己的日期目录仍然只认 md：那里是我们写剪贴板内容的地方，多出来的
     * 只有 assets 里的图片。
     */
    const QFileInfoList files =
        imported
            ? folder.entryInfoList(QDir::Files | QDir::Readable | QDir::Hidden, QDir::Name)
            : folder.entryInfoList({ QStringLiteral("*.md") },
                                   QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &file : files) {
        seen.insert(file.absoluteFilePath());
        reindexFile(file.absoluteFilePath(), imported, cache);
    }

    /* 只有导入的文件夹往下钻：自己的日期目录里只有 md 和一个 assets 子目录 */
    if (!imported || depth >= kMaxDepth)
        return;

    /* 这个目录本身也记一笔：里面一个文件都没有时，树还得有它这一行 */
    m_importedDirs.insert(QDir::cleanPath(dir));

    /*
     * 子目录也带上隐藏的：.idea / .vscode 这类项目配置目录本来就是项目的一部分。
     * （原来还专门跳过叫 assets 的子目录 —— 那是我们日期目录里的规矩，可这段
     * 只对导入目录跑，等于把别人项目里的 assets 也吞了，一并去掉。）
     */
    const QFileInfoList subDirs =
        folder.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QFileInfo &sub : subDirs) {
        const QString subPath = QDir::cleanPath(sub.absoluteFilePath());
        /*
         * 依赖 / 构建目录（node_modules、target、dist…）：**只留它这一行，
         * 不往里走**。
         *
         * 这是导入大目录卡住的根儿：H:\chat 三万四千个文件，三万三千个在
         * node_modules 里。名字都列出来、每个都过一遍库（哪怕不读内容），
         * 实测就是十几秒的卡死 —— 主流编辑器也是这么办的：PyCharm 把这类
         * 目录标成"排除"，VS Code 从搜索里排除掉。
         *
         * 只跳子目录，用户**自己导入的那个根**照常扫（哪怕它就叫 node_modules）。
         */
        if (contentSkipped(subPath)) {
            m_importedDirs.insert(subPath);      /* 行还在，只是不进去 */
            continue;
        }
        scanFolder(subPath, imported, seen, depth + 1, cache);
    }
}

void ClipboardStore::reindexFile(const QString &path, bool imported,
                                 QHash<QString, FileCacheEntry> *cache) {
    const QFileInfo info(path);
    const QString stamp = info.lastModified().toString(Qt::ISODate);

    /*
     * 大小和修改时间都没动过 -> 这个文件的元数据还是对的，不用重新解析。
     * 判据来自内存里那张表（见 rescan），不再一个文件查一次库。
     */
    const bool had = cache && cache->contains(path);
    if (had) {
        const FileCacheEntry known = cache->value(path);
        if (known.size == info.size() && known.mtime == stamp && known.imported == imported)
            return;
    }

    /* 日期取上一级目录名（2026-09-13）；不是日期目录就用文件自己的时间 */
    QString dateKey = info.dir().dirName();
    if (!dateDirPattern().match(dateKey).hasMatch())
        dateKey = info.lastModified().toString(QStringLiteral("yyyy-MM-dd"));

    /* 只有真有旧行才删：冷导入时每个文件都删两下纯属白费 */
    auto dropOld = [this, had, &path]() {
        if (had)
            forgetFile(path);
    };

    /*
     * 依赖 / 构建目录（node_modules、target…）里的东西连读都不读：它们只是
     * 被列出来、不进内容索引（条数 0；按文件名还是搜得到，见 searchFiles）。
     * 二进制文件同理，只记大小 / 时间 —— 见 readTextFile 的说明。
     */
    if (imported && contentSkipped(path)) {
        dropOld();
        refreshFileRow(path, dateKey, imported, 0);
        if (cache)
            cache->insert(path, FileCacheEntry{info.size(), stamp, imported});
        return;
    }

    /*
     * 文本才读进来分段。
     *
     * 二进制（png / exe / 压缩包）读不成文本：一个条目都不记，条数 0 ——
     * 记一条"空条目"只会让树上那个"1 条"骗人，搜索里也搜不出任何东西。
     */
    QString text;
    if (!readTextFile(info, &text)) {
        dropOld();
        refreshFileRow(path, dateKey, imported, 0);
        if (cache)
            cache->insert(path, FileCacheEntry{info.size(), stamp, imported});
        return;
    }

    dropOld();

    /* 按 "## 时分秒" 分段 */
    struct Segment {
        QString time;
        QString body;
    };
    QList<Segment> segments;
    {
        Segment current;
        bool started = false;
        const QStringList lines = text.split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line =
                raw.endsWith(QLatin1Char('\r')) ? raw.left(raw.size() - 1) : raw;
            if (line.startsWith(kEntryMark)) {
                if (started)
                    segments.append(current);
                current = Segment{};
                current.time = line.mid(3).trimmed();
                started = true;
            } else if (started) {
                current.body += line + QLatin1Char('\n');
            }
        }
        if (started)
            segments.append(current);
    }

    /* 没有段首标记（导入的外部文件）-> 整篇算一条，时间取文件修改时间 */
    if (segments.isEmpty()) {
        Segment whole;
        whole.time = info.lastModified().toString(QStringLiteral("HH:mm:ss"));
        whole.body = text;
        segments.append(whole);
    }

    int written = 0;
    for (const Segment &segment : std::as_const(segments)) {
        const QString body = segment.body.trimmed();

        QString type = QStringLiteral("text");
        QString asset;
        QString title;
        if (body.startsWith(QStringLiteral("!["))) {
            const int open = body.indexOf(QStringLiteral("]("));
            const int close = body.lastIndexOf(QLatin1Char(')'));
            if (open > 0 && close > open + 2) {
                const QString relative = body.mid(open + 2, close - open - 2);
                if (!relative.startsWith(QStringLiteral("http"))) {
                    type = QStringLiteral("image");
                    asset = QDir(info.absolutePath()).absoluteFilePath(relative);
                    title = tr("图片 %1").arg(segment.time);
                }
            }
        }
        if (title.isEmpty())
            title = body.isEmpty() ? tr("新建条目") : titleFor(body);

        const QDateTime parsed =
            QDateTime::fromString(dateKey + QLatin1Char('T') + segment.time, Qt::ISODate);
        const QDateTime when = parsed.isValid() ? parsed : info.lastModified();

        const QString hash = (type == QLatin1String("text") && !body.isEmpty())
                                 ? QString::number(qHash(body))
                                 : QString();

        recordEntry(path, when, type, title, previewFor(body), body.toUtf8().size(), hash, asset);
        ++written;
    }

    refreshFileRow(path, dateKey, imported, written);
    if (cache)
        cache->insert(path, FileCacheEntry{info.size(), stamp, imported});
}

/* ------------------------------------------------------------------ */
/* 左树                                                                */
/* ------------------------------------------------------------------ */

QSet<QString> ClipboardStore::searchFiles(const QString &query) const {
    QSet<QString> out;
    const QString like = QLatin1Char('%') + query + QLatin1Char('%');

    QSqlQuery entries(m_db);
    entries.prepare(QStringLiteral(
        "SELECT DISTINCT file_path FROM clip_entries WHERE title LIKE ? OR preview LIKE ?"));
    entries.addBindValue(like);
    entries.addBindValue(like);
    if (entries.exec()) {
        while (entries.next())
            out.insert(entries.value(0).toString());
    }

    /* 文件名本身命中的也算（搜 "0731" 应该能搜到 073100.md） */
    QSqlQuery files(m_db);
    files.prepare(QStringLiteral("SELECT path FROM clip_files WHERE path LIKE ?"));
    files.addBindValue(like);
    if (files.exec()) {
        while (files.next())
            out.insert(files.value(0).toString());
    }
    return out;
}

QVariantList ClipboardStore::tree(const QString &query, bool newestFirst) const {
    const QString needle = query.trimmed();
    QSet<QString> allowed;
    if (!needle.isEmpty())
        allowed = searchFiles(needle);

    struct MetaFile {
        QString path;
        QString folder;
        QString dateKey;
        qint64 size = 0;
        int entries = 0;
        bool imported = false;
    };

    QList<MetaFile> files;
    {
        QSqlQuery query2(m_db);
        if (!query2.exec(QStringLiteral("SELECT path, folder, date_key, size, entries, imported "
                                        "FROM clip_files")))
            return QVariantList();
        while (query2.next()) {
            MetaFile file;
            file.path = query2.value(0).toString();
            file.folder = query2.value(1).toString();
            file.dateKey = query2.value(2).toString();
            file.size = query2.value(3).toLongLong();
            file.entries = query2.value(4).toInt();
            file.imported = query2.value(5).toBool();
            if (!needle.isEmpty() && !allowed.contains(file.path))
                continue;
            files.append(file);
        }
    }

    struct Node {
        QString key;
        QString label;
        QString kind;
        QString path;
        int depth = 0;
        bool chronological = false;   /* 名字就是时间（日期目录那一支） */
        bool skipped = false;         /* 依赖 / 构建目录：只列一行，没进去扫 */
        QList<Node *> children;
        QList<const MetaFile *> leaves;
    };

    std::map<QString, Node> pool;
    QList<Node *> roots;

    for (const MetaFile &file : std::as_const(files)) {
        /* 这个文件挂在"哪一串目录节点"下面（由外到里） */
        QStringList chain;
        bool isDate = false;
        if (!file.imported) {
            /*
             * 剪贴板这一支：**两层** —— 先「剪贴板」，再日期目录。
             *
             * 树上是 `剪贴板 -> 2026-09-16 -> 011647.md`（用户要的层次，和磁盘上
             * 的布局一致：内容都在 <root>/剪贴板/ 下面，见 contentRoot）。
             * 以前只挂日期目录那一层（顶层一堆 2026-09-16），加了一层之后树要跟上，
             * 不然从树上根本看不出这些东西都在「剪贴板」底下。
             *
             * 日期目录名从路径推（不要 file.dateKey：那个可能是文件修改时间兜出来的，
             * 未必等于它所在的目录名）。
             */
            const QString folder = QDir::cleanPath(file.folder);
            chain << QDir::cleanPath(contentRoot()) << folder;
            isDate = true;
        } else {
            /* 找到它所属的那个导入根（可能套了好几层） */
            QString root;
            for (const QString &candidate : std::as_const(m_imported)) {
                const QString clean = QDir::cleanPath(candidate);
                if (file.folder == clean
                    || file.folder.startsWith(clean + QLatin1Char('/'))) {
                    if (clean.size() > root.size())
                        root = clean;
                }
            }
            if (root.isEmpty())
                continue;   /* 这个导入目录已经从设置里移除了 */

            QString acc = root;
            chain << acc;
            const QStringList parts =
                file.folder.mid(root.size()).split(QLatin1Char('/'), Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                acc += QLatin1Char('/') + part;
                chain << acc;
            }
        }

        Node *parent = nullptr;
        for (int i = 0; i < chain.size(); ++i) {
            const QString key = QStringLiteral("dir:") + QDir::cleanPath(chain.at(i));
            auto it = pool.find(key);
            if (it == pool.end()) {
                Node node;
                node.key = key;
                node.path = QDir::cleanPath(chain.at(i));
                node.label = QFileInfo(node.path).fileName();
                node.depth = i;
                /*
                 * 名字就是时间的那一支只有**日期那层**（i == 1）—— 「剪贴板」自己
                 * （i == 0）不是时间，它按普通文件夹画。
                 */
                node.chronological = isDate && i == 1;
                node.kind = isDate
                                ? (i == 0 ? QStringLiteral("folder") : QStringLiteral("date"))
                                : (i == 0 ? QStringLiteral("imported") : QStringLiteral("folder"));
                it = pool.emplace(key, node).first;
                if (parent)
                    parent->children.append(&it->second);
                else
                    roots.append(&it->second);
            }
            parent = &it->second;
        }
        if (parent)
            parent->leaves.append(&file);
    }

    /*
     * 一个文件都没有的目录也要有节点。
     *
     * 上面那一轮是**按文件**拼的，目录是文件的"路径顺带"出来的；导入的项目里
     * 那种空目录（H:\test\.idea）就再也没有出场机会 —— 用户报的"没遍历出所有
     * 内容"里就有它。rescan 时把扫到的目录都记在 m_importedDirs 里，这里补上：
     * 按路径从浅到深走，父目录一定已经建好了（导入根自己就是最浅的那一级）。
     *
     * 只在**没有搜索词**的时候补：搜索是在筛文件，把没命中的目录也画出来
     * 就成了"搜了个不存在的词，树上却还挂着一堆空目录"。
     */
    if (needle.isEmpty() && !m_importedDirs.isEmpty()) {
        QStringList dirs(m_importedDirs.begin(), m_importedDirs.end());
        std::sort(dirs.begin(), dirs.end(), [](const QString &a, const QString &b) {
            const int da = a.count(QLatin1Char('/'));
            const int db = b.count(QLatin1Char('/'));
            return da != db ? da < db : a < b;
        });
        for (const QString &dir : std::as_const(dirs)) {
            const QString key = QStringLiteral("dir:") + dir;
            if (pool.find(key) != pool.end())
                continue;

            /* 挂在哪个导入根下面、往里第几层（口径和文件那条路一样） */
            QString root;
            for (const QString &candidate : std::as_const(m_imported)) {
                const QString clean = QDir::cleanPath(candidate);
                if (dir == clean || dir.startsWith(clean + QLatin1Char('/'))) {
                    if (clean.size() > root.size())
                        root = clean;
                }
            }
            if (root.isEmpty())
                continue;
            const int depth =
                dir == root ? 0
                            : int(dir.mid(root.size())
                                      .split(QLatin1Char('/'), Qt::SkipEmptyParts)
                                      .size());

            Node node;
            node.key = key;
            node.path = dir;
            node.label = QFileInfo(dir).fileName();
            node.depth = depth;
            node.kind = depth == 0 ? QStringLiteral("imported") : QStringLiteral("folder");
            /* 没进去扫的那种（依赖 / 构建目录）：树上报一声，界面标"未索引" */
            node.skipped = contentSkipped(dir) && depth > 0;
            auto it = pool.emplace(key, node).first;

            Node *fresh = &it->second;
            auto parentIt = pool.find(QStringLiteral("dir:")
                                      + dir.left(dir.lastIndexOf(QLatin1Char('/'))));
            if (depth == 0 || parentIt == pool.end())
                roots.append(fresh);
            else
                parentIt->second.children.append(fresh);
        }
    }

    /*
     * 「剪贴板」这一层：一个文件都没有时也要有它这个节点。
     *
     * 上面那轮是**按文件**拼的，目录是顺带出来的；新装的机器上一条内容都没有，
     * 树上就什么都不显示，看着像坏了。补一个空节点（kind 用 "folder"：它是
     * 组织层，不是日期那一支）。
     *
     * 搜索时**不补** —— 那是在筛文件，旁边挂一个空目录没意义。
     */
    if (needle.isEmpty()) {
        const QString key = QStringLiteral("dir:") + QDir::cleanPath(contentRoot());
        if (pool.find(key) == pool.end()) {
            Node node;
            node.key = key;
            node.path = QDir::cleanPath(contentRoot());
            node.label = kContentDirName;
            node.depth = 0;
            node.kind = QStringLiteral("folder");
            roots.append(&pool.emplace(key, node).first->second);
        }
    }

    std::function<QVariantMap(Node *)> dump = [&](Node *node) -> QVariantMap {
        QList<Node *> childNodes = node->children;
        QList<const MetaFile *> leafFiles = node->leaves;

        std::sort(childNodes.begin(), childNodes.end(), [](const Node *a, const Node *b) {
            return a->label.localeAwareCompare(b->label) < 0;
        });
        std::sort(leafFiles.begin(), leafFiles.end(),
                  [](const MetaFile *a, const MetaFile *b) {
                      return QFileInfo(a->path).fileName().localeAwareCompare(
                                 QFileInfo(b->path).fileName()) < 0;
                  });
        /* 名字就是时间的这一支：最新在前时把顺序倒过来 */
        if (node->chronological && newestFirst) {
            std::reverse(childNodes.begin(), childNodes.end());
            std::reverse(leafFiles.begin(), leafFiles.end());
        }

        QVariantList children;
        int fileTotal = leafFiles.size();
        int entryTotal = 0;
        for (const MetaFile *file : std::as_const(leafFiles))
            entryTotal += file->entries;

        for (const Node *child : std::as_const(childNodes)) {
            const QVariantMap childMap = dump(const_cast<Node *>(child));
            fileTotal += childMap.value(QStringLiteral("files")).toInt();
            entryTotal += childMap.value(QStringLiteral("entries")).toInt();
            children.append(childMap);
        }

        for (const MetaFile *file : std::as_const(leafFiles)) {
            QVariantMap leaf;
            leaf.insert(QStringLiteral("key"), file->path);
            leaf.insert(QStringLiteral("label"), QFileInfo(file->path).fileName());
            leaf.insert(QStringLiteral("kind"), QStringLiteral("file"));
            leaf.insert(QStringLiteral("path"), file->path);
            leaf.insert(QStringLiteral("depth"), node->depth + 1);
            leaf.insert(QStringLiteral("entries"), file->entries);
            leaf.insert(QStringLiteral("size"), file->size);
            leaf.insert(QStringLiteral("imported"), file->imported);
            leaf.insert(QStringLiteral("dateKey"), file->dateKey);
            children.append(leaf);
        }

        QVariantMap out;
        out.insert(QStringLiteral("key"), node->key);
        out.insert(QStringLiteral("label"), node->label);
        out.insert(QStringLiteral("kind"), node->kind);
        out.insert(QStringLiteral("path"), node->path);
        out.insert(QStringLiteral("depth"), node->depth);
        out.insert(QStringLiteral("files"), fileTotal);
        out.insert(QStringLiteral("entries"), entryTotal);
        out.insert(QStringLiteral("skipped"), node->skipped);
        out.insert(QStringLiteral("children"), children);
        return out;
    };

    QList<Node *> top = roots;
    std::sort(top.begin(), top.end(), [this, newestFirst](const Node *a, const Node *b) {
        /*
         * 「剪贴板」永远排最前面 —— 它是这台机器剪贴板内容的入口，比导入的项目
         * 更常用。用户要的就是树顶上一个「剪贴板」，点开才是日期文件夹。
         */
        const QString clipKey = QLatin1String("dir:") + QDir::cleanPath(contentRoot());
        const bool aClip = a->key == clipKey;
        const bool bClip = b->key == clipKey;
        if (aClip != bClip)
            return aClip;

        const bool aDate = a->kind == QLatin1String("date");
        const bool bDate = b->kind == QLatin1String("date");
        if (aDate != bDate)
            return aDate;   /* 日期文件夹排在导入的文件夹前面 */
        if (aDate)
            return newestFirst ? a->label > b->label : a->label < b->label;
        return a->label.localeAwareCompare(b->label) < 0;
    });

    QVariantList out;
    for (Node *node : std::as_const(top))
        out.append(dump(node));
    return out;
}

/* ------------------------------------------------------------------ */
/* 文件操作                                                             */
/* ------------------------------------------------------------------ */

QString ClipboardStore::createFile(const QString &text) {
    const QDateTime now = QDateTime::currentDateTime();
    const QString dateKey = now.toString(QStringLiteral("yyyy-MM-dd"));
    const QString dir = dateDir(dateKey);
    if (!QDir().mkpath(dir))
        return QString();

    const QString path = newFilePath(dir, now);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();

    QString body = QStringLiteral("# %1\n\n## %2\n\n").arg(dateKey, now.toString(QStringLiteral("HH:mm:ss")));
    if (!text.isEmpty())
        body += text + QStringLiteral("\n\n");
    const QByteArray bytes = body.toUtf8();
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    if (!ok) {
        QFile::remove(path);
        return QString();
    }

    reindexFile(path, false);
    recount();
    emit changed();
    return path;
}

QString ClipboardStore::createNote(const QString &title, const QString &markdown,
                                   const QMap<QString, QByteArray> &assets) {
    const QDateTime now = QDateTime::currentDateTime();
    const QString dateKey = now.toString(QStringLiteral("yyyy-MM-dd"));
    const QString dir = dateDir(dateKey);
    if (!QDir().mkpath(dir))
        return QString();

    /*
     * 图片先落盘，正文后写。
     *
     * 顺序不能反：正文里已经写着 `assets/<名字>`，图没写出去就是个断链，而
     * 这个文件已经建出来、用户已经看得见了。先把图准备好，再写那个引用它们的
     * 文件，中间失败就直接放弃、不留下半成品。
     */
    if (!assets.isEmpty()) {
        const QString assetsDir = dir + QStringLiteral("/assets");
        if (!QDir().mkpath(assetsDir))
            return QString();
        for (auto it = assets.constBegin(); it != assets.constEnd(); ++it) {
            /* 只取文件名：脚本给的名字理论上不带路径，带上也不能让它跑出 assets */
            const QString name = QFileInfo(it.key()).fileName();
            if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
                continue;
            QFile image(assetsDir + QLatin1Char('/') + name);
            if (image.open(QIODevice::WriteOnly | QIODevice::Truncate))
                image.write(it.value());
        }
    }

    /*
     * 正文：`# 标题` + 识别出来的 Markdown。
     *
     * 识别结果自己已经带标题（`# 文档名`）时不重复加 —— 否则笔记开头会有两行
     * 一模一样的标题。
     */
    QString body = markdown.trimmed();
    const QString heading = QStringLiteral("# %1")
                                .arg(title.trimmed().isEmpty() ? QStringLiteral("识别结果")
                                                               : title.trimmed());
    if (!body.startsWith(QLatin1Char('#')))
        body = heading + QStringLiteral("\n\n") + body;
    if (!body.endsWith(QLatin1Char('\n')))
        body += QLatin1Char('\n');

    const QString path = newFilePath(dir, now);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    const QByteArray bytes = body.toUtf8();
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    if (!ok) {
        QFile::remove(path);
        return QString();
    }

    reindexFile(path, false);
    recount();
    emit changed();
    return path;
}

bool ClipboardStore::renameFile(const QString &path, const QString &newName) {
    QString name = newName.trimmed();
    if (name.isEmpty())
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
        || name.contains(QLatin1Char(':')))
        return false;
    if (!name.endsWith(QStringLiteral(".md"), Qt::CaseInsensitive))
        name += QStringLiteral(".md");

    const QFileInfo info(path);
    if (!info.exists())
        return false;

    const QString from = info.absoluteFilePath();
    const QString to = info.absolutePath() + QLatin1Char('/') + name;
    if (from == to)
        return true;
    if (QFileInfo::exists(to))
        return false;
    if (!QFile::rename(from, to))
        return false;

    QSqlQuery query(m_db);
    query.prepare(QStringLiteral("UPDATE clip_files SET path = ?, folder = ? WHERE path = ?"));
    query.addBindValue(to);
    query.addBindValue(QFileInfo(to).absolutePath());
    query.addBindValue(from);
    query.exec();

    query.prepare(QStringLiteral("UPDATE clip_entries SET file_path = ? WHERE file_path = ?"));
    query.addBindValue(to);
    query.addBindValue(from);
    query.exec();

    recount();
    emit changed();
    return true;
}

bool ClipboardStore::deleteFile(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists())
        return false;

    /* 这一份引用到的图片：只有没有任何文件再引用它时才跟着删 */
    QStringList assets;
    QSqlQuery query(m_db);
    query.prepare(QStringLiteral(
        "SELECT DISTINCT asset FROM clip_entries WHERE file_path = ? AND asset IS NOT NULL"));
    query.addBindValue(path);
    if (query.exec()) {
        while (query.next()) {
            const QString asset = query.value(0).toString();
            if (!asset.isEmpty())
                assets.append(asset);
        }
    }

    forgetFile(path);
    if (!QFile::remove(path))
        return false;

    for (const QString &asset : std::as_const(assets)) {
        QSqlQuery rest(m_db);
        rest.prepare(QStringLiteral("SELECT COUNT(*) FROM clip_entries WHERE asset = ?"));
        rest.addBindValue(asset);
        if (rest.exec() && rest.next() && rest.value(0).toInt() == 0)
            QFile::remove(asset);
    }

    recount();
    emit changed();
    return true;
}

bool ClipboardStore::fileExists(const QString &path) const {
    return !path.isEmpty() && QFileInfo::exists(path);
}

QString ClipboardStore::textOf(const QString &path) const {
    return readAllText(path);
}

/* ------------------------------------------------------------------ */
/* 保存位置 / 导入的文件夹                                                */
/* ------------------------------------------------------------------ */

QStringList ClipboardStore::importedFolders() const {
    return m_imported;
}

QVariantList ClipboardStore::importedFolderList() const {
    QVariantList out;
    for (const QString &dir : m_imported)
        out.append(dir);
    return out;
}

bool ClipboardStore::setRootPath(const QString &path) {
    const QString clean = QDir::cleanPath(path.trimmed());
    if (clean.isEmpty() || clean == QLatin1String("."))
        return false;
    if (!QDir().mkpath(clean))
        return false;

    m_rootPath = clean;
    QSettings().setValue(settingsKey(QStringLiteral("root")), clean);

    /*
     * 换目录之后当前文件指针也就废了（captureTarget 会自己发现路径对不上），
     * 这里顺手清掉，免得留着一条指不到任何地方的记录。
     */
    QSqlQuery(m_db).exec(QStringLiteral("DELETE FROM clip_state"));

    rescan();
    emit rootPathChanged();
    return true;
}

bool ClipboardStore::addImportedFolder(const QString &path) {
    const QString clean = QDir::cleanPath(path.trimmed());
    if (clean.isEmpty() || !QDir(clean).exists())
        return false;
    if (m_imported.contains(clean))
        return false;

    m_imported.append(clean);
    QSettings().setValue(settingsKey(QStringLiteral("imported")), m_imported);

    rescan();
    emit importedFoldersChanged();
    return true;
}

bool ClipboardStore::removeImportedFolder(const QString &path) {
    const QString clean = QDir::cleanPath(path.trimmed());
    const int at = m_imported.indexOf(clean);
    if (at < 0)
        return false;

    m_imported.removeAt(at);
    QSettings().setValue(settingsKey(QStringLiteral("imported")), m_imported);

    rescan();
    emit importedFoldersChanged();
    return true;
}

/* ------------------------------------------------------------------ */
/* 程序自己写剪贴板                                                      */
/* ------------------------------------------------------------------ */

void ClipboardStore::markOwnCopy() {
    m_skipNextCapture = true;
    m_skipStamp = QDateTime::currentMSecsSinceEpoch();
}

bool ClipboardStore::takeSkipNextCapture() {
    if (!m_skipNextCapture)
        return false;
    m_skipNextCapture = false;

    /*
     * 过期的标记不算数。
     *
     * Windows 的剪贴板通知是**异步**回来的（WM_CLIPBOARDUPDATE），自己写进去
     * 那一次通常几毫秒就回来；但如果标记是"按了 Ctrl+C 却没选中任何东西"
     * 这种空操作留下的，它就一直挂着，用户下一次真正的外部复制会被它吃掉。
     * 所以只认"刚刚"那一次。
     */
    constexpr qint64 kValidMs = 2000;
    return QDateTime::currentMSecsSinceEpoch() - m_skipStamp <= kValidMs;
}

void ClipboardStore::copyText(const QString &text) {
    if (text.isEmpty())
        return;
    markOwnCopy();
    QGuiApplication::clipboard()->setText(text);
}
