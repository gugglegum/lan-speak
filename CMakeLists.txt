cmake_minimum_required(VERSION 4.2)
project(LanSpeak)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
include(CheckIPOSupported)

add_library(LanSpeakCommon STATIC
    src/common/line_buffer.cpp
    src/common/text_codec.cpp)
target_include_directories(LanSpeakCommon PUBLIC src)

add_library(LanSpeakCoreSupport STATIC
    src/core/audio_math.cpp
    src/core/diagnostics.cpp
    src/core/jitter_buffer.cpp
    src/core/options.cpp
    src/core/pcm_audio.cpp
    src/core/peer_router.cpp
    src/core/room_mixer.cpp
    src/core/room_session.cpp
    src/core/telemetry_snapshot.cpp
    src/core/udp_audio_packet.cpp
    src/core/wasapi_audio.cpp
    src/core/wasapi_devices.cpp)

target_include_directories(LanSpeakCoreSupport PUBLIC src)

add_library(LanSpeakGuiSupport STATIC
    src/gui/about_dialog.cpp
    src/gui/audio_latency_dialog.cpp
    src/gui/contact_dialog.cpp
    src/gui/contact_list_model.cpp
    src/gui/core_process_controller.cpp
    src/gui/gdi_object_cache.cpp
    src/gui/gdi_surface.cpp
    src/gui/hotkey_utils.cpp
    src/gui/localization.cpp
    src/gui/osd_overlay.cpp
    src/gui/settings_store.cpp
    src/gui/telemetry_protocol.cpp
    src/gui/tray_icon.cpp
    src/gui/ui_icons.cpp
    src/gui/vu_math.cpp)
target_include_directories(LanSpeakGuiSupport PUBLIC src)

add_executable(LanSpeakCore
    main.cpp
    src/core/application.cpp)
target_compile_definitions(LanSpeakCore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(LanSpeakCore PRIVATE LanSpeakCommon LanSpeakCoreSupport ole32 uuid avrt ws2_32)

add_executable(LanSpeak WIN32
    gui.cpp
    src/gui/application.cpp
    LanSpeakGui.rc)
target_include_directories(LanSpeak PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(LanSpeak PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(LanSpeak PRIVATE LanSpeakCommon LanSpeakGuiSupport user32 gdi32 shell32 comctl32)

if(BUILD_TESTING)
    add_executable(LanSpeakTests tests/support_tests.cpp)
    target_link_libraries(LanSpeakTests PRIVATE
        LanSpeakCommon
        LanSpeakCoreSupport
        LanSpeakGuiSupport
        user32
        gdi32)
    add_test(NAME LanSpeakSupportTests COMMAND LanSpeakTests)
endif()

if(MSVC)
    foreach(target
            LanSpeakCommon
            LanSpeakCoreSupport
            LanSpeakGuiSupport
            LanSpeakCore
            LanSpeak)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /permissive-)
    endforeach()
    if(BUILD_TESTING)
        target_compile_definitions(LanSpeakTests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_compile_options(LanSpeakTests PRIVATE /utf-8 /W4 /permissive-)
    endif()
endif()

check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
if(ipo_supported)
    foreach(target
            LanSpeakCommon
            LanSpeakCoreSupport
            LanSpeakGuiSupport
            LanSpeakCore
            LanSpeak)
        set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    endforeach()
endif()
