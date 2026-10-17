# SPDX-License-Identifier: GPL-2.0-only
# Radio-side programs. They link devourer, which is GPL-2.0, so these are too.
cmake_minimum_required(VERSION 3.15)
option(WINAWDL_MT7925_ONLY "Build native MT7925 backend without devourer" OFF)

# devourer only picks the vcpkg toolchain when it is the top-level project.
if(WIN32 AND NOT WINAWDL_MT7925_ONLY AND NOT "$ENV{VCPKG_ROOT}" STREQUAL "")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
endif()

project(winawdl_radio CXX C)

if(WINAWDL_MT7925_ONLY)
    if(NOT WIN32)
        message(FATAL_ERROR "MT7925 backend requires Windows")
    endif()
    enable_testing()
    add_executable(winawdl_mt7925 winawdl_mt7925.cpp)
    add_executable(mt7925_bridge_test tests/mt7925_bridge_test.cpp)
    add_executable(pipe_server_test tests/pipe_server_test.cpp)
    add_executable(mt7925_txs_test tests/mt7925_txs_test.cpp)
    foreach(target winawdl_mt7925 mt7925_bridge_test pipe_server_test mt7925_txs_test)
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_libraries(${target} PRIVATE setupapi bcrypt)
        if(MSVC)
            target_compile_options(${target} PRIVATE /W4 /WX)
        endif()
    endforeach()
    add_test(NAME mt7925_bridge COMMAND mt7925_bridge_test)
    add_test(NAME pipe_server COMMAND pipe_server_test)
    add_test(NAME mt7925_txs COMMAND mt7925_txs_test)
    set_tests_properties(mt7925_bridge pipe_server mt7925_txs PROPERTIES TIMEOUT 30)
    return()
endif()

set(WINAWDL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# Only the RTL8852C, over the Windows register proxy.
foreach(chip JAGUAR1 8814 JAGUAR2_8822B JAGUAR2_8821C JAGUAR3_8822C
             JAGUAR3_8822E 8733B KESTREL_8852B)
    set(DEVOURER_${chip} OFF CACHE BOOL "" FORCE)
endforeach()
set(DEVOURER_KESTREL_8852C ON CACHE BOOL "" FORCE)
set(DEVOURER_PCIE ON CACHE BOOL "" FORCE)
set(DEVOURER_REGPROXY_INCLUDE_DIR "${WINAWDL_ROOT}/driver/regproxy"
    CACHE PATH "" FORCE)

# EXCLUDE_FROM_ALL keeps devourer's own demos and tests out of the build. The
# three used here hang off one target the Visual Studio generator can build.
add_subdirectory("${WINAWDL_ROOT}/upstream/devourer" devourer EXCLUDE_FROM_ALL)
add_custom_target(devourer_demos)
add_dependencies(devourer_demos rxdemo txdemo pcieprobe)

function(winawdl_radio_program name)
    add_executable(${name} ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_link_libraries(${name} PRIVATE devourer)
    if(MSVC)
        target_compile_options(${name} PRIVATE /W4 /WX)
        # devourer's DeviceConfig.h calls sscanf, which MSVC flags as deprecated.
        target_compile_definitions(${name} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endfunction()

winawdl_radio_program(awdl_sniff awdl_sniff.cpp)
winawdl_radio_program(winawdl_radio winawdl_radio.cpp radio_link.h)

# Needs no radio, so it runs anywhere.
add_executable(pipe_server_test tests/pipe_server_test.cpp pipe_server.h)
target_compile_features(pipe_server_test PRIVATE cxx_std_20)
target_include_directories(pipe_server_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(pipe_server_test PRIVATE /W4 /WX)
endif()

add_executable(block_ack_test tests/block_ack_test.cpp block_ack.h)
target_compile_features(block_ack_test PRIVATE cxx_std_20)
target_include_directories(block_ack_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(block_ack_test PRIVATE /W4 /WX)
endif()

add_executable(channel_width_test tests/channel_width_test.cpp)
target_compile_features(channel_width_test PRIVATE cxx_std_20)
target_include_directories(channel_width_test PRIVATE
    "${WINAWDL_ROOT}/upstream/devourer/src")
if(MSVC)
    target_compile_options(channel_width_test PRIVATE /W4 /WX)
endif()

add_executable(outgoing_session_test tests/outgoing_session_test.cpp
    outgoing_session.h block_ack.h)
target_compile_features(outgoing_session_test PRIVATE cxx_std_20)
target_include_directories(outgoing_session_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(outgoing_session_test PRIVATE /W4 /WX)
endif()

add_executable(rate_ladder_test tests/rate_ladder_test.cpp rate_ladder.h)
target_compile_features(rate_ladder_test PRIVATE cxx_std_20)
target_include_directories(rate_ladder_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(rate_ladder_test PRIVATE /W4 /WX)
endif()

add_executable(reorder_buffer_test tests/reorder_buffer_test.cpp reorder_buffer.h)
target_compile_features(reorder_buffer_test PRIVATE cxx_std_20)
target_include_directories(reorder_buffer_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(reorder_buffer_test PRIVATE /W4 /WX)
endif()

add_executable(frame_capture_test tests/frame_capture_test.cpp frame_capture.h)
target_compile_features(frame_capture_test PRIVATE cxx_std_20)
target_include_directories(frame_capture_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if(MSVC)
    target_compile_options(frame_capture_test PRIVATE /W4 /WX)
    target_compile_definitions(frame_capture_test PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# Checks devourer's address handling for the Windows backend, also without a
# radio: only its two header-only pieces are compiled.
add_executable(dma_address_test tests/dma_address_test.cpp)
target_compile_features(dma_address_test PRIVATE cxx_std_20)
target_include_directories(dma_address_test PRIVATE
    "${WINAWDL_ROOT}/upstream/devourer/src")
if(MSVC)
    target_compile_options(dma_address_test PRIVATE /W4 /WX)
endif()
