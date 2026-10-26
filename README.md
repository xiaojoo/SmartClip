# SmartClip

Windows 桌面的剪贴板 + 效率工具：剪贴板历史、截图 / 贴图、便签、翻译、长文汇总、文档识别、
内置终端、文件对比、文本校对与格式化。数据全在本机。

## 安装

**装**：下载 `smartclip-rec-<版本>.exe`，双击即可（自带 VC 运行库，干净机器不用先装东西）。
`min` 那一档小 6 MB，前提是对方显卡驱动正常；`full` 是什么都不裁的对照档。

**免安装**：下 `smartclip-<版本>-rec-portable.zip`，解压后运行里面的 `SmartClip.exe`。

**从源码**：Qt 6.11.2 MSVC 2022 64-bit + CMake + jom，QScintilla / libvterm 已经放在 `third/` 里。

```bat
cmake -S . -B build -G "Ninja" -DCMAKE_PREFIX_PATH=C:\Qt\6.11.2\msvc2022_64
cmake --build build --target SmartClip
packaging\build-release.bat        rem 出全部安装包 + zip + 尺寸表，见 packaging/README.md
```

**装好之后**：格式化、文档识别、图上选字换 PP-OCR、翻译这四件事用的是**本机上的外部工具**，
安装包不捆绑它们（缺了程序照常跑，界面上会明说缺哪个）。每一样怎么装、装完怎么确认，
见 [docs/安装依赖.md](docs/安装依赖.md) —— 设置面板里那句「怎么装」的链接也指到这篇。

## 声明

- 本仓库里**我们自己写的代码**按 CC0 1.0 Universal 释出（见 `LICENSE`），可以拿走、改、卖，不用署名。
- 但 `third/` 下的东西不是我们的，也不会因为这份 CC0 而改变：QScintilla 是 **GPL v3**，
  Scintilla 与 libvterm 是 MIT，Qt 是 **LGPL**。而 QScintilla 是**静态链进 `SmartClip.exe`** 的
  （`third/qscintilla/qscintilla.lib`），所以**发布出去的那个 exe 整体受 GPLv3 约束** ——
  要分发可执行文件，就得按 GPLv3 提供对应源码。CC0 覆盖不到这一层，这条不是客套话。
- 随包的 Python 脚本（`resources/scripts/`）调用的 OCR / 文档模型由各自上游许可，默认不装；
  缺它们程序照常跑，只是那几条路（文档识别、长文汇总的模型端）灰掉。
- 不担保：按 CC0 原样提供，出问题自己担着。

## 技术栈

- **Qt 6.11.2**：QML / Quick Controls（界面）+ Widgets + QScintilla（编辑区）+ Qt SQL（SQLite 存本机数据）。
- **Win32 / COM**：全局热键（`RegisterHotKey`）、DWM 圆角与窗口矩形、UI Automation（截图的自动框选）、
  SAPI（朗读）、Windows OCR（图上选字，C++/WinRT）。
- **终端**：ConPTY + libvterm。
- **打包**：windeployqt + 自写裁剪 + Inno Setup，四道闸（结构 / 启动 / 运行库 / 签名）跑不过就不出包。
- 界面文案是中文。

## 许可证

CC0 1.0 Universal —— 见 [LICENSE](LICENSE)。第三方组件的许可证以上「声明」一节为准。
