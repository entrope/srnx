cmake_minimum_required(VERSION 3.22 FATAL_ERROR)
cmake_policy(VERSION 3.22)
project(rinex VERSION 0.1 DESCRIPTION "RINEX parsing library in C")
include(CTest)
include_directories(${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/libtap)

find_program(LCOV lcov)
find_program(GENHTML genhtml)
set(CMAKE_C_FLAGS_COVERAGE "${CMAKE_C_FLAGS_RELWITHDEBINFO} -coverage -Og")
add_custom_target(coverage
    COMMAND ${LCOV} --directory . --capture --output-file coverage.info
    COMMAND ${GENHTML} --demangle-cpp -o coverage coverage.info
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

message(CHECK_START "Checking for NEON support")
try_compile(HAVE_NEON ${CMAKE_BINARY_DIR}/try_compile/neon
    SOURCES ${CMAKE_SOURCE_DIR}/rinex/parse_neon.c
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CMAKE_SOURCE_DIR}"
    COMPILE_DEFINITIONS -DHAVE_NEON)
if(HAVE_NEON)
    message(CHECK_PASS "yes")
else()
    message(CHECK_FAIL "no")
endif()
add_compile_definitions($<$<BOOL:${HAVE_NEON}>:HAVE_NEON>)

message(CHECK_START "Checking for AVX2 support")
try_compile(HAVE_AVX2 ${CMAKE_BINARY_DIR}/try_compile/avx2
    SOURCES ${CMAKE_SOURCE_DIR}/rinex/parse_avx2.c
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CMAKE_SOURCE_DIR}"
    COMPILE_DEFINITIONS -DHAVE_AVX2)
if(HAVE_AVX2)
    message(CHECK_PASS "yes")
else()
    message(CHECK_FAIL "no")
endif()
add_compile_definitions($<$<BOOL:${HAVE_AVX2}>:HAVE_AVX2>)

find_package(PkgConfig REQUIRED)
pkg_check_modules(BLAKE3 REQUIRED libblake3)

add_library(rinex SHARED
    rinex/crx_parse.c
    rinex/digest.c
    rinex/parse_std.c
    $<$<BOOL:${HAVE_NEON}>:rinex/parse_neon.c>
    $<$<BOOL:${HAVE_AVX2}>:rinex/parse_avx2.c>
    rinex/rinex_buffer.c
    rinex/rinex_crlf.c
    rinex/rinex_load.c
    rinex/rinex_mmap.c
    rinex/rinex_record.c
    rinex/rinex_stdio.c
    rinex/rnx_parse.c
    rinex/rnx_priv.c
    rinex/srnx.c
    rinex/srnx_parse.c
    rinex/transpose.c
    python/rinex_bridge.c)
target_include_directories(rinex PRIVATE ${BLAKE3_INCLUDE_DIRS})
target_link_directories(rinex PRIVATE ${BLAKE3_LIBRARY_DIRS})
target_link_libraries(rinex PRIVATE ${BLAKE3_LIBRARIES})
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(rinex PRIVATE -msse4.2)
endif()
target_compile_definitions(rinex PRIVATE HAVE_BLAKE3)
add_library(tap STATIC
    libtap/tap.c)

add_executable(rinex_analyze bin/rinex_analyze.c bin/driver.c)
target_link_libraries(rinex_analyze rinex)

add_executable(rinex_maxima bin/rinex_maxima.c bin/driver.c)
target_link_libraries(rinex_maxima rinex)

add_executable(rinex-merge bin/rinex_merge.c)
target_link_libraries(rinex-merge rinex)

add_executable(rinex_n_obs bin/rinex_n_obs.c bin/driver.c)
target_link_libraries(rinex_n_obs rinex)

add_executable(rinex_scan bin/rinex_scan.c bin/driver.c)
target_link_libraries(rinex_scan rinex)

add_executable(rnxcmp bin/rnxcmp.c)
target_link_libraries(rnxcmp rinex)

add_executable(rnx2srnx bin/rnx2srnx.c)
target_link_libraries(rnx2srnx rinex)

add_executable(rnx2srnx_search bin/rnx2srnx.c)
target_compile_definitions(rnx2srnx_search PRIVATE RNX2SRNX_SEARCH)
target_link_libraries(rnx2srnx_search rinex)

add_executable(srnx_scan bin/srnx_scan.c)
target_link_libraries(srnx_scan rinex)

add_executable(srnx_split bin/srnx_split.c)
target_link_libraries(srnx_split rinex)

add_executable(srnx2rnx bin/srnx2rnx.c)
target_link_libraries(srnx2rnx rinex)

include(InstallRequiredSystemLibraries)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")
set(CPACK_PACKAGE_VERSION_MAJOR "${rinex_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${rinex_VERSION_MINOR}")
set(CPACK_SOURCE_GENERATOR "TGZ")
include(CPack)

enable_testing()

# Integration tests use the same tracked sample as the parser unit tests.
add_executable(write_test_fixtures test/write_fixtures.c)
set(TEST_V3_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/fixture_v3.rnx")
set(TEST_BAD_FIXTURE "${CMAKE_CURRENT_BINARY_DIR}/fixture_invalid.rnx")
add_custom_command(OUTPUT "${TEST_V3_FIXTURE}" "${TEST_BAD_FIXTURE}"
    COMMAND write_test_fixtures "${TEST_V3_FIXTURE}" "${TEST_BAD_FIXTURE}"
    DEPENDS write_test_fixtures
    VERBATIM)
add_custom_target(test_fixtures DEPENDS "${TEST_V3_FIXTURE}" "${TEST_BAD_FIXTURE}")

add_executable(test_digest test/test_digest.c)
target_link_libraries(test_digest rinex tap)
target_include_directories(test_digest PRIVATE ${BLAKE3_INCLUDE_DIRS})
add_test(NAME digest COMMAND test_digest)
set_property(TEST digest PROPERTY LABELS function)

add_executable(bench_digest test/bench_digest.c)
target_link_libraries(bench_digest rinex)
target_include_directories(bench_digest PRIVATE ${BLAKE3_INCLUDE_DIRS})
add_test(NAME digest-bench COMMAND bench_digest)
set_property(TEST digest-bench PROPERTY LABELS benchmark)

add_executable(test_srnx_digest test/test_srnx_digest.c)
target_link_libraries(test_srnx_digest rinex tap)
target_compile_definitions(test_srnx_digest PRIVATE
    RNX2SRNX_PATH="$<TARGET_FILE:rnx2srnx>"
    FIXTURE_PATH="${TEST_V3_FIXTURE}")
add_dependencies(test_srnx_digest rnx2srnx test_fixtures)
add_test(NAME srnx_digest COMMAND test_srnx_digest)
set_property(TEST srnx_digest PROPERTY LABELS function)

add_executable(test_srnx_parser test/srnx_parser.c)
target_link_libraries(test_srnx_parser rinex tap)
target_compile_definitions(test_srnx_parser PRIVATE
    RNX2SRNX_PATH="$<TARGET_FILE:rnx2srnx>"
    FIXTURE_PATH="${TEST_V3_FIXTURE}")
add_dependencies(test_srnx_parser rnx2srnx test_fixtures)
add_test(NAME srnx_parser COMMAND test_srnx_parser)
set_property(TEST srnx_parser PROPERTY LABELS function)
add_test(NAME srnx_parser_socd COMMAND test_srnx_parser --socd)
set_property(TEST srnx_parser_socd PROPERTY LABELS function)

add_executable(test_bin_tools test/test_bin_tools.c)
target_link_libraries(test_bin_tools rinex tap)
target_compile_definitions(test_bin_tools PRIVATE
    RNX2SRNX_PATH="$<TARGET_FILE:rnx2srnx>"
    RINEX_N_OBS_PATH="$<TARGET_FILE:rinex_n_obs>"
    SRNX2RNX_PATH="$<TARGET_FILE:srnx2rnx>"
    RNXCMP_PATH="$<TARGET_FILE:rnxcmp>"
    FIXTURE_V3_PATH="${TEST_V3_FIXTURE}"
    FIXTURE_BAD_PATH="${TEST_BAD_FIXTURE}")
add_dependencies(test_bin_tools rnx2srnx srnx2rnx rinex_n_obs rnxcmp test_fixtures)
add_test(NAME bin_tools COMMAND test_bin_tools)
set_property(TEST bin_tools PROPERTY LABELS function)

add_executable(test_rinex_merge test/test_rinex_merge.c)
target_link_libraries(test_rinex_merge tap)
target_compile_definitions(test_rinex_merge PRIVATE
    RINEX_MERGE_PATH="$<TARGET_FILE:rinex-merge>"
    FIXTURE_V3_PATH="${TEST_V3_FIXTURE}")
add_dependencies(test_rinex_merge rinex-merge test_fixtures)
add_test(NAME rinex_merge COMMAND test_rinex_merge)
set_property(TEST rinex_merge PROPERTY LABELS function)

add_executable(test_crlf test/test_crlf.c)
target_link_libraries(test_crlf rinex tap)
add_test(NAME crlf COMMAND test_crlf)
set_property(TEST crlf PROPERTY LABELS function)

add_executable(test_record test/test_record.c)
target_link_libraries(test_record rinex tap)
add_test(NAME record COMMAND test_record)
set_property(TEST record PROPERTY LABELS function)

add_executable(test_empty_records test/test_empty_records.c)
target_link_libraries(test_empty_records rinex tap)
add_test(NAME empty_records COMMAND test_empty_records)
set_property(TEST empty_records PROPERTY LABELS function)

add_executable(test_event_records test/test_event_records.c)
target_link_libraries(test_event_records rinex tap)
add_test(NAME event_records COMMAND test_event_records)
set_property(TEST event_records PROPERTY LABELS function)

add_executable(test_parse_errors test/parse_errors.c)
target_link_libraries(test_parse_errors rinex tap)
add_test(NAME parse_errors COMMAND test_parse_errors)
set_property(TEST parse_errors PROPERTY LABELS function)

add_executable(test_rnx_v2 test/rinex_v2.c)
target_link_libraries(test_rnx_v2 rinex tap)
add_test(NAME rnx_v2 COMMAND test_rnx_v2)
set_property(TEST rnx_v2 PROPERTY LABELS function)

add_executable(test_rnx_v3 test/rinex_v3.c)
target_link_libraries(test_rnx_v3 rinex tap)
add_test(NAME rnx_v3 COMMAND test_rnx_v3)
set_property(TEST rnx_v3 PROPERTY LABELS function)

add_executable(test_crx_v2 test/crx_v2.c)
target_link_libraries(test_crx_v2 rinex tap)
add_test(NAME crx_v2 COMMAND test_crx_v2)
set_property(TEST crx_v2 PROPERTY LABELS function)

add_executable(test_crx_v3 test/crx_v3.c)
target_link_libraries(test_crx_v3 rinex tap)
add_test(NAME crx_v3 COMMAND test_crx_v3)
set_property(TEST crx_v3 PROPERTY LABELS function)

add_executable(test_transpose test/test_transpose.c)
target_link_libraries(test_transpose rinex)
add_test(NAME transpose COMMAND test_transpose)
set_property(TEST transpose PROPERTY LABELS function)
add_test(NAME transpose-bench COMMAND test_transpose -bench)
set_property(TEST transpose-bench PROPERTY LABELS benchmark)
add_test(NAME transpose-bench-std COMMAND test_transpose -bench)
set_property(TEST transpose-bench-std PROPERTY ENVIRONMENT "RINEX_SIMD=std")
set_property(TEST transpose-bench-std PROPERTY LABELS benchmark)

add_executable(test_reader_bounds test/test_reader_bounds.c)
target_link_libraries(test_reader_bounds rinex tap)
add_test(NAME reader_bounds COMMAND test_reader_bounds)
set_property(TEST reader_bounds PROPERTY LABELS function)

add_executable(test_load_alloc test/test_load_alloc.c)
target_link_libraries(test_load_alloc rinex tap)
add_test(NAME load_alloc COMMAND test_load_alloc)
set_property(TEST load_alloc PROPERTY LABELS function)

add_executable(test_evtf test/test_evtf.c)
target_link_libraries(test_evtf rinex tap)
target_compile_definitions(test_evtf PRIVATE
    RNX2SRNX_PATH="$<TARGET_FILE:rnx2srnx>"
    SRNX2RNX_PATH="$<TARGET_FILE:srnx2rnx>"
    RNX2SRNX_SEARCH_PATH="$<TARGET_FILE:rnx2srnx_search>")
add_dependencies(test_evtf rnx2srnx srnx2rnx rnx2srnx_search)
add_test(NAME evtf COMMAND test_evtf)
set_property(TEST evtf PROPERTY LABELS function)
add_test(NAME evtf_socd COMMAND test_evtf --socd)
set_property(TEST evtf_socd PROPERTY LABELS function)

add_executable(test_soc2 test/test_soc2.c)
target_link_libraries(test_soc2 rinex tap m)
target_compile_definitions(test_soc2 PRIVATE
    RNX2SRNX_PATH="$<TARGET_FILE:rnx2srnx>")
add_dependencies(test_soc2 rnx2srnx)
add_test(NAME soc2 COMMAND test_soc2)
set_property(TEST soc2 PROPERTY LABELS function)
