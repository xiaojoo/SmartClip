#include "ClipboardStore.h"

#include <QClipboard>
#include <QDateTime>
#include <QDir>
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

}  // namespace

ClipboardStore::ClipboardStore(QObject *parent) : QObject(parent) {}

QString ClipboardStore::defaultRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
           + QStringLiteral("/SmartClip");
}

QString ClipboardStore::settingsKey(const QString &key) {
    return QStringLiteral("clip/") + key;
}

QString ClipboardStore::dateDir(const QString &dateKey) const {
    return m_rootPath + QLatin1Char('/') + dateKey;
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
     * 元数据库还是放在应用数据目录（%APPDATA%/SmartClip/SmartClip/smartclip.db）——
     * 它是"这台机器上这份安装的索引"，和内容放哪儿无关：内容目录可以指到网盘，
     * 索引跟着本机走反而更合适（换目录不用重建）。
     */
    m_db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"));
    m_db.setDatabaseName(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                         + QStringLiteral("/smartclip.db"));
    if (!m_db.open())
        return false;

    QSqlQuery query(m_db);
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

void ClipboardStore::refreshFileRow(const QString &filePath, const QString &dateKey, bool imported) {
    const QFileInfo info(filePath);

    int entries = 0;
    QSqlQuery count(m_db);
    count.prepare(QStringLiteral("SELECT COUNT(*) FROM clip_entries WHERE file_path = ?"));
    count.addBindValue(filePath);
    if (count.exec() && count.next())
        entries = count.value(0).toInt();

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

    QDir root(m_rootPath);
    if (root.exists()) {
        const QFileInfoList dirs =
            root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &dir : dirs) {
            /* 根目录下只认 "2026-09-13" 这种日期目录，别把用户别的文件夹也索引进来 */
            if (!dateDirPattern().match(dir.fileName()).hasMatch())
                continue;
            scanFolder(dir.absoluteFilePath(), false, seen, 0);
        }
    }

    for (const QString &imported : std::as_const(m_imported))
        scanFolder(QDir::cleanPath(imported), true, seen, 0);

    /* 缓存里有、这次没扫到的：文件被删掉或改名了，元数据跟着清 */
    QStringList stale;
    QSqlQuery query(m_db);
    if (query.exec(QStringLiteral("SELECT path FROM clip_files"))) {
        while (query.next()) {
            const QString path = query.value(0).toString();
            if (!seen.contains(path))
                stale.append(path);
        }
    }
    for (const QString &path : std::as_const(stale))
        forgetFile(path);

    recount();
    emit changed();
}

void ClipboardStore::scanFolder(const QString &dir, bool imported, QSet<QString> &seen, int depth) {
    QDir folder(dir);
    if (!folder.exists())
        return;

    const QFileInfoList files = folder.entryInfoList(
        { QStringLiteral("*.md"), QStringLiteral("*.markdown"), QStringLiteral("*.txt") },
        QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &file : files) {
        if (!imported && file.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive) != 0)
            continue;   /* 自己的日期目录里只认 md */
        seen.insert(file.absoluteFilePath());
        reindexFile(file.absoluteFilePath(), imported);
    }

    /* 只有导入的文件夹往下钻：自己的日期目录里只有 md 和一个 assets 子目录 */
    if (!imported || depth >= kMaxDepth)
        return;

    const QFileInfoList subDirs =
        folder.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &sub : subDirs) {
        if (sub.fileName().compare(QLatin1String("assets"), Qt::CaseInsensitive) == 0)
            continue;
        scanFolder(sub.absoluteFilePath(), imported, seen, depth + 1);
    }
}

void ClipboardStore::reindexFile(const QString &path, bool imported) {
    const QFileInfo info(path);
    const QString stamp = info.lastModified().toString(Qt::ISODate);

    /* 大小和修改时间都没动过 -> 这个文件的元数据还是对的，不用重新解析 */
    QSqlQuery cached(m_db);
    cached.prepare(QStringLiteral("SELECT size, mtime, imported FROM clip_files WHERE path = ?"));
    cached.addBindValue(path);
    if (cached.exec() && cached.next()
        && cached.value(0).toLongLong() == info.size()
        && cached.value(1).toString() == stamp
        && cached.value(2).toBool() == imported)
        return;

    const QString text = readAllText(path);

    /* 日期取上一级目录名（2026-09-13）；不是日期目录就用文件自己的时间 */
    QString dateKey = info.dir().dirName();
    if (!dateDirPattern().match(dateKey).hasMatch())
        dateKey = info.lastModified().toString(QStringLiteral("yyyy-MM-dd"));

    forgetFile(path);

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
    }

    refreshFileRow(path, dateKey, imported);
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
            chain << QDir::cleanPath(file.folder);
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
                node.chronological = isDate;
                node.kind = isDate ? QStringLiteral("date")
                                   : (i == 0 ? QStringLiteral("imported")
                                             : QStringLiteral("folder"));
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
        out.insert(QStringLiteral("children"), children);
        return out;
    };

    QList<Node *> top = roots;
    std::sort(top.begin(), top.end(), [newestFirst](const Node *a, const Node *b) {
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
