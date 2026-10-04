#pragma once

#include <QPair>
#include <QString>
#include <QStringList>
#include <QVariantMap>

/*
 * 代码格式化的**格式模版**（每种语言一份），格式化时按这份模版去指挥工具。
 *
 * ===========================================================================
 * 为什么必须有这一层
 * ===========================================================================
 * src/Formatter.cpp 过去只是"把正文交给本机装了的工具"，一个风格参数都不给：
 * clang-format 于是用它自己的 LLVM 默认（2 空格缩进、80 列、行尾函数收成一行），
 * prettier 用它的默认（2 空格、80 列），black 用 88 列 —— 同一个应用里格式化
 * 两种语言会出来两套互不相干的观感，而且和用户在 IDEA 系列里用惯的那套对不上。
 * 这一层就是把"观感"从各工具手里收回来，变成我们自己写死、可以逐语言改的一份表。
 *
 * ===========================================================================
 * 取值是哪来的（不许凭印象编，所以逐条记了出处）
 * ===========================================================================
 * JetBrains 各 IDE 的**平台默认**（对 Java / C++ / C# 这些没有单独覆写的语言就是
 * 它的最终默认）取自 intellij-community 源码里的字段初始化：
 *   platform/code-style-api/src/com/intellij/psi/codeStyle/CodeStyleSettings.java
 *   platform/code-style-api/src/com/intellij/psi/codeStyle/CodeStyleDefaults.java
 *   缩进 4 / Tab 4 / 续行 8 / 不用 Tab 缩进 / 行宽 120 / case 相对 switch 缩进 /
 *   大括号一律同行（BRACE_STYLE、CLASS_BRACE_STYLE、METHOD_BRACE_STYLE 都是
 *   END_OF_LINE）/ 简单语句不许写成一行（KEEP_SIMPLE_*_IN_ONE_LINE 全 false）/
 *   连续空行最多保留 2（KEEP_BLANK_LINES_IN_CODE = 2）。
 * 各语言的覆写取自对应的 *CodeStyleSettings / *LanguageCodeStyleSettingsProvider：
 *   python  KEEP_BLANK_LINES_IN_CODE=1、文件末尾补一行（PyCodeStyleSettings）
 *   json    INDENT_SIZE=2（JsonLanguageCodeStyleSettingsProvider）
 *   yaml    INDENT_SIZE=2、CONTINUATION_INDENT_SIZE=2
 *   bash    INDENT_SIZE=2、TAB_SIZE=2、case 不缩进（ShCodeStyleSettings）
 *   html    不覆写缩进（吃平台默认 4），引号用双引号（HtmlCodeStyleSettings）
 *   properties 不覆写缩进；另有一项"键值分隔符两侧不留空格"我们落不了地
 *
 * **C++ / C# / JavaScript / CSS / SQL 的插件不在开源仓库里**（intellij-community
 * master 已不含 javascript / css 目录），拿不到它们的覆写值 —— 这几行就按平台
 * 默认走，并在 note 里写明"未公开覆写"，不假装是查来的。
 *
 * ===========================================================================
 * 模版落在哪儿
 * ===========================================================================
 * 内置这张表是**默认值**，同时每个语言会在
 *   %APPDATA%/SmartClip/SmartClip/styles/<语言>.json
 * 播种一份文件（已存在就不动），改完立刻生效，不用重启也不用重开菜单。
 * 这么做的理由和配色方案（schemes/*.json）一样：各工具的选项名不一样，
 * 让用户去改我们的命令行是不现实的；而且 clang-format 的键名在版本之间改过
 * （SpacesInParentheses 已删、AfterControlStatement 从 bool 变 enum），
 * 万一某天某个键在我们没测过的版本上不被接受，那是一张他能自己改掉的表，
 * 不是代码里的死值。
 */
namespace fmtstyle {

/*
 * 一份模版。字段只列**有工具能吃掉**的项 —— 放一个谁都读不到的字段就是死控件，
 * 所以像"引号用单双""属性对齐"这类 prettier / 内置 XML 都表达不了的项没有进来。
 */
struct Style {
    int indentSize = 4;              // 一级缩进
    int tabSize = 4;                 // Tab 宽度（只在 useTabs 时才有意义）
    int continuationIndent = 8;      // 续行缩进（IDEA 特意是 2 倍，和一级缩进不同）
    bool useTabs = false;            // 行首用 Tab 字符
    int rightMargin = 120;           // 行宽 / 折行边界
    bool indentCaseFromSwitch = true;  // case 相对 switch 再缩一级
    bool declarationBraceOnNextLine = false;  // class / function 的 { 换行（Allman 那半套）
    bool controlBraceOnNextLine = false;      // if / for / while 的 { 也换行（整套 Allman）
    bool keepSimpleStatementsOnOneLine = false;  // 允许 if (x) return; 这种一行语句
    int keepBlankLines = 2;          // 连续空行最多保留几个
    bool newlineAtEnd = true;        // 文件末尾补一个换行
    QString note;                    // 这行取值的出处，界面那行小字用
};

/* 引擎名（程序名认出来的），见 engineKind() */
enum class Engine {
    ClangFormat,
    Prettier,
    Black,
    Shfmt,
    Gofmt,          // 没有任何可传的风格选项
    Rustfmt,        // 同上（本轮不往它身上塞 --config）
    Rubocop,        // 要一份 .rubocop.yml，本轮没做适配
    PhpCsFixer,     // 要 --rules 或配置文件，本轮没做适配
    Builtin,        // 走 src/Formatter.cpp 里内置那三样
    None,           // 本机没有可用工具：模版一项都落不了地
    Other,          // 用户在设置里填的、认不出程序名的命令
};

/* 内置那张表：认不出来的语言退回平台默认（Style 的默认构造） */
Style builtinFor(const QString &language);

/* 全部有模版的语言 id（播种和设置面板都按这个顺序） */
QStringList languageIds();

/*
 * 模版目录：`<程序所在目录>/styles`（安装包 / 绿色 zip 都在自己家里），
 * 那一档建不出来或不可写时才退回 %APPDATA%/SmartClip/SmartClip/styles。
 * 结果进程内算一次就缓存。
 */
QString stylesDirPath();

/* 这个目录现在写不写得进去（界面上"改完立刻生效"那句话要配真话） */
bool stylesDirWritable();

/* 建目录 + 把缺的语言文件补出来（已存在的一个字都不改） */
void seedTemplates();

/*
 * 生效的那份模版 = 内置默认 + <语言>.json 里写到的那些项。
 * 按文件修改时间取缓存：这条路上每个语言每"格式化一次 / 刷一次设置面板"都会走。
 */
Style styleFor(const QString &language);

/* 上一次读这个语言的文件时攒下来的话（读成功是空串）；界面和自检用 */
QString loadErrorFor(const QString &language);

/* 从命令里认是哪个工具（扫每一个参数，所以 npx / node 那种包装也认得出来） */
Engine engineKind(const QString &command);

/* clang-format 的 --style 内联 YAML（不含 "--style=" 前缀） */
QString clangFormatStyle(const Style &style);

/*
 * 这个引擎该往命令里追加的参数（一个参数一段，直接拼进 QProcess 的参数表）。
 * 工具自己已经有同名选项时不追加 —— 用户在设置里写的那半句优先。
 */
QStringList styleArgs(const Style &style, Engine engine);

/*
 * 有的工具（rubocop）风格只能写在配置文件里，命令行塞不进去。
 * 返回 (文件名, 内容)，没有就给空对 —— 调用方把它写到那份临时文件旁边。
 */
QPair<QString, QString> engineConfigFile(const Style &style, Engine engine);

/*
 * 界面那一行小字：前段是模版本身（缩进/续行/行宽/大括号/空行），
 * 后段是"这份模版在当前这个工具身上能落地多少"。
 *
 * 大括号那两项只在**这门语言的语法里有大括号、而且当前引擎能摆它**时才出现 ——
 * JSON / YAML 的行上挂着"case 缩进"不是在说这门语言的事，是在说模版里有个数。
 */
QString summaryText(const Style &style, Engine engine, const QString &language);

/* 模版转成 map（播种文件用，也是 QML 侧 styleFor() 的返回） */
QVariantMap toMap(const Style &style);

}  // namespace fmtstyle
