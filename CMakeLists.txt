cmake_minimum_required(VERSION 3.21)
project(SmartClip VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Sql Widgets)

qt_add_executable(SmartClip
    src/main.cpp
    src/ClipboardStore.h
    src/ClipboardStore.cpp
    src/ClipboardManager.h
    src/ClipboardManager.cpp
)

qt_add_qml_module(SmartClip
    URI SmartClip
    VERSION 1.0
    QML_FILES
        Main.qml
        qml/components/DropdownMenu.qml
        qml/components/EditorArea.qml
        qml/components/FolderTree.qml
        qml/components/MainToolBar.qml
        qml/components/StatusBar.qml
        qml/delegates/TreeDelegate.qml
        qml/models/ClipboardModel.qml
        qml/utils/IconProvider.qml
        qml/utils/AppIcon.qml
        qml/utils/ThinScrollBar.qml
        js/ClipboardManager.js
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

target_link_libraries(SmartClip PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::Sql Qt6::Widgets)

include(GNUInstallDirs)
install(TARGETS SmartClip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})