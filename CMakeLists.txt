cmake_minimum_required(VERSION 3.26 FATAL_ERROR)
cmake_policy(VERSION 3.29)
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

try_compile(HAVE_NEON
    SOURCES ${CMAKE_SOURCE_DIR}/rinex/parse_neon.c
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CMAKE_SOURCE_DIR}"
    LOG_DESCRIPTION "NEON support"
    COMPILE_DEFINITIONS -DHAVE_NEON)
add_compile_definitions($<$<BOOL:${HAVE_NEON}>:HAVE_NEON>)

try_compile(HAVE_AVX2
    SOURCES ${CMAKE_SOURCE_DIR}/rinex/parse_avx2.c
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${CMAKE_SOURCE_DIR}"
    LOG_DESCRIPTION "AVX2 support"
    COMPILE_DEFINITIONS -DHAVE_AVX2)
add_compile_definitions($<$<BOOL:${HAVE_AVX2}>:HAVE_AVX2>)

add_library(rinex
    rinex/crx_parse.c
    rinex/parse_std.c
    rinex/parse_neon.c
    $<$<BOOL:${HAVE_NEON}>:rinex/parse_neon.c>
    $<$<BOOL:${HAVE_AVX2}>:rinex/parse_avx2.c>
    rinex/rinex_buffer.c
    rinex/rinex_mmap.c
    rinex/rinex_stdio.c
    rinex/rnx_parse.c
    rinex/rnx_priv.c
    rinex/transpose.c)
add_library(tap STATIC
    libtap/tap.c)

add_executable(rinex_analyze bin/rinex_analyze.c bin/driver.c)
target_link_libraries(rinex_analyze rinex)

add_executable(rinex_maxima bin/rinex_maxima.c bin/driver.c)
target_link_libraries(rinex_maxima rinex)

add_executable(rinex_n_obs bin/rinex_n_obs.c bin/driver.c)
target_link_libraries(rinex_n_obs rinex)

add_executable(rinex_scan bin/rinex_scan.c bin/driver.c)
target_link_libraries(rinex_scan rinex)

include(InstallRequiredSystemLibraries)
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/COPYING")
set(CPACK_PACKAGE_VERSION_MAJOR "${rinex_VERSION_MAJOR}")
set(CPACK_PACKAGE_VERSION_MINOR "${rinex_VERSION_MINOR}")
set(CPACK_SOURCE_GENERATOR "TGZ")
include(CPack)

enable_testing()

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
