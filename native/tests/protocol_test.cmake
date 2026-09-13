set(copy "${CMAKE_CURRENT_BINARY_DIR}/black-box-copy.edds")
set(lz4 "${CMAKE_CURRENT_BINARY_DIR}/black-box-lz4.edds")
set(dxt1 "${CMAKE_CURRENT_BINARY_DIR}/black-box-dxt1.edds")
set(odd_fourcc "${CMAKE_CURRENT_BINARY_DIR}/black-box-odd-fourcc.edds")
set(overflow "${CMAKE_CURRENT_BINARY_DIR}/black-box-overflow.edds")
set(png "${CMAKE_CURRENT_BINARY_DIR}/black-box-source.png")
set(tga "${CMAKE_CURRENT_BINARY_DIR}/black-box-source.tga")
set(jpg_source "${CMAKE_CURRENT_BINARY_DIR}/black-box-source.jpg")
set(tiff_source "${CMAKE_CURRENT_BINARY_DIR}/black-box-source.tiff")
set(png_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-png-result.edds")
set(tga_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-tga-result.edds")
set(jpg_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-jpg-result.edds")
set(tiff_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-tiff-result.edds")
set(png_metadata "${png_result}.meta")
set(jpg_metadata "${jpg_result}.meta")

set(gpu_source "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-source.tga")
set(gpu_flat "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-flat.tga")

execute_process(
  COMMAND "${FIXTURE}" "${copy}" "${lz4}" "${dxt1}" "${odd_fourcc}" "${overflow}"
    "${png}" "${tga}" "${jpg_source}" "${tiff_source}" "${gpu_source}" "${gpu_flat}"
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

# A row has to move while its own image converts, not only when the whole file is finished.
string(REGEX MATCHALL "\"kind\":\"progress\",\"id\":\"png\",\"progress\":[0-9.]+"
  png_progress "${batch_output}")
set(png_progress_between 0)
foreach(reported IN LISTS png_progress)
  if(NOT reported MATCHES "\"progress\":(0\\.000|1\\.000)$")
    math(EXPR png_progress_between "${png_progress_between} + 1")
  endif()
endforeach()
if(png_progress_between LESS 1)
  message(FATAL_ERROR "per-file progress never reported a step between 0 and 1: ${png_progress}")
endif()

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

# A destination reached through a dot segment is the same destination, so it is the same group.
set(walked_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-walked.ndjson")
set(walked_output "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-walked.edds")
set(walked_alias "${CMAKE_CURRENT_BINARY_DIR}/sub/../black-box-batch-walked.edds")
set(walked_independent "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-walked-other.edds")
file(REMOVE "${walked_output}" "${walked_independent}")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/sub")
file(WRITE "${walked_input}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":3}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"plain\",\"input\":\"${png}\",\"output\":\"${walked_output}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"walked\",\"input\":\"${tga}\",\"output\":\"${walked_alias}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"independent\",\"input\":\"${png}\",\"output\":\"${walked_independent}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1 INPUT_FILE "${walked_input}"
  RESULT_VARIABLE walked_result OUTPUT_VARIABLE walked_stdout ERROR_QUIET
)
if(NOT walked_result EQUAL 0 OR EXISTS "${walked_output}" OR
    NOT EXISTS "${walked_independent}" OR
    NOT walked_stdout MATCHES "\"converted\":1" OR NOT walked_stdout MATCHES "\"failed\":2")
  message(FATAL_ERROR "a walked destination was not grouped with the plain one: ${walked_stdout}")
endif()
foreach(expected IN ITEMS
    "\"id\":\"plain\",\"status\":\"Failed\""
    "\"id\":\"walked\",\"status\":\"Failed\""
    "\"id\":\"independent\",\"status\":\"Converted\"")
  if(NOT walked_stdout MATCHES "${expected}")
    message(FATAL_ERROR "walked collision omitted per-item status ${expected}: ${walked_stdout}")
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
# Pinned to one worker so "after the first commit" names one job, not whichever finished first.
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env "EDDS_CONVERT_FAIL=batch-cancel-after-first-commit"
    "EDDS_CONVERT_WORKERS=1"
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
string(REPLACE "Conversion None" "Conversion HDRCompression"
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

# JPG and TIFF reach conversion, metadata and preview through the same contract as PNG and TGA.
execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1
    --input "${jpg_source}" --output "${jpg_result}"
    --metadata "${jpg_metadata}" --resource-name Probe/black-box-jpg-result.edds
    --source-file black-box-source.jpg --guid 00112233445566AA
    --target-format enfusion-dds --format-compress copy --compress-threshold 80
    --conversion none --conversion-quality 1 --swizzling none
    --generate-mips true --mipmap-function filter --mipmap-filter box --tiled-texture true
  RESULT_VARIABLE jpg_convert_result
  OUTPUT_VARIABLE jpg_convert_output
  ERROR_VARIABLE jpg_convert_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT jpg_convert_result EQUAL 0)
  message(FATAL_ERROR "JPG conversion failed with ${jpg_convert_result}: ${jpg_convert_error}")
endif()
string(JSON jpg_convert_format GET "${jpg_convert_output}" pixelFormat)
string(JSON jpg_convert_mips GET "${jpg_convert_output}" mipCount)
if(NOT jpg_convert_format STREQUAL "BGRX8" OR NOT jpg_convert_mips EQUAL 5)
  message(FATAL_ERROR "unexpected JPG conversion result: ${jpg_convert_output}")
endif()
file(READ "${jpg_metadata}" jpg_metadata_text)
if(NOT jpg_metadata_text MATCHES "JPGResourceClass PC")
  message(FATAL_ERROR "JPG metadata did not name its Workbench resource class: ${jpg_metadata_text}")
endif()
execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${jpg_result}" --metadata "${jpg_metadata}"
  RESULT_VARIABLE jpg_inspect_result OUTPUT_VARIABLE jpg_inspect_output ERROR_QUIET
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(JSON jpg_inspect_format GET "${jpg_inspect_output}" metadata identity sourceFormat)
if(NOT jpg_inspect_result EQUAL 0 OR NOT jpg_inspect_format STREQUAL "jpg")
  message(FATAL_ERROR "JPG metadata did not round-trip its source format: ${jpg_inspect_output}")
endif()

execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1
    --input "${tiff_source}" --output "${tiff_result}"
    --target-format enfusion-dds --format-compress fastest --compress-threshold 80
    --conversion none --conversion-quality 1 --swizzling none
    --generate-mips false --mipmap-function filter --mipmap-filter box --tiled-texture true
  RESULT_VARIABLE tiff_convert_result
  OUTPUT_VARIABLE tiff_convert_output
  ERROR_VARIABLE tiff_convert_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT tiff_convert_result EQUAL 0)
  message(FATAL_ERROR "TIFF conversion failed with ${tiff_convert_result}: ${tiff_convert_error}")
endif()
string(JSON tiff_convert_format GET "${tiff_convert_output}" pixelFormat)
string(JSON tiff_convert_mips GET "${tiff_convert_output}" mipCount)
if(NOT tiff_convert_format STREQUAL "BGRX8" OR NOT tiff_convert_mips EQUAL 1)
  message(FATAL_ERROR "unexpected TIFF conversion result: ${tiff_convert_output}")
endif()

# An alias extension is not a registered resource class, so it is refused before anything is written.
foreach(alias IN ITEMS jpeg tif)
  set(alias_source "${CMAKE_CURRENT_BINARY_DIR}/black-box-alias.${alias}")
  set(alias_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-alias-${alias}.edds")
  file(REMOVE "${alias_result}")
  if(alias STREQUAL "jpeg")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${jpg_source}" "${alias_source}")
  else()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${tiff_source}" "${alias_source}")
  endif()
  execute_process(
    COMMAND "${CLI}" convert --machine --protocol 1 --input "${alias_source}" --output "${alias_result}"
    RESULT_VARIABLE alias_exit OUTPUT_VARIABLE alias_output ERROR_QUIET
  )
  if(NOT alias_exit EQUAL 4 OR EXISTS "${alias_result}")
    message(FATAL_ERROR ".${alias} was not refused before writing: ${alias_exit} ${alias_output}")
  endif()
  string(JSON alias_code GET "${alias_output}" error code)
  if(NOT alias_code STREQUAL "unsupported-source-extension")
    message(FATAL_ERROR ".${alias} returned the wrong refusal: ${alias_output}")
  endif()
endforeach()

# A damaged source is one item's failure; the pair already on disk is not touched by it.
set(damaged "${CMAKE_CURRENT_BINARY_DIR}/black-box-damaged.jpg")
file(WRITE "${damaged}" "not a JPEG at all, only bytes that end in .jpg")
file(SHA256 "${jpg_result}" intact_jpg_hash)
file(SHA256 "${jpg_metadata}" intact_jpg_metadata_hash)
execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1 --input "${damaged}"
    --output "${jpg_result}" --metadata "${jpg_metadata}"
    --resource-name Probe/black-box-jpg-result.edds --source-file black-box-damaged.jpg
    --guid 00112233445566AA
  RESULT_VARIABLE damaged_exit OUTPUT_VARIABLE damaged_output ERROR_QUIET
)
file(SHA256 "${jpg_result}" after_jpg_hash)
file(SHA256 "${jpg_metadata}" after_jpg_metadata_hash)
if(NOT damaged_exit EQUAL 3 OR NOT after_jpg_hash STREQUAL intact_jpg_hash OR
    NOT after_jpg_metadata_hash STREQUAL intact_jpg_metadata_hash)
  message(FATAL_ERROR "a refused JPG source changed the existing pair: ${damaged_exit} ${damaged_output}")
endif()

# A mixed batch keeps a bad TIFF to itself while the JPG beside it converts.
set(source_batch_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-sources.ndjson")
set(source_batch_jpg "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-source-jpg.edds")
set(source_batch_tiff "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-source-tiff.edds")
set(source_batch_bad "${CMAKE_CURRENT_BINARY_DIR}/black-box-batch-source-bad.edds")
file(REMOVE "${source_batch_jpg}" "${source_batch_tiff}" "${source_batch_bad}")
file(WRITE "${source_batch_input}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":3}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"jpg\",\"input\":\"${jpg_source}\",\"output\":\"${source_batch_jpg}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"broken\",\"input\":\"${damaged}\",\"output\":\"${source_batch_bad}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"tiff\",\"input\":\"${tiff_source}\",\"output\":\"${source_batch_tiff}\",\"metadata\":null,\"identity\":null,\"profile\":${batch_profile},\"expected\":null}\n"
  "{\"protocolVersion\":1,\"kind\":\"end\"}\n")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1
  INPUT_FILE "${source_batch_input}"
  RESULT_VARIABLE source_batch_result OUTPUT_VARIABLE source_batch_output ERROR_QUIET
)
if(NOT source_batch_result EQUAL 0 OR NOT EXISTS "${source_batch_jpg}" OR
    NOT EXISTS "${source_batch_tiff}" OR EXISTS "${source_batch_bad}" OR
    NOT source_batch_output MATCHES "\"converted\":2" OR
    NOT source_batch_output MATCHES "\"failed\":1")
  message(FATAL_ERROR "a batch of JPG and TIFF did not isolate its one bad item: ${source_batch_output}")
endif()

execute_process(
  COMMAND "${REFERENCE_READER}" "${copy}" "${lz4}" "${png_result}" "${tga_result}"
    "${jpg_result}" "${tiff_result}"
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
  RESULT_VARIABLE dxt1_inspect_result
  OUTPUT_VARIABLE dxt1_inspect_output
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT dxt1_inspect_result EQUAL 0)
  message(FATAL_ERROR "a stored DXT1 texture must remain inspectable")
endif()
string(JSON dxt1_preview_supported GET "${dxt1_inspect_output}" previewSupported)
string(JSON dxt1_channels GET "${dxt1_inspect_output}" channels)
if(NOT dxt1_preview_supported OR NOT dxt1_channels STREQUAL "RGB")
  message(FATAL_ERROR "DXT1 was not reported as decodable RGB: ${dxt1_inspect_output}")
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
string(JSON odd_fourcc_preview GET "${odd_fourcc_output}" previewSupported)
if(odd_fourcc_preview)
  message(FATAL_ERROR "an unknown FourCC was represented as previewable: ${odd_fourcc_output}")
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
  COMMAND "${CLI}" preview --machine --protocol 1 --mip 0 --input "${odd_fourcc}"
  RESULT_VARIABLE unsupported_preview_result
  OUTPUT_VARIABLE unsupported_preview_output
  ERROR_VARIABLE unsupported_preview_error
)
if(NOT unsupported_preview_result EQUAL 4)
  message(FATAL_ERROR "unsupported preview exit was ${unsupported_preview_result}, expected 4")
endif()

# The same block, decoded: a DXT1 texture is sixteen opaque black pixels, not a refusal.
execute_process(
  COMMAND "${CLI}" preview --machine --protocol 1 --mip 0 --input "${dxt1}"
  RESULT_VARIABLE dxt1_preview_result
  OUTPUT_VARIABLE dxt1_preview_output
  ERROR_VARIABLE dxt1_preview_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT dxt1_preview_result EQUAL 0)
  message(FATAL_ERROR "DXT1 preview failed with ${dxt1_preview_result}: ${dxt1_preview_error}")
endif()
string(JSON dxt1_preview_length GET "${dxt1_preview_output}" byteLength)
string(JSON dxt1_preview_pixels GET "${dxt1_preview_output}" pixelsBase64)
if(NOT dxt1_preview_length EQUAL 64 OR
   NOT dxt1_preview_pixels STREQUAL "AAAA/wAAAP8AAAD/AAAA/wAAAP8AAAD/AAAA/wAAAP8AAAD/AAAA/wAAAP8AAAD/AAAA/wAAAP8AAAD/AAAA/w==")
  message(FATAL_ERROR "DXT1 pixels were not independently expected: ${dxt1_preview_output}")
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

# ---------------------------------------------------------------------------------------------
# Every GPU conversion, end to end through the CLI, then read back by the independent reader: the
# header it wrote, the blocks it padded, the channels it really carries and how far the lossy ones
# drifted from the source. The reader shares no code with the converter.
# ---------------------------------------------------------------------------------------------

foreach(row IN ITEMS
    "none|BGRA8|RGBA|0"
    "dxt-compression|DXT5|RGBA|20"
    "red|R8|R|0"
    "red-hq-compression|BC4|R|10"
    "red-green|RG8|RG|0"
    "red-green-hq-compression|BC5|RG|10"
    "color-hq-compression|BC7|RGBA|20")
  string(REPLACE "|" ";" fields "${row}")
  list(GET fields 0 wire)
  list(GET fields 1 runtime)
  list(GET fields 2 channels)
  list(GET fields 3 bound)
  set(gpu_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-${wire}.edds")
  file(REMOVE "${gpu_result}")
  execute_process(
    COMMAND "${CLI}" convert --machine --protocol 1
      --input "${gpu_source}" --output "${gpu_result}"
      --target-format enfusion-dds --format-compress fastest --compress-threshold 80
      --conversion "${wire}" --swizzling none
      --generate-mips true --mipmap-function filter --mipmap-filter box --tiled-texture true
    RESULT_VARIABLE gpu_convert_result
    OUTPUT_VARIABLE gpu_convert_output
    ERROR_VARIABLE gpu_convert_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT gpu_convert_result EQUAL 0)
    message(FATAL_ERROR "${wire} conversion failed with ${gpu_convert_result}: ${gpu_convert_error}")
  endif()
  string(JSON gpu_convert_format GET "${gpu_convert_output}" pixelFormat)
  if(NOT gpu_convert_format STREQUAL "${runtime}")
    message(FATAL_ERROR "${wire} did not report ${runtime}: ${gpu_convert_output}")
  endif()

  execute_process(
    COMMAND "${CLI}" inspect --machine --protocol 1 --input "${gpu_result}"
    RESULT_VARIABLE gpu_inspect_result
    OUTPUT_VARIABLE gpu_inspect_output
    ERROR_VARIABLE gpu_inspect_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT gpu_inspect_result EQUAL 0)
    message(FATAL_ERROR "${wire} inspection failed with ${gpu_inspect_result}: ${gpu_inspect_error}")
  endif()
  string(JSON gpu_inspect_format GET "${gpu_inspect_output}" pixelFormat)
  string(JSON gpu_inspect_channels GET "${gpu_inspect_output}" channels)
  string(JSON gpu_inspect_preview GET "${gpu_inspect_output}" previewSupported)
  string(JSON gpu_inspect_mips GET "${gpu_inspect_output}" mipCount)
  if(NOT gpu_inspect_format STREQUAL "${runtime}" OR
     NOT gpu_inspect_channels STREQUAL "${channels}" OR
     NOT gpu_inspect_preview OR NOT gpu_inspect_mips EQUAL 4)
    message(FATAL_ERROR "${wire} was not inspected as ${runtime}/${channels}: ${gpu_inspect_output}")
  endif()

  # The smallest mip of the chain is one pixel inside one block, and it still previews.
  execute_process(
    COMMAND "${CLI}" preview --machine --protocol 1 --mip 3 --input "${gpu_result}"
    RESULT_VARIABLE gpu_preview_result
    OUTPUT_VARIABLE gpu_preview_output
    ERROR_VARIABLE gpu_preview_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  if(NOT gpu_preview_result EQUAL 0)
    message(FATAL_ERROR "${wire} smallest-mip preview failed: ${gpu_preview_error}")
  endif()
  string(JSON gpu_preview_length GET "${gpu_preview_output}" byteLength)
  if(NOT gpu_preview_length EQUAL 4)
    message(FATAL_ERROR "${wire} smallest mip is not one pixel: ${gpu_preview_output}")
  endif()

  execute_process(
    COMMAND "${REFERENCE_READER}" --gpu "${gpu_source}" "${gpu_result}" "${runtime}" "${bound}"
    RESULT_VARIABLE gpu_reference_result
    ERROR_VARIABLE gpu_reference_error
  )
  if(NOT gpu_reference_result EQUAL 0)
    message(FATAL_ERROR "${wire} failed independent verification: ${gpu_reference_error}")
  endif()
endforeach()

# A batch is the same job contract, so a GPU profile goes through it with no encoder path of its own.
set(gpu_batch_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-batch.ndjson")
set(gpu_batch_output "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-batch.edds")
set(gpu_batch_profile [=[{"TargetFormat":"EnfusionDDS","FormatCompress":"Fastest","CompressTreshold":80,"Conversion":"ColorHQCompression","ConversionQuality":0.5,"Swizzling":"None","GenerateMips":true,"MipMapFunction":"Filter","MipMapFilter":"Box","TiledTexture":true}]=])
file(REMOVE "${gpu_batch_output}")
file(WRITE "${gpu_batch_input}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":1}
"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"bc7\",\"input\":\"${gpu_source}\",\"output\":\"${gpu_batch_output}\",\"metadata\":null,\"identity\":null,\"profile\":${gpu_batch_profile},\"expected\":null}
"
  "{\"protocolVersion\":1,\"kind\":\"end\"}
")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1
  INPUT_FILE "${gpu_batch_input}"
  RESULT_VARIABLE gpu_batch_result OUTPUT_VARIABLE gpu_batch_stdout ERROR_VARIABLE gpu_batch_error
)
if(NOT gpu_batch_result EQUAL 0 OR NOT gpu_batch_stdout MATCHES "\"converted\":1")
  message(FATAL_ERROR "a GPU profile did not survive the batch contract: ${gpu_batch_error}
${gpu_batch_stdout}")
endif()
execute_process(
  COMMAND "${REFERENCE_READER}" --gpu "${gpu_source}" "${gpu_batch_output}" "BC7" "20"
  RESULT_VARIABLE gpu_batch_reference ERROR_VARIABLE gpu_batch_reference_error
)
if(NOT gpu_batch_reference EQUAL 0)
  message(FATAL_ERROR "the batch result failed independent verification: ${gpu_batch_reference_error}")
endif()

# A quality no conversion can use is refused by the batch parser, before any job starts.
set(dead_batch_input "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-dead-quality.ndjson")
set(dead_batch_profile [=[{"TargetFormat":"EnfusionDDS","FormatCompress":"Fastest","CompressTreshold":80,"Conversion":"None","ConversionQuality":0.5,"Swizzling":"None","GenerateMips":true,"MipMapFunction":"Filter","MipMapFilter":"Box","TiledTexture":true}]=])
file(WRITE "${dead_batch_input}"
  "{\"protocolVersion\":1,\"kind\":\"batch\",\"jobCount\":1}
"
  "{\"protocolVersion\":1,\"kind\":\"job\",\"id\":\"dead\",\"input\":\"${gpu_source}\",\"output\":\"${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-dead.edds\",\"metadata\":null,\"identity\":null,\"profile\":${dead_batch_profile},\"expected\":null}
"
  "{\"protocolVersion\":1,\"kind\":\"end\"}
")
execute_process(
  COMMAND "${CLI}" batch --machine --protocol 1
  INPUT_FILE "${dead_batch_input}"
  RESULT_VARIABLE dead_batch_result OUTPUT_VARIABLE dead_batch_stdout ERROR_QUIET
)
if(dead_batch_result EQUAL 0 OR EXISTS "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-dead.edds")
  message(FATAL_ERROR "the batch accepted a quality nothing would read: ${dead_batch_stdout}")
endif()

# The other DXT branch, on a source that declares no alpha at all: BC1, verified the same way.
set(dxt1_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-dxt1-branch.edds")
file(REMOVE "${dxt1_result}")
execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1
    --input "${tga}" --output "${dxt1_result}" --conversion dxt-compression
  RESULT_VARIABLE dxt1_branch_result
  OUTPUT_VARIABLE dxt1_branch_output
  ERROR_VARIABLE dxt1_branch_error
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT dxt1_branch_result EQUAL 0)
  message(FATAL_ERROR "the BC1 branch failed with ${dxt1_branch_result}: ${dxt1_branch_error}")
endif()
string(JSON dxt1_branch_format GET "${dxt1_branch_output}" pixelFormat)
if(NOT dxt1_branch_format STREQUAL "DXT1")
  message(FATAL_ERROR "a source without alpha did not take the BC1 branch: ${dxt1_branch_output}")
endif()
execute_process(
  COMMAND "${REFERENCE_READER}" --gpu "${tga}" "${dxt1_result}" "DXT1" "20"
  RESULT_VARIABLE dxt1_reference_result ERROR_VARIABLE dxt1_reference_error
)
if(NOT dxt1_reference_result EQUAL 0)
  message(FATAL_ERROR "the BC1 branch failed independent verification: ${dxt1_reference_error}")
endif()

# HDRCompression is recognized and refused, never replaced with the nearest LDR format.
execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1
    --input "${gpu_source}" --output "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-hdr.edds"
    --conversion hdr-compression
  RESULT_VARIABLE hdr_result OUTPUT_VARIABLE hdr_output ERROR_QUIET
)
if(NOT hdr_result EQUAL 4 OR EXISTS "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-hdr.edds")
  message(FATAL_ERROR "HDRCompression was not refused as unsupported: ${hdr_output}")
endif()
string(JSON hdr_code GET "${hdr_output}" error code)
if(NOT hdr_code STREQUAL "unsupported-setting")
  message(FATAL_ERROR "HDRCompression refusal changed shape: ${hdr_output}")
endif()

# Quality is a fraction that round-trips exactly, and only where a compressed encoder reads it.
set(quality_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-quality.edds")
set(quality_metadata "${quality_result}.meta")
file(REMOVE "${quality_result}" "${quality_metadata}")
execute_process(
  COMMAND "${CLI}" convert --machine --protocol 1
    --input "${gpu_source}" --output "${quality_result}"
    --conversion color-hq-compression --conversion-quality 0.403
    --metadata "${quality_metadata}" --resource-name "Probe/gpu.edds"
    --source-file "black-box-gpu-source.tga" --guid "0123456789ABCDEF"
  RESULT_VARIABLE quality_convert_result ERROR_VARIABLE quality_convert_error
  OUTPUT_QUIET
)
if(NOT quality_convert_result EQUAL 0)
  message(FATAL_ERROR "fractional quality was refused: ${quality_convert_error}")
endif()
file(READ "${quality_metadata}" quality_metadata_text)
if(NOT quality_metadata_text MATCHES "Conversion ColorHQCompression" OR
   NOT quality_metadata_text MATCHES "ConversionQuality 0\\.403")
  message(FATAL_ERROR "the recipe did not record its own conversion: ${quality_metadata_text}")
endif()
execute_process(
  COMMAND "${CLI}" inspect --machine --protocol 1 --input "${quality_result}"
    --metadata "${quality_metadata}"
  RESULT_VARIABLE quality_inspect_result OUTPUT_VARIABLE quality_inspect_output
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(JSON quality_conversion GET "${quality_inspect_output}" metadata recipe Conversion)
string(JSON quality_value GET "${quality_inspect_output}" metadata recipe ConversionQuality)
if(NOT quality_inspect_result EQUAL 0 OR
   NOT quality_conversion STREQUAL "ColorHQCompression" OR NOT quality_value EQUAL 0.403)
  message(FATAL_ERROR "the recipe did not round-trip its conversion: ${quality_inspect_output}")
endif()

foreach(refused IN ITEMS "none;0.5" "red;0.5" "red-green;0.5" "color-hq-compression;1.5"
    "color-hq-compression;0.4031")
  list(GET refused 0 refused_conversion)
  list(GET refused 1 refused_quality)
  execute_process(
    COMMAND "${CLI}" convert --machine --protocol 1
      --input "${gpu_source}" --output "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-refused.edds"
      --conversion "${refused_conversion}" --conversion-quality "${refused_quality}"
    RESULT_VARIABLE refused_result OUTPUT_VARIABLE refused_output ERROR_QUIET
  )
  if(refused_result EQUAL 0 OR
     EXISTS "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-refused.edds")
    message(FATAL_ERROR
      "${refused_conversion} accepted quality ${refused_quality}: ${refused_output}")
  endif()
endforeach()

# The container is not the conversion: COPY and LZ4 owe the same decoded pixels.
set(copy_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-copy.edds")
set(lz4_result "${CMAKE_CURRENT_BINARY_DIR}/black-box-gpu-lz4.edds")
foreach(pair IN ITEMS "copy;80;${copy_result}" "best;100;${lz4_result}")
  list(GET pair 0 compress)
  list(GET pair 1 threshold)
  list(GET pair 2 destination)
  execute_process(
    COMMAND "${CLI}" convert --machine --protocol 1
      --input "${gpu_flat}" --output "${destination}"
      --format-compress "${compress}" --compress-threshold "${threshold}"
      --conversion color-hq-compression
    RESULT_VARIABLE container_result ERROR_VARIABLE container_error OUTPUT_QUIET
  )
  if(NOT container_result EQUAL 0)
    message(FATAL_ERROR "${compress} conversion failed: ${container_error}")
  endif()
endforeach()
execute_process(COMMAND "${CLI}" preview --machine --protocol 1 --mip 0 --input "${copy_result}"
  OUTPUT_VARIABLE copy_pixels OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${CLI}" preview --machine --protocol 1 --mip 0 --input "${lz4_result}"
  OUTPUT_VARIABLE lz4_pixels_output OUTPUT_STRIP_TRAILING_WHITESPACE)
string(JSON copy_pixel_data GET "${copy_pixels}" pixelsBase64)
string(JSON lz4_pixel_data GET "${lz4_pixels_output}" pixelsBase64)
execute_process(COMMAND "${CLI}" inspect --machine --protocol 1 --input "${lz4_result}"
  OUTPUT_VARIABLE lz4_inspect OUTPUT_STRIP_TRAILING_WHITESPACE)
string(JSON lz4_container GET "${lz4_inspect}" mips 0 container)
if(NOT copy_pixel_data STREQUAL "${lz4_pixel_data}" OR NOT lz4_container STREQUAL "LZ4")
  message(FATAL_ERROR "the container changed the pixels or was never used: ${lz4_inspect}")
endif()
