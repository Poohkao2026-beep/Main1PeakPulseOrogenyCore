include(CheckIPOSupported)

function(peak_pulse_configure_project_target target)
  peak_pulse_configure_warnings(${target})
  peak_pulse_configure_sanitizers(${target})

  if(PEAK_PULSE_NATIVE_ARCH)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      target_compile_options(${target} PRIVATE -march=native)
    elseif(MSVC)
      message(
        FATAL_ERROR
        "PEAK_PULSE_NATIVE_ARCH has no portable MSVC equivalent; use a compiler-specific build configuration"
      )
    endif()
  endif()

  if(PEAK_PULSE_ENABLE_LTO)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error LANGUAGES CXX)
    if(NOT ipo_supported)
      message(FATAL_ERROR "LTO was requested but is unavailable: ${ipo_error}")
    endif()
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endfunction()
