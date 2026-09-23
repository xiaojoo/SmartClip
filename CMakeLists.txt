cmake_minimum_required(VERSION 3.21)
project(SmartClip VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 QuickWidgets Sql Widgets PrintSupport)

# ---------------------------------------------------------------------------
# 配置时那条 Qt6TaskTree / Qt6QmlAssetDownloaderPrivate 的 NOT FOUND —— **可以忽略**
#
# 现象（Qt Creator 的 Issues 里会标红，容易被当成错误）：
#     Could NOT find Qt6TaskTree (missing: Qt6TaskTree_DIR)
#     ... Qt6QmlAssetDownloaderPrivate_FOUND to FALSE ... dependency Qt6TaskTree
#     could not be found.
#
# 它是 Qt 自己的事：find_package(Qt6 ...) 会自动把所有 QML 插件包都 include 一遍
# （QtPublicPluginHelpers.cmake 的 __qt_internal_include_qml_plugin_packages），
# 其中一个可选插件 QmlAssetDownloaderPrivate（做素材下载用的，Qt Design Studio
# 那套才用）依赖第三方库 TaskTree，而这份 Qt 6.11.2 安装里没有 Qt6TaskTree
# （lib/cmake 下只有 Qt6QmlAssetDownloaderPrivate）。
# 本项目不用那个插件，所以**只是一条警告**：配置照样成功（exit 0、"Build files
# have been written"），编译和运行都不受影响。
#
# 别想着用 -DQT_SKIP_AUTO_QML_PLUGIN_INCLUSION=ON 把它消掉：实测那会把上面那套
# 插件登记一起关掉，引出一堆更麻烦的警告 ——
#     The qml plugin 'qtquick2plugin' is a dependency of 'SmartClip',
#     but the link target was not found
# 一共 34 条（qtquick2 / qml / models / workerscript / qtquickcontrols2 + 各套
# style 插件），而 qt_add_qml_module 正需要那份登记。一条换 34 条，划不来。
# ---------------------------------------------------------------------------

# 内置的 QScintilla（编辑器内核，见 third/qscintilla/CMakeLists.txt）
add_subdirectory(third/qscintilla)

# 底部终端面板（src/TerminalEngine.cpp）的仿真内核：纯 C99 的 VT 转义解析 + 字符网格
add_subdirectory(third/libvterm)

# ---------------------------------------------------------------------------
# 本源文件清单
#
# 单独列出来是给下面的"头文件依赖"用的（见"为什么每个源文件都依赖全部头"）。
# ---------------------------------------------------------------------------
set(SMARTCLIP_SOURCES
    src/main.cpp
    src/ClipboardStore.h
    src/ClipboardStore.cpp
    src/ClipboardManager.h
    src/ClipboardManager.cpp
    src/WindowHelper.h
    src/WindowHelper.cpp
    src/EditorController.h
    src/EditorController.cpp
    # 代码格式化（右键菜单那一条，见 src/Formatter.h）：认外部工具 + 三种内置
    src/Formatter.h
    src/Formatter.cpp
    # 编辑区校验（中文用词 / 代码语法，见 src/Checker.h）：本地规则 + 大模型
    src/Checker.h
    src/Checker.cpp
    # 两份正文的差异（文件对比功能，见 src/Diff.h）
    src/Diff.h
    src/Diff.cpp
    src/EditorViewItem.h
    src/EditorViewItem.cpp
    src/Screenshot.h
    src/Screenshot.cpp
    # 贴图窗口（"固定到桌面"那块图，见 src/PinWindow.h）：
    # 它自己是个 Q_OBJECT 的窗口 + 一份 QML 界面，还有"标注的画法"那一份共用实现
    # （截图合成也用）。单独成文件是因为 AUTOMOC 要求 Q_OBJECT 的类在自己的
    # 文件里（放在 Screenshot.cpp 里会报 "does not include Screenshot.moc"）。
    src/PinWindow.h
    src/PinWindow.cpp
    # 图上选字用的 OCR（Windows 自带那套，见 src/PinOcr.h）：
    # 只碰 WinRT 的一个文件，别的源文件只 include 它的头
    src/PinOcr.h
    src/PinOcr.cpp
    # 文档 / 图片识别成 Markdown（见 src/DocConvert.h + src/DocImport.h）：
    # DocConvert 是"跑本机程序"的窄约定（和 PinOcr 同一套做法），DocImport 是
    # 队列 + 落到笔记那一层（QML 单例 Doc）
    src/DocConvert.h
    src/DocConvert.cpp
    src/DocImport.h
    src/DocImport.cpp
    src/NoteThumbs.h
    src/NoteThumbs.cpp
    src/NoteLinkModel.h
    src/NoteLinkModel.cpp
    src/StickyNoteStore.h
    src/StickyNoteStore.cpp
    src/StickyNotes.h
    src/StickyNotes.cpp
    # 翻译（LLM 客户端 + 桌面卡片，见 src/Translate.h）：
    src/Translate.h
    src/Translate.cpp
    # 底部终端面板（见 src/TerminalEngine.h）：ConPTY 搬字节 + libvterm 仿真网格
    # + 一个自绘的字符网格视图（TerminalView 是 QQuickPaintedItem，不是原生子窗口）
    src/TerminalPty.h
    src/TerminalPty.cpp
    src/TerminalEngine.h
    src/TerminalEngine.cpp
    src/TerminalView.h
    src/TerminalView.cpp
    # 剪贴板内容汇总（见 src/Summarize.h）：分批问模型分类归并 -> 落成待审草稿
    #（QML 单例 Sum，和翻译 / 校验共用 LlmClient 那一套配置）
    src/Summarize.h
    src/Summarize.cpp
    # 朗读（翻译卡片上那个「播放」按钮：系统自带的语音合成，见 src/Speech.h）：
    src/Speech.h
    src/Speech.cpp
    src/TrayIcon.h
    src/TrayIcon.cpp
    src/SelfTest.h
    src/SelfTest.cpp
    # 便签自检单独一个文件（`--note-test` 只跑它，见 src/SelfTest.h）：
    # 和 SelfTest.cpp 那三千多行分开，改便签时不用把编辑区 / 截图那些
    # 有时序问题的检查也跑一遍。
    src/SelfTestNotes.cpp
    # 翻译自检（`--translate-test` 只跑它，见 src/SelfTest.h）：
    src/SelfTestTranslate.cpp
    # 文档识别自检（`--doc-test` 只跑它，见 src/SelfTest.h）：
    src/SelfTestDoc.cpp
    # 新增工具自检（`--tool-test` 只跑它：对比 / 格式化 / 校验 / Markdown）：
    src/SelfTestTools.cpp
    # 汇总自检（`--summarize-test` 只跑它：区间取原文 / 回复解析 / 草稿采纳 / 归档）：
    src/SelfTestSummarize.cpp
    # 终端自检（`--terminal-test` 只跑它：转义解析灌字节 + 真 PowerShell 走一遍）：
    src/SelfTestTerminal.cpp
)

qt_add_executable(SmartClip ${SMARTCLIP_SOURCES})

# ---------------------------------------------------------------------------
# 【必须留着】每个 .cpp 都依赖**全部**工程头文件
#
# 这套配置（中文版 MSVC + NMake Makefiles JOM）**不跟踪 #include 出来的头**：
# 只改 src/ClipboardStore.h、不改任何 .cpp，重新构建时只有
# mocs_compilation.cpp 会被重编，main.cpp / EditorViewItem.cpp 一个都不动
# （实测，见 build.log 里那些"注意: 包含文件"—— 编译器 /showIncludes 的输出
# 是中文的，CMake 那侧按英文前缀解析，于是什么依赖都没记下来；换成 Ninja 或
# VS 生成器、或者构建前 set VSLANG=1033，也都能好）。
#
# 后果不是"少编一次"这么轻：头里加/删一个成员之后，被改到的那个 .cpp 按新
# 布局编，没重编的 .cpp 还按旧布局用同一个类 —— 同一个对象在两份代码里大小
# 不一样，成员会被写到对象外面去。实测症状：QML 读一个 QStringList 属性读出
# 1872555072 这种"长度"，紧接着在 getter 里访问冲突崩溃（0xC0000005），
# 而且崩溃点离真正的原因（没重编）隔着好几层，极难查。
#
# 这个工程只有 7 个源文件，让每个源文件都依赖全部工程头：改头必然全量重编，
# 慢几秒，换掉一整类"改完头文件出玄学崩溃"。
# ---------------------------------------------------------------------------
file(GLOB SMARTCLIP_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(_src IN LISTS SMARTCLIP_SOURCES)
    if(_src MATCHES "\\.(cpp|cc|cxx)$")
        set_property(SOURCE "${_src}" APPEND PROPERTY OBJECT_DEPENDS ${SMARTCLIP_HEADERS})
    endif()
endforeach()

# ---------------------------------------------------------------------------
# Qt CMake 策略
#
# 不显式设置的话，qt_add_qml_module 每次配置都会各报一次 QTP0001 / QTP0004
# 开发者警告（CMake Warning (dev) ... "Use the qt_policy command to set the
# policy and suppress this warning"）。
#
# QTP0001：QML 模块默认资源前缀
#   NEW = /qt/qml/<URI>   OLD = /<URI>（旧行为，设成 OLD 会报"将被移除"）
#   本项目没有硬编码的 "qrc:/..." 引用，图片资源走独立的
#   qt_add_resources(PREFIX "/icons")，所以取 NEW 是安全的。
# QTP0004：QML 子目录要各自生成 qmldir
#   本项目 QML 分在 qml/components、qml/delegates、qml/models、qml/utils 下，
#   采用推荐行为。
# ---------------------------------------------------------------------------
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_qml_module(SmartClip
    URI SmartClip
    VERSION 1.0
    QML_FILES
        Main.qml
        qml/components/AppToolTip.qml
        qml/components/DropdownMenu.qml
        qml/components/EditorArea.qml
        qml/components/MarkdownView.qml
        qml/components/FindBar.qml
        qml/components/AskCard.qml
        qml/components/DiffPane.qml
        qml/components/DocCard.qml
        qml/components/FolderTree.qml
        qml/components/SettingsPanel.qml
        qml/components/StatusBar.qml
        qml/components/TabStrip.qml
        qml/components/TerminalPanel.qml
        qml/components/ToolButton.qml
        qml/components/TopBar.qml
        qml/components/WindowControls.qml
        qml/delegates/TreeDelegate.qml
        qml/models/ClipboardModel.qml
        qml/notes/StickyNoteWindow.qml
        qml/notes/NoteMenu.qml
        qml/translate/TranslateCard.qml
        qml/screenshot/CaptureOverlay.qml
        qml/screenshot/PinOverlay.qml
        qml/screenshot/RecognitionCard.qml
        qml/utils/IconProvider.qml
        qml/utils/AppIcon.qml
        qml/utils/ThinScrollBar.qml
        js/EditorMenus.js
        js/FolderManager.js
        js/TimeUtils.js
)


qt_add_resources(SmartClip "app_icons"
    PREFIX "/icons"
    BASE "resources/icons"
    FILES
        resources/icons/branch.svg
        resources/icons/chevron-down.svg
        resources/icons/chevron-left.svg
        resources/icons/chevron-right.svg
        resources/icons/chevron-up.svg
        resources/icons/close.svg
        resources/icons/file.svg
        resources/icons/folder.svg
        resources/icons/gear.svg
        resources/icons/grid.svg
        resources/icons/image.svg
        resources/icons/more.svg
        resources/icons/note.svg
        resources/icons/play.svg
        resources/icons/plus.svg
        resources/icons/refresh.svg
        resources/icons/search.svg
        resources/icons/trash.svg
)

# ---------------------------------------------------------------------------
# 应用图标（resources/brand/）
#
# 形是"折带 S"：块状 S 中心线 + 45° 斜接分面，每个折面一档蓝。
# 色阶是从强调色 #4c96d8 拉出来的四档，且**同时**扛得住深底和浅底：
#     #25547a  #3a7cb0  #4c96d8  #8fc7ef
# 对瓦片 #313335 依次 1.59 / 2.83 / 4.02 / 7.00:1，对浅底 #f2f3f5 依次
# 7.20 / 4.03 / 2.84 / 1.63:1 —— 再深一档（#1e4a6b，1.36:1）在深底上会没，
# 再亮一档（#a9d6f5，1.39:1）在浅底上会没，所以两头都出不了这个窗口。
#
# 只有一档图形（16..256 通吃），不再像早先那样分"全档 / 紧凑档"两张：
# 分档是因为那张三卡图的卡间错位只有 8/128（16px 时 1px），必然并团。
# 这里改成单档的前提是把 S 的"口"留到 16/128 —— 实测 16px 上还剩 1px 缝
# （设计值 2px，抗锯齿吃掉一半），够读出四段横竖。口一旦关掉（比如把带宽
# 加回 26），16px 就是一个实心块，所以 SelfTest.cpp 里那条断言量的就是这个
# 缝还剩几格，不是量"图标有没有加载"。
#
# app.ico 里 16/24/32/48/64/128/256 七个条目都是**按该尺寸真实矢量光栅化**
# 的（不是高清图缩放下来的），重烘脚本见仓库外的 build/logo-proposals/mkico.py。
# ---------------------------------------------------------------------------
qt_add_resources(SmartClip "app_brand"
    PREFIX "/brand"
    BASE "resources/brand"
    FILES
        resources/brand/smartclip.svg
        resources/brand/smartclip-bare.svg
        resources/brand/app.ico
)

if (WIN32)
    # exe 本身的图标（资源管理器 / 固定到任务栏 / 属性页那一栏）。
    # 窗口和托盘的图标是另一回事，走上面 qrc 里那份（见 main.cpp、TrayIcon.cpp）。
    set(SMARTCLIP_APP_ICO "${CMAKE_CURRENT_SOURCE_DIR}/resources/brand/app.ico")
    configure_file(resources/brand/app.rc.in
                   "${CMAKE_CURRENT_BINARY_DIR}/smartclip_app.rc" @ONLY)
    # 必须显式挂这条依赖：rc.exe 只盯着 .rc 的**文本**，而 .rc 的内容跟 app.ico
    # 的字节无关（里面就一行绝对路径）。实测只换 app.ico 不碰 .rc 时，
    # smartclip_app.rc.res 根本不重编，exe 里带的还是上一版图标 —— 换图标没生效
    # 但构建全绿，只有把 exe 里的图标抠出来逐像素比才发现（见 build 目录的验法）。
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/smartclip_app.rc"
                                PROPERTIES OBJECT_DEPENDS "${SMARTCLIP_APP_ICO}")
    target_sources(SmartClip PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/smartclip_app.rc")
endif()

# ---------------------------------------------------------------------------
# 图上选字那条路要调的小脚本（PP-OCR 那条）。
#
# 只放**模板**在 qrc 里，第一次用到时由 PinOcr::runnerScriptPath() 落到
# %APPDATA%/SmartClip/SmartClip/ppocr_runner.py —— 落到磁盘上用户才能自己看、
# 自己改（qrc 里那份是只读的）。
# ---------------------------------------------------------------------------
qt_add_resources(SmartClip "app_scripts"
    PREFIX "/scripts"
    BASE "resources/scripts"
    FILES
        resources/scripts/ppocr_runner.py
)

# ---------------------------------------------------------------------------
# 文档识别那三个小脚本（见 src/DocConvert.h）。
#
# 和 ppocr_runner.py 一样只放**模板**在 qrc 里，第一次用到时由
# DocConvert::runnerScriptPath() 落到 %APPDATA%/SmartClip/SmartClip/ 下 ——
# 落到磁盘上用户才能自己看、自己改。
#
# 三个 runner 共用一个 doc_runner_common.py（那份是**契约**：三个都 import 它，
# 所以 DocConvert::runnerScriptPath 每次用到都覆盖一遍）。
# ---------------------------------------------------------------------------
qt_add_resources(SmartClip "app_doc_scripts"
    PREFIX "/scripts"
    BASE "resources/scripts"
    FILES
        resources/scripts/doc_runner_common.py
        resources/scripts/doc_runner_rapid.py
        resources/scripts/doc_runner_paddleocr_vl.py
        resources/scripts/doc_runner_granite.py
)

target_link_libraries(SmartClip PRIVATE
    Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickWidgets
    Qt6::Sql Qt6::Widgets Qt6::PrintSupport
    qscintilla
    libvterm
    dwmapi
    # 朗读（src/Speech.cpp）：SAPI 那套的 CoCreateInstance / CoTaskMemFree 在这
    # 个库里（sapi.h 自己也有 #pragma comment(lib) 兜着，这里显式写一遍更清楚）。
    ole32
)

# ---------------------------------------------------------------------------
# 图上选字（Windows 自带 OCR，见 src/PinOcr.h）
#
# 走 C++/WinRT：头在 Windows SDK 的 **cppwinrt** 目录下，而 MSVC 默认**不**把那个
# 目录加进包含路径（um / shared / ucrt / winrt 才加）—— 所以这里自己找。
#
# 找不到（SDK 缺件 / 不是 Windows）就不定义 SMARTCLIP_HAS_WINOCR：PinOcr.cpp 里
# 有一份"没有 OCR"的兜底实现，界面那边 ocrAvailable=false，选字这条路整个收起来，
# 别的功能一点不受影响。
#
# windowsapp.lib 是 WinRT 激活要的（RoGetActivationFactory 在那儿）。
# ---------------------------------------------------------------------------
if (WIN32)
    set(_smartclip_cppwinrt "")
    if (DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
        # vcvars 会摆好这两个（Qt Creator 的 MSVC 套件也会）
        set(_smartclip_cppwinrt "$ENV{WindowsSdkDir}Include/$ENV{WindowsSDKVersion}cppwinrt")
    endif()
    if (NOT EXISTS "${_smartclip_cppwinrt}/winrt/Windows.Media.Ocr.h")
        # 退路：这台机器装了好几套 SDK 时，挑版本号最大的那套
        file(GLOB _smartclip_sdk_dirs LIST_DIRECTORIES true
             "C:/Program Files (x86)/Windows Kits/10/Include/*")
        list(SORT _smartclip_sdk_dirs)
        list(REVERSE _smartclip_sdk_dirs)
        foreach(_dir IN LISTS _smartclip_sdk_dirs)
            if (EXISTS "${_dir}/cppwinrt/winrt/Windows.Media.Ocr.h")
                set(_smartclip_cppwinrt "${_dir}/cppwinrt")
                break()
            endif()
        endforeach()
    endif()

    if (EXISTS "${_smartclip_cppwinrt}/winrt/Windows.Media.Ocr.h")
        target_include_directories(SmartClip PRIVATE "${_smartclip_cppwinrt}")
        target_compile_definitions(SmartClip PRIVATE SMARTCLIP_HAS_WINOCR=1)
        target_link_libraries(SmartClip PRIVATE windowsapp)
        message(STATUS "图上选字：用 Windows 自带 OCR（cppwinrt: ${_smartclip_cppwinrt}）")
    else()
        message(STATUS "图上选字：没找到 SDK 的 cppwinrt 头，OCR 这条路关掉（其余功能照旧）")
    endif()
endif()

include(GNUInstallDirs)
install(TARGETS SmartClip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
