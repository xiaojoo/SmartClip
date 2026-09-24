#pragma once

#include <QColor>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

class QFileSystemWatcher;
class QTimer;

/*
 * 主题（QML 单例 Theme，注册见 src/main.cpp；色表在 src/Theme.cpp）。
 *
 * ---------------------------------------------------------------------------
 * 为什么是"按深色值查表"，而不是给每个颜色起一个 token 名
 * ---------------------------------------------------------------------------
 * 深色是历史遗留地散在代码里的：清点过一遍，范围内（主窗口 + 编辑器 + 终端）
 * 有 74 个不同的十六进制字面量、约 200 处使用。一处一处改名成 token 要人判断
 * 两百次，改错一处就是一个"看着对、其实错位"的坑。这里换成：
 *
 *      color: Theme.c("#313335", Theme.light)
 *
 * 表里**没有**的色原样返回 —— 也就是深色档是恒等映射。这条性质很重要：
 * 切回深色时界面和改造前逐字节一样，自检里那条"切回来必须等于原值"就是钉这个的。
 * 浅色只需要维护 src/Theme.cpp 里那一张表，不用再去 200 个地方各改一遍。
 *
 * 返回 QString 而不是 QColor 也是同一类考虑：原来那些位置写的是字符串字面量，
 * 其中有一批是传给原生侧的 QString 属性（DiffPane 的 paperColor / textColor
 * 那几条，见 qml/components/DiffPane.qml:669），返回 QColor 会在那些地方直接断掉。
 *
 * ---------------------------------------------------------------------------
 * 第二个参数不是冗余
 * ---------------------------------------------------------------------------
 * QML **不追函数调用内部的依赖**（这个工程踩过两次，见 Main.qml 里 mapToItem
 * 那段注释）。函数体里读 m_light，绑定是醒不过来的。把 Theme.light 当参数传进去，
 * 属性读取发生在绑定表达式里，切换时才会重算。
 * C++ 侧不用传：自己读 m_light，并接 lightChanged 重画。
 *
 * ---------------------------------------------------------------------------
 * 这一版覆盖到哪
 * ---------------------------------------------------------------------------
 * 主窗口 + 编辑器 + 终端。便签窗口、翻译卡片、识别卡片、截图覆盖层、DocCard
 * 还是深色（那几块单独一轮再扫，见 BACKLOG）。
 */
class AppTheme final : public QObject {
    Q_OBJECT

    /*  false = 深色（默认，和这个工程一直以来的样子一致）；true = 白色 */
    Q_PROPERTY(bool light READ light WRITE setLight NOTIFY lightChanged)

    /*
     * 换一次方案就 +1。**QML 里凡是 Theme.c("#xxx", ...) 的第二个参数都传它**。
     *
     * 为什么不能继续传 Theme.light：QML 的绑定只追踪表达式里出现过的属性，
     * 传 Theme.light 的话，"从 Light 换成另一套浅底方案"这种 light 值不变的切换，
     * 全界面一个像素都不会重画 —— 得重启才算数。rev 每次换方案必变，
     * 绑定就一定能醒。light 留给"这一档到底是深还是浅"那些真正的分支用。
     */
    Q_PROPERTY(int rev READ rev NOTIFY revChanged)

    /*
     * 方案这一组必须都是 Q_PROPERTY：只写 getter 的话 QML 读到的是 undefined，
     * 于是设置里那一栏**安静地空着**（Repeater 的 model 是 undefined → 0 条），
     * 后端 setScheme() 却完全正常 —— 自检那条"列出 Dark / Light"就是这么抓出来的。
     */
    Q_PROPERTY(QString scheme READ scheme NOTIFY schemeChanged)
    Q_PROPERTY(QString schemeError READ schemeError NOTIFY schemeErrorChanged)
    Q_PROPERTY(QStringList schemeNames READ schemeNames NOTIFY schemeFilesChanged)

public:
    explicit AppTheme(QObject *parent = nullptr);

    static AppTheme *instance() { return s_self; }

    bool light() const { return m_light; }
    void setLight(bool on);
    int rev() const { return m_rev; }

    /* QML：Theme.toggle() 翻一档并落盘 */
    Q_INVOKABLE void toggle();

    /* QML 走这个（带 light 参数，见上面那条"第二个参数不是冗余"） */
    Q_INVOKABLE QString c(const QString &darkHex, bool lightMode) const;
    /* C++ 走这个 */
    QString c(const QString &darkHex) const { return c(darkHex, m_light); }
    QColor color(const QString &darkHex) const { return QColor(c(darkHex)); }

    /* 原生侧要的几个语义位：这些地方原来不是字面量，是算出来/传进来的 */
    QColor chrome() const { return color(QStringLiteral("#313335")); }
    QColor paper() const { return color(QStringLiteral("#1e1f22")); }
    QColor ink() const { return color(QStringLiteral("#d6d7da")); }
    QColor line() const { return color(QStringLiteral("#4b4d4f")); }

    /*
     * 终端那 16 色 ANSI 调色板。空表 = 用 libvterm 自带那一份（内置 Dark 就是空表），
     * 所以切回 Dark 时逐格和改造前一样。
     *
     * 原来这一张表写死在代码里（浅色档 16 个色是 2026-09-24 按白底重挑的：
     * libvterm 的 3 号是 #e0e000 纯黄，PowerShell 提示符那条路径正用它，白底上读不出来）。
     * 现在它从**配色方案**里读，见下面 schemesDir() / setScheme()。
     */
    QVector<QColor> ansiPalette() const;

    // ------------------------------------------------------------------
    // 配色方案（照 IDEA 那套：内置只读 + 用户文件 + 改完即生效）
    // ------------------------------------------------------------------

    /* 当前方案名："Dark" / "Light" / 用户文件名（不含 .json） */
    QString scheme() const { return m_scheme; }
    /* 能选哪些：内置两档 + 方案目录里的 *.json */
    QStringList schemeNames() const;
    /*
     * 上一次加载有什么毛病（JSON 读坏了、某个色写错了、没人用的键）。
     * 空 = 干净。设置页顶部红字就是它 —— 手改文件的人第一眼要看得见。
     */
    QString schemeError() const { return m_error; }
    /* 方案目录（不存在就建出来），"打开所在文件夹"用 */
    Q_INVOKABLE QString schemesDir() const;

    /* 切方案：写盘（QSettings ui/scheme），并同步 ui/theme 那一档 */
    Q_INVOKABLE void setScheme(const QString &name);
    /* 重读目录和当前文件（文件监视没盯住时手动兜底） */
    Q_INVOKABLE void reloadSchemes() { applyScheme(m_scheme); }
    /*
     * 把**当前生效的整张表**写成一份用户方案（带 _doc：每个键是界面上哪一块）。
     * 成功返回空串，失败返回一句人话。设置页"另存为…"走这条。
     */
    Q_INVOKABLE QString saveSchemeAs(const QString &name);

    /* 落盘用的键（QSettings：ui/theme = "light" / "dark"，ui/scheme = 方案名） */
    static constexpr const char *kKey = "ui/theme";
    static constexpr const char *kSchemeKey = "ui/scheme";

signals:
    void lightChanged();
    void revChanged();
    void schemeChanged();
    void schemeErrorChanged();
    void schemeFilesChanged();

private:
    bool m_light = false;
    int m_rev = 0;
    QString m_scheme = QStringLiteral("Dark");
    /* 有效表 = 内置那一档（Dark 空 / Light 84 行）叠上用户文件的 ui 段 */
    QHash<QString, QString> m_ui;
    /* 16 项按索引对齐；invalid = 这一格用 libvterm 自带 */
    QVector<QColor> m_ansi;
    QString m_error;

    /* 读文件 + 合并 + 记账（m_ui / m_ansi / m_error / m_light），最后发信号 */
    void applyScheme(const QString &name);
    /* 把方案目录和当前文件都挂到监视器上（编辑器"先删后写"会把文件级监视弄丢） */
    void watchSchemeFiles();

    QFileSystemWatcher *m_watch = nullptr;
    /* 一次保存常常连着发好几个信号，攒 150ms 只重载一次（不然会连着重刷好几遍） */
    QTimer *m_reloadTimer = nullptr;

    static AppTheme *s_self;
};
