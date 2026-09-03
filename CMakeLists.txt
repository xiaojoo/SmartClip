cmake_minimum_required(VERSION 3.21)
project(SmartClip VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick Sql Widgets)

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
    QML_FILES qml/Main.qml
)

target_link_libraries(SmartClip PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::Sql Qt6::Widgets)

include(GNUInstallDirs)
install(TARGETS SmartClip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
