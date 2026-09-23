#pragma once

#include <QColor>
#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <vterm.h>

#include <deque>
#include <vector>

class TerminalPty;

/*
 * 一个格子的渲染信息。
 *
 * 这是引擎和视图之间唯一的格子约定：视图**不** include vterm.h，拿到的是已经
 * 解析成 QColor / 布尔位的普通结构体。理由是 libvterm 的 VTermColor 有三种形态
 * （索引色 / RGB / 带"用默认色"标志），把这套判断留在引擎里，视图那边就只剩
 * "照色画字"。
 */
struct TerminalCell
{
    quint32 chars[VTERM_MAX_CHARS_PER_CELL] = {};  // 码点，第 0 个是主字符，后面是组合字符
    int charCount = 0;
    int width = 1;       // 1 = 普通格；2 = 中日韩宽字符（占两格）；0 = 上一格的尾巴，不画
    QColor fg, bg;       // 已解析成 RGB（默认色已经按主题填好）
    bool bold = false;
    bool italic = false;
    bool underline = false;
    bool strikeOut = false;
    bool reverse = false;
    bool conceal = false;
};

/*
 * 一个终端会话：伪控制台（字节进出）+ libvterm（字符网格）+ 回滚缓冲。
 *
 * 分工：
 *   TerminalPty   —— 只搬字节；
 *   libvterm      —— 把字节流仿真成"这一屏现在长什么样"（含备用屏、颜色、光标）；
 *   本类          —— 把两边接起来，外加 libvterm 不管的那件事：**回滚**。
 *
 * 为什么要自己管回滚：这份 libvterm（0.3.3）没有内置 scrollback，它在主屏某一行
 * 被顶出顶部时回调 sb_pushline 把那一行交给我们存；备用屏（vim / less）期间不推，
 * 所以全屏程序退出之后不会留下它那一屏的残影 —— 这个行为正好是我们要的。
 * 反过来 sb_popline（变高时把历史倒回屏上）**不接**：倒回来的行会被 ConPTY 的整屏
 * 重画盖掉，白丢历史 —— 数字和推理在 TerminalEngine::screenCallbacks() 那里。
 *
 * 行号约定（视图和自检都用它）：
 *   row <  0            回滚区，从最老的一行往新数（-1 = 最靠近屏幕顶部的那一行）
 *   row in [0, rows)    当前屏
 * 也就是说"屏顶上面第 3 行"是 row = -3。
 */
class TerminalEngine : public QObject
{
    Q_OBJECT

public:
    static constexpr int kHistoryLimit = 5000;

    explicit TerminalEngine(QObject *parent = nullptr);
    ~TerminalEngine() override;

    /* 起 shell；失败时 error 带原因（界面直接显示这一句） */
    bool start(const QString &program, const QStringList &args,
               const QString &workingDir, QString *error);

    /* 只结束 shell，网格和回滚留着（标签关掉之前还要能看见最后几行） */
    void closeSession();

    /* 清回滚区（面板上那个垃圾桶） */
    void clearHistory();

    /*
     * 清完这一小段时间里，被顶出屏幕的行不进回滚。
     *
     * 垃圾桶发的是 Ctrl+L，shell 收到是「滚动 + 重画提示符」，这一滚会经
     * sb_pushline 把刚清掉的内容又塞回我们这儿（自检量到：清完 history=1、
     * 还能翻到旧行）。开一个几百毫秒的窗口把这一批丢掉。
     * 只丢「进回滚」这一步 —— 屏上的字一个不少，所以窗口里真有新输出也看得见。
     */
    void suppressHistoryBriefly();


    /* 面板/窗口改了尺寸：同步伪控制台和网格的行列数 */
    void setSize(int cols, int rows);

    int cols() const { return m_cols; }
    int rows() const { return m_rows; }
    int historyRows() const { return int(m_history.size()); }

    /* 取格子（见文件头的行号约定）。越界返回 false。 */
    bool cellAt(int row, int col, TerminalCell *out) const;

    /* 整行文字（自检和"复制这一行"用；行尾空格已去掉） */
    QString lineText(int row) const;

    /* 输入：键盘、粘贴、以及原样字节（Ctrl+C 这类控制字符走这里） */
    void sendKey(int vtermKey, int modifiers);
    void sendText(const QString &text);
    void sendBytes(const QByteArray &bytes);
    void pasteText(const QString &text);   // 按 bracketed paste 的规矩包起来

    /*
     * 鼠标。对面（vim / fzf）开了鼠标上报时，界面上的点击不该再当成选字，
     * 要转成转义序列发过去。行列是**当前屏**的坐标，不是回滚区的。
     * 编码同样交给 libvterm：它知道对面要 SGR 1006 还是老的 CSXTERM。
     */
    void mouseButton(int button, bool pressed, int row, int col, int modifiers);
    void mouseMove(int row, int col, int modifiers);

    /* 对面有没有开鼠标上报（开了才走上面那两个，否则鼠标用来选字） */
    bool mouseReporting() const { return m_mouseLevel > 0; }

    bool running() const;
    QString title() const { return m_title; }
    /* shell 自己报的标题为空时，界面退化成显示程序名（和 VS Code 一样） */
    QString displayTitle() const;
    bool altScreen() const { return m_altScreen; }
    void cursorPos(int *row, int *col) const;
    bool cursorVisible() const { return m_cursorVisible; }
    QString program() const;

    /* 主题默认色：libvterm 说"用默认前景/背景"时用它 */
    void setDefaultColors(const QColor &fg, const QColor &bg);
    QColor defaultFg() const { return m_defaultFg; }
    QColor defaultBg() const { return m_defaultBg; }

    /*
     * 自检用：不经过伪控制台，直接把一段"终端输出字节流"喂进网格。
     * 这样能脱离界面、脱离 shell 验转义解析（见 src/SelfTestTerminal.cpp）。
     */
    void feedBytesForTest(const QByteArray &bytes);

signals:
    /* 屏幕内容变了（视图据此 update()） */
    void contentsChanged();
    void cursorChanged();
    void titleChanged();
    void bell();
    void exited(int exitCode);
    /*
     * 引擎往 shell 那边写了字节（按键回显的转义、以及对 DA / DSR 查询的应答）。
     *
     * 界面不用它。留着是因为"有没有正确应答查询"这件事只有从这一侧才看得见，
     * 而它是个真会出事的点：ConPTY 会替对面的程序问 CSI 6 n（光标在哪），
     * 终端不答，那些程序就一直等 —— 自检钉的就是这条（见 SelfTestTerminal.cpp）。
     */
    void wroteToShell(const QByteArray &bytes);

private:
    void applyOutput(const QByteArray &bytes);

    /* libvterm 那套回调的注册表（静态存储期，见 .cpp 里那段说明） */
    static const VTermScreenCallbacks *screenCallbacks();

    static int onDamage(VTermRect rect, void *user);
    static int onMoveRect(VTermRect dest, VTermRect src, void *user);
    static int onMoveCursor(VTermPos pos, VTermPos oldpos, int visible, void *user);
    static int onTermprop(VTermProp prop, VTermValue *val, void *user);
    static int onBell(void *user);
    static int onSbPush(int cols, const VTermScreenCell *cells, void *user);
    /* sb_popline 故意不接：见 TerminalEngine::screenCallbacks() 里那段实测记录 */
    static int onSbClear(void *user);
    static void onTerminalOutput(const char *bytes, size_t len, void *user);

    void fillCell(const VTermScreenCell &in, TerminalCell *out) const;

    VTerm *m_vt = nullptr;
    VTermScreen *m_screen = nullptr;
    TerminalPty *m_pty = nullptr;

    /* 回滚区：每行是一个定长（推入时的 cols）格子数组；deque 是为了砍最老一行不搬家 */
    std::deque<std::vector<VTermScreenCell>> m_history;

    int m_cols = 80;
    int m_rows = 24;
    int m_mouseLevel = 0;   // VTERM_PROP_MOUSE 的值：0 = 不上报
    bool m_cursorVisible = true;
    bool m_altScreen = false;
    QString m_title;
    QByteArray m_titleFrag;   // OSC 是分段送来的，攒到 final 为止
    QColor m_defaultFg { QStringLiteral("#d4d4d4") };
    QColor m_defaultBg { QStringLiteral("#1e1f22") };
    bool m_contentsDirty = false;
    /* 见 suppressHistoryBriefly：按内容对上才丢，对不上立刻停止丢 */
    QStringList m_histDropQueue;
};
