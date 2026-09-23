#pragma once

#include <QColor>
#include <QObject>
#include <QString>

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

public:
    explicit AppTheme(QObject *parent = nullptr);

    static AppTheme *instance() { return s_self; }

    bool light() const { return m_light; }
    void setLight(bool on);

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

    /* 落盘用的键（QSettings：ui/theme = "light" / "dark"） */
    static constexpr const char *kKey = "ui/theme";

signals:
    void lightChanged();

private:
    bool m_light = false;
    static AppTheme *s_self;
};
