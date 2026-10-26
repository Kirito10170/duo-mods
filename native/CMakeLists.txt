# SPDX-License-Identifier: GPL-3.0-or-later
# Title modules for Eden Duo companions. Same shape as igawa6/eden-duo-companions native/modules:
# one <TITLEID>.so per game, built against the header-only module SDK from an Eden Duo checkout.
#
#   cmake -S native -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDEN_SOURCE_ROOT=/path/to/eden-duo
#   Android: add -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
#            -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static

cmake_minimum_required(VERSION 3.22)
project(duo-mods-native LANGUAGES CXX)

if(ANDROID AND NOT ANDROID_STL STREQUAL "c++_static")
    message(FATAL_ERROR "Native DSMod modules must use -DANDROID_STL=c++_static")
endif()

set(EDEN_SOURCE_ROOT "" CACHE PATH "Eden Duo checkout providing src/core/mods (module SDK)")
if(NOT IS_ABSOLUTE "${EDEN_SOURCE_ROOT}")
    message(FATAL_ERROR "Set -DEDEN_SOURCE_ROOT=/absolute/path/to/eden-duo")
endif()
foreach(header IN ITEMS dsmod_module_abi.h dsmod_module_extensions.h modules/dsmod_module_sdk.h)
    if(NOT EXISTS "${EDEN_SOURCE_ROOT}/src/core/mods/${header}")
        message(FATAL_ERROR "Eden Duo SDK header missing: ${header}")
    endif()
endforeach()

# dsmod_add_module(<target> TITLE <title id> SOURCES <file>... EXPORTS <symbol>...)
function(dsmod_add_module target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "TITLE" "SOURCES;EXPORTS")
    add_library(${target} SHARED ${ARG_SOURCES})
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/modules"
        "${EDEN_SOURCE_ROOT}/src")
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${ARG_TITLE}"
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    set(script "{\n    global:\n")
    foreach(symbol IN LISTS ARG_EXPORTS)
        string(APPEND script "        ${symbol};\n")
    endforeach()
    string(APPEND script "    local:\n        *;\n};\n")
    set(script_file "${CMAKE_CURRENT_BINARY_DIR}/${target}.exports")
    file(GENERATE OUTPUT "${script_file}" CONTENT "${script}")
    target_link_options(${target} PRIVATE "-Wl,--no-undefined" "-Wl,--version-script=${script_file}")
    if(ANDROID)
        target_link_options(${target} PRIVATE "-Wl,--exclude-libs,ALL")
    endif()
endfunction()

# The Legend of Zelda: Tears of the Kingdom, 1.4.2.
dsmod_add_module(dsmod-totk
    TITLE 0100F2C0115B6000
    SOURCES modules/0100F2C0115B6000.cpp
    EXPORTS eden_dsmod_get_module eden_dsmod_get_extensions)
