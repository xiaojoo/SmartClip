# QScintilla 2.14.1 —— 内置编译（vendored）
#
# 为什么内置而不是找系统包：
#   1) 官方只提供 qmake 工程，没有 CMakeLists，集成到本项目必须自己写一份；
#   2) 需要固定版本并保证 Qt6 下的可重现构建。
#
# 编译要点（这几条都是实测踩出来的，改动前先看）：
#   * 不能定义 QT_NO_KEYWORDS —— QScintilla 自己的 moc 代码要用 signals/slots 关键字。
#   * Scintilla 本体的源文件是 .cpp（不是 .cxx），漏了会出现上百个无法解析的外部符号。
#   * Qsci/*.h 里有 Q_OBJECT 但对应 .cpp 不含它，必须把头文件显式加进目标，
#     否则 AUTOMOC 不扫，链接时报 metaObject / qt_metacast 缺失。
#   * Catalogue.cpp 引用全部 lexer，lexers/*.cpp 必须全编。
#
# 授权：QScintilla 本体为 GPLv3 或 Riverbank 商业授权（见该目录 LICENSE）；
#       其内的 Scintilla 为宽松许可。本项目按 GPLv3 使用时无需额外处理。
cmake_minimum_required(VERSION 3.21)

set(QSCI_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(SCINTILLA_DIR "${QSCI_DIR}/scintilla")

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets PrintSupport)

file(GLOB QSCI_SOURCES
    "${QSCI_DIR}/src/*.cpp"
    "${SCINTILLA_DIR}/src/*.cpp"
    "${SCINTILLA_DIR}/lexlib/*.cpp"
    "${SCINTILLA_DIR}/lexers/*.cpp"
)
file(GLOB QSCI_HEADERS "${QSCI_DIR}/src/Qsci/*.h")

add_library(qscintilla STATIC ${QSCI_SOURCES} ${QSCI_HEADERS})

target_include_directories(qscintilla
    PUBLIC
        "${QSCI_DIR}/src"
        "${QSCI_DIR}/src/Qsci"
        "${SCINTILLA_DIR}/include"
        "${SCINTILLA_DIR}/src"
    PRIVATE
        "${SCINTILLA_DIR}/lexlib"
)

target_compile_definitions(qscintilla PRIVATE
    SCINTILLA_QT
    SCI_LEXER
    INCLUDE_DEPRECATED_FEATURES
)

# /bigobj：lexer 与 Scintilla 的翻译单元很大，默认目标文件段数会超
target_compile_options(qscintilla PRIVATE /bigobj)

target_link_libraries(qscintilla PUBLIC
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::PrintSupport
)

# AUTOMOC 必须开：Qsci 的类都靠它生成元对象
set_target_properties(qscintilla PROPERTIES AUTOMOC ON)

# 第三方代码的警告不看
if(MSVC)
    # 用 /W0（"警告级别 0"）而不是 /w（"压制所有警告"）：
    #
    # 两个都是"不看警告"，但生成器对它们的处理不一样。/w 只能塞进
    # AdditionalOptions，于是生成的 vcxproj 里 WarningLevel 属性是空的，
    # MSBuild 就按默认补一个 /W1 —— 命令行上 /W1 和 /w 撞在一起，cl 每个
    # 翻译单元都吐一条 warning D9025（正在重写"/W1"用"/w"）。一条无害的
    # 警告换来 300+ 条噪音。
    #
    # /W0 认得出来是"警告级别"这个属性（WarningLevel=TurnOffAllWarnings），
    # MSBuild 就不会再补 /W1，D9025 自然没有。压制效果一样。
    target_compile_options(qscintilla PRIVATE /W0)
endif()
