set(copy "${CMAKE_CURRENT_BINARY_DIR}/black-box-copy.edds")
set(lz4 "${CMAKE_CURRENT_BINARY_DIR}/black-box-lz4.edds")
set(dxt1 "${CMAKE_CURRENT_BINARY_DIR}/black-box-dxt1.edds")
set(odd_fourcc "${CMAKE_CURRENT_BINARY_DIR}/black-box-odd-fourcc.edds")
set(overflow "${CMAKE_CURRENT_BINARY_DIR}/black-box-overflow.edds")

execute_process(
  COMMAND "${FIXTURE}" "${copy}" "${lz4}" "${dxt1}" "${odd_fourcc}" "${overflow}"
  RESULT_VARIABLE fixture_result
  ERROR_VARIABLE fixture_error
)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "fixture generation failed with ${fixture_result}: ${fixture_error}")
endif()

execute_process(
  COMMAND "${REFERENCE_READER}" "${copy}" "${lz4}"
  RESULT_VARIABLE reference_result
  ERROR_VARIABLE reference_error
)
if(NOT reference_result EQUAL 0)
  message(FATAL_ERROR "independent fixture read failed with ${reference_result}: ${reference_error}")
endif()

execute_process(
  COMMAND "${CLI}" protocol --machine
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "protocol failed with ${result}: ${error}")
endif()

execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${copy}"
  RESULT_VARIABLE inspect_result
  OUTPUT_VARIABLE inspect_output
  ERROR_VARIABLE inspect_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT inspect_result EQUAL 0)
  message(FATAL_ERROR "COPY inspect failed with ${inspect_result}: ${inspect_error}")
endif()
string(JSON inspect_kind GET "${inspect_output}" kind)
string(JSON inspect_width GET "${inspect_output}" width)
string(JSON inspect_mips GET "${inspect_output}" mipCount)
string(JSON level_zero GET "${inspect_output}" mips 0 level)
string(JSON level_zero_container GET "${inspect_output}" mips 0 container)
if(NOT inspect_kind STREQUAL "inspect" OR NOT inspect_width EQUAL 3 OR
   NOT inspect_mips EQUAL 2 OR NOT level_zero EQUAL 0 OR
   NOT level_zero_container STREQUAL "COPY")
  message(FATAL_ERROR "unexpected COPY inspection: ${inspect_output}")
endif()

execute_process(
  COMMAND "${CLI}" preview --machine --protocol 1 --mip 1 --input "${copy}"
  RESULT_VARIABLE preview_result
  OUTPUT_VARIABLE preview_output
  ERROR_VARIABLE preview_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT preview_result EQUAL 0)
  message(FATAL_ERROR "COPY preview failed with ${preview_result}: ${preview_error}")
endif()
string(JSON preview_pixels GET "${preview_output}" pixelsBase64)
if(NOT preview_pixels STREQUAL "ChQeKA==")
  message(FATAL_ERROR "selected mip pixels were not independently expected: ${preview_output}")
endif()

execute_process(
  COMMAND "${CLI}" preview --machine --protocol 1 --mip 0 --input "${lz4}"
  RESULT_VARIABLE lz4_result
  OUTPUT_VARIABLE lz4_output
  ERROR_VARIABLE lz4_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT lz4_result EQUAL 0)
  message(FATAL_ERROR "LZ4 preview failed with ${lz4_result}: ${lz4_error}")
endif()
string(JSON lz4_pixels GET "${lz4_output}" pixelsBase64)
if(NOT lz4_pixels STREQUAL "AQID/wQFBv8=")
  message(FATAL_ERROR "LZ4/BGRX pixels were not independently expected: ${lz4_output}")
endif()

execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${dxt1}"
  RESULT_VARIABLE unsupported_inspect_result
  OUTPUT_VARIABLE unsupported_inspect_output
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT unsupported_inspect_result EQUAL 0)
  message(FATAL_ERROR "unsupported pixels must remain inspectable")
endif()
string(JSON preview_supported GET "${unsupported_inspect_output}" previewSupported)
if(preview_supported)
  message(FATAL_ERROR "DXT1 was represented as previewable: ${unsupported_inspect_output}")
endif()

execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${odd_fourcc}"
  RESULT_VARIABLE odd_fourcc_result
  OUTPUT_VARIABLE odd_fourcc_output
  ERROR_VARIABLE odd_fourcc_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT odd_fourcc_result EQUAL 0)
  message(FATAL_ERROR "odd FourCC inspection failed with ${odd_fourcc_result}: ${odd_fourcc_error}")
endif()
string(JSON odd_fourcc_value GET "${odd_fourcc_output}" dds fourCC)
if(NOT odd_fourcc_value STREQUAL [=[Q"\?]=])
  message(FATAL_ERROR "FourCC was not safely represented in JSON: ${odd_fourcc_output}")
endif()

execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${overflow}"
  RESULT_VARIABLE overflow_result
  OUTPUT_VARIABLE overflow_output
  ERROR_QUIET
)
if(NOT overflow_result EQUAL 3)
  message(FATAL_ERROR "integer-overflow seed exit was ${overflow_result}, expected 3: ${overflow_output}")
endif()

execute_process(
  COMMAND "${CLI}" preview --machine --protocol 1 --mip 0 --input "${dxt1}"
  RESULT_VARIABLE unsupported_preview_result
  OUTPUT_VARIABLE unsupported_preview_output
  ERROR_VARIABLE unsupported_preview_error
)
if(NOT unsupported_preview_result EQUAL 4)
  message(FATAL_ERROR "unsupported preview exit was ${unsupported_preview_result}, expected 4")
endif()

execute_process(COMMAND "${CLI}" inspect --machine --protocol 2 --input "${copy}"
  RESULT_VARIABLE bad_protocol_result OUTPUT_VARIABLE bad_protocol_output ERROR_QUIET)
execute_process(COMMAND "${CLI}" inspect --machine --protocol 1
  RESULT_VARIABLE bad_invocation_result OUTPUT_QUIET ERROR_QUIET)
if(NOT bad_protocol_result EQUAL 2 OR NOT bad_invocation_result EQUAL 2)
  message(FATAL_ERROR "invalid invocations do not have stable exit 2")
endif()
string(JSON bad_protocol_category GET "${bad_protocol_output}" error category)
if(NOT bad_protocol_category STREQUAL "invalid-invocation")
  message(FATAL_ERROR "invalid invocation machine category changed: ${bad_protocol_output}")
endif()

string(JSON protocol_version ERROR_VARIABLE json_error GET "${output}" protocolVersion)
if(json_error OR NOT protocol_version EQUAL 1)
  message(FATAL_ERROR "bad protocol response: ${output}")
endif()

string(JSON kind GET "${output}" kind)
if(NOT kind STREQUAL "protocol")
  message(FATAL_ERROR "bad protocol kind: ${output}")
endif()

string(JSON first_command GET "${output}" commands 0)
string(JSON second_command GET "${output}" commands 1)
if(NOT first_command STREQUAL "inspect" OR NOT second_command STREQUAL "preview")
  message(FATAL_ERROR "bad command list: ${output}")
endif()
