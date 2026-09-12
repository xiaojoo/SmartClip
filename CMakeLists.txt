cmake_minimum_required(VERSION 3.21)
project(SmartClip VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 QuickWidgets Sql Widgets PrintSupport)

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

qt_add_executable(SmartClip
    src/main.cpp
    src/ClipboardStore.h
    src/ClipboardStore.cpp
    src/ClipboardManager.h
    src/ClipboardManager.cpp
    src/WindowHelper.h
    src/WindowHelper.cpp
    src/EditorController.h
    src/EditorController.cpp
    src/EditorViewItem.h
    src/EditorViewItem.cpp
    src/SelfTest.h
    src/SelfTest.cpp
)

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
        qml/components/FindBar.qml
        qml/components/FolderTree.qml
        qml/components/SettingsPanel.qml
        qml/components/StatusBar.qml
        qml/components/ToolButton.qml
        qml/components/TopBar.qml
        qml/components/WindowControls.qml
        qml/delegates/TreeDelegate.qml
        qml/models/ClipboardModel.qml
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
        resources/icons/play.svg
        resources/icons/plus.svg
        resources/icons/refresh.svg
        resources/icons/search.svg
        resources/icons/trash.svg
)

target_link_libraries(SmartClip PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::QuickWidgets Qt6::Sql
    Qt6::Widgets Qt6::PrintSupport
    qscintilla
)

include(GNUInstallDirs)
install(TARGETS SmartClip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
