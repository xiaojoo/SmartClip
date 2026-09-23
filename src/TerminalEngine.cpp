#include "TerminalEngine.h"

#include "TerminalPty.h"

#include <QFileInfo>
#include <QTimer>

namespace {

/*
 * 把一行**原始格子**拼成文字，口径和 TerminalEngine::lineText() 对齐：
 * 宽字符的尾巴格（chars[0]==0xffffffff）跳过、空格子补一个空格、行尾空格去掉。
 * 只拿它判断"刚滚进来的这一行，是不是清屏之前屏上的那一行"（见 onSbPush）。
 */
QString cellsText(const VTermScreenCell *cells, int cols)
{
    QString line;
    for (int c = 0; c < cols; ++c) {
        const quint32 first = cells[c].chars[0];
        if (first == 0xffffffffu)
            continue;
        if (first == 0) {
            line += QLatin1Char(' ');
            continue;
        }
        QString piece;
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cells[c].chars[i] != 0; ++i)
            piece += QChar(cells[c].chars[i]);
        line += piece;
    }
    while (!line.isEmpty() && line.at(line.size() - 1) == QLatin1Char(' '))
        line.chop(1);
    return line;
}

/* 一个"什么都没有"的格子：越界 / 回滚行比当前屏窄时补它 */
VTermScreenCell blankCell()
{
    VTermScreenCell c {};
    c.chars[0] = U' ';
    c.width = 1;
    /*
     * 这两位不能省。VTermColor 的 type=0 **不是**"用默认色"，而是"下面那三个 rgb
     * 字节 / 那个索引是权威" —— 零初始化就等于"调色板第 0 号 = 纯黑"。fillCell 会
     * 照着 convert_color_to_rgb 给你一块黑，于是"回滚行比当前屏窄"补出来的那段是黑的。
     */
    c.fg.type = VTERM_COLOR_DEFAULT_FG;
    c.bg.type = VTERM_COLOR_DEFAULT_BG;
    return c;
}

}  // namespace

/*
 * 这套回调结构体必须活在静态存储期：vterm_screen_set_callbacks 存的是**指针**，
 * 不是拷贝，写成函数里的局部变量就是悬垂指针（崩溃点还会离现场很远）。
 * 作为成员函数是为了能拿到那些 private 的 static 回调。
 */
const VTermScreenCallbacks *TerminalEngine::screenCallbacks()
{
    static VTermScreenCallbacks cbs = [] {
        VTermScreenCallbacks c {};
        c.damage = &TerminalEngine::onDamage;
        c.moverect = &TerminalEngine::onMoveRect;
        c.movecursor = &TerminalEngine::onMoveCursor;
        c.settermprop = &TerminalEngine::onTermprop;
        c.bell = &TerminalEngine::onBell;
        c.sb_pushline = &TerminalEngine::onSbPush;
        /*
         * sb_popline **故意不实现**（原来有一份，2026-09-23 删掉）。
         *
         * libvterm 变高时会从顶部把行"倒回"屏上（screen.c:676 那段 backfill），
         * 我们那份历史是它唯一的数据源 —— 倒一次少一次。而 ConPTY 收到新尺寸之后
         * 会把整屏按**它自己的缓冲**重画一遍，把倒回来的那几行又盖掉。
         * 实测（--terminal-test 里那条"终端变高一行都不许丢"）：宽度不变、
         * 44 行涨到 134 行，"回滚 + 屏上有字"从 645 掉到 555 —— **90 行没了**，
         * 正好等于新长出来的行数；屏上最后还是那 44 行（第 0..43 行）。
         * 也就是说倒回来的行一次都没露过面，只是被吃掉。
         *
         * 不实现它：libvterm 拿不到行就 break（680 行那个判断），新露出来的那些屏行
         * 留空，历史一行不少 —— 条子也就不会像用户报的那样"最大化之后消失、
         * 内容被截断、滚不动"。
         */
        c.sb_clear = &TerminalEngine::onSbClear;
        return c;
    }();
    return &cbs;
}

TerminalEngine::TerminalEngine(QObject *parent) : QObject(parent)
{
    m_vt = vterm_new(m_rows, m_cols);
    vterm_set_utf8(m_vt, 1);

    m_screen = vterm_obtain_screen(m_vt);
    /* 备用屏不开的话 vim / less 那一整屏会画到主屏上，退出之后满屏残影 */
    vterm_screen_enable_altscreen(m_screen, 1);
    vterm_screen_set_callbacks(m_screen, screenCallbacks(), this);
    vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_ROW);
    vterm_screen_reset(m_screen, 1);

    vterm_output_set_callback(m_vt, &TerminalEngine::onTerminalOutput, this);

    m_pty = new TerminalPty(this);
    connect(m_pty, &TerminalPty::output, this, &TerminalEngine::applyOutput);
    connect(m_pty, &TerminalPty::exited, this, &TerminalEngine::exited);
}

TerminalEngine::~TerminalEngine()
{
    m_pty->shutdown();
    if (m_vt)
        vterm_free(m_vt);   // 连带释放 screen 和回滚区里那份格子内存
}

bool TerminalEngine::start(const QString &program, const QStringList &args,
                           const QString &workingDir, QString *error)
{
    return m_pty->start(program, args, workingDir, m_cols, m_rows, error);
}

void TerminalEngine::applyOutput(const QByteArray &bytes)
{
    vterm_input_write(m_vt, bytes.constData(), size_t(bytes.size()));
    /*
     * 一批字节喂完再统一发一次"变了"：damage 回调是按行来的，一屏刷屏能打出
     * 几十次，每次都让视图 update() 反而更慢（QQuickItem::update 本身会合并，
     * 但信号槽那一圈的开销是白花的）。
     */
    if (m_contentsDirty) {
        m_contentsDirty = false;
        emit contentsChanged();
    }
}

void TerminalEngine::feedBytesForTest(const QByteArray &bytes)
{
    applyOutput(bytes);
}

void TerminalEngine::closeSession()
{
    m_pty->shutdown();
}

void TerminalEngine::clearHistory()
{
    m_history.clear();
    /*
     * 屏本身交给 shell 去清（CSI 2J），我们只清自己那份回滚。
     * 反过来做会出问题：把屏清空了，shell 以为光标还在原位，下一行输出就落在
     * 一个用户看不见的地方。
     */
}

void TerminalEngine::setSize(int cols, int rows)
{
    cols = qMax(20, cols);
    rows = qMax(4, rows);
    if (cols == m_cols && rows == m_rows)
        return;
    m_cols = cols;
    m_rows = rows;
    /*
     * 改尺寸之前先把 SGR 画笔复位 —— 这一条就是那个"最大化之后右边一片黑"的根。
     *
     * libvterm 补出新格子时用的是**当前画笔**的颜色，不是默认色：
     *   src/screen.c: clearcell()      -> cell->pen = screen->pen
     *   src/screen.c: erase_internal() -> cell->pen = { screen->pen.fg, screen->pen.bg }
     * 而 ConPTY 重排整屏时，画笔上挂着的是控制台默认属性那一套 —— 背景 = 调色板
     * 第 0 号 = **纯黑 #000000**，不是我们的主题底色 #1e1f22。于是变宽/变高之后
     * 那些没人再写过的格子，底色就永久是黑；视图照格子的颜色铺矩形，铺出来就是
     * 一条硬边黑带（自检在一帧里数到 91 块 #ff000000 的单格矩形，起点正好停在
     * 改尺寸**之前**的宽度上；用户机器上同一处量到 2382 px 的黑，且因为 shell
     * 空闲不再重画，一直黑着不自愈）。
     *
     * 在这里注一个 ESC[m，把 fg/bg 交还给"用默认色"那两个标志位（fillCell 认它，
     * 会换成主题色），改尺寸补出来的格子就带主题底色了。
     * 只在改尺寸这一拍复位，平时程序自己发的 "\x1b[41m\x1b[K"（涂到行尾）不受影响。
     */
    vterm_input_write(m_vt, "\x1b[m", 3);
    /*
     * 顺序有讲究：先改网格再改伪控制台。反过来时对面会按**新**尺寸重排，
     * 而我们的格子还是旧尺寸，重排结果写进来会错位一列。
     */
    vterm_set_size(m_vt, rows, cols);
    m_pty->resize(cols, rows);
    m_contentsDirty = true;
    emit contentsChanged();
}

bool TerminalEngine::cellAt(int row, int col, TerminalCell *out) const
{
    if (col < 0 || col >= m_cols)
        return false;

    VTermScreenCell c;
    if (row < 0) {
        const int idx = int(m_history.size()) + row;   // -1 = 最后一行
        if (idx < 0 || idx >= int(m_history.size()))
            return false;
        const auto &line = m_history[size_t(idx)];
        c = col < int(line.size()) ? line[size_t(col)] : blankCell();
    } else if (row >= m_rows) {
        return false;
    } else if (!vterm_screen_get_cell(m_screen, VTermPos { row, col }, &c)) {
        return false;
    }

    fillCell(c, out);
    return true;
}

void TerminalEngine::fillCell(const VTermScreenCell &in, TerminalCell *out) const
{
    /*
     * 宽字符的**续格**在 libvterm 里不是 width==0，而是 chars[0] == 0xffffffff
     * （见 src/screen.c：写宽字符时把后面那一格的 chars[0] 置 -1，
     * vterm_screen_get_cell 再靠"下一格是不是 -1"反推前一格的 width）。
     * 这里翻成我们自己的约定：续格 width = 0、不带字符。不翻的话视图会拿
     * 0xffffffff 去 fromUcs4 画一个假字，复制出来的文本里也混进垃圾码点。
     */
    const bool continuation = in.chars[0] == 0xffffffffu;

    out->charCount = 0;
    if (!continuation) {
        for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && in.chars[i] != 0; ++i)
            out->chars[out->charCount++] = in.chars[i];
    }
    else {
        /* 续格必须把码点也清掉：视图那边复用同一个 scratch 数组，留着上一帧的值
           就会读到一个"没有字却有字"的格子（自检量黑块时真被它骗过一次）。 */
        out->chars[0] = 0;
    }
    out->width = continuation ? 0 : in.width;

    out->bold = in.attrs.bold != 0;
    out->italic = in.attrs.italic != 0;
    out->underline = in.attrs.underline != VTERM_UNDERLINE_OFF;
    out->strikeOut = in.attrs.strike != 0;
    out->reverse = in.attrs.reverse != 0;
    out->conceal = in.attrs.conceal != 0;

    /*
     * "用默认色"是一个**标志位**，不是一个索引值：不先判就直接丢给
     * vterm_screen_convert_color_to_rgb()，它会照着 rgb 那三个字节（此时是脏的）
     * 给你一个随机颜色 —— 表现是"某些行字是花的"。
     */
    VTermColor fg = in.fg;
    if (VTERM_COLOR_IS_DEFAULT_FG(&fg))
        out->fg = m_defaultFg;
    else {
        vterm_screen_convert_color_to_rgb(m_screen, &fg);
        out->fg = QColor(fg.rgb.red, fg.rgb.green, fg.rgb.blue);
    }

    VTermColor bg = in.bg;
    if (VTERM_COLOR_IS_DEFAULT_BG(&bg))
        out->bg = m_defaultBg;
    else {
        vterm_screen_convert_color_to_rgb(m_screen, &bg);
        out->bg = QColor(bg.rgb.red, bg.rgb.green, bg.rgb.blue);
        /*
         * **擦出来**的格子（chars[0]==0：ED/EL、换行滚进来的那几行、改尺寸补的列）
         * 不许带纯黑底，换成主题底色。
         *
         * libvterm 的 erase 用的是当前画笔的颜色（终端行话叫 bce，见 screen.c 的
         * erase_internal），而 ConPTY 的画笔上挂着的是控制台默认属性那一对 ——
         * 背景 = 调色板 0 号 = #000000。那不是"这块要涂黑"，是"控制台默认背景是黑"
         * 被顺带写进了画笔。我们量到的每一片黑都是这一条：改尺寸那一帧 91 块
         * #ff000000 的单格矩形、空闲重排 8 块（都是空格，charCount=0）。
         * setSize() 里注的 ESC[m 管住了"改尺寸"那一拍；这一条管住平时输出里的擦除。
         *
         * 只改底色、只管空格：`\e[41mX`（红底写字）和 `\e[41m\e[K`（红底涂到行尾）
         * 都照原样过 —— 前者有字，后者是红不是黑。自检里那两条盯着它。
         */
        if (in.chars[0] == 0 && out->bg == QColor(0, 0, 0) && m_defaultBg != QColor(0, 0, 0))
            out->bg = m_defaultBg;
    }
}

QString TerminalEngine::lineText(int row) const
{
    QString line;
    TerminalCell cell;
    for (int col = 0; col < m_cols; ++col) {
        if (!cellAt(row, col, &cell) || cell.width == 0)
            continue;
        if (cell.charCount == 0)
            line += QLatin1Char(' ');
        else
            line += QString::fromUcs4(cell.chars, cell.charCount);
    }
    while (!line.isEmpty() && line.at(line.size() - 1) == QLatin1Char(' '))
        line.chop(1);
    return line;
}

void TerminalEngine::sendKey(int vtermKey, int modifiers)
{
    /*
     * 交给 libvterm 编码，不在这里手搓 "\x1b[A" 那种字符串：方向键发 ESC [ A 还是
     * ESC O A 取决于对面有没有开 DECCKM（application cursor keys），bracketed paste、
     * 鼠标模式同理。这些模式是 shell 里的程序自己用转义序列开关的，只有 libvterm
     * 知道当前状态。
     */
    vterm_keyboard_key(m_vt, VTermKey(vtermKey), VTermModifier(modifiers));
}

void TerminalEngine::sendText(const QString &text)
{
    for (const uint cp : text.toUcs4())
        vterm_keyboard_unichar(m_vt, cp, VTERM_MOD_NONE);
}

void TerminalEngine::sendBytes(const QByteArray &bytes)
{
    m_pty->write(bytes);
}

void TerminalEngine::pasteText(const QString &text)
{
    /* bracketed paste：前后包 ESC[200~ / ESC[201~，让 shell 知道这一大段是粘贴
       而不是"用户按得飞快"—— 不然多行粘贴会被逐行当真执行。 */
    vterm_keyboard_start_paste(m_vt);
    sendText(text);
    vterm_keyboard_end_paste(m_vt);
}

void TerminalEngine::mouseButton(int button, bool pressed, int row, int col, int modifiers)
{
    /*
     * 先定位再按：这份 libvterm 的 mouse_button 用的是它自己记的 mouse_col/row，
     * 而那个值只有 vterm_mouse_move 会改（见 src/mouse.c）。少了这一步，点击会
     * 报到上一次鼠标所在的那一格去。
     */
    vterm_mouse_move(m_vt, row, col, VTermModifier(modifiers));
    vterm_mouse_button(m_vt, button, pressed, VTermModifier(modifiers));
}

void TerminalEngine::mouseMove(int row, int col, int modifiers)
{
    vterm_mouse_move(m_vt, row, col, VTermModifier(modifiers));
}

void TerminalEngine::suppressHistoryBriefly()
{
    /* 先把屏上这几行的文字记下来：它们马上就要被 Ctrl+L 滚进回滚，而那正是用户要清的 */
    m_histDropQueue.clear();
    for (int r = 0; r < m_rows; ++r)
        m_histDropQueue << lineText(r);
    clearHistory();
}

bool TerminalEngine::running() const
{
    return m_pty->running();
}

QString TerminalEngine::program() const
{
    return m_pty->program();
}

QString TerminalEngine::displayTitle() const
{
    const QString exe = QFileInfo(m_pty->program()).completeBaseName();
    /*
     * PowerShell 一上来把控制台标题设成**自己的全路径**，直接拿去当标签名就是一长串
     * `C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe`。这种时候退回程序名
     * （powershell）；用户 cd 到别处后 shell 报的真标题照用。
     */
    if (m_title.trimmed().isEmpty()
        || m_title.trimmed().compare(m_pty->program(), Qt::CaseInsensitive) == 0)
        return exe.isEmpty() ? tr("终端") : exe;
    return m_title;
}

void TerminalEngine::cursorPos(int *row, int *col) const
{
    VTermPos pos {};
    vterm_state_get_cursorpos(vterm_obtain_state(m_vt), &pos);
    if (row)
        *row = pos.row;
    if (col)
        *col = pos.col;
}

void TerminalEngine::setDefaultColors(const QColor &fg, const QColor &bg)
{
    m_defaultFg = fg;
    m_defaultBg = bg;
    m_contentsDirty = true;
    emit contentsChanged();
}

// ---------------------------------------------------------------- 回调

int TerminalEngine::onDamage(VTermRect, void *user)
{
    static_cast<TerminalEngine *>(user)->m_contentsDirty = true;
    return 1;
}

int TerminalEngine::onMoveRect(VTermRect, VTermRect, void *user)
{
    static_cast<TerminalEngine *>(user)->m_contentsDirty = true;
    return 1;
}

int TerminalEngine::onMoveCursor(VTermPos, VTermPos, int, void *user)
{
    auto *self = static_cast<TerminalEngine *>(user);
    self->m_contentsDirty = true;
    emit self->cursorChanged();
    return 1;
}

int TerminalEngine::onTermprop(VTermProp prop, VTermValue *val, void *user)
{
    auto *self = static_cast<TerminalEngine *>(user);
    switch (prop) {
    case VTERM_PROP_ALTSCREEN:
        self->m_altScreen = val->boolean != 0;
        self->m_contentsDirty = true;
        emit self->contentsChanged();
        break;
    case VTERM_PROP_CURSORVISIBLE:
        self->m_cursorVisible = val->boolean != 0;
        emit self->cursorChanged();
        break;
    case VTERM_PROP_TITLE: {
        /* OSC 标题是分片来的（长标题会切几段），攒到 final 那一片再落地 */
        const VTermStringFragment frag = val->string;
        if (frag.initial)
            self->m_titleFrag.clear();
        self->m_titleFrag.append(frag.str, int(frag.len));
        if (frag.final) {
            self->m_title = QString::fromUtf8(self->m_titleFrag);
            self->m_titleFrag.clear();
            emit self->titleChanged();
        }
        break;
    }
    case VTERM_PROP_MOUSE:
        self->m_mouseLevel = val->number;
        break;
    default:
        /* 光标形状、行内自动换行这些，这一版先不接（不影响可用性） */
        break;
    }
    return 1;
}

int TerminalEngine::onBell(void *user)
{
    emit static_cast<TerminalEngine *>(user)->bell();
    return 1;
}

int TerminalEngine::onSbPush(int cols, const VTermScreenCell *cells, void *user)
{
    auto *self = static_cast<TerminalEngine *>(user);
    /*
     * 刚按过垃圾桶：Ctrl+L 在 shell 那边是「把整屏滚上去」，这一滚会经 sb_pushline
     * 把刚清掉的旧内容又塞回回滚。
     *
     * 丢的判断按**内容**对上才丢：clearBuffer() 之前把屏上那几行的文字记进队列，
     * 滚进来的行和队首一模一样才丢掉并出队；**对不上就整个停止丢**、正常存。
     * 按时间窗、按行数预算都试过，都会吃掉真新的输出（自检当场量到
     * 「清完立刻灌 600 行，history 还是 0」和「krow 1 找不着」）。
     */
    if (!self->m_histDropQueue.isEmpty()) {
        if (cellsText(cells, cols) == self->m_histDropQueue.first()) {
            self->m_histDropQueue.removeFirst();
            return 1;
        }
        self->m_histDropQueue.clear();
    }
    std::vector<VTermScreenCell> line(cells, cells + cols);
    self->m_history.push_back(std::move(line));
    while (int(self->m_history.size()) > TerminalEngine::kHistoryLimit)
        self->m_history.pop_front();
    return 1;
}

int TerminalEngine::onSbClear(void *user)
{
    static_cast<TerminalEngine *>(user)->m_history.clear();
    return 1;
}

void TerminalEngine::onTerminalOutput(const char *bytes, size_t len, void *user)
{
    auto *self = static_cast<TerminalEngine *>(user);
    const QByteArray out(bytes, int(len));
    emit self->wroteToShell(out);
    self->m_pty->write(out);
}
