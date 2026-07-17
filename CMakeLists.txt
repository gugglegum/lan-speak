cmake_minimum_required(VERSION 4.2)
project(LanSpeak VERSION 1.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LANSPEAK_VERSION_OVERRIDE "" CACHE STRING "Override the LAN Speak build version")
set(LANSPEAK_VERSION "")
if(NOT LANSPEAK_VERSION_OVERRIDE STREQUAL "")
    set(LANSPEAK_VERSION "${LANSPEAK_VERSION_OVERRIDE}")
elseif(DEFINED ENV{LANSPEAK_VERSION} AND NOT "$ENV{LANSPEAK_VERSION}" STREQUAL "")
    set(LANSPEAK_VERSION "$ENV{LANSPEAK_VERSION}")
else()
    find_package(Git QUIET)
    if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match --match "[0-9]*.[0-9]*.[0-9]*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE LANSPEAK_GIT_TAG_RESULT
            OUTPUT_VARIABLE LANSPEAK_GIT_TAG
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(LANSPEAK_GIT_TAG_RESULT EQUAL 0)
            set(LANSPEAK_VERSION "${LANSPEAK_GIT_TAG}")
        endif()
    endif()
endif()

if(LANSPEAK_VERSION STREQUAL "")
    set(LANSPEAK_VERSION "${PROJECT_VERSION}-alpha-dev")
endif()
if(NOT LANSPEAK_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-[0-9A-Za-z.-]+)?(\\+[0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "Invalid LAN Speak version: ${LANSPEAK_VERSION}")
endif()

set(LANSPEAK_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(LANSPEAK_VERSION_MINOR "${CMAKE_MATCH_2}")
set(LANSPEAK_VERSION_PATCH "${CMAKE_MATCH_3}")
set(LANSPEAK_VERSION_REVISION 0)
if(LANSPEAK_VERSION MATCHES "-(alpha|beta|rc)\\.([0-9]+)")
    set(LANSPEAK_VERSION_REVISION "${CMAKE_MATCH_2}")
endif()
foreach(component
        LANSPEAK_VERSION_MAJOR
        LANSPEAK_VERSION_MINOR
        LANSPEAK_VERSION_PATCH
        LANSPEAK_VERSION_REVISION)
    if(${component} GREATER 65535)
        message(FATAL_ERROR "${component} must fit in a Windows VERSIONINFO component")
    endif()
endforeach()

set(LANSPEAK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LANSPEAK_GENERATED_DIR}")
configure_file(
    cmake/lanspeak_version.h.in
    "${LANSPEAK_GENERATED_DIR}/lanspeak_version.h"
    @ONLY)

set(LANSPEAK_FILE_DESCRIPTION "LAN Speak")
set(LANSPEAK_INTERNAL_NAME "LanSpeak")
set(LANSPEAK_ORIGINAL_FILENAME "LanSpeak.exe")
configure_file(
    cmake/lanspeak_version.rc.in
    "${LANSPEAK_GENERATED_DIR}/LanSpeakVersion.rc"
    @ONLY)
set(LANSPEAK_FILE_DESCRIPTION "LAN Speak Core")
set(LANSPEAK_INTERNAL_NAME "LanSpeakCore")
set(LANSPEAK_ORIGINAL_FILENAME "LanSpeakCore.exe")
configure_file(
    cmake/lanspeak_version.rc.in
    "${LANSPEAK_GENERATED_DIR}/LanSpeakCoreVersion.rc"
    @ONLY)
message(STATUS "LAN Speak build version: ${LANSPEAK_VERSION}")

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
    src/gui/network_adapters.cpp
    src/gui/network_settings_dialog.cpp
    src/gui/osd_overlay.cpp
    src/gui/settings_store.cpp
    src/gui/telemetry_protocol.cpp
    src/gui/tray_icon.cpp
    src/gui/ui_icons.cpp
    src/gui/vu_math.cpp)
target_include_directories(LanSpeakGuiSupport PUBLIC src "${LANSPEAK_GENERATED_DIR}")
target_link_libraries(LanSpeakGuiSupport PUBLIC iphlpapi ws2_32)

add_executable(LanSpeakCore
    main.cpp
    src/core/application.cpp
    "${LANSPEAK_GENERATED_DIR}/LanSpeakCoreVersion.rc")
target_compile_definitions(LanSpeakCore PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(LanSpeakCore PRIVATE LanSpeakCommon LanSpeakCoreSupport ole32 uuid avrt ws2_32)

add_executable(LanSpeak WIN32
    gui.cpp
    src/gui/application.cpp
    LanSpeakGui.rc
    "${LANSPEAK_GENERATED_DIR}/LanSpeakVersion.rc")
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
