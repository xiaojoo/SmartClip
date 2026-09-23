#include "Theme.h"

#include <QHash>
#include <QSettings>

AppTheme *AppTheme::s_self = nullptr;

namespace {

/*
 * 深色 → 浅色那一张表。
 *
 * 抄的是 IntelliJ / PyCharm 的 "IntelliJ Light"：面板 #f2f2f2、内容底 #ffffff、
 * 正文 #000000、边框 #c4c4c4、语法四件套是藏青关键字 / 绿字符串 / 蓝数字 / 灰注释。
 * 强调蓝 #4c96d8 两边共用（所以它不进表 —— 表里没有的按恒等处理）。
 *
 * 每一行左边那个深色就是现在代码里写着的值，注释标它在这儿是干什么的，
 * 改配色只改这一张表，不去动那 200 个使用点。
 */
const QHash<QString, QString> &lightTable() {
    static const QHash<QString, QString> kTable {
        // ---- 面：从最底到浮起 ----
        { QStringLiteral("#313335"), QStringLiteral("#f2f3f5") },  // 窗口底 / 图标条 / 顶栏 / 底栏
        { QStringLiteral("#26282b"), QStringLiteral("#f2f3f5") },  // 设置面板侧栏、列表底色
        { QStringLiteral("#2b2d30"), QStringLiteral("#ffffff") },  // 气泡、选中标签、输入框
        { QStringLiteral("#3c3f41"), QStringLiteral("#ffffff") },  // 下拉菜单 / 确认卡 / 查找条面板
        { QStringLiteral("#1e1f22"), QStringLiteral("#ffffff") },  // 编辑区纸色
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
        { QStringLiteral("#2f659c"), QStringLiteral("#a8cdf5") },  // 编辑器选中底（A 档他嫌浅，2026-09-23 改挑 B 档）
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

}  // namespace

AppTheme::AppTheme(QObject *parent) : QObject(parent) {
    s_self = this;
    /* 键值就两个 ASCII 词（"light" / "dark"），没存过 = 深色 */
    m_light = QSettings().value(QLatin1String(kKey)).toString() == QLatin1String("light");
}

void AppTheme::setLight(bool on) {
    if (m_light == on)
        return;
    m_light = on;
    QSettings().setValue(QLatin1String(kKey), on ? QStringLiteral("light") : QStringLiteral("dark"));
    emit lightChanged();
}

void AppTheme::toggle() {
    setLight(!m_light);
}

QString AppTheme::c(const QString &darkHex, bool lightMode) const {
    if (!lightMode)
        return darkHex;
    const auto it = lightTable().constFind(keyOf(darkHex));
    return it == lightTable().constEnd() ? darkHex : *it;
}
