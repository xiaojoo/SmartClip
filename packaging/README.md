# 打包发布（Windows / x64）

一条命令出全部产物：Release 编译 → windeployqt → 裁三档 → **验证这包能不能起来** → 绿色 zip → Inno Setup 安装包 → 量出来的尺寸表。

```bat
packaging\build-release.bat            rem 全链
packaging\build-release.bat trimonly   rem 跳过编译，只重出包（省几分钟）
```

产物落在 `build\dist\`：三档安装包 `smartclip-{full,rec,min}-<版本>.exe` + 三个免安装 zip `smartclip-<版本>-{full,rec,min}-portable.zip`。
版本号取 `CMakeLists.txt` 里 `project(SmartClip VERSION ...)` 的第三个 token，改那儿就行。

依赖的工具链路径都可以用环境变量覆盖（默认值就是这台机器的位置）：
`QTDIR` / `JOMDIR` / `CMK` / `VCCall` / `SEVENZ`。`ISCC.exe` 按 Inno Setup 6/7 的常见位置自动找。

## 三档是什么区别

| 档 | 裁掉了什么 | 安装包 | 免安装 zip | 什么时候用 |
|---|---|---|---|---|
| `rec` | 见下面第 1 条 | 25.9 MB | 36.4 MB | **默认发人用这个** |
| `full` | 什么都不裁（windeployqt 出来啥就是啥） | 27.7 MB | 39.6 MB | 对照基线；怀疑"是不是裁坏了"时 |
| `min` | rec 再去掉 `opengl32sw.dll` + `D3Dcompiler_47.dll` | 19.7 MB | 27.4 MB | 确定对方显卡驱动正常时 |

`rec` 相对 `full` 裁掉的是：`qmltooling\`（QML 调试器插件）、所有 `*.qmltypes`（只有设计期/调试期用）、
`QtQuick\Dialogs` 和 `QtQuick\NativeStyle`、八个程序根本不会加载的 Controls 样式
（Imagine / Material / Universal / Windows / FluentWinUI3 及各自 StyleImpl）、五个用不到的 SQL 驱动
（只留 `qsqlite`）。程序锁死了 Fusion 样式（见 `src/main.cpp` 的 `setStyle`），所以 **Basic + Fusion 必须留着**。

三档都带 app-local 的 `vcruntime140.dll` / `vcruntime140_1.dll` / `msvcp140.dll`（约 1.5 MB），
换掉的是 windeployqt 默认会塞进包里的 24.4 MB `vc_redist.x64.exe` —— 干净机器不用先装 VC 运行库。

## 四道闸：跑不过就不出包

1. **结构闸**：`pkg-rec` 里必须有 `SmartClip.exe`、`Qt6Core/Gui/Qml/Quick/QuickControls2(+Fusion).dll`、
   `platforms\qwindows.dll`、`sqldrivers\qsqlite.dll`，以及 `qml\QtQml`、`qml\QtQuick`、
   `Controls`、`Layouts`、`Window`、`Templates` 六个目录。缺任何一项 → 退出码 4，一个包都不出。
2. **启动闸**：把 PATH 清成只剩 `C:\Windows\system32;C:\Windows`（Qt 完全不在里面），在 `pkg-rec\` 目录里跑
   `SmartClip.exe --summarize-test`。过了才说明依赖全自带；不过 → 退出码 5。
   这条是踩过之后加的：曾经有一版包 `--no-quick-import` 把整个 `qml\` 树跳过了，尺寸看着更漂亮，
   但那种包在别人机器上**双击根本起不来**，而只看尺寸是发现不了的。
3. **运行库闸**：包里找不到 app-local CRT → 退出码 6（说明 `vcvars64.bat` 没跑成）。
4. **签名闸**：配了 `SMARTCLIP_SIGN_CMD` 但签名或验签失败 → 退出码 7，**立刻停**，不会带着"以为签了"的产物继续跑。

## 代码签名（可选，不设就完全跳过）

跑之前设一条环境变量，脚本把要签的文件全路径拼在这条命令**最后**：

```bat
set "SMARTCLIP_SIGN_CMD=signtool sign /fd sha256 /td sha256 /tr http://timestamp.digicert.com /f H:\keys\smartclip.pfx /p 口令"
rem 或者证书已经在 Windows 证书存储里（U 盾 / Azure Key Vault）：
set "SMARTCLIP_SIGN_CMD=signtool sign /fd sha256 /td sha256 /tr http://timestamp.digicert.com /sha1 3F2C1A..."
```

签的对象：

* `pkg\SmartClip.exe` —— 在裁三档**之前**签，所以三个 zip 和三个安装包里的 exe 都自带签名，绿色版也是签过的；
* 三个 `smartclip-<档>-<版本>.exe` 安装包；
* Inno 生成的**卸载器**（`SignedUninstaller`）。

签完之后脚本会用 `signtool verify /pa` 复核一遍，"签了"是量出来的不是假设的。
Qt 那些第三方 DLL **不重签**（它们带 Nokia 的签名，重签会破坏原签名链）。

**卸载器签名靠的是 ISCC 的 `-s` 选项，不是 `.iss` 里的段**：Inno 没有 `[SignTools]` 脚本段，
工具必须在调用编译器时注册（`-s"scsign=<签名命令> $f"`），`.iss` 里只写 `SignTool=scsign`
+ `SignedUninstaller=yes`。这条是实测钉的：不给 `-s` 时 ISCC 直接报
`Value of [Setup] section directive "SignTool" is invalid`，给了就编译通过。
所以口令和证书路径都不在跟踪文件里（`.iss` 从环境读 `SMARTCLIP_SIGN_CMD` 只为决定"要不要开这两行"）。

实测证据（一次性自签证书，测完即删）：Inno 把两样东西交给签名工具 ——
`dist\uninst.e32.tmp`（就是卸载器，打进包后才改名 `unins000.exe`）和 `smartclip-rec-0.1.0.exe` 本体；
把那份 `uninst.e32.tmp` 复制出来验，得到
`Signature Index: 0 (Primary Signature) / Issued to: SmartClip packaging test / SHA1 512B8054…`，
唯一的错是 `A certificate chain processed, but terminated in a root certificate which is not trusted`
—— 自签证书必然如此，不是链子的问题。
`signtool` 能签 `.tmp` 扩展名（试过），所以那个临时名字不碍事。

签名命令里有三个坑：`;` 会被 `.iss` 当注释开头；`!` 会被本脚本的延迟展开吃掉（口令里有就出事）；
命令第一个 token 是程序名，**路径带空格时 Inno 解析不了** —— 要么把 `signtool.exe` 所在目录加进 PATH
后直接写 `signtool sign …`，要么用一个不含空格的完整路径。

## 缺点 / 已知没做的

* **未签名的安装包在别人机器上会被 SmartScreen 拦**（"Windows 已保护你的电脑"），要点「更多信息 → 仍要运行」。
  消掉它需要代码签名证书（OV 大约 $100–400/年），换打包工具解决不了。
* `min` 档去掉软件渲染兜底，**没有在真的没显卡驱动的机器上试过**，只有尺寸数字。
* 只出 x64（`ArchitecturesAllowed=x64compatible`），ARM64 / 32 位机器装不上。
* 安装是 `PrivilegesRequired=lowest`（落到用户目录），不写系统 PATH、不要管理员；
  代价是多用户机器上每个用户各装一份。
* 没有自动更新：出新版本就是重跑这条链、把新包发出去。
* `.iss` 命令行里如果出现 `;` 会被 Inno 当成注释开头（口令里有分号要注意）。
* 尺寸表里 `tree_MB` 是按文件字节算的，和 `du -sh`（按簇算）会差几个百分点，不是不一致，是两把尺子。
