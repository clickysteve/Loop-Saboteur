cmake_minimum_required(VERSION 3.22)
project(LoopSaboteur VERSION 0.42.5 LANGUAGES C CXX)
# NB: C is required alongside CXX because juce_graphics pulls in SheenBidi,
# which is a C library. Without `C` here, CMake never generates a C compile
# rule and the linker chokes on the missing juce_graphics_Sheenbidi.c.o.

# ---------- macOS universal binary (Intel + Apple Silicon) ----------
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "" FORCE)
endif()

# ---------- JUCE ----------
# JUCE is expected as a sibling-level clone (or symlink) at ./JUCE.
# This repo deliberately doesn't vendor JUCE — see .gitignore.
add_subdirectory(JUCE)

# ---------- Plugin target ----------
juce_add_plugin(LoopSaboteur
    COMPANY_NAME             "AMFAS"
    IS_SYNTH                 FALSE
    IS_MIDI_EFFECT           FALSE
    NEEDS_MIDI_INPUT         FALSE
    NEEDS_MIDI_OUTPUT        FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    PLUGIN_MANUFACTURER_CODE Amfs
    PLUGIN_CODE              Lpsb
    FORMATS                  AU VST3
    PRODUCT_NAME             "Loop Saboteur"
    BUNDLE_ID                "com.amfas.loopsaboteur"

    # AU-specific: audio effect (aufx), not a MIDI processor.
    AU_MAIN_TYPE             "kAudioUnitType_Effect"
)

# ---------- Sources ----------
target_sources(LoopSaboteur
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

# ---------- Include path for our headers ----------
target_include_directories(LoopSaboteur
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

# ---------- C++17 ----------
target_compile_features(LoopSaboteur PRIVATE cxx_std_17)

# ---------- Compile definitions ----------
target_compile_definitions(LoopSaboteur
    PUBLIC
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
)

# ---------- JUCE modules ----------
juce_generate_juce_header(LoopSaboteur)

target_link_libraries(LoopSaboteur
    PRIVATE
        juce::juce_audio_utils
        juce::juce_gui_extra
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---------- macOS Developer ID code signing ----------
# Leave LOOPSAB_CODESIGN_IDENTITY empty for unsigned local dev builds. Set it at
# configure time to enable signing, e.g.:
#   cmake -B build \
#         -DLOOPSAB_CODESIGN_IDENTITY="Developer ID Application: Your Name (TEAMID)" \
#         -DCMAKE_BUILD_TYPE=Release
# Signing runs as a post-build step on each format's bundle (.component, .vst3, .app).
# Hardened runtime + secure timestamp are enabled — required for notarization.
set(LOOPSAB_CODESIGN_IDENTITY "" CACHE STRING
    "Developer ID Application identity for code signing (empty = unsigned)")

if(APPLE AND LOOPSAB_CODESIGN_IDENTITY)
    set(LOOPSAB_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/Resources/LoopSaboteur.entitlements")
    if(NOT EXISTS "${LOOPSAB_ENTITLEMENTS}")
        message(FATAL_ERROR
            "Codesign requested but entitlements file not found: ${LOOPSAB_ENTITLEMENTS}")
    endif()

    foreach(fmt IN ITEMS AU VST3)
        set(signed_target "LoopSaboteur_${fmt}")
        if(TARGET ${signed_target})
            add_custom_command(TARGET ${signed_target} POST_BUILD
                COMMAND /usr/bin/codesign
                        --force
                        --options runtime
                        --timestamp
                        --sign "${LOOPSAB_CODESIGN_IDENTITY}"
                        --entitlements "${LOOPSAB_ENTITLEMENTS}"
                        "$<TARGET_BUNDLE_DIR:${signed_target}>"
                COMMENT "Code signing ${signed_target} with Developer ID"
                VERBATIM)
        endif()
    endforeach()

    message(STATUS "LoopSaboteur: Developer ID signing ENABLED (${LOOPSAB_CODESIGN_IDENTITY})")
else()
    message(STATUS "LoopSaboteur: Developer ID signing DISABLED (unsigned local build)")
endif()
