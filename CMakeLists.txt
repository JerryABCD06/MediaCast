# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

cmake_minimum_required(VERSION 3.21)

# RC 是资源编译器。res/MCast.rc 里放的是程序图标和版本信息 ——
# 系统就是靠那份版本信息里的 FileDescription 认出这个程序叫 "Media Cast" 的，
# 缺了它，媒体面板只能显示可执行文件的路径。
project(MediaCastReceiver LANGUAGES CXX RC)

# 用 C++20 而不是 C++17，是被 C++/WinRT 逼的：它的 base.h 在 C++17 模式下会去
# 包含 <experimental/coroutine>，而 MSVC 14.51 已经把那个实验性的头文件判成编译
# 错误了（提示"即将移除"）。我们自己的代码没有任何必须留在 C++17 的写法，
# 整体提上去不影响别的文件 —— 整个工程已经用 C++20 完整编过一遍。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Widgets 是界面，Network 提供 QLocalSocket（命名管道通信要靠它）。
# Quick + QuickControls2 是给新界面（QML）用的，和旧的 Widgets 界面并存。
# LinguistTools 提供 qt_add_translations（lupdate / lrelease 那一套）。
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Quick QuickControls2 LinguistTools)

qt_standard_project_setup()

# 让 CMake 自动把 .qrc 编成 Qt 的资源。
# res/MCast.qrc 里装的是**窗口图标**（QApplication::setWindowIcon 用的那份），
# 和 res/MCast.rc 里那份各管一边 —— 为什么需要两份，写在 MCast.qrc 的注释里。
#
# 注意：这个开关必须在建 target 之前设好。target 一旦建出来，它的 AUTORCC 属性就
# 定下来了，后面再改这个变量对它没用。
set(CMAKE_AUTORCC ON)

# ── FluentUI（QML 组件库）───────────────────────────────────────────────
#
# 放在 third_party/ 下，和 mpv 那种"运行时依赖"分开：
# mpv 是预先编好的二进制，FluentUI 是要跟我们一起编的**源码依赖**。
#
# 为什么用 add_subdirectory 而不是引一个预编译包：FluentUI 是个 QML 插件，
# 它的 DLL 必须和主程序的构建配置对上（Debug 版要 fluentuiplugind.dll，
# Release 版要 fluentuiplugin.dll）。交给 CMake 一起编，它自己就配平了 ——
# 这个坑我们之前在 FluentUI3Style 上已经踩过一次。
#
# 注意 FLUENTUI_QML_PLUGIN_DIRECTORY 一定要改掉：它的默认值是
# <QtSDK>/qml/FluentUI，也就是往 Qt 安装目录里写东西。我们不干那事。
set(FLUENTUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLUENTUI_QML_PLUGIN_DIRECTORY
    "${CMAKE_BINARY_DIR}/fluentui-qml/FluentUI"
    CACHE PATH "" FORCE)
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FluentUI"
    "${CMAKE_BINARY_DIR}/fluentui-tree")

qt_add_executable(MCast
    src/main.cpp

    # 播放核心：协议无关的那一层
    src/core/MediaPlayer.h
    src/core/NowPlaying.h
    src/core/NowPlaying.cpp
    src/core/MpvCore.h
    src/core/MpvCore.cpp
    src/core/MpvQmlItem.h
    src/core/MpvQmlItem.cpp
    src/core/PlaybackController.h
    src/core/PlaybackController.cpp
    src/core/AppSettings.h
    src/core/AppSettings.cpp
    # 语言：有哪些、现在用哪个、哪个键对应哪句话。界面和托盘都走它。
    src/core/Tr.h
    src/core/Tr.cpp
    # 法律文本的入口：界面要什么它给什么，不碰界面（见文件头注释）
    src/core/LegalDocs.h
    src/core/LegalDocs.cpp
    src/core/LibMpvPlayer.h
    src/core/LibMpvPlayer.cpp
    src/core/MpvMediaPlayer.h
    src/core/MpvMediaPlayer.cpp

    # 协议：以后加 AirPlay 之类，就是再加一个同级的子目录
    src/protocols/dlna/DlnaRenderer.h
    src/protocols/dlna/DlnaRenderer.cpp
    src/protocols/dlna/SsdpService.h
    src/protocols/dlna/SsdpService.cpp
    src/protocols/dlna/HttpServer.h
    src/protocols/dlna/HttpServer.cpp
    src/protocols/dlna/UpnpXml.h
    src/protocols/dlna/UpnpXml.cpp
    src/protocols/dlna/SoapHandler.h
    src/protocols/dlna/SoapHandler.cpp
    src/protocols/dlna/GenaManager.h
    src/protocols/dlna/GenaManager.cpp

    # 跟操作系统打交道的地方，按平台分
    src/platform/windows/WindowsMediaControls.h
    src/platform/windows/WindowsMediaControls.cpp
    src/platform/windows/WindowFrame.h
    src/platform/windows/WindowFrame.cpp

    # 界面
    src/ui/TrayIcon.h
    src/ui/TrayIcon.cpp

    # 界面（QML）。旧的那套 Widgets 界面（MainWindow）2026-09-14 退场了。
    src/ui/NewUiWindow.h
    src/ui/NewUiWindow.cpp

    # 界面状态：语言 + 深浅的唯一源头，界面和原生控件都向它看齐。
    src/ui/UiState.h
    src/ui/UiState.cpp

    res/MCast.rc
    res/MCast.qrc
    res/QmlUi.qrc
    res/Lang.qrc
)

# 跨目录的 include 写成 "core/LibMpvPlayer.h" 这样从 src 起算的路径 ——
# 一眼看得出文件在哪儿。同目录的还是短名字（"MediaPlayer.h"）。
target_include_directories(MCast PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(MCast PRIVATE Qt6::Widgets Qt6::Network)

# QML 界面要用 Quick。FluentUI 那个插件是运行时由 QML 引擎加载的，
# 不需要链接，但必须先编出来 —— 下面拷模块目录时要用到它的产物。
target_link_libraries(MCast PRIVATE Qt6::Quick Qt6::QuickControls2)
add_dependencies(MCast fluentuiplugin)

# QML 引擎按 <导入路径>/FluentUI/qmldir 找模块，所以把整个模块目录
# 放到 exe 旁边的 qml/ 下。main() 里会把那个目录加进导入路径。
add_custom_command(TARGET MCast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FLUENTUI_QML_PLUGIN_DIRECTORY}"
            "$<TARGET_FILE_DIR:MCast>/qml/FluentUI"
    COMMENT "复制 FluentUI QML 模块到构建目录")

# ── Windows 媒体面板（SMTC）───────────────────────────────────────────────
#
# 就是按音量键弹出来的那个面板。WindowsMediaControls.cpp 用了 C++/WinRT，
# 而 cppwinrt 的头文件在 Windows SDK 的 cppwinrt 子目录里 —— 那个目录本来就在
# MSVC 的默认包含路径上，不用额外加 -I。
#
# 需要显式链接的只有 windowsapp.lib：RoGetActivationFactory、HSTRING 那一套
# 都从它转发过去，标准库里没有。
target_link_libraries(MCast PRIVATE windowsapp.lib)

# ── libmpv（内嵌播放后端）──────────────────────────────────────────────────
#
# 开发包放在项目的 runtime 目录下，和 mpv.exe 那一份并列。
#
# 注意 libmpv.lib 不是原包里的东西 —— 原包给的 libmpv.dll.a 是 MinGW 格式，MSVC 用不了。
# 这个是拿 dumpbin 的导出表生成 .def 之后，再用 MSVC 自带的 lib 命令生成的。
# 要重新生成（比如换了 mpv 版本）：
#   1. dumpbin /exports libmpv-2.dll      （只留 mpv_ 开头的那些名字）
#   2. 用它们写一个 libmpv.def，头两行是 LIBRARY libmpv-2.dll 和 EXPORTS
#   3. lib /def:libmpv.def /machine:x64 /out:libmpv.lib
set(MPV_DEV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/mpv-dev")

if(NOT (EXISTS "${MPV_DEV_DIR}/include/mpv/client.h"))
    message(FATAL_ERROR
        "找不到 libmpv 开发包：${MPV_DEV_DIR}\n"
        "需要 include/mpv/client.h 和 libmpv.lib。\n"
        "如果只是换了 mpv 版本，按上面注释里的三条命令重新生成导入库即可。")
endif()

target_include_directories(MCast PRIVATE "${MPV_DEV_DIR}/include")
target_link_libraries(MCast PRIVATE "${MPV_DEV_DIR}/libmpv.lib")

# 运行时也要能找到那个 DLL，直接复制到 exe 旁边。
add_custom_command(TARGET MCast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MPV_DEV_DIR}/libmpv-2.dll"
            "$<TARGET_FILE_DIR:MCast>"
    COMMENT "复制 libmpv-2.dll 到构建目录")

# 源码里有中文字符串。MSVC 默认按系统代码页(中文 Windows 上是 GBK)读取源文件，
# 会把 UTF-8 的中文读成乱码，所以显式要求它按 UTF-8 解释源码。
target_compile_options(MCast PRIVATE /utf-8)

set_target_properties(MCast PROPERTIES
    WIN32_EXECUTABLE TRUE
)

# ── 语言 ─────────────────────────────────────────────────────────────────
#
# 语言文件是 lang/*.json，明文，**不编译** —— 加一种语言就是往那个目录里丢一个
# 文件，程序启动时自己扫。所以这里没有 lupdate / lrelease 那一套（.ts / .qm /
# qt_add_translations 全都不用了），只做一件事：把整个目录拷到 exe 旁边。
#
# 规矩（文件格式、meta 两个字段、键名怎么起、怎么加一种语言）写在
# lang/README.md 里。
#
# 唯一编进 exe 的是基准语言 en_US.json（见 res/Lang.qrc）：lang 目录被删了、
# 或者发布时漏拷的时候靠它兜底 —— 界面最坏是英文，不会是满屏键名。
add_custom_command(TARGET MCast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/lang"
            "$<TARGET_FILE_DIR:MCast>/lang"
    COMMENT "复制语言文件到构建目录")

# ── 法律文本与第三方许可 ────────────────────────────────────────────────
#
# 这两样**必须跟着程序走**：GPL 和 LGPL 都要求分发时附带许可文本，而且
# 程序里的"法律"页要当场读它们（见 src/core/LegalDocs.h 里为什么读文件、
# 不编进 qrc）。开发目录里也拷一份 —— 不然本地跑起来法律页是空的，
# 而且启动自检会一直报"许可文件缺失"。
#
# legal/ 只放**面向用户**的那几份；内部维护用的（CHECKLIST、RELEASE-NOTICES、
# SUPPORT、CODE_OF_CONDUCT）留在 docs/legal/ 不下发。
add_custom_command(TARGET MCast POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:MCast>/legal"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/PRIVACY.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/LEGAL-NOTICES.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/TRADEMARKS.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/CODECS.md"
            "$<TARGET_FILE_DIR:MCast>/legal"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/licenses"
            "$<TARGET_FILE_DIR:MCast>/licenses"
    COMMENT "复制法律声明与第三方许可到构建目录")
