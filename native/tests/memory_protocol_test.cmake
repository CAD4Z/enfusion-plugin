set(source "${CMAKE_CURRENT_BINARY_DIR}/memory-source.png")
set(single "${CMAKE_CURRENT_BINARY_DIR}/memory-single.edds")
set(request "${CMAKE_CURRENT_BINARY_DIR}/memory-batch.ndjson")
execute_process(COMMAND "${FIXTURE}" --memory "${source}" RESULT_VARIABLE fixture_status)
if(NOT fixture_status EQUAL 0)
  message(FATAL_ERROR "compressed memory fixture could not be written")
endif()

execute_process(COMMAND "${CLI}" convert --machine --protocol 1
  --input "${source}" --output "${single}"
  RESULT_VARIABLE single_status OUTPUT_QUIET ERROR_VARIABLE single_error)
if(NOT single_status EQUAL 0)
  message(FATAL_ERROR "reference conversion failed: ${single_error}")
endif()
file(SHA256 "${single}" expected_hash)
set(profile [=[{"TargetFormat":"EnfusionDDS","FormatCompress":"Fastest","CompressTreshold":80,"RemoveMips":0,"Conversion":"None","ConversionQuality":1,"Swizzling":"None","ContainsMips":false,"GenerateMips":true,"Normalize":false,"MipMapFunction":"Filter","MipMapFilter":"Box","TiledTexture":true}]=])
file(WRITE "${request}" "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":8}\n")
foreach(index RANGE 0 7)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/memory-batch-${index}.edds")
  file(REMOVE "${output}" "${output}.meta")
  file(APPEND "${request}"
    "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"${index}\",\"input\":\"${source}\",\"output\":\"${output}\",\"metadata\":null,\"identity\":null,\"profile\":${profile},\"expected\":null}\n")
endforeach()
file(APPEND "${request}" "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env EDDS_CONVERT_WORKERS=8
  "${CLI}" batch --machine --protocol 1 INPUT_FILE "${request}"
  RESULT_VARIABLE batch_status OUTPUT_VARIABLE events ERROR_VARIABLE diagnostics)
if(NOT batch_status EQUAL 0 OR NOT events MATCHES "\"converted\":8,\"failed\":0,\"cancelled\":0")
  message(FATAL_ERROR "compressed batch did not finish: ${events}\n${diagnostics}")
endif()
string(REGEX MATCHALL "\"kind\":\"result\"" results "${events}")
list(LENGTH results result_count)
if(NOT result_count EQUAL 8)
  message(FATAL_ERROR "allocation retries leaked intermediate item results: ${events}")
endif()
foreach(index RANGE 0 7)
  set(output "${CMAKE_CURRENT_BINARY_DIR}/memory-batch-${index}.edds")
  file(SHA256 "${output}" actual_hash)
  if(NOT actual_hash STREQUAL expected_hash)
    message(FATAL_ERROR "quota retries changed the encoded pixels of item ${index}")
  endif()
endforeach()

# The source is transparent black. Check known pixels as well as agreement with single conversion.
execute_process(COMMAND "${CLI}" preview --machine --protocol 1 --mip 10 --input "${single}"
  RESULT_VARIABLE preview_status OUTPUT_VARIABLE preview ERROR_VARIABLE preview_error)
if(NOT preview_status EQUAL 0)
  message(FATAL_ERROR "compressed fixture preview failed: ${preview_error}")
endif()
string(JSON bytes GET "${preview}" byteLength)
string(JSON pixels GET "${preview}" pixelsBase64)
if(NOT bytes EQUAL 4 OR NOT pixels STREQUAL "AAAAAA==")
  message(FATAL_ERROR "compressed fixture did not preserve its known transparent-black mip: ${preview}")
endif()

# This fault occurs after decode/encode quota retries and before publication of a registered pair.
set(output "${CMAKE_CURRENT_BINARY_DIR}/memory-rollback.edds")
file(REMOVE "${output}.meta")
file(WRITE "${output}" "previous EDDS bytes")
file(SHA256 "${output}" previous_hash)
file(WRITE "${request}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":1}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"rollback\",\"input\":\"${source}\",\"output\":\"${output}\",\"metadata\":\"${output}.meta\",\"identity\":{\"guid\":\"0123456789ABCDEF\",\"name\":\"Memory/memory-rollback.edds\",\"sourceFile\":\"memory-source.png\"},\"profile\":${profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env EDDS_CONVERT_FAIL=metadata-write
  "${CLI}" batch --machine --protocol 1 INPUT_FILE "${request}"
  RESULT_VARIABLE rollback_status OUTPUT_VARIABLE rollback_events ERROR_VARIABLE rollback_error)
file(SHA256 "${output}" after_hash)
if(NOT rollback_status EQUAL 0 OR NOT rollback_events MATCHES "\"converted\":0,\"failed\":1" OR
   NOT after_hash STREQUAL previous_hash OR EXISTS "${output}.meta")
  message(FATAL_ERROR "failed quota-retried conversion mutated the previous pair: ${rollback_events}\n${rollback_error}")
endif()
file(GLOB temps "${CMAKE_CURRENT_BINARY_DIR}/memory-*.edds*.edds-convert-*.tmp")
if(temps)
  message(FATAL_ERROR "quota retries left sibling transaction temps: ${temps}")
endif()
