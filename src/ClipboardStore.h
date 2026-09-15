#pragma once

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QSqlDatabase>
#include <QStringList>
#include <QVariantList>

class QDateTime;
class QImage;

/*
 * 剪贴板内容的落盘 + 元数据。
 *
 * ===========================================================================
 * 内容不进数据库
 * ===========================================================================
 * 剪贴板内容全部落成**真实文件**，数据库里只留元数据（文件清单 + 每条的
 * 时间、类型、标题、摘要、去重哈希）。这样做的直接好处：
 *   * 内容用任何编辑器 / 记事本 / Markdown 阅读器都能直接看、直接改；
 *   * 保存目录指向网盘就同步走了，不需要这个程序在场；
 *   * 库文件小，坏了大不了重扫一遍磁盘重建（见 rescan()）。
 *
 * 磁盘布局（rootPath 可在设置面板里改，默认「文档/SmartClip」）：
 *
 *     <root>/2026-09-13/073100.md          当天的剪贴板内容
 *     <root>/2026-09-13/073545.md          前一个文件超过 20K 之后的新文件
 *     <root>/2026-09-13/assets/*.png       图片（md 里用相对路径引用）
 *
 * 文件名 = 写进这个文件的**第一条内容的时间**（时分秒），所以名字本身就
 * 说明"这个文件是从几点几分开始记的"，按名字排序天然是时间顺序。
 *
 * 文件内容格式（追加，一段一条）：
 *
 *     # 2026-09-13
 *
 *     ## 07:31:00
 *
 *     剪贴板正文…
 *
 * 图片那一段的正文是 `![图片](assets/20260913-073100-123.png)` ——
 * 存的是相对路径，整个日期目录搬走也还指向得到。
 */
class ClipboardStore final : public QObject {
    Q_OBJECT

    /* 剪贴板文件的保存根目录（设置面板里可改） */
    Q_PROPERTY(QString rootPath READ rootPath NOTIFY rootPathChanged)
    /*
     * 用户导入的"外部文件夹"（整棵目录照原样看，新内容不会写进去）。
     *
     * 暴露给 QML 的是 QVariantList 而不是 QStringList：QStringList 在 QML 里
     * 当属性读时会被当成"序列"再转一层，直接取 .length / 下标会报
     * "Sequence length out of range"（设置面板那一栏踩过）。QVariantList
     * 就是普通的 JS 数组，读起来没有这些坑。
     */
    Q_PROPERTY(QVariantList importedFolders READ importedFolderList NOTIFY importedFoldersChanged)

    /* 状态栏用：一共多少个文件 / 多少条内容 */
    Q_PROPERTY(int fileCount READ fileCount NOTIFY changed)
    Q_PROPERTY(int entryCount READ entryCount NOTIFY changed)

public:
    explicit ClipboardStore(QObject *parent = nullptr);

    /*
     * 打开元数据库并扫一遍磁盘。
     *
     * 会把上一版那个"正文存在库里"的 clipboard_items 表**直接删掉**
     * （连同旧的那批 images/*.png，见 .cpp 的 migrateFromLegacy()）。
     */
    bool open();

    /* ---- 剪贴板采集（ClipboardManager 调） ---- */

    /* 文本：去重之后追加到当天的 md；真的写进去了返回 true */
    bool captureText(const QString &text);
    /* 图片：PNG 落到当天目录的 assets/，md 里写一条引用 */
    bool captureImage(const QImage &image);

    /* ---- "程序自己写进剪贴板的内容不采集" ---- */

    /*
     * 标记"这一次剪贴板变化是我们自己造成的"。
     *
     * 编辑器里的 Ctrl+C / 菜单里的复制 / 点列表回填剪贴板……都会触发
     * QClipboard::dataChanged，不挡掉的话刚复制的正文立刻又被采集一遍。
     * 见 ClipboardManager::capture 的第一行。
     *
     * 这个标记只在 markOwnCopy() 之后的 2 秒内有效（见 .cpp 的
     * takeSkipNextCapture）：Windows 的剪贴板通知是异步回来的，正常
     * 几毫秒就到；真要是因为"按了 Ctrl+C 但没选中任何东西"这类空操作
     * 把标记留在那儿，它自己会过期，不会把用户下一次真正的外部复制吃掉。
     */
    void markOwnCopy();
    bool takeSkipNextCapture();

    /* 把文本写回系统剪贴板（编辑器的"复制全文 / 复制此行"走这里） */
    Q_INVOKABLE void copyText(const QString &text);

    /* ---- 保存位置 / 导入的文件夹 ---- */

    QString rootPath() const { return m_rootPath; }
    /* 换根目录：建目录、重扫、记进 QSettings；失败返回 false */
    Q_INVOKABLE bool setRootPath(const QString &path);

    QStringList importedFolders() const;
    /* QML 侧读的那一份（QVariantList，见上面 Q_PROPERTY 的说明） */
    QVariantList importedFolderList() const;
    Q_INVOKABLE bool addImportedFolder(const QString &path);
    Q_INVOKABLE bool removeImportedFolder(const QString &path);

    /* ---- 左树数据 ---- */

    /*
     * 返回**嵌套**的树：日期文件夹 -> md 文件（导入的文件夹是另一棵，
     * 它里面的子目录会一层层挂下去）。
     *
     * query 非空时只留命中它的文件（按条目的标题 / 摘要匹配，见 searchFiles）。
     *
     * 文件夹节点 { key, label, kind("date"/"imported"/"folder"), path, depth,
     *              files, entries, children: [...] }
     * 文件节点   { key(path), label, kind: "file", path, depth, entries, size, imported }
     */
    Q_INVOKABLE QVariantList tree(const QString &query = QString(), bool newestFirst = true) const;

    /* 重扫磁盘刷新元数据缓存（启动 / F5 / 建删改文件之后都会调） */
    Q_INVOKABLE void rescan();

    /* ---- 文件操作 ---- */

    /* 在今天这一组里新建一个 md 并返回它的路径（左侧树那个 "+"）；失败返回空串 */
    Q_INVOKABLE QString createFile(const QString &text = QString());

    /*
     * 新建一份**带完整正文**的笔记（文档识别那条路用的，见 src/DocImport.h）。
     *
     * 和 createFile 的区别：那个是"空白笔记 + 追加一段"，标题固定成日期和时间，
     * 正文由调用方自己拼；这里是"这份内容就是整篇"，标题是文档名，图片要一起
     * 落进 assets/。
     *
     * assets 的键是正文里引用的**文件名**（DocConvert 已经把引用改写成
     * `assets/<名字>` 了），值是图片数据。写不进去的图跳过，不影响正文。
     *
     * 返回新文件的路径；失败返回空串。
     */
    QString createNote(const QString &title, const QString &markdown,
                       const QMap<QString, QByteArray> &assets);
    /* 改名（只改文件名，不动目录）；同名文件已存在则失败 */
    Q_INVOKABLE bool renameFile(const QString &path, const QString &newName);
    /* 删文件（它引用到的图片一起清掉，除非还有别的文件引用） */
    Q_INVOKABLE bool deleteFile(const QString &path);
    Q_INVOKABLE bool fileExists(const QString &path) const;
    /* 读文件正文（"复制全文"这类用；失败返回空串） */
    Q_INVOKABLE QString textOf(const QString &path) const;

    int fileCount() const { return m_fileCount; }
    int entryCount() const { return m_entryCount; }

signals:
    /* 内容 / 文件有变化：QML 侧据此重建左树 */
    void changed();
    void rootPathChanged();
    void importedFoldersChanged();

private:
    /*
     * 往当天的 md 里追加一条。
     *
     * now       这条内容的时间（文件名、标题里的时间都用它）
     * type      "text" / "image"
     * title     摘要标题（左树和搜索用）
     * payload   写进文件的那一段正文（图片那一段是 markdown 引用）
     * hash      文本去重用（图片给空串 = 不去重）
     * assetPath 图片文件路径（文本给空串）
     */
    bool appendEntry(const QDateTime &now, const QString &type, const QString &title,
                     const QString &payload, const QString &hash, const QString &assetPath);

    QString dateDir(const QString &dateKey) const;
    /*
     * 当前该往哪个文件里追加（今天这一组）。
     *
     * 记在 clip_state 表里，而不是"目录里最新的那个 md" —— 后者会把
     * 用户自己新建 / 手写的 md 也一起append，而那一份是人家在写的笔记。
     */
    QString captureTarget(const QString &dateKey, const QString &dir) const;
    /* 该目录里下一个可用的 <时分秒>.md（同一秒撞名就 -2、-3…） */
    QString newFilePath(const QString &dir, const QDateTime &now) const;

    /* 元数据：插一条条目 + 更新它所在文件的汇总行 */
    void recordEntry(const QString &filePath, const QDateTime &when, const QString &type,
                     const QString &title, const QString &preview, qint64 bytes,
                     const QString &hash, const QString &assetPath);
    /* entries >= 0 = 已知条数（不再 SELECT COUNT，见 .cpp） */
    void refreshFileRow(const QString &filePath, const QString &dateKey, bool imported,
                        int entries = -1);
    void forgetFile(const QString &filePath);
    bool hashExists(const QString &hash) const;
    /* 搜索：命中 query 的文件路径集合（按条目标题 / 摘要 + 文件名） */
    QSet<QString> searchFiles(const QString &query) const;

    /*
     * 上次扫盘时一个文件的状态（rescan 时整表读进内存，见 .cpp）。
     */
    struct FileCacheEntry {
        qint64 size = 0;
        QString mtime;
        bool imported = false;
    };

    /*
     * 扫一个目录（imported = 是不是"导入的文件夹"）。
     *
     * 导入的那种是**整棵原样**收：什么后缀都算（cpp / xml / png…），隐藏目录
     * （.idea）也进去；自己的日期目录只认 md。
     *
     * cache 是"上次扫盘的结果"（路径 -> 大小 / 时间 / 是否导入），rescan 时整表
     * 读进内存带下来 —— 逐文件去查数据库，三万个文件就是三万次 prepare+exec，
     * 那才是导入大目录卡住的主因（见 .cpp 里 rescan 的说明）。
     */
    void scanFolder(const QString &dir, bool imported, QSet<QString> &seen, int depth,
                    QHash<QString, FileCacheEntry> *cache);
    /*
     * 把一个文件解析成条目元数据（文件没变过就直接返回）。
     *
     * 自己写的文件按 "## 时分秒" 分段；外部文件（导入的文件夹里那些）
     * 没有这个记号时整篇算一条，时间取文件的修改时间。二进制（png / exe…）
     * 不读内容，"条数"记 0 —— 见 .cpp 里 readTextFile 的说明。
     */
    void reindexFile(const QString &path, bool imported,
                     QHash<QString, FileCacheEntry> *cache = nullptr);

    void recount();

    /*
     * 上一版把正文存在 SQLite 里（clipboard_items 表 + images/*.png）。
     * 这一版内容全在文件里，旧表对不上了：连表带图片一起清掉。
     */
    void migrateFromLegacy();

    static QString defaultRoot();
    static QString settingsKey(const QString &key);
    static QString titleFor(const QString &text);
    static QString previewFor(const QString &text);

    QSqlDatabase m_db;
    QString m_rootPath;
    QStringList m_imported;
    /*
     * 导入目录里扫到的**每一个**目录（含隐藏目录、空目录），每次 rescan 重填。
     *
     * 树是从 clip_files 那个表里的**文件**拼出来的（见 tree()），所以"一个文件
     * 都没有的目录"根本不会有节点 —— 用户报的 H:\test\.idea 就是这样消失的。
     * 扫盘时顺手把目录也记下来，拼树时补上。
     */
    QSet<QString> m_importedDirs;
    mutable bool m_skipNextCapture = false;
    /* 标记是什么时候置上的（见 takeSkipNextCapture 的 2 秒有效期） */
    qint64 m_skipStamp = 0;

    int m_fileCount = 0;
    int m_entryCount = 0;

    /* md 写满多少字节就换下一个文件（20K） */
    static constexpr qint64 kFileLimit = 20 * 1024;
    /* 导入的文件夹往下钻几层（防止软链接套娃） */
    static constexpr int kMaxDepth = 8;
};
