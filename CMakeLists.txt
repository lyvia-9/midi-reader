cmake_minimum_required(VERSION 3.22)
project(MidiReader VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(MIDI_READER_BUILD_APP "Build the JUCE application" ON)
option(MIDI_READER_BUILD_TESTS "Build the core parity tests" ON)

# ---------------------------------------------------------------------------
# 核心：纯 C++17，无 JUCE 依赖。SMF 读取、乐器库、sidecar、三条规则、投影、tempo map。
# 与 proto/（Python 原型）逐条对齐，由 tests/ParityTests 拿 testdata/cases 检查。
add_library(reader_core STATIC
    src/core/Smf.cpp
    src/core/Instruments.cpp
    src/core/Sidecar.cpp
    src/core/Rules.cpp
    src/core/Analyze.cpp
    src/core/Project.cpp
    src/core/TempoMap.cpp
    src/core/TextCodec.cpp)
target_include_directories(reader_core PUBLIC src third_party)
target_compile_features(reader_core PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(reader_core PRIVATE /utf-8 /W3)
else()
    target_compile_options(reader_core PRIVATE -Wall -Wextra)
endif()

if(MIDI_READER_BUILD_TESTS)
    enable_testing()
    add_executable(ParityTests tests/ParityTests.cpp)
    target_link_libraries(ParityTests PRIVATE reader_core)
    if(MSVC)
        target_compile_options(ParityTests PRIVATE /utf-8)
    endif()
    add_test(NAME ParityTests COMMAND ParityTests "${CMAKE_CURRENT_SOURCE_DIR}/testdata/cases")
endif()

if(NOT MIDI_READER_BUILD_APP)
    return()
endif()

# ---------------------------------------------------------------------------
# JUCE 7 与 pianoroll-core：给了本地目录就用本地的，否则下载固定版本。
include(FetchContent)
set(JUCE_DIR "" CACHE PATH "JUCE 7 source directory (empty = download 7.0.12)")
set(PIANO_ROLL_CORE_DIR "" CACHE PATH "pianoroll-core source directory (empty = download db18fea)")

if(JUCE_DIR)
    add_subdirectory("${JUCE_DIR}" juce EXCLUDE_FROM_ALL)
else()
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 7.0.12
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

set(PIANO_ROLL_CORE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PIANO_ROLL_CORE_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(PIANO_ROLL_CORE_BUILD_JUCE_UI ON CACHE BOOL "" FORCE)
if(PIANO_ROLL_CORE_DIR)
    add_subdirectory("${PIANO_ROLL_CORE_DIR}" piano-roll-core EXCLUDE_FROM_ALL)
else()
    # codex/pianoroll-view-0.5.2，接口设计文档基于的提交
    FetchContent_Declare(pianorollcore
        GIT_REPOSITORY https://github.com/rmkstudio/pianoroll-core.git
        GIT_TAG db18fea6b159b912848dec87cc0d88821ce892d4)
    FetchContent_MakeAvailable(pianorollcore)
endif()

juce_add_gui_app(MidiReader
    PRODUCT_NAME "MIDI Reader"
    COMPANY_NAME "rmkstudio"
    VERSION ${PROJECT_VERSION})

target_sources(MidiReader PRIVATE
    src/app/Main.cpp
    src/app/MainComponent.cpp
    src/app/Document.cpp
    src/app/MultiLaneCanvas.cpp
    src/app/SidePanel.cpp
    src/app/Transport.cpp
    src/app/LaneDetailView.cpp)

target_compile_definitions(MidiReader PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:MidiReader,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:MidiReader,JUCE_VERSION>"
    JUCE_DISPLAY_SPLASH_SCREEN=0)

if(MSVC)
    target_compile_options(MidiReader PRIVATE /utf-8)
endif()

target_link_libraries(MidiReader
    PRIVATE
        reader_core
        PianoRollCore::View
        juce::juce_gui_extra
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
