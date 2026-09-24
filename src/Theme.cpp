#include "Theme.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

AppTheme *AppTheme::s_self = nullptr;

namespace {

/*
 * 内置方案 "Light" 的那张表（"Dark" 是空表 = 恒等）。
 *
 * 抄的是 IntelliJ / PyCharm 的 "IntelliJ Light"：面板 #f2f2f2、内容底 #ffffff、
 * 正文 #000000、边框 #c4c4c4、语法四件套是藏青关键字 / 绿字符串 / 蓝数字 / 灰注释。
 * 强调蓝 #4c96d8 两边共用（所以它不在表里 —— 表里没有的按恒等处理）。
 *
 * 每一行左边那个深色就是代码里写着的值，注释标它在这儿是干什么的。
 * 这张表现在是**方案的默认值**：用户可以在 %APPDATA%/SmartClip/schemes/*.json
 * 里按同样的"深色值 → 新值"格式覆盖任意几条，没写的沿用这里（见 applyScheme）。
 * 想改配色不必再动这一份、也不必重编：设置 → 配色方案 → 另存为…，然后改文件。
 */
const QHash<QString, QString> &builtinLightUi() {
    static const QHash<QString, QString> kTable {
        // ---- 面：从最底到浮起 ----
        { QStringLiteral("#313335"), QStringLiteral("#f2f3f5") },  // 窗口底 / 图标条 / 顶栏 / 底栏
        { QStringLiteral("#26282b"), QStringLiteral("#f2f3f5") },  // 设置面板侧栏、列表底色
        { QStringLiteral("#2b2d30"), QStringLiteral("#ffffff") },  // 气泡、选中标签、输入框
        { QStringLiteral("#3c3f41"), QStringLiteral("#ffffff") },  // 下拉菜单 / 确认卡 / 查找条面板
        { QStringLiteral("#1e1f22"), QStringLiteral("#ffffff") },  // 编辑区纸色
        { QStringLiteral("#1e1f22@tabstrip"), QStringLiteral("#f2f3f5") },  // 页签条底：和纸色同一个深色值，浅色下要"灰面压白卡"，所以带角色才分得开
        { QStringLiteral("#252526"), QStringLiteral("#f2f3f5") },  // 编辑器旁的窄侧栏
        { QStringLiteral("#2d2d30"), QStringLiteral("#ebebeb") },  // 另一种选中底
        { QStringLiteral("#404043"), QStringLiteral("#dcdcdc") },  // 滚动条槽 / 分隔带
        // ---- 面：hover / 次级 ----
        { QStringLiteral("#3a3d41"), QStringLiteral("#e1e4e8") },
        { QStringLiteral("#3a3a3d"), QStringLiteral("#e0e0e0") },
        { QStringLiteral("#3a3e42"), QStringLiteral("#e1e1e1") },
        { QStringLiteral("#3c3f44"), QStringLiteral("#e4e4e4") },
        { QStringLiteral("#33363a"), QStringLiteral("#f0f0f0") },
        { QStringLiteral("#333840"), QStringLiteral("#e5e6eb") },
        { QStringLiteral("#34373b"), QStringLiteral("#f0f0f0") },
        { QStringLiteral("#2f3234"), QStringLiteral("#ededed") },
        { QStringLiteral("#2a2d2e"), QStringLiteral("#eaeaea") },
        { QStringLiteral("#2b2d2f"), QStringLiteral("#ebebeb") },
        { QStringLiteral("#1e2023"), QStringLiteral("#fdfdfd") },
        { QStringLiteral("#1e1f21"), QStringLiteral("#fefefe") },
        // ---- 描边 / 分隔线 ----
        { QStringLiteral("#4b4d4f"), QStringLiteral("#c9cdd4") },  // 主边框（窗口描边也用它）
        { QStringLiteral("#46484a"), QStringLiteral("#e5e6eb") },
        { QStringLiteral("#45484c"), QStringLiteral("#e5e6eb") },
        { QStringLiteral("#43454a"), QStringLiteral("#e5e6eb") },
        { QStringLiteral("#484c50"), QStringLiteral("#e5e6eb") },
        { QStringLiteral("#4a4d50"), QStringLiteral("#e5e6eb") },
        { QStringLiteral("#4d5157"), QStringLiteral("#e5e6eb") },
        { QStringLiteral("#55585d"), QStringLiteral("#86909c") },
        { QStringLiteral("#565a60"), QStringLiteral("#86909c") },
        { QStringLiteral("#5c6066"), QStringLiteral("#4e5969") },
        { QStringLiteral("#606366"), QStringLiteral("#4e5969") },
        // ---- 文字 ----
        { QStringLiteral("#e8e8e8"), QStringLiteral("#1d2129") },  // 正文 / 强调文字
        { QStringLiteral("#d6d7da"), QStringLiteral("#1d2129") },  // 气泡文字、语法默认色
        { QStringLiteral("#ced0d6"), QStringLiteral("#4e5969") },
        { QStringLiteral("#c8ccd1"), QStringLiteral("#4e5969") },
        { QStringLiteral("#cccccc"), QStringLiteral("#1d2129") },
        { QStringLiteral("#d4d4d4"), QStringLiteral("#1d2129") },
        { QStringLiteral("#bbbbbb"), QStringLiteral("#1d2129") },  // 标签 / 页签主文字
        { QStringLiteral("#b4b8bf"), QStringLiteral("#4e5969") },
        { QStringLiteral("#9aa0a8"), QStringLiteral("#4e5969") },  // 图标常态色
        { QStringLiteral("#9aa0a6"), QStringLiteral("#4e5969") },
        { QStringLiteral("#8a9098"), QStringLiteral("#4e5969") },
        { QStringLiteral("#8a9199"), QStringLiteral("#4e5969") },
        { QStringLiteral("#8b929e"), QStringLiteral("#4e5969") },
        { QStringLiteral("#7d838c"), QStringLiteral("#86909c") },
        { QStringLiteral("#77808c"), QStringLiteral("#86909c") },
        { QStringLiteral("#7d7d7d"), QStringLiteral("#4e5969") },  // muted 文字
        { QStringLiteral("#6f737a"), QStringLiteral("#86909c") },
        { QStringLiteral("#6f767e"), QStringLiteral("#86909c") },
        // ---- 选中 / 链接 ----
        { QStringLiteral("#3a4a5a"), QStringLiteral("#d2e7f7") },  // 图标条那一格的选中底
        { QStringLiteral("#2f3a44"), QStringLiteral("#d3e5f5") },  // 列表选中行
        { QStringLiteral("#2c3f52"), QStringLiteral("#bcd9f2") },  // 更强的选中
        { QStringLiteral("#0e639c"), QStringLiteral("#1177bb") },
        { QStringLiteral("#3d78b8"), QStringLiteral("#2f6f9f") },
        { QStringLiteral("#7fa8c8"), QStringLiteral("#4a7fa5") },
        { QStringLiteral("#214283"), QStringLiteral("#1a4d8f") },
        { QStringLiteral("#1653cb"), QStringLiteral("#a8cdf5") },  // 编辑器选中底（2026-09-24：他先给 #2b6be8，又说"再深一点"→ 按 HSL 亮度 ×0.82 推到 #1653cb；浅色档仍是 #a8cdf5）
        // ---- 语义：错 / 警 / 增删 ----
        { QStringLiteral("#c8503c"), QStringLiteral("#c0342a") },
        { QStringLiteral("#e06c75"), QStringLiteral("#cc4b51") },
        { QStringLiteral("#ff3b30"), QStringLiteral("#d1342b") },
        { QStringLiteral("#c75450"), QStringLiteral("#b52f28") },
        { QStringLiteral("#c42b1c"), QStringLiteral("#a02010") },
        { QStringLiteral("#7a4448"), QStringLiteral("#ffd7d7") },  // 删除行的底
        { QStringLiteral("#3a2224"), QStringLiteral("#ffecec") },
        { QStringLiteral("#1e3524"), QStringLiteral("#e6ffec") },  // 新增行的底
        { QStringLiteral("#7bc47f"), QStringLiteral("#2f7d32") },  // 新增行的字
        { QStringLiteral("#3f6b48"), QStringLiteral("#2f7d32") },
        { QStringLiteral("#d7a85b"), QStringLiteral("#ad6800") },
        { QStringLiteral("#c8b74f"), QStringLiteral("#1d2129") },
        { QStringLiteral("#8a7a3c"), QStringLiteral("#ad6800") },
        { QStringLiteral("#63572c"), QStringLiteral("#fff3c4") },  // 查找命中的底
        { QStringLiteral("#3a3320"), QStringLiteral("#fff8dc") },
        // ---- 预览那份 HTML 的 CSS（src/EditorController.cpp 的 kMarkdownCss）----
        { QStringLiteral("#26282c"), QStringLiteral("#f2f3f5") },  // 代码块底
        { QStringLiteral("#d7ba7d"), QStringLiteral("#a15c00") },  // 行内 code 的字

        // ---- 语法高亮（src/EditorViewItem.cpp 那 11 个角色）----
        { QStringLiteral("#cf8e6d"), QStringLiteral("#000080") },  // 关键字
        { QStringLiteral("#6aab73"), QStringLiteral("#008000") },  // 字符串
        { QStringLiteral("#2aacb8"), QStringLiteral("#0000ff") },  // 数字
        { QStringLiteral("#7a7e85"), QStringLiteral("#808080") },  // 注释
        { QStringLiteral("#56a8f5"), QStringLiteral("#1d2129") },  // 函数
        { QStringLiteral("#c77dbb"), QStringLiteral("#1d2129") },  // 类型
        { QStringLiteral("#bcbec4"), QStringLiteral("#1d2129") },  // 运算符
        { QStringLiteral("#b3ae60"), QStringLiteral("#000080") },  // 预处理
        { QStringLiteral("#e8bf6a"), QStringLiteral("#000080") },  // 标签
        { QStringLiteral("#ff6b68"), QStringLiteral("#ff0000") },  // 错误
    };
    return kTable;
}

QString keyOf(const QString &hex) {
    return hex.trimmed().toLower();
}

/* 定义在下面那个匿名 namespace 里（同一个未命名空间，先声明给 parseFontSection 用） */
void appendErr(QString &err, const QString &file, const QString &what);

/*
 * 方案文件里 ui 段的键：一个色值，后面可以跟 "@角色"（见 AppTheme::c）。
 * 键写错了查表永远查不到，界面上就是"我改了没变" —— 和值写错同一类，必须拦下来报出去。
 */
bool isUiKey(const QString &s) {
    static const QRegularExpression re(
        QStringLiteral("^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})(@[A-Za-z][A-Za-z0-9]*)?$"));
    return re.match(s.trimmed()).hasMatch();
}

/* "#rrggbb" / "#rgb"，别的都不认（方案文件是人手改的，宁可认严一点） */
bool isHexColor(const QString &s) {
    static const QRegularExpression re(QStringLiteral("^#([0-9a-fA-F]{3}|[0-9a-fA-F]{6})$"));
    return re.match(s.trimmed()).hasMatch();
}

/*
 * 内置方案 "Light" 的终端 16 色。右边那列是 libvterm 自带的值，**实测**从引擎里问
 * 出来的（自检那行"ANSI 深色档 libvterm 自带"就是它）。白底上 0~7 要么刺眼
 * （#e0e000）要么发灰；8~15（"亮"档）只能往深里走，否则和 0~7 分不开 ——
 * 浅色终端的通行做法（VS Code Light+ / Windows Terminal One Light 同样）。
 */
const QVector<QColor> &builtinLightAnsi() {
    static const QVector<QColor> kAnsi = {
        QColor(QStringLiteral("#303133")),  // 0  ← #000000
        QColor(QStringLiteral("#b3261e")),  // 1  ← #e00000
        QColor(QStringLiteral("#0a7a3a")),  // 2  ← #00e000
        QColor(QStringLiteral("#9a6700")),  // 3  ← #e0e000  PowerShell 提示符那一号
        QColor(QStringLiteral("#0b57d0")),  // 4  ← #0000e0
        QColor(QStringLiteral("#a3179b")),  // 5  ← #e000e0
        QColor(QStringLiteral("#0e7490")),  // 6  ← #00e0e0
        QColor(QStringLiteral("#6b7280")),  // 7  ← #e0e0e0
        QColor(QStringLiteral("#9aa0a6")),  // 8  ← #808080
        QColor(QStringLiteral("#e04b3a")),  // 9  ← #ff4040
        QColor(QStringLiteral("#2ea05a")),  // 10 ← #40ff40
        QColor(QStringLiteral("#c98a00")),  // 11 ← #ffff40
        QColor(QStringLiteral("#4285f4")),  // 12 ← #4040ff
        QColor(QStringLiteral("#d062c6")),  // 13 ← #ff40ff
        QColor(QStringLiteral("#35a3b5")),  // 14 ← #40ffff
        QColor(QStringLiteral("#303133")),  // 15 ← #ffffff
    };
    return kAnsi;
}

/*
 * "另存为…"写出去时附在 _doc 里的那份说明：键是深色档那个色值，说的是它在界面上
 * 管哪一块。方案文件是给人手改的，没有这份对照就得靠猜"#313335 是哪块"。
 * 只列常动的那几十个；没列到的键照样能用，只是没注释。
 */
const QHash<QString, QString> &uiDoc() {
    static const QHash<QString, QString> kDoc = {
        { QStringLiteral("#313335"), QStringLiteral("窗口底 / 图标条 / 顶栏 / 底栏") },
        { QStringLiteral("#26282b"), QStringLiteral("设置面板侧栏、列表底色") },
        { QStringLiteral("#2b2d30"), QStringLiteral("气泡、选中标签、输入框底") },
        { QStringLiteral("#3c3f41"), QStringLiteral("下拉菜单 / 确认卡 / 查找条面板") },
        { QStringLiteral("#1e1f22"), QStringLiteral("编辑区纸色（正文底）") },
        { QStringLiteral("#1e1f22@tabstrip"), QStringLiteral("页签条底（和纸色同一个深色值；想单独改这一条就用这个带角色的键）") },
        { QStringLiteral("#252526"), QStringLiteral("编辑器旁的窄侧栏") },
        { QStringLiteral("#2d2d30"), QStringLiteral("另一种选中底") },
        { QStringLiteral("#404043"), QStringLiteral("滚动条槽 / 分隔带") },
        { QStringLiteral("#4b4d4f"), QStringLiteral("主边框（窗口描边、滚动条滑块也用它）") },
        { QStringLiteral("#5f6266"), QStringLiteral("滚动条滑块悬停") },
        { QStringLiteral("#e8e8e8"), QStringLiteral("正文 / 强调文字") },
        { QStringLiteral("#d6d7da"), QStringLiteral("气泡文字、语法默认色") },
        { QStringLiteral("#bbbbbb"), QStringLiteral("标签 / 页签主文字") },
        { QStringLiteral("#cccccc"), QStringLiteral("次级文字") },
        { QStringLiteral("#9aa0a8"), QStringLiteral("图标常态色") },
        { QStringLiteral("#7d7d7d"), QStringLiteral("muted 文字") },
        { QStringLiteral("#3a4a5a"), QStringLiteral("图标条那一格的选中底") },
        { QStringLiteral("#2f3a44"), QStringLiteral("列表选中行") },
        { QStringLiteral("#2c3f52"), QStringLiteral("更强的选中") },
        { QStringLiteral("#0e639c"), QStringLiteral("强调色 / 链接（深色档）") },
        { QStringLiteral("#1653cb"), QStringLiteral("编辑器选中底") },
        { QStringLiteral("#c8503c"), QStringLiteral("错误 / 危险") },
        { QStringLiteral("#e06c75"), QStringLiteral("错误文字另一种") },
        { QStringLiteral("#7a4448"), QStringLiteral("diff 删除行的底") },
        { QStringLiteral("#1e3524"), QStringLiteral("diff 新增行的底") },
        { QStringLiteral("#7bc47f"), QStringLiteral("diff 新增行的字") },
        { QStringLiteral("#63572c"), QStringLiteral("查找命中的底") },
        { QStringLiteral("#26282c"), QStringLiteral("预览里代码块的底") },
        { QStringLiteral("#d7ba7d"), QStringLiteral("预览里行内 code 的字") },
        { QStringLiteral("#cf8e6d"), QStringLiteral("语法：关键字") },
        { QStringLiteral("#6aab73"), QStringLiteral("语法：字符串") },
        { QStringLiteral("#2aacb8"), QStringLiteral("语法：数字") },
        { QStringLiteral("#7a7e85"), QStringLiteral("语法：注释") },
        { QStringLiteral("#56a8f5"), QStringLiteral("语法：函数") },
        { QStringLiteral("#c77dbb"), QStringLiteral("语法：类型") },
        { QStringLiteral("#bcbec4"), QStringLiteral("语法：运算符") },
        { QStringLiteral("#b3ae60"), QStringLiteral("语法：预处理") },
        { QStringLiteral("#e8bf6a"), QStringLiteral("语法：标签") },
        { QStringLiteral("#ff6b68"), QStringLiteral("语法：错误") },
    };
    return kDoc;
}

/*
 * 方案 font 段的键 →界面上那一排菜单里的叫法。清单就是 Theme.h 上那个宏，
 * 认键、报错时点名、置灰那格的提示三处都从这一份来。
 */
const QHash<QString, QString> &fontLabels() {
    static const QHash<QString, QString> kLabels = [] {
        QHash<QString, QString> m;
#define F(key, label) m.insert(QString::fromLatin1(key), QStringLiteral(label));
        SCHEMEFONT_CASE(F)
#undef F
        return m;
    }();
    return kLabels;
}

/*
 * font 段：类型/范围不对的那一条丢掉并点名报出来，其余照用 —— 和 ui 段同一套规矩。
 * 段里没有的键**不进 map**（调用点靠"有没有这个键"决定回不回落到注册表），
 * 所以"方案没写字号"和"方案把字号写成 12"是两件事，别混。
 */
void parseFontSection(const QJsonObject &f, const QString &file, QVariantMap &out, QString &err)
{
    auto intInRange = [](const QJsonValue &v, int lo, int hi, int *got) {
        if (!v.isDouble() || v.toDouble() != static_cast<double>(static_cast<int>(v.toDouble())))
            return false;
        *got = v.toInt();
        return *got >= lo && *got <= hi;
    };
    for (auto it = f.constBegin(); it != f.constEnd(); ++it) {
        const QString k = it.key();
        const QJsonValue v = it.value();
        int n = 0;
        double d = 0.0;
        if (!fontLabels().contains(k)) {
            appendErr(err, file, QStringLiteral("font 段没有 \"%1\" 这一项（认的：%2），这条没有生效")
                                    .arg(k).arg(QStringList(fontLabels().keys()).join(QStringLiteral(" / "))));
            continue;
        }
        bool ok = false;
        if (k == QLatin1String(SchemeFont::kFamily) || k == QLatin1String(SchemeFont::kTerminalFamily)) {
            /* 字体名必须是**英文家族名**：Scintilla 走 toLatin1，中文名会压成问号 */
            ok = v.isString() && !v.toString().trimmed().isEmpty();
            if (ok)
                out.insert(k, v.toString().trimmed());
        } else if (k == QLatin1String(SchemeFont::kWrap)) {
            ok = v.isBool();
            if (ok)
                out.insert(k, v.toBool());
        } else if (k == QLatin1String(SchemeFont::kCommentSize)) {
            ok = intInRange(v, 0, 72, &n);      // 0 = 跟随正文
            if (ok)
                out.insert(k, n);
        } else if (k == QLatin1String(SchemeFont::kSize)
                   || k == QLatin1String(SchemeFont::kTerminalSize)) {
            ok = intInRange(v, 6, 72, &n);
            if (ok)
                out.insert(k, n);
        } else {                                  // lineHeight
            ok = v.isDouble() && (d = v.toDouble()) >= 1.0 && d <= 3.0;
            if (ok)
                out.insert(k, d);
        }
        if (!ok)
            appendErr(err, file, QStringLiteral("font.%1（%2）的值 %3 不对或超出范围，这条没有生效")
                                    .arg(k, fontLabels().value(k), v.toVariant().toString()));
    }
}

}  // namespace

namespace {

/*
 * 方案目录：%APPDATA%/SmartClip/SmartClip/schemes
 *
 * 放在和 notes.json、smartclip.db 同一个目录下，但**另起一个子目录**：
 * 这一份是给人手改、给人进版本库的，和注册表里那些程序自己写的键分开。
 */
QString schemesDirPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + QStringLiteral("/schemes");
}

/* 一个方案文件里坏了几处，攒成一段人话；每条一行，带文件名，方便对着改 */
void appendErr(QString &err, const QString &file, const QString &what) {
    err += (err.isEmpty() ? QString() : QStringLiteral("\n"))
           + QStringLiteral("%1：%2").arg(file, what);
}

}  // namespace

AppTheme::AppTheme(QObject *parent) : QObject(parent) {
    s_self = this;

    /*
     * 选哪套方案。老版本只有 ui/theme（light/dark 两个词），所以：
     * 没存过 ui/scheme 就按 ui/theme 推一档内置方案，别把老用户的观感换掉。
     */
    QSettings st;
    const QString saved = st.value(QLatin1String(kSchemeKey)).toString();
    m_light = st.value(QLatin1String(kKey)).toString() == QLatin1String("light");
    applyScheme(saved.isEmpty() ? (m_light ? QStringLiteral("Light")
                                           : QStringLiteral("Dark"))
                                : saved);

    /*
     * 改完文件立刻生效（用户 2026-09-24 要的就是这个："以后可以直接更改文件"）。
     * 盯**目录**而不是只盯文件：编辑器保存通常是"删掉旧的再写新的"，那样文件级
     * 监视会整个失效，目录级不会。攒 150ms 是因为一次保存常常连着发好几个信号。
     */
    m_watch = new QFileSystemWatcher(this);
    connect(m_watch, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString &) { m_reloadTimer->start(); });
    connect(m_watch, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &) { m_reloadTimer->start(); });
    m_reloadTimer = new QTimer(this);
    m_reloadTimer->setSingleShot(true);
    m_reloadTimer->setInterval(150);
    connect(m_reloadTimer, &QTimer::timeout, this, [this]() {
        applyScheme(m_scheme);
        watchSchemeFiles();          // 文件被换过，监视要重新挂
        emit schemeFilesChanged();   // 目录里可能多了/少了方案
    });
    watchSchemeFiles();
}

void AppTheme::watchSchemeFiles()
{
    if (!m_watch)
        return;
    const QString dir = schemesDirPath();
    if (!QDir().mkpath(dir))
        return;
    if (!m_watch->directories().contains(dir))
        m_watch->addPath(dir);
    const QString file = QDir(dir).absoluteFilePath(m_scheme + QStringLiteral(".json"));
    if (QFile::exists(file) && !m_watch->files().contains(file))
        m_watch->addPath(file);
}

QString AppTheme::schemesDir() const
{
    const QString dir = schemesDirPath();
    QDir().mkpath(dir);
    return QDir(dir).absolutePath();
}

QStringList AppTheme::schemeNames() const
{
    QStringList out{ QStringLiteral("Dark"), QStringLiteral("Light") };
    const QDir dir(schemesDirPath());
    const auto files = dir.entryList({ QStringLiteral("*.json") },
                                     QDir::Files | QDir::Readable, QDir::Name);
    for (const QString &f : files)
        out << QFileInfo(f).completeBaseName();
    return out;
}

void AppTheme::setScheme(const QString &name)
{
    if (name == m_scheme)
        return;
    applyScheme(name);
    watchSchemeFiles();
}

void AppTheme::applyScheme(const QString &name)
{
    const QString want = name.isEmpty() ? QStringLiteral("Dark") : name;
    QHash<QString, QString> ui;
    QVector<QColor> ansi(16);        // 全 invalid = 用 libvterm 自带那一份
    QVariantMap font;                // 只放方案写了的那几项（空 = 字体全凭注册表）
    bool light = false;
    QString err;
    /*
     * keepPrevious = 这份文件**整体**不能用（不存在 / 打不开 / JSON 读不出来）。
     * 单个键写错不算整体失败：那一条丢掉、其余照用，错误一样报出来。
     * 这两种以前都走同一条早退，结果"一个色打错了"会把整份丢回去 —— 自检
     * 那条"某个色写错只丢那一条"当场把它抓出来了。
     */
    bool keepPrevious = false;

    if (want == QLatin1String("Dark") || want == QLatin1String("Light")) {
        light = (want == QLatin1String("Light"));
        if (light) {
            ui = builtinLightUi();
            ansi = builtinLightAnsi();
        }
    } else {
        const QString file = want + QStringLiteral(".json");
        const QString path = QDir(schemesDirPath()).absoluteFilePath(file);
        QFile f(path);
        QJsonParseError pe{};
        QJsonDocument doc;
        if (!f.open(QIODevice::ReadOnly)) {
            appendErr(err, file, QStringLiteral("打不开：%1").arg(f.errorString()));
            keepPrevious = true;
        } else if (!(doc = QJsonDocument::fromJson(f.readAll(), &pe)).isObject()
                   || pe.error != QJsonParseError::NoError) {
            /*
             * 整份读不出来就**一行都不改**：手改文件时打漏一个括号是常事，
             * 这时候把界面换成半份方案（或干脆退回内置）比"维持原样"难查得多。
             */
            appendErr(err, file,
                      QStringLiteral("第 %1 列读不下去：%2（这一份没有生效，还是原来那套）")
                          .arg(pe.offset + 1).arg(pe.errorString()));
            keepPrevious = true;
        } else {
            const QJsonObject o = doc.object();
            const QString base = o.value(QStringLiteral("basedOn")).toString();
            if (base == QLatin1String("Light")) {
                ui = builtinLightUi();
                ansi = builtinLightAnsi();
                light = true;
            } else if (base != QLatin1String("Dark") && !base.isEmpty()) {
                appendErr(err, file,
                          QStringLiteral("basedOn 只认 Dark / Light，写的是 \"%1\"，按 Dark 继承")
                              .arg(base));
            }
            /* mode 可以显式写，覆盖 basedOn 推出来的那一档（深色底但改了很多色的人用） */
            if (o.contains(QStringLiteral("mode")))
                light = o.value(QStringLiteral("mode")).toString() == QLatin1String("light");

            const QJsonObject u = o.value(QStringLiteral("ui")).toObject();
            for (auto it = u.constBegin(); it != u.constEnd(); ++it) {
                if (!isUiKey(it.key())) {
                    appendErr(err, file,
                              QStringLiteral("键 \"%1\" 不是色值（要 #rrggbb，可以带 @角色 后缀），"
                                              "这条没有生效").arg(it.key()));
                    continue;
                }
                const QString v = it.value().toString();
                if (!isHexColor(v)) {
                    /* 这一条丢掉、其余照用：缺的本来就退回内置，写错的那条也一样处理 */
                    appendErr(err, file,
                              QStringLiteral("%1 的值 \"%2\" 不是 #rrggbb，这条没有生效")
                                  .arg(it.key(), v));
                    continue;
                }
                ui.insert(keyOf(it.key()), v.trimmed().toLower());
            }
            const QJsonObject t = o.value(QStringLiteral("terminal")).toObject();
            for (auto it = t.constBegin(); it != t.constEnd(); ++it) {
                bool ok = false;
                const int idx = it.key().toInt(&ok);
                const QString v = it.value().toString();
                if (!ok || idx < 0 || idx > 15) {
                    appendErr(err, file,
                              QStringLiteral("terminal 的键要是 0~15，写的是 \"%1\"，这条没有生效")
                                  .arg(it.key()));
                    continue;
                }
                if (!isHexColor(v)) {
                    appendErr(err, file,
                              QStringLiteral("terminal %1 的值 \"%2\" 不是 #rrggbb，这条没有生效")
                                  .arg(idx).arg(v));
                    continue;
                }
                ansi[idx] = QColor(v);
            }
            /* font 段：字体 / 字号 / 注释字号 / 行高 / 自动换行 / 终端那两项 */
            if (o.contains(QStringLiteral("font")) && !o.value(QStringLiteral("font")).isObject()) {
                appendErr(err, file, QStringLiteral("font 要是一个对象（\"font\": { \"size\": 14 }），"
                                                    "这一整段没有生效"));
            } else {
                parseFontSection(o.value(QStringLiteral("font")).toObject(), file, font, err);
            }
        }
    }

    /* 整体不能用：只报错误，其余一律不动（单个键写错不走这条路） */
    if (keepPrevious) {
        m_error = err;
        emit schemeErrorChanged();
        return;
    }

    m_ui = ui;
    m_ansi = ansi;
    m_font = font;
    m_light = light;
    m_scheme = want;
    m_error = err;

    QSettings st;
    st.setValue(QLatin1String(kSchemeKey), m_scheme);
    st.setValue(QLatin1String(kKey), m_light ? QStringLiteral("light") : QStringLiteral("dark"));

    emit schemeChanged();
    emit schemeErrorChanged();
    /*
     * lightChanged 在这里的含义是"外观要整体重刷一遍"：原生那一侧（QScintilla 的
     * 调色板、DWM 描边、宿主 palette、终端 ANSI）全都挂在这个信号上，
     * 所以哪怕两档都是 light 底、只是换了几个色，也必须发。
     * rev 是 QML 那一侧的对应物：绑定都传它，换方案必变。
     */
    ++m_rev;
    emit revChanged();
    emit lightChanged();
}

void AppTheme::setLight(bool on) {
    setScheme(on ? QStringLiteral("Light") : QStringLiteral("Dark"));
}

void AppTheme::toggle() {
    setScheme(m_light ? QStringLiteral("Dark") : QStringLiteral("Light"));
}

QString AppTheme::c(const QString &darkHex, bool lightMode) const {
    /*
     * 第二个参数**不参与查表**了：QML 的绑定只追踪表达式里出现的属性读取，
     * 看不见函数体内部读了什么，所以调用点必须传一个属性进来当触发器
     * （现在传的是 Theme.rev —— 换方案就 +1，光传 Theme.light 的话，
     * 从 Light 换成另一套浅底方案时界面不会动）。
     *
     * 查的是**当前方案的有效表**；表里没有的键原样返回，所以内置 Dark（空表）
     * 是恒等映射 —— 自检里那条"切回来必须等于原值"钉的就是这个。
     *
     * "@角色"后缀是给**同一个深色值在界面上担着两个角色、两个角色又想要不同浅色值**
     * 那种情况留的口子（页签条底和编辑区纸色都是 #1e1f22，浅色下撞成一个，选中那枚
     * 页签就丢了背景）。调用点写 `Theme.c("#1e1f22@tabStrip", Theme.rev)`：先查带角色
     * 的那条，没有再按裸 hex 查，都没有就返回**去掉后缀的 hex 本身** —— 后缀只是键、
     * 不是颜色，带进返回值就是个非法色值，深色档也就破了恒等。
     */
    Q_UNUSED(lightMode)
    const QString raw = darkHex.trimmed();
    const int at = raw.indexOf(QLatin1Char('@'));
    const QString bare = at < 0 ? raw : raw.left(at);
    if (at >= 0) {                          /* 先认带"@角色"的那一条 */
        const auto r = m_ui.constFind(keyOf(raw));
        if (r != m_ui.constEnd())
            return *r;
    }
    const auto it = m_ui.constFind(keyOf(bare));
    return it == m_ui.constEnd() ? bare : *it;
}

QVector<QColor> AppTheme::ansiPalette() const {
    /* 全 invalid（内置 Dark）时交回空表，让引擎去用 libvterm 自带那一份 */
    bool any = false;
    for (const QColor &c : m_ansi)
        any = any || c.isValid();
    return any ? m_ansi : QVector<QColor> {};
}

QString AppTheme::fontOverrideNote(const QString &key) const
{
    /* 没被方案定住 → 空串：调用点直接把它当提示文字用，空就是"没有提示" */
    if (!m_font.contains(key))
        return QString();
    return QStringLiteral("%1由方案「%2」的 font 段指定，要改就去改那个文件")
        .arg(fontLabels().value(key, key), m_scheme);
}

QString AppTheme::saveSchemeAs(const QString &name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty())
        return QStringLiteral("方案名不能空着");
    if (clean == QLatin1String("Dark") || clean == QLatin1String("Light"))
        return QStringLiteral("Dark / Light 是内置两档，换一个名字");
    if (clean.contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]"))))
        return QStringLiteral("方案名里不能有 \\\\ / : * ? \" < > | 这些字符");

    /*
     * 写的是**当前生效的整张表**（不是"和内置不同的那几条"）：手改的时候看得见
     * 全部键，比"改了哪几条"好读；而且 basedOn 写清楚，以后内置那张表加了新键，
     * 这份文件不会莫名其妙漏掉。
     */
    QJsonObject ui;
    for (auto it = m_ui.constBegin(); it != m_ui.constEnd(); ++it)
        ui.insert(it.key(), it.value());
    QJsonObject term;
    for (int i = 0; i < m_ansi.size() && i < 16; ++i)
        if (m_ansi.at(i).isValid())
            term.insert(QString::number(i), m_ansi.at(i).name());
    /* 字体那几项只写"方案里真的定了的"：另存为不该顺手把注册表里那套钉死 */
    QJsonObject font;
    for (auto it = m_font.constBegin(); it != m_font.constEnd(); ++it)
        font.insert(it.key(), QJsonValue::fromVariant(it.value()));
    QJsonObject doc;
    for (auto it = uiDoc().constBegin(); it != uiDoc().constEnd(); ++it)
        doc.insert(it.key(), it.value());

    QJsonObject o;
    o.insert(QStringLiteral("name"), clean);
    o.insert(QStringLiteral("basedOn"), m_light ? QStringLiteral("Light")
                                                : QStringLiteral("Dark"));
    o.insert(QStringLiteral("mode"), m_light ? QStringLiteral("light")
                                             : QStringLiteral("dark"));
    o.insert(QStringLiteral("_说明"),
             QStringLiteral("ui 的键是深色档那个色值，值是这个方案要用的色；"
                            "没写的键沿用 basedOn 那一档。同一个深色值在界面上担两个角色时，"
                            "可以用 色值@角色 单独指一个（例如 #1e1f22@tabstrip 只管页签条底，"
                            "裸 #1e1f22 管编辑区纸色）。terminal 的键是 0~15 号 ANSI 色。"
                            "font 那一段管字体：family 字体、size 字号(6~72)、commentSize 注释字号(0=跟随正文)、"
                            "lineHeight 行高倍数(1.0~3.0)、wrap 自动换行(true/false)、"
                            "terminalFamily 终端字体、terminalSize 终端字号；"
                            "写了哪几项就以方案为准（界面对应那排按钮会置灰），没写的项照用设置里那个值。"
                            "改完保存，界面立刻生效；_doc/_说明 只是给人看的，程序不读。"));
    o.insert(QStringLiteral("_doc"), doc);
    o.insert(QStringLiteral("ui"), ui);
    if (!term.isEmpty())
        o.insert(QStringLiteral("terminal"), term);
    if (!font.isEmpty())
        o.insert(QStringLiteral("font"), font);

    const QString dir = schemesDirPath();
    if (!QDir().mkpath(dir))
        return QStringLiteral("建不出来方案目录：%1").arg(dir);
    const QString path = QDir(dir).absoluteFilePath(clean + QStringLiteral(".json"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QStringLiteral("写不进去：%1（%2）").arg(path, f.errorString());
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    emit schemeFilesChanged();
    return QString();
}
