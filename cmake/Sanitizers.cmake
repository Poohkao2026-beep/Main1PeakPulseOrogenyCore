function(peak_pulse_configure_sanitizers target)
  if(NOT PEAK_PULSE_ENABLE_ASAN AND NOT PEAK_PULSE_ENABLE_UBSAN AND NOT PEAK_PULSE_ENABLE_TSAN)
    return()
  endif()

  if(MSVC)
    if(PEAK_PULSE_ENABLE_UBSAN OR PEAK_PULSE_ENABLE_TSAN)
      message(FATAL_ERROR "This MSVC toolchain does not support the requested sanitizer")
    endif()
    target_compile_options(${target} PRIVATE /fsanitize=address)
    target_link_options(${target} PRIVATE /fsanitize=address)
    return()
  endif()

  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "Sanitizers are unsupported by ${CMAKE_CXX_COMPILER_ID}")
  endif()

  set(sanitizer_flags)
  if(PEAK_PULSE_ENABLE_ASAN)
    list(APPEND sanitizer_flags address)
  endif()
  if(PEAK_PULSE_ENABLE_UBSAN)
    list(APPEND sanitizer_flags undefined)
  endif()
  if(PEAK_PULSE_ENABLE_TSAN)
    list(APPEND sanitizer_flags thread)
  endif()

  list(JOIN sanitizer_flags "," sanitizer_list)
  target_compile_options(
    ${target}
    PRIVATE -fsanitize=${sanitizer_list} -fno-omit-frame-pointer
  )
  target_link_options(${target} PRIVATE -fsanitize=${sanitizer_list})
endfunction()
