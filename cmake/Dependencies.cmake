include(FetchContent)

function(peak_pulse_fetch_gtest)
  set(gtest_force_shared_crt ON CACHE BOOL "Use the shared MSVC runtime" FORCE)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    FIND_PACKAGE_ARGS NAMES GTest
  )
  FetchContent_MakeAvailable(googletest)
endfunction()

function(peak_pulse_fetch_benchmark)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "Disable Google Benchmark tests" FORCE)
  set(BENCHMARK_ENABLE_WERROR OFF CACHE BOOL "Keep upstream warnings out of project Werror policy" FORCE)
  FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.8.3
    FIND_PACKAGE_ARGS NAMES benchmark
  )
  FetchContent_MakeAvailable(googlebenchmark)
endfunction()
