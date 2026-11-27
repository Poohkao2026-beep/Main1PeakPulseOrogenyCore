cmake_minimum_required(VERSION 3.25)
project(PeakPulseInstalledConsumer LANGUAGES CXX)

set(peak_pulse_export_suffixes
    lib/cmake/PeakPulseOrogenyCore
    lib64/cmake/PeakPulseOrogenyCore
    share/cmake/PeakPulseOrogenyCore)
if(CMAKE_LIBRARY_ARCHITECTURE)
  list(PREPEND peak_pulse_export_suffixes
       "lib/${CMAKE_LIBRARY_ARCHITECTURE}/cmake/PeakPulseOrogenyCore")
endif()

# The current install provides a target export, but not a package config file.
find_file(PEAK_PULSE_TARGETS_FILE
          NAMES PeakPulseOrogenyCoreTargets.cmake
          PATH_SUFFIXES ${peak_pulse_export_suffixes}
          REQUIRED)
include("${PEAK_PULSE_TARGETS_FILE}")
find_package(Threads REQUIRED)

add_executable(peak_pulse_consumer main.cpp)
target_link_libraries(peak_pulse_consumer PRIVATE peak_pulse::peak_pulse Threads::Threads)

enable_testing()
add_test(NAME peak_pulse_installed_consumer COMMAND peak_pulse_consumer)
