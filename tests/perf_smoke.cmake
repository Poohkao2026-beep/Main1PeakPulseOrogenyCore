if(NOT DEFINED PERF_EXECUTABLE OR NOT DEFINED OUTPUT_JSON)
  message(FATAL_ERROR "PERF_EXECUTABLE and OUTPUT_JSON are required")
endif()

execute_process(
  COMMAND
    "${PERF_EXECUTABLE}"
    --scenario mpmc
    --duration-sec 1
    --warmup-sec 0
    --repetitions 1
    --items 10000
    --json "${OUTPUT_JSON}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
  TIMEOUT 12
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Performance smoke failed (${result}): ${stdout}${stderr}")
endif()
if(NOT EXISTS "${OUTPUT_JSON}")
  message(FATAL_ERROR "Performance smoke did not create its JSON output")
endif()

file(READ "${OUTPUT_JSON}" json)
string(JSON schema ERROR_VARIABLE json_error GET "${json}" schema_version)
if(json_error OR NOT schema EQUAL 1)
  message(FATAL_ERROR "Invalid JSON schema_version: ${json_error}")
endif()
string(JSON scenario ERROR_VARIABLE json_error GET "${json}" results 0 scenario)
if(json_error OR NOT scenario STREQUAL "mpmc")
  message(FATAL_ERROR "Expected mpmc result: ${json_error}")
endif()
string(JSON queue ERROR_VARIABLE json_error GET "${json}" results 0 queue)
if(json_error OR NOT queue STREQUAL "ring_buffer")
  message(FATAL_ERROR "Expected ring_buffer result: ${json_error}")
endif()
