set(copy "${CMAKE_CURRENT_BINARY_DIR}/black-box-copy.edds")
set(lz4 "${CMAKE_CURRENT_BINARY_DIR}/black-box-lz4.edds")
set(dxt1 "${CMAKE_CURRENT_BINARY_DIR}/black-box-dxt1.edds")
set(odd_fourcc "${CMAKE_CURRENT_BINARY_DIR}/black-box-odd-fourcc.edds")
set(overflow "${CMAKE_CURRENT_BINARY_DIR}/black-box-overflow.edds")
set(png "${CMAKE_CURRENT_BINARY_DIR}/black-box-source.png")
set(tga "${CMAKE_CURRENT_BINARY_DIR}/black-box-source.tga")
set(png_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-png-result.edds")
set(tga_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-tga-result.edds")
set(png_metadata "${png_result}.meta")

execute_process(
  COMMAND "${FIXTURE}" "${copy}" "${lz4}" "${dxt1}" "${odd_fourcc}" "${overflow}" "${png}" "${tga}"
  RESULT_VARIABLE fixture_result
  ERROR_VARIABLE fixture_error
)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "fixture generation failed with ${fixture_result}: ${fixture_error}")
endif()

set(batch_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch.ndjson")
set(batch_png "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-png.edds")
set(batch_tga "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-tga.edds")
set(batch_profile [=[{"TargetFormat":"EnfusionDDS","FormatCompress":"Fastest","CompressTreshold":80,"Conversion":"None","ConversionQuality":1,"Swizzling":"None","GenerateMips":false,"MipMapFunction":"Filter","MipMapFilter":"Box","TiledTexture":true}]=])
file(WRITE "${batch_input}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":3}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"png\",\"input\":\"${png}\",\"output\":\"${batch_png}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"bad\",\"input\":\"${CMAKE_CURRENT_BINARY_DIR}/missing.png\",\"output\":\"${CMAKE_CURRENT_BINARY_DIR}/missing.edds\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"tga\",\"input\":\"${tga}\",\"output\":\"${batch_tga}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1
  INPUT_FILE "${batch_input}"
  RESULT_VARIABLE batch_result OUTPUT_VARIABLE batch_output ERROR_VARIABLE batch_error
)
if(NOT batch_result EQUAL 0 OR NOT EXISTS "${batch_png}" OR NOT EXISTS "${batch_tga}" OR
    EXISTS "${CMAKE_CURRENT_BINARY_DIR}/missing.edds")
  message(FATAL_ERROR "mixed batch failed with ${batch_result}: ${batch_error}\n${batch_output}")
endif()
string(REGEX MATCHALL "\"kind\":\"result\"" batch_results "${batch_output}")
list(LENGTH batch_results batch_result_count)
if(NOT batch_result_count EQUAL 3 OR NOT batch_output MATCHES "\"converted\":2" OR
    NOT batch_output MATCHES "\"failed\":1")
  message(FATAL_ERROR "mixed batch statuses were not isolated: ${batch_output}")
endif()
foreach(expected IN ITEMS
    "\"id\":\"png\",\"status\":\"Converted\""
    "\"id\":\"bad\",\"status\":\"Failed\""
    "\"id\":\"tga\",\"status\":\"Converted\"")
  if(NOT batch_output MATCHES "${expected}")
    message(FATAL_ERROR "mixed batch omitted per-item status ${expected}: ${batch_output}")
  endif()
endforeach()

set(collision_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-collision.ndjson")
set(collision_output "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-collision.edds")
set(collision_independent "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-independent.edds")
file(REMOVE "${collision_output}" "${collision_independent}")
file(WRITE "${collision_input}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":3}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"collision-png\",\"input\":\"${png}\",\"output\":\"${collision_output}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"collision-tga\",\"input\":\"${tga}\",\"output\":\"${collision_output}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"independent\",\"input\":\"${png}\",\"output\":\"${collision_independent}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1 INPUT_FILE "${collision_input}"
  RESULT_VARIABLE collision_result OUTPUT_VARIABLE collision_stdout ERROR_QUIET
)
if(NOT collision_result EQUAL 0 OR EXISTS "${collision_output}" OR
    NOT EXISTS "${collision_independent}" OR NOT collision_stdout MATCHES "\"converted\":1" OR
    NOT collision_stdout MATCHES "\"failed\":2")
  message(FATAL_ERROR "collision group was not isolated: ${collision_stdout}")
endif()
foreach(expected IN ITEMS
    "\"id\":\"collision-png\",\"status\":\"Failed\""
    "\"id\":\"collision-tga\",\"status\":\"Failed\""
    "\"id\":\"independent\",\"status\":\"Converted\"")
  if(NOT collision_stdout MATCHES "${expected}")
    message(FATAL_ERROR "collision batch omitted per-item status ${expected}: ${collision_stdout}")
  endif()
endforeach()

set(hundred_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-100.ndjson")
file(WRITE "${hundred_input}" "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":100}\n")
foreach(at RANGE 0 99)
  file(APPEND "${hundred_input}"
    "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"${at}\",\"input\":\"${tga}\",\"output\":\"${CMAKE_CURRENT_BINARY_DIR}/batch-100-${at}.edds\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n")
endforeach()
file(APPEND "${hundred_input}" "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1 INPUT_FILE "${hundred_input}"
  RESULT_VARIABLE hundred_result OUTPUT_VARIABLE hundred_output ERROR_VARIABLE hundred_error
)
if(NOT hundred_result EQUAL 0 OR NOT hundred_output MATCHES "\"converted\":100")
  message(FATAL_ERROR "100-item batch failed with ${hundred_result}: ${hundred_error}\n${hundred_output}")
endif()
foreach(at RANGE 0 99)
  if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/batch-100-${at}.edds" OR
      NOT hundred_output MATCHES "\"id\":\"${at}\",\"status\":\"Converted\"")
    message(FATAL_ERROR "100-item batch omitted output or status for ${at}")
  endif()
endforeach()

set(too_many_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-too-many.ndjson")
file(WRITE "${too_many_input}" "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":257}\n")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1 INPUT_FILE "${too_many_input}"
  RESULT_VARIABLE too_many_result OUTPUT_VARIABLE too_many_output ERROR_QUIET
)
if(NOT too_many_result EQUAL 2 OR EXISTS "${CMAKE_CURRENT_BINARY_DIR}/batch-257-0.edds")
  message(FATAL_ERROR "oversized batch was not refused before encoding: ${too_many_output}")
endif()

set(cancel_marker "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch.cancel")
file(WRITE "${cancel_marker}" "cancel")
file(REMOVE "${batch_png}" "${batch_tga}")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1 --cancel-file "${cancel_marker}"
  INPUT_FILE "${batch_input}"
  RESULT_VARIABLE cancelled_result OUTPUT_VARIABLE cancelled_output ERROR_QUIET
)
if(NOT cancelled_result EQUAL 5 OR EXISTS "${batch_png}" OR EXISTS "${batch_tga}" OR
    NOT cancelled_output MATCHES "\"cancelled\":3")
  message(FATAL_ERROR "cooperative batch cancellation launched queued work: ${cancelled_output}")
endif()
foreach(id IN ITEMS png bad tga)
  if(NOT cancelled_output MATCHES "\"id\":\"${id}\",\"status\":\"Cancelled\"")
    message(FATAL_ERROR "pre-cancelled batch omitted cancelled status for ${id}: ${cancelled_output}")
  endif()
endforeach()

set(mid_cancel_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-mid-cancel.ndjson")
set(mid_cancel_marker "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-mid.cancel")
set(mid_cancel_first "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-mid-first.edds")
set(mid_cancel_second "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-mid-second.edds")
set(mid_cancel_third "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-mid-third.edds")
file(REMOVE "${mid_cancel_marker}" "${mid_cancel_first}" "${mid_cancel_second}" "${mid_cancel_third}")
file(WRITE "${mid_cancel_input}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":3}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"done\",\"input\":\"${png}\",\"output\":\"${mid_cancel_first}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"queued-1\",\"input\":\"${tga}\",\"output\":\"${mid_cancel_second}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"queued-2\",\"input\":\"${png}\",\"output\":\"${mid_cancel_third}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "EDDS_CONVERT_FAIL=batch-cancel-after-first-commit"
    "${CLI}" batch --machine --protocol 1 --cancel-file "${mid_cancel_marker}"
  INPUT_FILE "${mid_cancel_input}"
  RESULT_VARIABLE mid_cancel_result OUTPUT_VARIABLE mid_cancel_output ERROR_QUIET
)
if(NOT mid_cancel_result EQUAL 5 OR NOT EXISTS "${mid_cancel_first}" OR
    EXISTS "${mid_cancel_second}" OR EXISTS "${mid_cancel_third}" OR
    NOT mid_cancel_output MATCHES "\"id\":\"done\",\"status\":\"Converted\"" OR
    NOT mid_cancel_output MATCHES "\"id\":\"queued-1\",\"status\":\"Cancelled\"" OR
    NOT mid_cancel_output MATCHES "\"id\":\"queued-2\",\"status\":\"Cancelled\"" OR
    NOT mid_cancel_output MATCHES "\"converted\":1" OR
    NOT mid_cancel_output MATCHES "\"cancelled\":2")
  message(FATAL_ERROR "mid-batch cancellation did not preserve honest completed results: ${mid_cancel_output}")
endif()

foreach(bad_case IN ITEMS malformed incompatible unknown)
  set(bad_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-${bad_case}.ndjson")
  if(bad_case STREQUAL "malformed")
    file(WRITE "${bad_input}" "{broken}\n")
  elseif(bad_case STREQUAL "incompatible")
    file(WRITE "${bad_input}" "{\"protocolVersion\":2,\"kind\":\"batch\",\"jobCount\":1}\n")
  else()
    file(WRITE "${bad_input}" "{\"protocolVersion\":1,\"kind\":\"surprise\"}\n")
  endif()
  execute_process(
    COMMAND "${CLI}" batch --machine --protocol 1 INPUT_FILE "${bad_input}"
    RESULT_VARIABLE bad_batch_result OUTPUT_QUIET ERROR_QUIET
  )
  if(NOT bad_batch_result EQUAL 2)
    message(FATAL_ERROR "${bad_case} batch input returned ${bad_batch_result}, expected 2")
  endif()
endforeach()

set(stale_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-stale.edds")
execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1 --input "${png}" --output "${stale_result}"
    --expect-source-revision 0:0 --expect-output-revision missing
    --expect-metadata-revision missing
  RESULT_VARIABLE stale_exit OUTPUT_VARIABLE stale_output ERROR_QUIET
)
if(NOT stale_exit EQUAL 3 OR EXISTS "${stale_result}")
  message(FATAL_ERROR "stale source guard wrote output or returned ${stale_exit}: ${stale_output}")
endif()
string(JSON stale_code GET "${stale_output}" error code)
if(NOT stale_code STREQUAL "stale-source")
  message(FATAL_ERROR "stale source guard returned the wrong code: ${stale_output}")
endif()

set(unsupported_profile_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-unsupported-profile.edds")
execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1 --input "${png}"
    --output "${unsupported_profile_result}" --tiled-texture false
  RESULT_VARIABLE unsupported_profile_exit OUTPUT_VARIABLE unsupported_profile_output ERROR_QUIET
)
if(NOT unsupported_profile_exit EQUAL 4 OR EXISTS "${unsupported_profile_result}")
  message(FATAL_ERROR "unsupported profile wrote output or returned ${unsupported_profile_exit}")
endif()
string(JSON unsupported_profile_code GET "${unsupported_profile_output}" error code)
if(NOT unsupported_profile_code STREQUAL "unsupported-setting")
  message(FATAL_ERROR "unsupported profile returned the wrong refusal: ${unsupported_profile_output}")
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
  COMMAND "${CLI}" convert --machine --protocol 1
    --input "${png}" --output "${png_result}"
    --metadata "${png_metadata}" --resource-name Probe/black-box-png-result.edds
    --source-file black-box-source.png --guid 0123456789ABCDEF
    --target-format enfusion-dds --format-compress copy --compress-threshold 80
    --conversion none --conversion-quality 1 --swizzling none
    --generate-mips true --mipmap-function filter --mipmap-filter box
    --tiled-texture true
  RESULT_VARIABLE png_convert_result
  OUTPUT_VARIABLE png_convert_output
  ERROR_VARIABLE png_convert_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT png_convert_result EQUAL 0)
  message(FATAL_ERROR "PNG conversion failed with ${png_convert_result}: ${png_convert_error}")
endif()

file(SHA256 "${png_result}" original_png_hash)
file(SHA256 "${png_metadata}" original_metadata_hash)
foreach(stage IN ITEMS
    output-write output-flush metadata-write metadata-flush
    output-backup-rename metadata-backup-rename output-commit-rename metadata-commit-rename)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "EDDS_CONVERT_FAIL=${stage}"
      "${CLI}" convert --machine --protocol 1
      --input "${png}" --output "${png_result}"
      --metadata "${png_metadata}" --resource-name Probe/black-box-png-result.edds
      --source-file black-box-source.png --guid 0123456789ABCDEF
      --target-format enfusion-dds --format-compress best --compress-threshold 100
      --conversion none --conversion-quality 1 --swizzling none
      --generate-mips false --mipmap-function filter --mipmap-filter box --tiled-texture true
    RESULT_VARIABLE fault_exit OUTPUT_QUIET ERROR_QUIET
  )
  file(SHA256 "${png_result}" fault_png_hash)
  file(SHA256 "${png_metadata}" fault_metadata_hash)
  if(NOT fault_exit EQUAL 6 OR NOT fault_png_hash STREQUAL original_png_hash OR
      NOT fault_metadata_hash STREQUAL original_metadata_hash)
    message(FATAL_ERROR "fault ${stage} did not roll the prior pair back exactly")
  endif()
endforeach()

execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${png_result}" --metadata "${png_metadata}"
  RESULT_VARIABLE metadata_inspect_result
  OUTPUT_VARIABLE metadata_inspect_output
  ERROR_VARIABLE metadata_inspect_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT metadata_inspect_result EQUAL 0)
  message(FATAL_ERROR "metadata inspection failed with ${metadata_inspect_result}: ${metadata_inspect_error}")
endif()
string(JSON metadata_schema GET "${metadata_inspect_output}" metadata schemaVersion)
string(JSON metadata_guid GET "${metadata_inspect_output}" metadata identity guid)
string(JSON metadata_source GET "${metadata_inspect_output}" metadata identity sourceFile)
string(JSON metadata_compress GET "${metadata_inspect_output}" metadata recipe FormatCompress)
if(NOT metadata_schema EQUAL 1 OR NOT metadata_guid STREQUAL "0123456789ABCDEF" OR
   NOT metadata_source STREQUAL "black-box-source.png" OR NOT metadata_compress STREQUAL "Copy")
  message(FATAL_ERROR "unexpected structured metadata: ${metadata_inspect_output}")
endif()
set(unsupported_metadata "${CMAKE_CURRENT_BINARY_DIR}/black-box-unsupported.edds.meta")
file(READ "${png_metadata}" unsupported_metadata_text)
string(REPLACE "Conversion None" "Conversion DXTCompression"
  unsupported_metadata_text "${unsupported_metadata_text}")
file(WRITE "${unsupported_metadata}" "${unsupported_metadata_text}")
execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${png_result}"
    --metadata "${unsupported_metadata}" --identity-only
  RESULT_VARIABLE identity_result OUTPUT_VARIABLE identity_output ERROR_VARIABLE identity_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT identity_result EQUAL 0)
  message(FATAL_ERROR "unsupported identity inspection failed: ${identity_error}")
endif()
string(JSON identity_guid GET "${identity_output}" unsupportedMetadata identity guid)
if(NOT identity_guid STREQUAL "0123456789ABCDEF")
  message(FATAL_ERROR "unsupported metadata lost its identity: ${identity_output}")
endif()
string(JSON png_convert_kind GET "${png_convert_output}" kind)
string(JSON png_convert_format GET "${png_convert_output}" pixelFormat)
string(JSON png_convert_mips GET "${png_convert_output}" mipCount)
if(NOT png_convert_kind STREQUAL "convert" OR
   NOT png_convert_format STREQUAL "BGRA8" OR NOT png_convert_mips EQUAL 2)
  message(FATAL_ERROR "unexpected PNG conversion result: ${png_convert_output}")
endif()

execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1
    --input "${tga}" --output "${tga_result}"
    --target-format enfusion-dds --format-compress fastest --compress-threshold 80
    --conversion none --conversion-quality 1 --swizzling none
    --generate-mips false --mipmap-function filter --mipmap-filter box
    --tiled-texture true
  RESULT_VARIABLE tga_convert_result
  OUTPUT_VARIABLE tga_convert_output
  ERROR_VARIABLE tga_convert_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT tga_convert_result EQUAL 0)
  message(FATAL_ERROR "TGA conversion failed with ${tga_convert_result}: ${tga_convert_error}")
endif()
string(JSON tga_convert_format GET "${tga_convert_output}" pixelFormat)
string(JSON tga_convert_mips GET "${tga_convert_output}" mipCount)
if(NOT tga_convert_format STREQUAL "BGRX8" OR NOT tga_convert_mips EQUAL 1)
  message(FATAL_ERROR "unexpected TGA conversion result: ${tga_convert_output}")
endif()
file(SHA256 "${tga_result}" detached_hash)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "EDDS_CONVERT_FAIL=output-commit-rename"
    "${CLI}" convert --machine --protocol 1 --input "${tga}" --output "${tga_result}"
    --format-compress copy --generate-mips true
  RESULT_VARIABLE detached_fault_exit OUTPUT_QUIET ERROR_QUIET
)
file(SHA256 "${tga_result}" detached_fault_hash)
if(NOT detached_fault_exit EQUAL 6 OR NOT detached_fault_hash STREQUAL detached_hash OR
    EXISTS "${tga_result}.meta")
  message(FATAL_ERROR "detached rollback changed EDDS or authored metadata")
endif()

execute_process(
  COMMAND "${REFERENCE_READER}" "${copy}" "${lz4}" "${png_result}" "${tga_result}"
  RESULT_VARIABLE converted_reference_result
  ERROR_VARIABLE converted_reference_error
)
if(NOT converted_reference_result EQUAL 0)
  message(FATAL_ERROR "independent converted-output read failed: ${converted_reference_error}")
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
