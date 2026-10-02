# The font area end to end: the executable writes a font from a synthetic TrueType file, and the
# independent reference reads it back. Expected values come from the fixture and the spec, never
# from the generator.

set(work "${CMAKE_CURRENT_BINARY_DIR}/font-black-box")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}/out")
execute_process(COMMAND "${FONT_FIXTURE}" "${work}" RESULT_VARIABLE fixture_result ERROR_VARIABLE fixture_error)
if(NOT fixture_result EQUAL 0)
  message(FATAL_ERROR "font fixtures could not be written: ${fixture_error}")
endif()

set(font "${work}/out/SDF_FixtureSans32.fnt")
set(atlas "${work}/out/SDF_FixtureSans32.edds")
set(recipe "${font}.meta")

function(expect_refusal label expected_exit expected_code)
  execute_process(COMMAND "${CLI}" ${ARGN} RESULT_VARIABLE exit OUTPUT_VARIABLE output ERROR_QUIET)
  string(JSON code ERROR_VARIABLE json_error GET "${output}" error code)
  if(NOT exit EQUAL ${expected_exit} OR json_error OR NOT code STREQUAL "${expected_code}")
    message(FATAL_ERROR "${label}: expected exit ${expected_exit} ${expected_code}, got ${exit}: ${output}")
  endif()
endfunction()

function(expect_no_temps label)
  file(GLOB temps "${work}/out/*.enfusion-*.tmp")
  if(temps)
    message(FATAL_ERROR "${label} left sibling temporaries: ${temps}")
  endif()
endfunction()

function(hash_font prefix)
  foreach(member IN ITEMS fnt edds meta)
    set(path "${font}")
    if(member STREQUAL "edds")
      set(path "${atlas}")
    elseif(member STREQUAL "meta")
      set(path "${recipe}")
    endif()
    file(SHA256 "${path}" digest)
    set(${prefix}_${member} "${digest}" PARENT_SCOPE)
  endforeach()
endfunction()

# The executable names its font area.
execute_process(COMMAND "${CLI}" protocol --machine OUTPUT_VARIABLE protocol)
string(JSON font_commands ERROR_VARIABLE json_error LENGTH "${protocol}" areas font)
string(JSON first_command GET "${protocol}" areas font 0)
string(JSON second_command GET "${protocol}" areas font 1)
if(json_error OR NOT font_commands EQUAL 2 OR NOT first_command STREQUAL "generate" OR
    NOT second_command STREQUAL "inspect")
  message(FATAL_ERROR "the protocol does not name the font area: ${protocol}")
endif()

# A new font and a new recipe beside it.
execute_process(
  COMMAND "${CLI}" font generate --machine --protocol 1 --input "${work}/fixture-gpos.ttf"
    --output "${font}" --resource-name Fixture/GUI/Fonts/SDF_FixtureSans32.fnt --size 32
    --characters "${work}/fixture.charset.txt"
  RESULT_VARIABLE exit OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "font generation failed with ${exit}: ${output}${error}")
endif()
string(JSON kind GET "${output}" kind)
string(JSON guid GET "${output}" guid)
string(JSON glyphs GET "${output}" glyphCount)
string(JSON pairs GET "${output}" pairCount)
string(JSON missing_count LENGTH "${output}" missing)
string(JSON first_missing GET "${output}" missing 0)
string(JSON second_missing GET "${output}" missing 1)
string(JSON drawn_count LENGTH "${output}" drawn)
string(JSON drawn GET "${output}" drawn 0)
string(JSON family GET "${output}" source family)
string(JSON cell GET "${output}" cell)
# Eighteen mapped characters, the box drawn for U+25A1; x and Ж are not in the font.
if(NOT kind STREQUAL "font-generate" OR NOT guid MATCHES "^[0-9A-F]+$" OR NOT glyphs EQUAL 19 OR
    NOT pairs EQUAL 5 OR NOT missing_count EQUAL 2 OR NOT first_missing EQUAL 120 OR
    NOT second_missing EQUAL 1046 OR NOT drawn_count EQUAL 1 OR NOT drawn EQUAL 9633 OR
    NOT family STREQUAL "Fixture Sans")
  message(FATAL_ERROR "unexpected generation result: ${output}")
endif()
string(LENGTH "${guid}" guid_length)
if(NOT guid_length EQUAL 16)
  message(FATAL_ERROR "a new font needs a 16-digit GUID: ${guid}")
endif()
foreach(path IN ITEMS "${font}" "${atlas}" "${recipe}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR "generation did not write ${path}")
  endif()
endforeach()
if(EXISTS "${atlas}.meta")
  message(FATAL_ERROR "the generator registered its atlas; only the font has a recipe")
endif()
expect_no_temps("generation")
file(READ "${recipe}" recipe_text)
set(expected_recipe "MetaFileClass {
 Name \"{${guid}}Fixture/GUI/Fonts/SDF_FixtureSans32.fnt\"
 Configurations {
  FNTResourceClass PC {
   SourceFile \"../fixture-gpos.ttf\"
   Characters \"../fixture.charset.txt\"
   FontSize 32
  }
  FNTResourceClass XBOX_ONE : PC {
  }
  FNTResourceClass PS4 : PC {
  }
  FNTResourceClass LINUX : PC {
  }
 }
}
")
if(NOT recipe_text STREQUAL expected_recipe)
  message(FATAL_ERROR "the recipe is not canonical:\n${recipe_text}")
endif()
execute_process(
  COMMAND "${FONT_REFERENCE}" gpos 32 "${font}" "${atlas}" "${KERNING_GOLDEN}"
  RESULT_VARIABLE exit ERROR_VARIABLE report)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "the independent reference refused the font:\n${report}")
endif()

# Inspection reads back the header, the cell, the ranges and the counts.
execute_process(
  COMMAND "${CLI}" font inspect --machine --protocol 1 --input "${font}"
  RESULT_VARIABLE exit OUTPUT_VARIABLE inspection)
string(JSON inspected_kind GET "${inspection}" kind)
string(JSON inspected_name GET "${inspection}" name)
string(JSON inspected_size GET "${inspection}" size)
string(JSON inspected_type GET "${inspection}" type)
string(JSON inspected_r GET "${inspection}" r)
string(JSON inspected_cell GET "${inspection}" cell)
string(JSON inspected_glyphs GET "${inspection}" glyphCount)
string(JSON inspected_pairs GET "${inspection}" pairCount)
string(JSON inspected_ranges LENGTH "${inspection}" ranges)
string(JSON first_range GET "${inspection}" ranges 0 first)
string(JSON shared_range GET "${inspection}" ranges 12 count)
if(NOT exit EQUAL 0 OR NOT inspected_kind STREQUAL "font-inspect" OR
    NOT inspected_name STREQUAL "SDF_FixtureSans32" OR NOT inspected_size EQUAL 32 OR
    NOT inspected_type EQUAL 2 OR NOT inspected_r EQUAL 8 OR NOT inspected_cell EQUAL cell OR
    NOT inspected_glyphs EQUAL 19 OR NOT inspected_pairs EQUAL 5 OR NOT inspected_ranges EQUAL 18 OR
    NOT first_range EQUAL 32 OR NOT shared_range EQUAL 2)
  message(FATAL_ERROR "unexpected inspection: ${inspection}")
endif()
execute_process(
  COMMAND "${CLI}" font inspect --machine --protocol 1 --input "${work}/fixture-kern.ttf"
  RESULT_VARIABLE exit OUTPUT_VARIABLE source)
string(JSON source_family GET "${source}" family)
string(JSON source_style GET "${source}" style)
if(NOT exit EQUAL 0 OR NOT source_family STREQUAL "Fixture" OR NOT source_style STREQUAL "Medium")
  message(FATAL_ERROR "the source font does not name itself by its typographic names: ${source}")
endif()

# Regeneration by recipe: the GUID stays, whatever else the recipe gathered is dropped.
string(REPLACE "   FontSize 32\n" "   FontSize 24\n   Shadow 1\n" edited "${recipe_text}")
string(REPLACE " Configurations {\n" " Author \"someone\"\n ChangeDate -603265858\n // a comment\n Configurations {\n" edited "${edited}")
file(WRITE "${recipe}" "${edited}")
execute_process(
  COMMAND "${CLI}" font generate --machine --protocol 1 --meta "${recipe}"
  RESULT_VARIABLE exit OUTPUT_VARIABLE output ERROR_VARIABLE error)
string(JSON regenerated_guid GET "${output}" guid)
if(NOT exit EQUAL 0 OR NOT regenerated_guid STREQUAL guid)
  message(FATAL_ERROR "regeneration lost the GUID or failed: ${output}${error}")
endif()
string(REPLACE "FontSize 32" "FontSize 24" expected_recipe "${expected_recipe}")
file(READ "${recipe}" recipe_text)
if(NOT recipe_text STREQUAL expected_recipe)
  message(FATAL_ERROR "the regenerated recipe is not canonical:\n${recipe_text}")
endif()
execute_process(
  COMMAND "${FONT_REFERENCE}" gpos 24 "${font}" "${atlas}"
  RESULT_VARIABLE exit ERROR_VARIABLE report)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "the regenerated font does not hold up at 24:\n${report}")
endif()
expect_no_temps("regeneration")

# A recipe whose GUID cannot be read is refused before anything is written.
hash_font(before)
string(REPLACE "{${guid}}" "{${guid}X}" broken "${recipe_text}")
file(WRITE "${recipe}" "${broken}")
file(SHA256 "${recipe}" before_meta)
expect_refusal("unreadable GUID" 3 malformed-guid font generate --machine --protocol 1 --meta "${recipe}")
expect_refusal("unreadable GUID over an existing font" 3 malformed-guid
  font generate --machine --protocol 1 --input "${work}/fixture-gpos.ttf" --output "${font}"
  --resource-name Fixture/SDF_FixtureSans32.fnt)
hash_font(after)
if(NOT before_fnt STREQUAL after_fnt OR NOT before_edds STREQUAL after_edds OR NOT before_meta STREQUAL after_meta)
  message(FATAL_ERROR "a refused recipe still changed the font")
endif()
file(WRITE "${recipe}" "${recipe_text}")

# A new font over an existing one keeps that font's identity and nothing else of its recipe; the
# built-in set stands in for a character file nobody named. Another identity is refused.
execute_process(
  COMMAND "${CLI}" font generate --machine --protocol 1 --input "${work}/fixture-kern.ttf"
    --output "${font}" --resource-name Fixture/GUI/Fonts/SDF_FixtureSans32.fnt
  RESULT_VARIABLE exit OUTPUT_VARIABLE output ERROR_VARIABLE error)
string(JSON replaced_guid GET "${output}" guid)
string(JSON replaced_glyphs GET "${output}" glyphCount)
if(NOT exit EQUAL 0 OR NOT replaced_guid STREQUAL guid OR NOT replaced_glyphs EQUAL 16)
  message(FATAL_ERROR "replacing a font lost its GUID or its built-in set: ${output}${error}")
endif()
file(READ "${recipe}" replaced_recipe)
if(replaced_recipe MATCHES "Characters" OR NOT replaced_recipe MATCHES "SourceFile \"../fixture-kern.ttf\"" OR
    NOT replaced_recipe MATCHES "FontSize 32")
  message(FATAL_ERROR "the replacing font kept the previous recipe:\n${replaced_recipe}")
endif()
expect_refusal("another GUID" 3 metadata-guid-mismatch
  font generate --machine --protocol 1 --input "${work}/fixture-gpos.ttf" --output "${font}"
  --resource-name Fixture/SDF_FixtureSans32.fnt --guid 0123456789ABCDEF)

# Without GPOS the kern table carries the pairs, and without a cap height H stands in.
execute_process(
  COMMAND "${CLI}" font generate --machine --protocol 1 --input "${work}/fixture-kern.ttf"
    --output "${work}/out/SDF_FixtureMedium32.fnt" --resource-name Fixture/SDF_FixtureMedium32.fnt
    --characters "${work}/fixture.charset.txt" --guid 0123456789abcdef
  RESULT_VARIABLE exit OUTPUT_VARIABLE output ERROR_VARIABLE error)
string(JSON chosen_guid GET "${output}" guid)
if(NOT exit EQUAL 0 OR NOT chosen_guid STREQUAL "0123456789abcdef")
  message(FATAL_ERROR "a caller's GUID was not kept as given: ${output}${error}")
endif()
execute_process(
  COMMAND "${FONT_REFERENCE}" kern 32 "${work}/out/SDF_FixtureMedium32.fnt" "${work}/out/SDF_FixtureMedium32.edds"
  RESULT_VARIABLE exit ERROR_VARIABLE report)
if(NOT exit EQUAL 0)
  message(FATAL_ERROR "the kern-table font does not hold up:\n${report}")
endif()

# Fonts the engine cannot draw are refused with their reason, and nothing is written.
set(refused "${work}/out/SDF_Refused.fnt")
expect_refusal("CFF outlines" 4 unsupported-outline-format
  font generate --machine --protocol 1 --input "${work}/fixture-cff.otf" --output "${refused}" --resource-name R.fnt)
expect_refusal("a variable font" 4 variable-font-unsupported
  font generate --machine --protocol 1 --input "${work}/fixture-variable.ttf" --output "${refused}" --resource-name R.fnt)
foreach(size IN ITEMS 7 41)
  expect_refusal("size ${size}" 4 font-size-out-of-range
    font generate --machine --protocol 1 --input "${work}/fixture-gpos.ttf" --output "${refused}"
    --resource-name R.fnt --size ${size})
endforeach()
expect_refusal("an atlas over 4096" 4 atlas-too-large
  font generate --machine --protocol 1 --input "${work}/fixture-crowded.ttf" --output "${refused}"
  --resource-name R.fnt --size 40 --characters "${work}/crowded.charset.txt")
foreach(member IN ITEMS "${refused}" "${work}/out/SDF_Refused.edds" "${refused}.meta")
  if(EXISTS "${member}")
    message(FATAL_ERROR "a refused font wrote ${member}")
  endif()
endforeach()

# Every failure on the way to publication leaves the previous three files as they were. The font
# that fails to arrive differs from the one in place in all three files, so a file that was
# replaced and not put back shows.
hash_font(before)
foreach(stage IN ITEMS font-atlas-write font-fnt-write font-meta-write font-flush
    font-atlas-backup font-fnt-backup font-meta-backup font-atlas-commit font-fnt-commit font-meta-commit)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "EDDS_CONVERT_FAIL=${stage}"
      "${CLI}" font generate --machine --protocol 1 --input "${work}/fixture-gpos.ttf" --output "${font}"
      --resource-name Fixture/GUI/Fonts/SDF_FixtureSans32.fnt --size 20
      --characters "${work}/fixture.charset.txt"
    RESULT_VARIABLE exit OUTPUT_QUIET ERROR_QUIET)
  hash_font(after)
  if(NOT exit EQUAL 6 OR NOT before_fnt STREQUAL after_fnt OR NOT before_edds STREQUAL after_edds OR
      NOT before_meta STREQUAL after_meta)
    message(FATAL_ERROR "a failure at ${stage} (exit ${exit}) did not leave the previous font whole")
  endif()
  expect_no_temps("a failure at ${stage}")
endforeach()

# Cancellation before publication is a cancellation, not a half-written font.
file(WRITE "${work}/cancel" "")
expect_refusal("cancellation" 5 cancelled
  font generate --machine --protocol 1 --meta "${recipe}" --cancel-file "${work}/cancel")
hash_font(after)
if(NOT before_fnt STREQUAL after_fnt OR NOT before_edds STREQUAL after_edds OR NOT before_meta STREQUAL after_meta)
  message(FATAL_ERROR "a cancelled generation changed the font")
endif()
expect_no_temps("cancellation")

# Invocations that do not say which font, or say it twice, are refused as invocations.
expect_refusal("no resource name" 2 invalid-options
  font generate --machine --protocol 1 --input "${work}/fixture-gpos.ttf" --output "${refused}")
expect_refusal("a recipe and a source at once" 2 invalid-options
  font generate --machine --protocol 1 --meta "${recipe}" --input "${work}/fixture-gpos.ttf")
expect_refusal("an output that is not a font" 2 invalid-options
  font generate --machine --protocol 1 --input "${work}/fixture-gpos.ttf" --output "${work}/out/x.edds"
  --resource-name R.fnt)
expect_refusal("a recipe that is not a font recipe" 2 invalid-options
  font generate --machine --protocol 1 --meta "${atlas}")
expect_refusal("another protocol" 2 invalid-options
  font inspect --machine --protocol 2 --input "${font}")
expect_refusal("an unknown font command" 2 invalid-command font convert --machine --protocol 1)
