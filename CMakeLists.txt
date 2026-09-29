cmake_minimum_required(VERSION 3.22)

project(DeepSeekMidiBridge VERSION 0.3.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (MSVC)
    add_compile_options(/utf-8 /bigobj /MP)
endif()

# ------------------------------------------------------------------------------
# Link the C runtime statically. That way the shipped plugin (and the installer)
# run on a machine that has no "Visual C++ Redistributable" installed - which is
# what makes a one-click install on another computer possible.
# Only POD/COM data crosses the VST3 boundary, so the module-local heap is safe.
# ------------------------------------------------------------------------------
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ------------------------------------------------------------------------------
# JUCE (vendored in third_party/JUCE so the build works offline)
# ------------------------------------------------------------------------------
add_subdirectory(third_party/JUCE)

# ------------------------------------------------------------------------------
# Shared sources
# ------------------------------------------------------------------------------
set(DMB_CORE_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/source/core)
set(DMB_PLUGIN_DIR ${CMAKE_CURRENT_SOURCE_DIR}/source/plugin)

file(GLOB DMB_CORE_SOURCES   CONFIGURE_DEPENDS ${DMB_CORE_DIR}/*.cpp ${DMB_CORE_DIR}/*.h)
file(GLOB DMB_PLUGIN_SOURCES CONFIGURE_DEPENDS ${DMB_PLUGIN_DIR}/*.cpp ${DMB_PLUGIN_DIR}/*.h)

set(DMB_TESTS_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/source/tests/RunTests.cpp)

# Core = JUCE-free-of-GUI logic shared by the plugins, the tests and the CLI tools.
# An INTERFACE library keeps a single source of truth without duplicating build logic.
add_library(dmb_core INTERFACE)
target_sources(dmb_core INTERFACE ${DMB_CORE_SOURCES})
target_include_directories(dmb_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_compile_definitions(dmb_core INTERFACE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STANDALONE_APPLICATION=0)
target_link_libraries(dmb_core INTERFACE
    juce::juce_core
    juce::juce_audio_basics
    juce::juce_data_structures
    juce::juce_events)

# ------------------------------------------------------------------------------
# Plugin target #1 - audio effect with MIDI in/out and audio passthrough.
# This is the general purpose build: it can be dropped anywhere in a track's
# device chain and still receives the track's MIDI.
# ------------------------------------------------------------------------------
juce_add_plugin(DeepSeekMidiBridgeFx
    COMPANY_NAME              "DeepSeek MIDI Bridge"
    COMPANY_WEBSITE           "https://api-docs.deepseek.com/"
    BUNDLE_ID                 "com.deepseekmidibridge.fx"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          TRUE
    NEEDS_MIDI_OUTPUT         TRUE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   FALSE
    PLUGIN_MANUFACTURER_CODE  Dsmb
    PLUGIN_NAME               "DeepSeek MIDI Bridge"
    PLUGIN_CODE               DmB1
    PRODUCT_NAME              "DeepSeek MIDI Bridge"
    VST3_CATEGORIES           Fx
    FORMATS                   VST3 Standalone)

# ------------------------------------------------------------------------------
# Plugin target #2 - MIDI-only variant (no audio buses at all). Some hosts only
# allow a plugin to be placed *before* the instrument (i.e. in the MIDI effect
# slot) when it has no audio buses, which is what is needed for the plugin's
# generated MIDI to reach a downstream instrument.
# ------------------------------------------------------------------------------
juce_add_plugin(DeepSeekMidiBridgeMidi
    COMPANY_NAME              "DeepSeek MIDI Bridge"
    COMPANY_WEBSITE           "https://api-docs.deepseek.com/"
    BUNDLE_ID                 "com.deepseekmidibridge.midi"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          TRUE
    NEEDS_MIDI_OUTPUT         TRUE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   FALSE
    PLUGIN_MANUFACTURER_CODE  Dsmb
    PLUGIN_NAME               "DeepSeek MIDI Bridge (MIDI)"
    PLUGIN_CODE               DmB2
    PRODUCT_NAME              "DeepSeek MIDI Bridge (MIDI)"
    VST3_CATEGORIES           Fx
    FORMATS                   VST3)

foreach(dmb_plugin_target DeepSeekMidiBridgeFx DeepSeekMidiBridgeMidi)
    target_sources(${dmb_plugin_target} PRIVATE ${DMB_PLUGIN_SOURCES})
    target_compile_definitions(${dmb_plugin_target} PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        JUCE_DISPLAY_SPLASH_SCREEN=0
        JUCE_VST3_CAN_REPLACE_VST2=0)
    target_link_libraries(${dmb_plugin_target}
        PRIVATE
            dmb_core
            juce::juce_audio_utils
            juce::juce_gui_extra
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
endforeach()

if (WIN32)
    target_link_libraries(DeepSeekMidiBridgeFx   PRIVATE ole32 shell32)
    target_link_libraries(DeepSeekMidiBridgeMidi PRIVATE ole32 shell32)
endif()

target_compile_definitions(DeepSeekMidiBridgeMidi PRIVATE DMB_MIDI_ONLY=1)
target_compile_definitions(DeepSeekMidiBridgeFx   PRIVATE DMB_MIDI_ONLY=0)

# ------------------------------------------------------------------------------
# Console test / verification app
# ------------------------------------------------------------------------------
juce_add_console_app(DeepSeekMidiBridgeTests
    PRODUCT_NAME "DeepSeekMidiBridgeTests")

target_sources(DeepSeekMidiBridgeTests PRIVATE ${DMB_TESTS_SOURCES})
target_compile_definitions(DeepSeekMidiBridgeTests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STANDALONE_APPLICATION=1
    # lets the suite load the freshly built .vst3 in a real VST3 host
    JUCE_PLUGINHOST_VST3=1)
target_link_libraries(DeepSeekMidiBridgeTests
    PRIVATE
        dmb_core
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_recommended_config_flags)

# ------------------------------------------------------------------------------
# Console tool: same core, no DAW required
# ------------------------------------------------------------------------------
juce_add_console_app(DeepSeekMidiBridgeCli
    PRODUCT_NAME "dmb-cli")

target_sources(DeepSeekMidiBridgeCli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source/cli/main.cpp)
target_compile_definitions(DeepSeekMidiBridgeCli PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STANDALONE_APPLICATION=1)
target_link_libraries(DeepSeekMidiBridgeCli
    PRIVATE
        dmb_core
        juce::juce_audio_utils
        juce::juce_recommended_config_flags)

if (WIN32)
    target_link_libraries(DeepSeekMidiBridgeCli PRIVATE shell32)
endif()

set_target_properties(DeepSeekMidiBridgeCli PROPERTIES OUTPUT_NAME "dmb-cli")

# ------------------------------------------------------------------------------
# Installer / uninstaller: one small GUI executable that installs the plugin and
# (as a copy of itself) removes it again. scripts/build_installer.ps1 appends the
# payload to it and produces dist\DeepSeekMidiBridge-Setup-<version>.exe
# ------------------------------------------------------------------------------
juce_add_gui_app(DmbInstaller
    PRODUCT_NAME              "DeepSeekMidiBridgeSetup"
    COMPANY_NAME              "DeepSeek MIDI Bridge"
    BUNDLE_ID                 "com.deepseekmidibridge.setup"
    VERSION                   ${PROJECT_VERSION})

target_sources(DmbInstaller PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/Main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/InstallerCommon.cpp)

target_compile_definitions(DmbInstaller PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    DMB_VERSION="${PROJECT_VERSION}")

target_link_libraries(DmbInstaller
    PRIVATE
        juce::juce_gui_extra
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

if (WIN32)
    target_link_libraries(DmbInstaller PRIVATE shell32)
endif()

set_target_properties(DmbInstaller PROPERTIES OUTPUT_NAME "DeepSeekMidiBridgeSetup")

# ------------------------------------------------------------------------------
# Convenience: make sure the .vst3 bundles end up in a predictable place.
# ------------------------------------------------------------------------------
set(DMB_ARTEFACT_HINT "${CMAKE_BINARY_DIR}/DeepSeekMidiBridgeFx_artefacts"
    CACHE INTERNAL "where the VST3 bundles are built")
