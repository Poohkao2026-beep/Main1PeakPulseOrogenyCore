cmake_minimum_required(VERSION 3.25)

project(PeakPulseOrogenyCore VERSION 0.1.0 LANGUAGES CXX)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)
include(CTest)
include(cmake/ProjectOptions.cmake)
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

option(PEAK_PULSE_BUILD_TESTS "Build GoogleTest tests" ON)
option(PEAK_PULSE_BUILD_BENCHMARKS "Build Google Benchmark benchmarks" ON)
option(PEAK_PULSE_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
option(PEAK_PULSE_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(PEAK_PULSE_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(PEAK_PULSE_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(PEAK_PULSE_ENABLE_LTO "Enable interprocedural optimization" OFF)
option(PEAK_PULSE_NATIVE_ARCH "Optimize for the build machine's architecture" OFF)
option(PEAK_PULSE_MUTATE_WEAKEN_ORDER "Mutation-test: weaken queue publication to relaxed" OFF)

if(PEAK_PULSE_ENABLE_TSAN AND PEAK_PULSE_ENABLE_ASAN)
  message(FATAL_ERROR "ThreadSanitizer and AddressSanitizer cannot be enabled together")
endif()

add_library(peak_pulse INTERFACE)
add_library(peak_pulse::peak_pulse ALIAS peak_pulse)
target_include_directories(
  peak_pulse
  INTERFACE
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(peak_pulse INTERFACE cxx_std_20)

find_package(Threads REQUIRED)

if(PEAK_PULSE_BUILD_TESTS)
  enable_testing()
  include(cmake/Dependencies.cmake)
  peak_pulse_fetch_gtest()

  add_executable(
    peak_pulse_tests
    tests/ring_buffer_tests.cpp
    tests/ring_buffer_stress_tests.cpp
    tests/perf_statistics_tests.cpp
  )
  set_target_properties(
    peak_pulse_tests
    PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  )
  target_link_libraries(
    peak_pulse_tests
    PRIVATE peak_pulse GTest::gtest_main Threads::Threads
  )
  peak_pulse_configure_project_target(peak_pulse_tests)
  if(PEAK_PULSE_MUTATE_WEAKEN_ORDER)
    target_compile_definitions(
      peak_pulse_tests
      PRIVATE PEAK_PULSE_MUTATE_WEAKEN_ORDER
    )
  endif()
  try_compile(
    PEAK_PULSE_CAPACITY_ONE_COMPILES
    ${CMAKE_CURRENT_BINARY_DIR}/capacity_one_compile_check
    SOURCES ${PROJECT_SOURCE_DIR}/tests/compile_fail_capacity_one.cpp
    CMAKE_FLAGS
      "-DCMAKE_CXX_STANDARD=20"
      "-DCMAKE_CXX_STANDARD_REQUIRED=ON"
      "-DINCLUDE_DIRECTORIES:STRING=${PROJECT_SOURCE_DIR}/include"
    OUTPUT_VARIABLE PEAK_PULSE_CAPACITY_ONE_OUTPUT
  )
  if(PEAK_PULSE_CAPACITY_ONE_COMPILES)
    message(FATAL_ERROR "Capacity-one queue unexpectedly compiled")
  endif()
  if(NOT PEAK_PULSE_CAPACITY_ONE_OUTPUT MATCHES "Capacity must be at least 2")
    message(FATAL_ERROR "Capacity-one compile failed for an unexpected reason:\n${PEAK_PULSE_CAPACITY_ONE_OUTPUT}")
  endif()
  message(STATUS "Verified: capacity-one queue fails its compile-time assertion")
  include(GoogleTest)
  gtest_discover_tests(
    peak_pulse_tests
    DISCOVERY_TIMEOUT 30
    PROPERTIES TIMEOUT 180
  )
endif()

add_executable(peak_pulse_perf benchmarks/mainperformance.cpp)
set_target_properties(
  peak_pulse_perf
  PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
)
target_link_libraries(peak_pulse_perf PRIVATE peak_pulse Threads::Threads)
peak_pulse_configure_project_target(peak_pulse_perf)
execute_process(
  COMMAND git rev-parse --short HEAD
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  OUTPUT_VARIABLE PEAK_PULSE_GIT_COMMIT
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
if(NOT PEAK_PULSE_GIT_COMMIT)
  set(PEAK_PULSE_GIT_COMMIT "unavailable")
endif()
string(TOUPPER "${CMAKE_BUILD_TYPE}" PEAK_PULSE_BUILD_TYPE_UPPER)
target_compile_definitions(
  peak_pulse_perf
  PRIVATE
    PEAK_PULSE_BUILD_TYPE="$<CONFIG>"
    PEAK_PULSE_BUILD_FLAGS="${CMAKE_CXX_FLAGS} $<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}> $<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}> $<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO}> $<$<CONFIG:MinSizeRel>:${CMAKE_CXX_FLAGS_MINSIZEREL}>"
    PEAK_PULSE_GIT_COMMIT="${PEAK_PULSE_GIT_COMMIT}"
    $<$<BOOL:${PEAK_PULSE_ENABLE_ASAN}>:PEAK_PULSE_ASAN_ENABLED>
    $<$<BOOL:${PEAK_PULSE_ENABLE_UBSAN}>:PEAK_PULSE_UBSAN_ENABLED>
    $<$<BOOL:${PEAK_PULSE_ENABLE_TSAN}>:PEAK_PULSE_TSAN_ENABLED>
)

if(PEAK_PULSE_BUILD_TESTS)
  add_test(
    NAME peak_pulse_perf_smoke
    COMMAND
      ${CMAKE_COMMAND}
      -DPERF_EXECUTABLE=$<TARGET_FILE:peak_pulse_perf>
      -DOUTPUT_JSON=${CMAKE_CURRENT_BINARY_DIR}/peak_pulse_perf_smoke.json
      -P ${PROJECT_SOURCE_DIR}/tests/perf_smoke.cmake
  )
  set_tests_properties(peak_pulse_perf_smoke PROPERTIES TIMEOUT 15)
endif()

if(PEAK_PULSE_BUILD_BENCHMARKS)
  include(cmake/Dependencies.cmake)
  peak_pulse_fetch_benchmark()

  add_executable(peak_pulse_bench benchmarks/ring_buffer_bench.cpp)
  set_target_properties(
    peak_pulse_bench
    PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF
  )
  target_link_libraries(
    peak_pulse_bench
    PRIVATE peak_pulse benchmark::benchmark benchmark::benchmark_main Threads::Threads
  )
  peak_pulse_configure_project_target(peak_pulse_bench)
endif()

install(
  TARGETS peak_pulse
  EXPORT PeakPulseOrogenyCoreTargets
)
install(
  DIRECTORY include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
  EXPORT PeakPulseOrogenyCoreTargets
  NAMESPACE peak_pulse::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/PeakPulseOrogenyCore
)
