cmake_minimum_required(VERSION 3.16)
project(flashdb_edge_tsdb LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------- 1. Unmodified Upstream FlashDB Static Library + Virtual NOR Bridge ----------
add_library(flashdb_upstream STATIC
    upstream/FlashDB/src/fdb.c
    upstream/FlashDB/src/fdb_tsdb.c
    upstream/FlashDB/src/fdb_utils.c
    upstream/fal_shim/fal_sim_bridge.c
)
target_include_directories(flashdb_upstream PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/fal_shim
    ${CMAKE_CURRENT_SOURCE_DIR}/upstream/FlashDB/inc
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
if(MSVC)
    target_compile_definitions(flashdb_upstream PUBLIC _CRT_SECURE_NO_WARNINGS)
    target_compile_options(flashdb_upstream PRIVATE /W2)
else()
    target_compile_options(flashdb_upstream PRIVATE -Wall)
endif()

# ---------- 2. Enhanced Pure-C11 Edge TSDB Library (Zero-Malloc Core) ----------
set(FDB_EDGE_SOURCES
    src/fdb_edge_crc32c.c
    src/fdb_edge_hlc.c
    src/fdb_edge_codec.c
    src/fdb_edge_tsdb.c
    src/fdb_edge_wal_sync.c
    src/fdb_edge_privacy.c
    src/fdb_edge_flash_sim.c
    ports/stm32/stm32_fal_flash_port.c
)

function(fdb_edge_configure_library target)
    target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
    if(MSVC)
        target_compile_definitions(${target} PUBLIC _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wshadow -Werror)
    endif()
endfunction()

add_library(fdb_edge STATIC ${FDB_EDGE_SOURCES})
fdb_edge_configure_library(fdb_edge)

# ---------- 3. Unified CLI & Raspberry Pi File-Backed Edge Collector ----------
add_executable(fdb_edge_cli
    cli/fdb_edge_cli.c
)
target_link_libraries(fdb_edge_cli PRIVATE fdb_edge flashdb_upstream)
if(NOT WIN32)
    target_link_libraries(fdb_edge_cli PRIVATE m)
endif()
if(MSVC)
    target_compile_options(fdb_edge_cli PRIVATE /W4)
else()
    target_compile_options(fdb_edge_cli PRIVATE -Wall -Wextra)
endif()

add_executable(rpi_edge_collector
    ports/raspberry_pi/rpi_edge_collector.c
)
target_link_libraries(rpi_edge_collector PRIVATE fdb_edge)
if(MSVC)
    target_compile_options(rpi_edge_collector PRIVATE /W4)
else()
    target_compile_options(rpi_edge_collector PRIVATE -Wall -Wextra)
endif()

# ---------- 4. CTest Suite ----------
enable_testing()

foreach(test_name IN ITEMS
    test_upstream_repro
    test_hlc_and_codec
    test_edge_tsdb_and_faults
    test_wal_sync_and_privacy
    test_flash_sim_bounds
    test_privacy_regressions
    test_wal_regressions
)
    add_executable(${test_name} tests/${test_name}.c)
    target_link_libraries(${test_name} PRIVATE fdb_edge flashdb_upstream)
    if(NOT WIN32)
        target_link_libraries(${test_name} PRIVATE m)
    endif()
    if(MSVC)
        target_compile_options(${test_name} PRIVATE /W4)
    else()
        target_compile_options(${test_name} PRIVATE -Wall -Wextra)
    endif()
    if(NOT test_name STREQUAL "test_privacy_regressions" AND NOT test_name STREQUAL "test_wal_regressions")
        add_test(NAME ${test_name} COMMAND ${test_name})
    endif()
endforeach()

foreach(case IN ITEMS pem_boundaries adjacent_credentials max_size)
    add_test(NAME privacy_${case} COMMAND test_privacy_regressions ${case})
endforeach()

foreach(case IN ITEMS test_codec_timestamp_and_ieee_bits test_codec_malformed_input test_codec_wire_compatibility)
    add_test(NAME ${case} COMMAND test_hlc_and_codec ${case})
endforeach()

foreach(case IN ITEMS split_flush_retry split_flush_full nor_rollover_recovery raw_hlc_recovery flash_geometry_bounds)
    add_test(NAME ${case} COMMAND test_edge_tsdb_and_faults ${case})
endforeach()

foreach(case IN ITEMS cobs resync sequences export)
    add_test(NAME wal_${case} COMMAND test_wal_regressions ${case})
endforeach()

add_executable(test_rpi_collector tests/test_rpi_collector.c)
target_link_libraries(test_rpi_collector PRIVATE fdb_edge)
add_test(NAME rpi_collector_file_roundtrip
    COMMAND ${CMAKE_COMMAND}
        "-DCOLLECTOR=$<TARGET_FILE:rpi_edge_collector>"
        "-DFIXTURE=$<TARGET_FILE:test_rpi_collector>"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/rpi_collector_test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_rpi_collector.cmake")

add_test(NAME test_cli_audit
    COMMAND ${CMAKE_COMMAND}
        "-DCLI=$<TARGET_FILE:fdb_edge_cli>"
        "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/cli-audit"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli_audit.cmake")

# Rebuild the library and its consumer with the same public struct layout.
add_library(fdb_edge_stm32_test STATIC ${FDB_EDGE_SOURCES})
fdb_edge_configure_library(fdb_edge_stm32_test)
target_compile_definitions(fdb_edge_stm32_test PRIVATE FDB_EDGE_STM32_HAL)
target_include_directories(fdb_edge_stm32_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/upstream/fal_shim)
set(STM32_PROFILE_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/ports/stm32/fdb_cfg.h")
if(MSVC)
    target_compile_options(fdb_edge_stm32_test PUBLIC "/FI${STM32_PROFILE_HEADER}")
else()
    target_compile_options(fdb_edge_stm32_test PUBLIC -include "${STM32_PROFILE_HEADER}")
endif()
add_executable(test_stm32_profile tests/test_stm32_profile.c)
target_link_libraries(test_stm32_profile PRIVATE fdb_edge_stm32_test)
target_include_directories(test_stm32_profile PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/upstream/fal_shim)
add_test(NAME stm32_profile_and_fal_io COMMAND test_stm32_profile)
