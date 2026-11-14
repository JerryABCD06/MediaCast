# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

cmake_minimum_required(VERSION 3.21)

# RC 是资源编译器。三个进程各自的 .rc 文件里放程序图标和版本信息；Qt 窗口图标
# 另由 MCast.qrc 提供。
project(MediaCastReceiver LANGUAGES CXX RC)

include(CTest)

# The runtime dependency is shared by the application and the headless MPV
# integration test. Keep its location defined before the test targets.
set(MPV_DEV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/mpv-dev")

# 用 C++20 而不是 C++17，是被 C++/WinRT 逼的：它的 base.h 在 C++17 模式下会去
# 包含 <experimental/coroutine>，而 MSVC 14.51 已经把那个实验性的头文件判成编译
# 错误了（提示"即将移除"）。我们自己的代码没有任何必须留在 C++17 的写法，
# 整体提上去不影响别的文件 —— 整个工程已经用 C++20 完整编过一遍。
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── 跨进程基础设施 ──────────────────────────────────────────────────────
#
# A（协议）/ B（前端）/ C（Shell）后续会共用这一层。它刻意不链接 Qt：
# 消息布局、长度约束和管道行为都必须能脱离 Qt/QML 单独测试。
add_library(mcast_ipc STATIC
    src/ipc/IpcProtocol.h
    src/ipc/IpcProtocol.cpp
    src/ipc/NamedPipe.h
    src/ipc/NamedPipe.cpp
    src/ipc/ProtocolControlDispatcher.h
    src/ipc/ProtocolControlDispatcher.cpp
    src/ipc/Messages.h
    src/ipc/Messages.cpp
    src/ipc/PlaybackCodec.h
    src/ipc/PlaybackCodec.cpp
    src/ipc/PlaybackPipeGateway.h
    src/ipc/PlaybackPipeGateway.cpp
    src/playback/PlaybackCommandCoalescer.h
    src/playback/PlaybackCommandCoalescer.cpp
    src/playback/PlanningPlaybackGateway.h
    src/playback/PlanningPlaybackGateway.cpp
    src/playback/LateBindingPlaybackGateway.h
    src/playback/LateBindingPlaybackGateway.cpp
    src/supervisor/RestartController.h
    src/supervisor/RestartController.cpp
    src/supervisor/ProcessIdentity.h
    src/supervisor/ProcessIdentity.cpp
    src/supervisor/ChildProcess.h
    src/supervisor/ChildProcess.cpp
)
target_compile_features(mcast_ipc PUBLIC cxx_std_20)
target_compile_options(mcast_ipc PUBLIC /utf-8)
target_include_directories(mcast_ipc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mcast_ipc PUBLIC advapi32.lib bcrypt.lib)

add_library(mcast_net STATIC
    src/platform/windows/net/SocketRuntime.h
    src/platform/windows/net/SocketRuntime.cpp)
target_compile_features(mcast_net PUBLIC cxx_std_20)
target_compile_options(mcast_net PUBLIC /utf-8)
target_include_directories(mcast_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mcast_net PUBLIC ws2_32.lib iphlpapi.lib)

add_library(mcast_cast STATIC
    src/cast/CastTypes.h
    src/cast/CastArbiter.h
    src/cast/CastArbiter.cpp
    src/cast/CastClaimCoordinator.h
    src/cast/CastClaimCoordinator.cpp)
target_compile_features(mcast_cast PUBLIC cxx_std_20)
target_compile_options(mcast_cast PUBLIC /utf-8)
target_include_directories(
    mcast_cast PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(mcast_dlna_core STATIC
    src/protocols/dlna/UpnpXml.h
    src/protocols/dlna/UpnpXml.cpp
    src/protocols/dlna/core/HttpRequest.h
    src/protocols/dlna/core/HttpRequest.cpp
    src/protocols/dlna/core/SsdpMessages.h
    src/protocols/dlna/core/SsdpMessages.cpp)
target_compile_features(mcast_dlna_core PUBLIC cxx_std_20)
target_compile_options(mcast_dlna_core PUBLIC /utf-8)
target_include_directories(
    mcast_dlna_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

add_library(mcast_dlna_net STATIC
    src/protocols/dlna/net/HttpServer.h
    src/protocols/dlna/net/HttpServer.cpp
    src/protocols/dlna/net/SsdpInterfaceSelector.h
    src/protocols/dlna/net/SsdpInterfaceSelector.cpp
    src/protocols/dlna/net/SsdpService.h
    src/protocols/dlna/net/SsdpService.cpp
    src/protocols/dlna/net/SsdpSocket.h
    src/protocols/dlna/net/SsdpSocket.cpp)
target_compile_features(mcast_dlna_net PUBLIC cxx_std_20)
target_compile_options(mcast_dlna_net PUBLIC /utf-8)
target_include_directories(
    mcast_dlna_net PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(mcast_dlna_net PUBLIC mcast_net mcast_dlna_core)

add_library(mcast_dlna_protocol STATIC
    src/protocols/dlna/claim/DlnaClaimAdapter.h
    src/protocols/dlna/claim/DlnaClaimAdapter.cpp
    src/protocols/dlna/claim/DlnaClaimRuntime.h
    src/protocols/dlna/claim/DlnaClaimRuntime.cpp
    src/protocols/dlna/claim/DlnaClaimHttpHandler.h
    src/protocols/dlna/claim/DlnaClaimHttpHandler.cpp
    src/protocols/dlna/runtime/DlnaHttpFrontend.h
    src/protocols/dlna/runtime/DlnaHttpFrontend.cpp
    src/protocols/dlna/runtime/DlnaClaimConfirmationWorker.h
    src/protocols/dlna/runtime/DlnaClaimConfirmationWorker.cpp
    src/protocols/dlna/runtime/GenaManager.h
    src/protocols/dlna/runtime/GenaManager.cpp
    src/protocols/dlna/runtime/DlnaProtocolRuntime.h
    src/protocols/dlna/runtime/DlnaProtocolRuntime.cpp
    src/protocols/dlna/playback/DlnaPlaybackPlanner.h
    src/protocols/dlna/playback/DlnaPlaybackPlanner.cpp
    src/protocols/dlna/session/DlnaSession.h
    src/protocols/dlna/session/DlnaSession.cpp
    src/protocols/dlna/soap/SoapRouter.h
    src/protocols/dlna/soap/SoapRouter.cpp
    src/protocols/dlna/soap/SoapXml.h
    src/protocols/dlna/soap/SoapXml.cpp)
target_compile_features(mcast_dlna_protocol PUBLIC cxx_std_20)
target_compile_options(mcast_dlna_protocol PUBLIC /utf-8)
target_include_directories(
    mcast_dlna_protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
    mcast_dlna_protocol
    PUBLIC mcast_dlna_net mcast_cast mcast_ipc
    PRIVATE xmllite.lib shlwapi.lib ole32.lib winhttp.lib)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE MCAST_BUILD_ID
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT MCAST_BUILD_ID)
    set(MCAST_BUILD_ID "unknown")
endif()
target_compile_definitions(mcast_ipc PUBLIC MCAST_BUILD_ID="${MCAST_BUILD_ID}")

# 三个生产进程。这里先定义轻量 UiSkeleton，供进程监督和入口保护测试使用；
# 真实 Qt/QML Ui 在文件后半部分定义。
add_executable(
    MCast.Shell
    src/shell/ShellLanguage.h
    src/shell/ShellLanguage.cpp
    src/shell/ShellSettings.h
    src/shell/ShellSettings.cpp
    src/shell/ShellMain.cpp
    src/platform/windows/TrayIcon.h
    src/platform/windows/TrayIcon.cpp
    src/platform/windows/SplashWindow.h
    src/platform/windows/SplashWindow.cpp
    res/MCast.Shell.rc)
target_include_directories(
    MCast.Shell PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(MCast.Shell PRIVATE /utf-8)
set(MCAST_COMMON_CONTROLS_MANIFEST
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
target_link_libraries(
    MCast.Shell PRIVATE
    mcast_ipc dwmapi.lib d2d1.lib d3d11.lib dxgi.lib dcomp.lib
    windowscodecs.lib advapi32.lib ole32.lib shell32.lib uxtheme.lib
    windowsapp.lib)
set_target_properties(MCast.Shell PROPERTIES WIN32_EXECUTABLE TRUE)

add_executable(
    MCast.Protocol
    src/skeleton/ProtocolMain.cpp
    src/skeleton/SkeletonClient.cpp
    src/skeleton/ProtocolDlnaConfirmationPort.cpp
    src/skeleton/ProtocolDlnaConfirmationPort.h
    res/MCast.Protocol.rc)
target_link_libraries(
    MCast.Protocol PRIVATE mcast_ipc mcast_dlna_protocol)
set_target_properties(MCast.Protocol PROPERTIES WIN32_EXECUTABLE TRUE)

add_dependencies(MCast.Shell MCast.Protocol)

if(BUILD_TESTING)
    add_executable(
        MCast.UiSkeleton
        src/skeleton/UiMain.cpp
        src/skeleton/SkeletonClient.cpp
        src/skeleton/ProtocolDlnaConfirmationPort.cpp)
    target_link_libraries(
        MCast.UiSkeleton PRIVATE mcast_ipc mcast_dlna_protocol)
    add_dependencies(MCast.Shell MCast.UiSkeleton)

    add_executable(
        mcast_qtfree_event_loop_tests
        tests/qtfree_event_loop_tests.cpp
        src/ui_avalonia_next/native/QtFree/EventLoop.cpp)
    target_include_directories(
        mcast_qtfree_event_loop_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui_avalonia_next/native")
    target_compile_options(
        mcast_qtfree_event_loop_tests PRIVATE /utf-8)
    add_test(
        NAME mcast_qtfree_event_loop_tests
        COMMAND mcast_qtfree_event_loop_tests)
    set_tests_properties(
        mcast_qtfree_event_loop_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_qtfree_json_tests
        tests/qtfree_json_tests.cpp
        src/ui_avalonia_next/native/QtFree/Json.cpp)
    target_include_directories(
        mcast_qtfree_json_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui_avalonia_next/native")
    target_compile_options(
        mcast_qtfree_json_tests PRIVATE /utf-8)
    add_test(
        NAME mcast_qtfree_json_tests
        COMMAND mcast_qtfree_json_tests)
    set_tests_properties(
        mcast_qtfree_json_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_qtfree_media_types_tests
        tests/qtfree_media_types_tests.cpp
        src/ui_avalonia_next/native/QtFree/MediaTypes.cpp)
    target_include_directories(
        mcast_qtfree_media_types_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui_avalonia_next/native")
    target_compile_options(
        mcast_qtfree_media_types_tests PRIVATE /utf-8)
    add_test(
        NAME mcast_qtfree_media_types_tests
        COMMAND mcast_qtfree_media_types_tests)
    set_tests_properties(
        mcast_qtfree_media_types_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_qtfree_player_contract_tests
        tests/qtfree_player_contract_tests.cpp)
    target_include_directories(
        mcast_qtfree_player_contract_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui_avalonia_next/native")
    target_compile_options(
        mcast_qtfree_player_contract_tests PRIVATE /utf-8)
    add_test(
        NAME mcast_qtfree_player_contract_tests
        COMMAND mcast_qtfree_player_contract_tests)
    set_tests_properties(
        mcast_qtfree_player_contract_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_qtfree_metadata_pipeline_tests
        tests/qtfree_metadata_pipeline_tests.cpp
        src/ui_avalonia_next/native/QtFree/Json.cpp
        src/ui_avalonia_next/native/QtFree/MediaTypes.cpp
        src/ui_avalonia_next/native/QtFree/MetadataPipeline.cpp)
    target_include_directories(
        mcast_qtfree_metadata_pipeline_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/ui_avalonia_next/native")
    target_compile_options(
        mcast_qtfree_metadata_pipeline_tests PRIVATE /utf-8)
    add_test(
        NAME mcast_qtfree_metadata_pipeline_tests
        COMMAND mcast_qtfree_metadata_pipeline_tests)
    set_tests_properties(
        mcast_qtfree_metadata_pipeline_tests PROPERTIES TIMEOUT 15)

    add_executable(mcast_ipc_tests tests/ipc_tests.cpp)
    target_link_libraries(mcast_ipc_tests PRIVATE mcast_ipc)
    add_test(NAME mcast_ipc_tests COMMAND mcast_ipc_tests)
    set_tests_properties(mcast_ipc_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_metadata_pipeline_tests
        tests/metadata_pipeline_tests.cpp
        src/core/MetadataPipeline.cpp
        src/core/MetadataPipeline.h
        src/core/NowPlaying.cpp
        src/core/NowPlaying.h)
    target_include_directories(
        mcast_metadata_pipeline_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(
        mcast_metadata_pipeline_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_metadata_pipeline_tests PRIVATE Qt6::Core)
    set_target_properties(
        mcast_metadata_pipeline_tests PROPERTIES AUTOMOC TRUE)
    add_test(
        NAME mcast_metadata_pipeline_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_runner.ps1"
            -Executable "$<TARGET_FILE:mcast_metadata_pipeline_tests>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(
        mcast_metadata_pipeline_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_protocol_control_dispatcher_tests
        tests/protocol_control_dispatcher_tests.cpp)
    target_link_libraries(
        mcast_protocol_control_dispatcher_tests PRIVATE mcast_ipc)
    add_test(
        NAME mcast_protocol_control_dispatcher_tests
        COMMAND mcast_protocol_control_dispatcher_tests)
    set_tests_properties(
        mcast_protocol_control_dispatcher_tests PROPERTIES TIMEOUT 15)

    add_executable(
        mcast_protocol_dlna_confirmation_port_tests
        tests/protocol_dlna_confirmation_port_tests.cpp
        src/skeleton/ProtocolDlnaConfirmationPort.cpp)
    target_link_libraries(
        mcast_protocol_dlna_confirmation_port_tests
        PRIVATE mcast_ipc mcast_dlna_protocol)
    add_test(
        NAME mcast_protocol_dlna_confirmation_port_tests
        COMMAND mcast_protocol_dlna_confirmation_port_tests)
    set_tests_properties(
        mcast_protocol_dlna_confirmation_port_tests PROPERTIES TIMEOUT 15)

    add_executable(
        mcast_net_tests
        tests/socket_runtime_tests.cpp)
    target_link_libraries(mcast_net_tests PRIVATE mcast_net)
    add_test(NAME mcast_net_tests COMMAND mcast_net_tests)

    add_executable(
        mcast_cast_arbiter_tests
        tests/cast_arbiter_tests.cpp)
    target_link_libraries(mcast_cast_arbiter_tests PRIVATE mcast_cast)
    add_test(
        NAME mcast_cast_arbiter_tests
        COMMAND mcast_cast_arbiter_tests)
    set_tests_properties(mcast_cast_arbiter_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_cast_confirmation_dialog_smoke
        tests/cast_confirmation_dialog_smoke.cpp
        src/platform/windows/CastConfirmationDialog.h
        src/platform/windows/CastConfirmationDialog.cpp)
    target_include_directories(
        mcast_cast_confirmation_dialog_smoke PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(
        mcast_cast_confirmation_dialog_smoke PRIVATE /utf-8)
    target_link_options(
        mcast_cast_confirmation_dialog_smoke PRIVATE
        ${MCAST_COMMON_CONTROLS_MANIFEST})
    target_link_libraries(
        mcast_cast_confirmation_dialog_smoke PRIVATE comctl32.lib)

    add_executable(
        mcast_playback_codec_tests
        tests/playback_codec_tests.cpp)
    target_link_libraries(mcast_playback_codec_tests PRIVATE mcast_ipc)
    add_test(
        NAME mcast_playback_codec_tests
        COMMAND mcast_playback_codec_tests)

    add_executable(
        mcast_playback_pipe_gateway_tests
        tests/playback_pipe_gateway_tests.cpp)
    target_link_libraries(
        mcast_playback_pipe_gateway_tests PRIVATE mcast_ipc)
    add_test(
        NAME mcast_playback_pipe_gateway_tests
        COMMAND mcast_playback_pipe_gateway_tests)

    add_executable(
        mcast_playback_command_coalescer_tests
        tests/playback_command_coalescer_tests.cpp)
    target_link_libraries(
        mcast_playback_command_coalescer_tests PRIVATE mcast_ipc)
    add_test(
        NAME mcast_playback_command_coalescer_tests
        COMMAND mcast_playback_command_coalescer_tests)

    add_executable(
        mcast_late_binding_playback_gateway_tests
        tests/late_binding_playback_gateway_tests.cpp)
    target_link_libraries(
        mcast_late_binding_playback_gateway_tests PRIVATE mcast_ipc)
    add_test(
        NAME mcast_late_binding_playback_gateway_tests
        COMMAND mcast_late_binding_playback_gateway_tests)

    add_executable(
        mcast_playback_bridge_tests
        tests/playback_bridge_tests.cpp
        src/ui_process/PlaybackBridge.cpp
        src/ui_process/PlaybackBridge.h
        src/core/MetadataPipeline.cpp
        src/core/MetadataPipeline.h
        src/core/PlaybackController.cpp
        src/core/PlaybackController.h
        src/core/MediaPlayer.h
        src/core/NowPlaying.cpp
        src/core/NowPlaying.h)
    target_include_directories(
        mcast_playback_bridge_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(mcast_playback_bridge_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_playback_bridge_tests PRIVATE mcast_ipc Qt6::Core)
    set_target_properties(
        mcast_playback_bridge_tests PROPERTIES AUTOMOC TRUE)
    add_test(
        NAME mcast_playback_bridge_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_runner.ps1"
            -Executable "$<TARGET_FILE:mcast_playback_bridge_tests>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(
        mcast_planning_playback_gateway_tests
        tests/planning_playback_gateway_tests.cpp)
    target_link_libraries(
        mcast_planning_playback_gateway_tests PRIVATE mcast_ipc)
    add_test(
        NAME mcast_planning_playback_gateway_tests
        COMMAND mcast_planning_playback_gateway_tests)
    set_tests_properties(
        mcast_planning_playback_gateway_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_ui_settings_model_tests
        tests/ui_settings_model_tests.cpp
        src/ui_process/UiSettingsModel.h
        src/ui_process/UiSettingsModel.cpp)
    target_include_directories(
        mcast_ui_settings_model_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(mcast_ui_settings_model_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_ui_settings_model_tests PRIVATE Qt6::Core)
    set_target_properties(
        mcast_ui_settings_model_tests PROPERTIES AUTOMOC TRUE)
    add_test(
        NAME mcast_ui_settings_model_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_runner.ps1"
            -Executable "$<TARGET_FILE:mcast_ui_settings_model_tests>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(
        mcast_ui_interaction_state_tests
        tests/ui_interaction_state_tests.cpp
        src/ui_process/UiInteractionState.h
        src/ui_process/UiInteractionState.cpp)
    target_include_directories(
        mcast_ui_interaction_state_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(
        mcast_ui_interaction_state_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_ui_interaction_state_tests PRIVATE Qt6::Core)
    set_target_properties(
        mcast_ui_interaction_state_tests PROPERTIES AUTOMOC TRUE)
    add_test(
        NAME mcast_ui_interaction_state_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_runner.ps1"
            -Executable "$<TARGET_FILE:mcast_ui_interaction_state_tests>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(
        mcast_shell_settings_tests
        tests/shell_settings_tests.cpp
        src/shell/ShellSettings.cpp)
    target_include_directories(
        mcast_shell_settings_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(mcast_shell_settings_tests PRIVATE /utf-8)
    target_link_libraries(mcast_shell_settings_tests PRIVATE windowsapp.lib)
    add_test(NAME mcast_shell_settings_tests COMMAND mcast_shell_settings_tests)

    add_executable(
        mcast_upnp_xml_tests
        tests/upnp_xml_tests.cpp)
    target_include_directories(
        mcast_upnp_xml_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(mcast_upnp_xml_tests PRIVATE /utf-8)
    target_link_libraries(mcast_upnp_xml_tests PRIVATE mcast_dlna_core)
    add_test(NAME mcast_upnp_xml_tests COMMAND mcast_upnp_xml_tests)

    add_executable(
        mcast_dlna_core_tests
        tests/dlna_core_tests.cpp)
    target_link_libraries(mcast_dlna_core_tests PRIVATE mcast_dlna_core)
    add_test(NAME mcast_dlna_core_tests COMMAND mcast_dlna_core_tests)

    add_executable(mcast_dlna_net_tests tests/dlna_net_tests.cpp)
    target_link_libraries(mcast_dlna_net_tests PRIVATE mcast_dlna_net)
    add_test(NAME mcast_dlna_net_tests COMMAND mcast_dlna_net_tests)
    set_tests_properties(mcast_dlna_net_tests PROPERTIES TIMEOUT 20)

    add_executable(
        mcast_ssdp_service_tests
        tests/ssdp_service_tests.cpp)
    target_link_libraries(
        mcast_ssdp_service_tests PRIVATE mcast_dlna_net)
    add_test(
        NAME mcast_ssdp_service_tests
        COMMAND mcast_ssdp_service_tests)
    set_tests_properties(
        mcast_ssdp_service_tests PROPERTIES TIMEOUT 20)

    add_executable(mcast_http_server_tests tests/http_server_tests.cpp)
    target_link_libraries(mcast_http_server_tests PRIVATE mcast_dlna_net)
    add_test(NAME mcast_http_server_tests COMMAND mcast_http_server_tests)
    set_tests_properties(mcast_http_server_tests PROPERTIES TIMEOUT 20)

    add_executable(
        mcast_playback_gateway_tests
        tests/playback_gateway_tests.cpp)
    target_include_directories(
        mcast_playback_gateway_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(mcast_playback_gateway_tests PRIVATE /utf-8)
    add_test(
        NAME mcast_playback_gateway_tests
        COMMAND mcast_playback_gateway_tests)

    add_executable(
        mcast_dlna_session_tests
        tests/dlna_session_tests.cpp)
    target_include_directories(
        mcast_dlna_session_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(mcast_dlna_session_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_dlna_session_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_session_tests
        COMMAND mcast_dlna_session_tests)
    set_tests_properties(mcast_dlna_session_tests PROPERTIES TIMEOUT 20)

    add_executable(
        mcast_dlna_playback_planner_tests
        tests/dlna_playback_planner_tests.cpp)
    target_link_libraries(
        mcast_dlna_playback_planner_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_playback_planner_tests
        COMMAND mcast_dlna_playback_planner_tests)
    set_tests_properties(
        mcast_dlna_playback_planner_tests PROPERTIES TIMEOUT 10)

    add_executable(mcast_soap_router_tests tests/soap_router_tests.cpp)
    target_link_libraries(
        mcast_soap_router_tests PRIVATE mcast_dlna_protocol)
    add_test(NAME mcast_soap_router_tests COMMAND mcast_soap_router_tests)
    set_tests_properties(mcast_soap_router_tests PROPERTIES TIMEOUT 20)

    add_executable(
        mcast_dlna_claim_adapter_tests
        tests/dlna_claim_adapter_tests.cpp)
    target_link_libraries(
        mcast_dlna_claim_adapter_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_claim_adapter_tests
        COMMAND mcast_dlna_claim_adapter_tests)
    set_tests_properties(
        mcast_dlna_claim_adapter_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_dlna_claim_runtime_tests
        tests/dlna_claim_runtime_tests.cpp)
    target_link_libraries(
        mcast_dlna_claim_runtime_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_claim_runtime_tests
        COMMAND mcast_dlna_claim_runtime_tests)
    set_tests_properties(
        mcast_dlna_claim_runtime_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_dlna_claim_http_handler_tests
        tests/dlna_claim_http_handler_tests.cpp)
    target_link_libraries(
        mcast_dlna_claim_http_handler_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_claim_http_handler_tests
        COMMAND mcast_dlna_claim_http_handler_tests)
    set_tests_properties(
        mcast_dlna_claim_http_handler_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_dlna_http_frontend_tests
        tests/dlna_http_frontend_tests.cpp)
    target_link_libraries(
        mcast_dlna_http_frontend_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_http_frontend_tests
        COMMAND mcast_dlna_http_frontend_tests)
    set_tests_properties(
        mcast_dlna_http_frontend_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_dlna_protocol_runtime_tests
        tests/dlna_protocol_runtime_tests.cpp)
    target_link_libraries(
        mcast_dlna_protocol_runtime_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_protocol_runtime_tests
        COMMAND mcast_dlna_protocol_runtime_tests)
    set_tests_properties(
        mcast_dlna_protocol_runtime_tests PROPERTIES TIMEOUT 15)

    add_test(
        NAME mcast_dlna_shadow_e2e
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/dlna_shadow_e2e.ps1"
            -ShellExe "$<TARGET_FILE:MCast.Shell>"
            -ProtocolExe "$<TARGET_FILE:MCast.Protocol>"
            -UiExe "$<TARGET_FILE:MCast.Ui>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(
        mcast_shell_language_tests
        tests/shell_language_tests.cpp
        src/shell/ShellLanguage.cpp
        src/shell/ShellLanguage.h)
    target_include_directories(
        mcast_shell_language_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(
        mcast_shell_language_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_shell_language_tests PRIVATE windowsapp.lib)
    add_test(
        NAME mcast_shell_language_tests
        COMMAND mcast_shell_language_tests)
    set_tests_properties(
        mcast_shell_language_tests PROPERTIES TIMEOUT 10)
    set_tests_properties(
        mcast_dlna_shadow_e2e
        PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)

    add_executable(
        mcast_gena_manager_tests
        tests/gena_manager_tests.cpp)
    target_link_libraries(
        mcast_gena_manager_tests PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_gena_manager_tests
        COMMAND mcast_gena_manager_tests)
    set_tests_properties(
        mcast_gena_manager_tests PROPERTIES TIMEOUT 15)

    add_executable(
        mcast_dlna_claim_confirmation_worker_tests
        tests/dlna_claim_confirmation_worker_tests.cpp)
    target_link_libraries(
        mcast_dlna_claim_confirmation_worker_tests
        PRIVATE mcast_dlna_protocol)
    add_test(
        NAME mcast_dlna_claim_confirmation_worker_tests
        COMMAND mcast_dlna_claim_confirmation_worker_tests)
    set_tests_properties(
        mcast_dlna_claim_confirmation_worker_tests PROPERTIES TIMEOUT 10)

    add_executable(
        mcast_image_sequence_playlist_tests
        tests/image_sequence_playlist_tests.cpp
        src/core/ImageSequencePlaylist.cpp
        src/core/ImageSequencePlaylist.h)
    target_include_directories(
        mcast_image_sequence_playlist_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_options(
        mcast_image_sequence_playlist_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_image_sequence_playlist_tests PRIVATE Qt6::Core)
    add_test(
        NAME mcast_image_sequence_playlist_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_runner.ps1"
            -Executable "$<TARGET_FILE:mcast_image_sequence_playlist_tests>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")

    add_executable(
        mcast_mpv_image_sequence_tests
        tests/mpv_image_sequence_tests.cpp
        src/core/MediaPlayer.h
        src/core/MpvCore.cpp
        src/core/MpvCore.h
        src/core/ImageSequencePlaylist.cpp
        src/core/ImageSequencePlaylist.h
        src/core/ImageSequenceStrategy.cpp
        src/core/ImageSequenceStrategy.h
        src/core/WrappedTsProxy.cpp
        src/core/WrappedTsProxy.h)
    target_include_directories(
        mcast_mpv_image_sequence_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${MPV_DEV_DIR}/include")
    target_compile_options(
        mcast_mpv_image_sequence_tests PRIVATE /utf-8)
    target_link_libraries(
        mcast_mpv_image_sequence_tests PRIVATE
        Qt6::Widgets Qt6::Network "${MPV_DEV_DIR}/libmpv.lib")
    set_target_properties(
        mcast_mpv_image_sequence_tests PROPERTIES AUTOMOC TRUE)
    add_test(
        NAME mcast_mpv_image_sequence_tests
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/qt_test_runner.ps1"
            -Executable "$<TARGET_FILE:mcast_mpv_image_sequence_tests>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(
        mcast_mpv_image_sequence_tests PROPERTIES TIMEOUT 15)

    add_test(
        NAME mcast_process_skeleton_smoke
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_skeleton_smoke.ps1"
            -ShellExe "$<TARGET_FILE:MCast.Shell>"
            -ProtocolExe "$<TARGET_FILE:MCast.Protocol>"
            -UiExe "$<TARGET_FILE:MCast.UiSkeleton>")
    set_tests_properties(mcast_process_skeleton_smoke PROPERTIES TIMEOUT 20)

    add_test(
        NAME mcast_process_same_shell_two_ui_rounds
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_skeleton_smoke.ps1"
            -ShellExe "$<TARGET_FILE:MCast.Shell>"
            -ProtocolExe "$<TARGET_FILE:MCast.Protocol>"
            -UiExe "$<TARGET_FILE:MCast.UiSkeleton>"
            -UiRounds 2)
    set_tests_properties(
        mcast_process_same_shell_two_ui_rounds PROPERTIES TIMEOUT 30)

    add_test(
        NAME mcast_shell_single_instance_handoff
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/shell_single_instance.ps1"
            -ShellExe "$<TARGET_FILE:MCast.Shell>"
            -ProtocolExe "$<TARGET_FILE:MCast.Protocol>"
            -UiExe "$<TARGET_FILE:MCast.UiSkeleton>")
    set_tests_properties(
        mcast_shell_single_instance_handoff PROPERTIES TIMEOUT 30)

    add_test(
        NAME mcast_shell_foreground_handoff_race
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/shell_foreground_handoff_race.ps1"
            -ShellExe "$<TARGET_FILE:MCast.Shell>"
            -ProtocolExe "$<TARGET_FILE:MCast.Protocol>"
            -UiExe "$<TARGET_FILE:MCast.UiSkeleton>")
    set_tests_properties(
        mcast_shell_foreground_handoff_race PROPERTIES TIMEOUT 30)

    add_test(
        NAME mcast_protocol_receiving_command
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_command.ps1"
            -ShellExe "$<TARGET_FILE:MCast.Shell>"
            -ProtocolExe "$<TARGET_FILE:MCast.Protocol>"
            -UiExe "$<TARGET_FILE:MCast.UiSkeleton>")
    set_tests_properties(
        mcast_protocol_receiving_command PROPERTIES TIMEOUT 20)

    add_test(
        NAME mcast_direct_internal_entry_guards
        COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/entry_guards.ps1"
            -ShellExe "$<TARGET_FILE:MCast.Shell>"
            -ProtocolExe "$<TARGET_FILE:MCast.Protocol>"
            -UiExe "$<TARGET_FILE:MCast.Ui>"
            -QtBin "$<TARGET_FILE_DIR:Qt6::Core>")
    set_tests_properties(
        mcast_direct_internal_entry_guards PROPERTIES TIMEOUT 20)
endif()

# Qt Widgets 只用于极小的宿主窗口，Network 提供 QLocalSocket；
# Quick + QuickControls2 是主窗口。
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Quick QuickControls2)

qt_standard_project_setup()

# 让 CMake 自动把 .qrc 编成 Qt 的资源。
# res/MCast.qrc 里装的是**窗口图标**（QApplication::setWindowIcon 用的那份），
# 和各进程 .rc 里的可执行文件图标各管一边。
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

# QML 引擎按 <导入路径>/FluentUI/qmldir 找模块，所以把整个模块目录
# 放到 Ui 可执行文件旁边的 qml/ 下。
#
# 这一步**踩过坑，别改回 Ui 的 POST_BUILD**：挂到 Ui 上的话，只有自己的 QML
# （走 qrc）改了、Ui 重新链接时才会拷一次；而**只改库里的 QML**（比如
# FluAppBar.qml）时 Ui 没有理由重链，拷贝就不跑 —— 表现是"改了库、重编了、
# 跑起来还是老样子"（亲眼见了一次：按下态的字形怎么改都不变）。
#
# 所以给它一个自己的目标：每次构建都跟着源目录抄一遍（几 MB，代价可以忽略），
# 顺序上靠 add_dependencies 等插件先编好。跨目录不能用 TARGET POST_BUILD
# （CMake 会报 "TARGET was not created in this directory"）。
add_custom_target(mcast_copy_fluentui_qml ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${FLUENTUI_QML_PLUGIN_DIRECTORY}"
            "$<TARGET_FILE_DIR:MCast.Ui>/qml/FluentUI"
    COMMENT "复制 FluentUI QML 模块到构建目录")
add_dependencies(mcast_copy_fluentui_qml fluentuiplugin)

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
if(NOT (EXISTS "${MPV_DEV_DIR}/include/mpv/client.h"))
    message(FATAL_ERROR
        "找不到 libmpv 开发包：${MPV_DEV_DIR}\n"
        "需要 include/mpv/client.h 和 libmpv.lib。\n"
        "如果只是换了 mpv 版本，按上面注释里的三条命令重新生成导入库即可。")
endif()

# ── 真实 Qt/QML Ui 进程 ─────────────────────────────────────────────────
#
# 它由生产 Shell 启动；无 Qt 的 MCast.UiSkeleton 只用于进程监督自动测试。
qt_add_executable(MCast.Ui
    src/ui_process/UiMain.cpp
    src/ui_process/PlaybackBridge.h
    src/ui_process/PlaybackBridge.cpp
    src/ui_process/UiConfirmationBridge.h
    src/ui_process/UiConfirmationBridge.cpp
    src/ui_process/UiSession.h
    src/ui_process/UiSession.cpp
    src/ui_process/UiInteractionState.h
    src/ui_process/UiInteractionState.cpp
    src/ui_process/UiSettingsModel.h
    src/ui_process/UiSettingsModel.cpp
    src/core/LegalDocs.h
    src/core/LegalDocs.cpp
    src/core/MediaPlayer.h
    src/core/ImageSequencePlaylist.h
    src/core/ImageSequencePlaylist.cpp
    src/core/ImageSequenceStrategy.h
    src/core/ImageSequenceStrategy.cpp
    src/core/WrappedTsProxy.h
    src/core/WrappedTsProxy.cpp
    src/core/NowPlaying.h
    src/core/NowPlaying.cpp
    src/core/MpvCore.h
    src/core/MpvCore.cpp
    src/core/MpvQmlItem.h
    src/core/MpvQmlItem.cpp
    src/core/PlaybackController.h
    src/core/PlaybackController.cpp
    src/core/MetadataPipeline.h
    src/core/MetadataPipeline.cpp
    src/core/Tr.h
    src/core/Tr.cpp
    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/UiState.h
    src/ui/UiState.cpp
    src/ui/DeviceInfo.h
    src/ui/DeviceInfo.cpp
    src/platform/windows/WindowFrame.h
    src/platform/windows/WindowFrame.cpp
    src/platform/windows/FullscreenWindowController.h
    src/platform/windows/FullscreenWindowController.cpp
    src/platform/windows/Win32FullscreenWindow.h
    src/platform/windows/Win32FullscreenWindow.cpp
    src/platform/windows/SystemAccentColors.h
    src/platform/windows/SystemAccentColors.cpp
    src/platform/MediaControls.h
    src/platform/windows/WindowsMediaControls.h
    src/platform/windows/WindowsMediaControls.cpp
    src/platform/windows/WindowsMediaControlsService.cpp
    res/MCast.Ui.rc
    res/MCast.qrc
    res/QmlUi.qrc
    res/Lang.qrc
)
target_include_directories(
    MCast.Ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(
    MCast.Ui PRIVATE "${MPV_DEV_DIR}/include")
target_link_libraries(
    MCast.Ui PRIVATE
    Qt6::Widgets Qt6::Quick Qt6::QuickControls2 Qt6::Network
    mcast_ipc windowsapp.lib dwmapi.lib shcore.lib shlwapi.lib
    d2d1.lib d3d11.lib dxgi.lib dcomp.lib windowscodecs.lib advapi32.lib
    "${MPV_DEV_DIR}/libmpv.lib")
target_compile_options(MCast.Ui PRIVATE /utf-8)
set_target_properties(MCast.Ui PROPERTIES WIN32_EXECUTABLE TRUE)
add_dependencies(MCast.Ui fluentuiplugin)
add_dependencies(MCast.Shell MCast.Ui)
add_custom_target(mcast_copy_libmpv ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${MPV_DEV_DIR}/libmpv-2.dll"
            "$<TARGET_FILE_DIR:MCast.Ui>"
    COMMENT "复制 libmpv-2.dll 到 Ui 进程目录")
add_dependencies(MCast.Ui mcast_copy_libmpv)

# ── 语言 ─────────────────────────────────────────────────────────────────
#
# 一种语言是 lang/<code>/ 下的一个完整语言包。Shell 扫描并下发语言目录，
# Ui 只读取 Shell 指定的 ui.json。这里没有 lupdate / lrelease 那一套
# （.ts / .qm / qt_add_translations 全都不用了），只负责把整个目录拷到
# exe 旁边。
#
# 规矩（文件格式、meta 两个字段、键名怎么起、怎么加一种语言）写在
# lang/README.md 里。
#
# 唯一编进 exe 的是基准界面表 en_US/ui.json（见 res/Lang.qrc）：lang 目录被删了、
# 或者发布时漏拷的时候靠它兜底 —— 界面最坏是英文，不会是满屏键名。
# 和上面的 FluentUI QML 一样，不能挂成 Ui 的 POST_BUILD：只改语言 JSON
# 时 Ui 没有理由重新链接，POST_BUILD 不跑，程序就会继续读上一次的文案。
add_custom_target(mcast_copy_lang ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/lang"
            "$<TARGET_FILE_DIR:MCast.Shell>/lang"
    COMMENT "复制语言文件到构建目录")
add_dependencies(MCast.Shell mcast_copy_lang)

# Metadata presentation policy is data, not compiled UI text. Protocol forwards
# raw metadata; Ui applies the same filter to protocol and local-file data
# before showing it locally and publishing it to Windows media controls.
add_custom_target(mcast_copy_metadata_pipeline ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:MCast.Ui>/config"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config/metadata-pipeline.json"
            "$<TARGET_FILE_DIR:MCast.Ui>/config/metadata-pipeline.json"
    COMMENT "复制 Ui 元数据管线规则到构建目录")
add_dependencies(MCast.Ui mcast_copy_metadata_pipeline)

# ── 法律文本与第三方许可 ────────────────────────────────────────────────
#
# 这两样**必须跟着程序走**：GPL 和 LGPL 都要求分发时附带许可文本，而且
# 程序里的"法律"页要当场读它们（见 src/core/LegalDocs.h 里为什么读文件、
# 不编进 qrc）。开发目录里也拷一份 —— 不然本地跑起来法律页是空的，
# 而且启动自检会一直报"许可文件缺失"。
#
# legal/ 只放**面向用户**的那几份；内部维护用的（CHECKLIST、RELEASE-NOTICES、
# SUPPORT、CODE_OF_CONDUCT）留在 docs/legal/ 不下发。
add_custom_target(mcast_copy_legal ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:MCast.Ui>/legal"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/PRIVACY.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/LEGAL-NOTICES.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/TRADEMARKS.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/docs/legal/CODECS.md"
            "$<TARGET_FILE_DIR:MCast.Ui>/legal"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/licenses"
            "$<TARGET_FILE_DIR:MCast.Ui>/licenses"
    COMMENT "复制法律声明与第三方许可到构建目录")
add_dependencies(MCast.Ui mcast_copy_legal)

