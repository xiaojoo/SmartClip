#pragma once

#include <QObject>
#include <QVariantList>
#include <QSqlDatabase>

class QImage;

class ClipboardStore final : public QObject {
    Q_OBJECT
public:
    explicit ClipboardStore(QObject *parent = nullptr);
    bool open();
    bool addText(const QString &text);
    bool addImage(const QImage &image);
    /*
     * 列表数据。
     *
     * 注意：这里的 "content" 字段给的是**摘要**（单行、截断），
     * 不是正文。正文一律不进 QML —— 见下面 previewFor() 的说明。
     */
    Q_INVOKABLE QVariantList items(const QString &query = {}) const;

    Q_INVOKABLE void copyItem(qint64 id) const;

    /*
     * 按 id 取正文（或图片路径）。
     *
     * 正文不进 QML 之后，需要看正文的地方（编辑器）要用到它时现取。
     * 图片条目返回的是文件路径，和历史行为一致。
     */
    Q_INVOKABLE QString contentOf(qint64 id) const;


    /*
     * 把任意文本写回系统剪贴板。
     *
     * 给"复制此行"用：虚拟化正文没有跨行选区，复制粒度是行，
     * 而那一行是正文里的一个片段、在库里没有自己的条目，
     * 所以不能走 copyItem(id)。
     *
     * 同样要置 skipNextCapture：这次写入会触发 QClipboard::dataChanged，
     * 不挡掉的话采集线程会把刚复制的内容又当成新剪贴板内容存一遍。
     */
    Q_INVOKABLE void copyText(const QString &text) const;

    bool takeSkipNextCapture();

signals:
    void changed();

private:
    QString dataDir() const;
    QString titleFor(const QString &text) const;

    /*
     * 给列表用的正文摘要。
     *
     * 为什么必须有这个东西：
     * 库里最长的条目有 66 万字符。原来 items() 把整篇 content 塞进
     * QVariantList 交给 QML，于是"点开一条 3000 行的条目"这个动作里，
     * 光是把这个 25 万字符的值赋给 QML 属性就同步阻塞 2078ms（实测，
     * 而且关掉虚拟化视图也一样慢，所以跟渲染无关）。
     *
     * 列表只需要一行摘要，所以这里把正文压成单行、限长，
     * 正文改由 contentOf(id) 按需取。
     */
    static QString previewFor(const QString &text);

    QSqlDatabase m_db;
    mutable bool m_skipNextCapture = false;
};
