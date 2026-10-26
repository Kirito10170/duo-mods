# SPDX-License-Identifier: GPL-3.0-or-later
# Title modules for Eden Duo companions. Same shape as igawa6/eden-duo-companions native/modules:
# one <TITLEID>.so per game, built against the header-only module SDK from an Eden Duo checkout.
#
#   cmake -S native -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DEDEN_SOURCE_ROOT=/path/to/eden-duo \
#         -DZSTD_SOURCE_ROOT=/path/to/zstd
#   Android: add -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
#            -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static

cmake_minimum_required(VERSION 3.22)
project(duo-mods-native LANGUAGES C CXX)

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

# zstd v1.5.7 (decompression only) for TOTK's romfs, and Eden's BCn decoder for BNTX textures.
set(ZSTD_SOURCE_ROOT "" CACHE PATH "facebook/zstd checkout (v1.5.7)")
if(NOT EXISTS "${ZSTD_SOURCE_ROOT}/lib/zstd.h")
    message(FATAL_ERROR "Set -DZSTD_SOURCE_ROOT=/absolute/path/to/zstd")
endif()
if(NOT EXISTS "${EDEN_SOURCE_ROOT}/externals/bc_decoder/bc_decoder.cpp")
    message(FATAL_ERROR "Eden Duo checkout is missing externals/bc_decoder")
endif()
set(zstd_lib "${ZSTD_SOURCE_ROOT}/lib")
add_library(dsmod-zstd STATIC
    ${zstd_lib}/common/debug.c
    ${zstd_lib}/common/entropy_common.c
    ${zstd_lib}/common/error_private.c
    ${zstd_lib}/common/fse_decompress.c
    ${zstd_lib}/common/xxhash.c
    ${zstd_lib}/common/zstd_common.c
    ${zstd_lib}/decompress/huf_decompress.c
    ${zstd_lib}/decompress/zstd_ddict.c
    ${zstd_lib}/decompress/zstd_decompress.c
    ${zstd_lib}/decompress/zstd_decompress_block.c)
target_include_directories(dsmod-zstd PUBLIC "${zstd_lib}")
target_compile_definitions(dsmod-zstd PRIVATE ZSTD_DISABLE_ASM=1 ZSTD_MULTITHREAD=0
    ZSTD_LEGACY_SUPPORT=0 XXH_NAMESPACE=DSMOD_ZSTD_)
set_target_properties(dsmod-zstd PROPERTIES POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)

add_library(dsmod-bc-decoder STATIC "${EDEN_SOURCE_ROOT}/externals/bc_decoder/bc_decoder.cpp")
target_include_directories(dsmod-bc-decoder PUBLIC "${EDEN_SOURCE_ROOT}/externals/bc_decoder")
target_compile_features(dsmod-bc-decoder PRIVATE cxx_std_20)
set_target_properties(dsmod-bc-decoder PROPERTIES POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)

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
    SOURCES modules/0100F2C0115B6000.cpp modules/totk_assets.cpp
    EXPORTS eden_dsmod_get_module eden_dsmod_get_extensions)
target_link_libraries(dsmod-totk PRIVATE dsmod-zstd dsmod-bc-decoder)

# Host-side check of the asset decoder against a romfs dump (not shipped).
if(NOT ANDROID)
    add_executable(totk-assets-tool modules/totk_assets_tool.cpp modules/totk_assets.cpp)
    target_compile_features(totk-assets-tool PRIVATE cxx_std_20)
    target_include_directories(totk-assets-tool PRIVATE "${EDEN_SOURCE_ROOT}/src")
    target_link_libraries(totk-assets-tool PRIVATE dsmod-zstd dsmod-bc-decoder)

    add_executable(totk-gamedata-test modules/totk_gamedata_test.cpp)
    target_compile_features(totk-gamedata-test PRIVATE cxx_std_20)
    target_compile_options(totk-gamedata-test PRIVATE -UNDEBUG)
endif()
