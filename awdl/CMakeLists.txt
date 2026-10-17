# SPDX-License-Identifier: GPL-3.0-or-later
# AWDL daemon: OWL's library and event core on a Windows I/O and timing layer.
# OWL is GNU C, hence a MinGW build apart from the MSVC radio side.
cmake_minimum_required(VERSION 3.15)
project(winawdl_awdl C)

# The file names in log lines and in the programs themselves start at the
# repository, not at the folder of the disk it was cloned into.
get_filename_component(REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
add_compile_options("-ffile-prefix-map=${REPOSITORY}=.")

set(WINAWDL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(OWL "${WINAWDL_ROOT}/upstream/owl")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)

# compat/ comes first so OWL's Unix includes resolve to the shims.
# -mno-ms-bitfields lays packed structs out as GCC does on Linux.
add_library(owl_build INTERFACE)
target_include_directories(owl_build INTERFACE
    compat "${OWL}/src" "${OWL}/radiotap" "${OWL}/daemon" win
    "${WINAWDL_ROOT}/radio")
target_compile_options(owl_build INTERFACE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/compat/owl_win.h" -mno-ms-bitfields)

add_library(radiotap STATIC "${OWL}/radiotap/radiotap.c")
target_link_libraries(radiotap PRIVATE owl_build)
# Its bounds checks subtract pointers through unsigned long, which is 32 bits
# on Windows. The differences are small, so the truncation cancels out.
target_compile_options(radiotap PRIVATE -Wno-pointer-to-int-cast)

set(OWL_SOURCES log state election sync channel schedule tx rx frame crc32 wire
    peers version hashmap siphash24 circular_buffer)
list(TRANSFORM OWL_SOURCES PREPEND "${OWL}/src/")
list(TRANSFORM OWL_SOURCES APPEND ".c")
add_library(awdl STATIC ${OWL_SOURCES})
target_link_libraries(awdl PUBLIC owl_build radiotap)

# The host adapter is kept clear of owl_build: -mno-ms-bitfields must not
# reach the Windows networking structures it fills in.
add_library(host_if STATIC win/host_if.c win/host_frame.c)
target_include_directories(host_if PUBLIC win)
target_compile_options(host_if PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_if PRIVATE iphlpapi)

set(WIN_SOURCES
    win/main.c win/ev_win.c win/io_win.c win/netutils_win.c win/radio_client.c
    win/redact.c win/frame_fixup.c win/tcp_merge.c win/service_bridge.c win/slot_policy.c
    compat/ether.c)
add_executable(awdld "${OWL}/daemon/core.c" ${WIN_SOURCES})
set_source_files_properties("${OWL}/daemon/core.c" PROPERTIES
    COMPILE_DEFINITIONS "awdl_is_multicast_eaw=winawdl_is_multicast_eaw;awdl_init_full_action_frame=winawdl_init_full_action_frame")
set_source_files_properties(${WIN_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
target_link_libraries(awdld PRIVATE awdl host_if bcrypt)
# Static, so the daemon does not need the MinGW runtime DLLs beside it.
# The wrap routes OWL's log calls through main.c, which can redact them.
target_link_options(awdld PRIVATE -static -Wl,--wrap=log_log)

add_executable(host_frame_test tests/host_frame_test.c)
target_compile_options(host_frame_test PRIVATE -Wall -Wextra -Werror)
target_link_libraries(host_frame_test PRIVATE host_if)
target_link_options(host_frame_test PRIVATE -static)

add_executable(tcp_merge_test tests/tcp_merge_test.c win/tcp_merge.c)
target_include_directories(tcp_merge_test PRIVATE win)
target_compile_options(tcp_merge_test PRIVATE -Wall -Wextra -Werror)
target_link_options(tcp_merge_test PRIVATE -static)

add_executable(frame_fixup_test tests/frame_fixup_test.c win/frame_fixup.c)
target_include_directories(frame_fixup_test PRIVATE win)
target_compile_options(frame_fixup_test PRIVATE -Wall -Wextra -Werror)
target_link_options(frame_fixup_test PRIVATE -static)

add_executable(service_bridge_test tests/service_bridge_test.c win/service_bridge.c
    win/frame_fixup.c)
target_include_directories(service_bridge_test PRIVATE win)
target_compile_options(service_bridge_test PRIVATE -Wall -Wextra -Werror)
target_link_options(service_bridge_test PRIVATE -static)

add_executable(redact_test tests/redact_test.c win/redact.c)
target_include_directories(redact_test PRIVATE win)
target_compile_options(redact_test PRIVATE -Wall -Wextra -Werror)
target_link_options(redact_test PRIVATE -static)

# Includes the real event core with deterministic clock/I/O boundaries.
add_executable(multicast_timing_test tests/multicast_timing_test.c
    win/slot_policy.c compat/ether.c)
target_link_libraries(multicast_timing_test PRIVATE awdl)
target_link_options(multicast_timing_test PRIVATE -static -Wl,--wrap=log_log)

# Actual serializer against frozen baseline bytes: only the Dualband bit changes.
add_executable(dualband_frame_test tests/dualband_frame_test.c
    win/slot_policy.c win/frame_fixup.c compat/ether.c)
target_link_libraries(dualband_frame_test PRIVATE awdl)
target_link_options(dualband_frame_test PRIVATE -static -Wl,--wrap=log_log)
