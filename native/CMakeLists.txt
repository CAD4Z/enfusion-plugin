cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()

project(enfusion VERSION 0.2.0 LANGUAGES C)

option(ENFUSION_BUILD_TESTS "Build the native test suite" ON)
option(ENFUSION_BUILD_FUZZER "Build the libFuzzer targets (Clang only)" OFF)

function(enfusion_configure_target target)
  set_target_properties("${target}" PROPERTIES
    C_STANDARD 17
    C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO
  )
  if(MSVC)
    set_property(TARGET "${target}" PROPERTY
      MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    target_compile_options("${target}" PRIVATE /W4 /WX /sdl)
  else()
    target_compile_options("${target}" PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# Textures: the EDDS container, the source decoders, the GPU encoders, the mip pipeline, the
# Workbench metadata and the batch pool. include/edds is the API; src/edds holds its own headers.
add_library(edds_core STATIC
  src/edds/edds.c
  src/edds/image.c
  src/edds/memory.c
  src/edds/sources.c
  src/edds/conversions.c
  src/edds/swizzling.c
  src/edds/gpu.c
  src/edds/bc7.c
  src/edds/bc6.c
  src/edds/decode/png.c
  src/edds/decode/tga.c
  src/edds/decode/jpeg.c
  src/edds/decode/tiff.c
  src/edds/decode/dds.c
  src/edds/decode/hdr.c
  src/edds/convert.c
  src/edds/metadata.c
  src/edds/meta_text.c
  src/edds/batch.c
  src/edds/pool.c)
target_include_directories(edds_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/edds")
target_link_libraries(edds_core PUBLIC Threads::Threads)
if(UNIX)
  target_link_libraries(edds_core PUBLIC m)
endif()
enfusion_configure_target(edds_core)

# SDF fonts from static TrueType: the reader, the field, the atlas layout and FNT5. The atlas
# itself is written through edds_core, and the recipe is read with its meta_text.
add_library(font_core STATIC
  src/font/recipe.c
  src/font/characters.c
  src/font/sfnt.c
  src/font/kerning.c
  src/font/shape.c
  src/font/msdf.c
  src/font/fnt.c
  src/font/generate.c)
target_include_directories(font_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/edds")
target_link_libraries(font_core PUBLIC edds_core)
if(UNIX)
  target_link_libraries(font_core PUBLIC m)
endif()
enfusion_configure_target(font_core)

# One executable, one first-level subcommand per area: `enfusion edds ...`, `enfusion font ...`.
add_executable(enfusion
  src/cli/main.c
  src/cli/process.c
  src/cli/publish.c
  src/cli/edds.c
  src/cli/font.c)
target_include_directories(enfusion PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/edds")
target_link_libraries(enfusion PRIVATE edds_core font_core)
enfusion_configure_target(enfusion)

install(TARGETS enfusion RUNTIME DESTINATION .)
install(FILES NOTICE.txt THIRD-PARTY.md DESTINATION .)

include(CTest)
if(BUILD_TESTING AND ENFUSION_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME edds_hdr COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/edds/hdr_test.py" "$<TARGET_FILE:enfusion>")
  # EDDS: synthetic sources and an independent reader. Production code is not its own oracle.
  add_library(edds_test_fixture STATIC tests/edds/fixture.c)
  target_include_directories(edds_test_fixture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests/edds")
  enfusion_configure_target(edds_test_fixture)

  add_executable(edds-fixture tests/edds/fixture_generator.c)
  target_link_libraries(edds-fixture PRIVATE edds_test_fixture)
  enfusion_configure_target(edds-fixture)

  # Deliberately does not link edds_core: production code must not be its own test oracle.
  add_executable(edds-reference-reader tests/edds/reference_reader.c)
  enfusion_configure_target(edds-reference-reader)

  add_executable(edds-core-test tests/edds/core_test.c)
  target_link_libraries(edds-core-test PRIVATE edds_core edds_test_fixture)
  enfusion_configure_target(edds-core-test)
  add_test(NAME edds_core COMMAND edds-core-test)

  add_executable(edds-mip-modes-test tests/edds/mip_modes_test.c)
  target_link_libraries(edds-mip-modes-test PRIVATE edds_core edds_test_fixture)
  enfusion_configure_target(edds-mip-modes-test)
  add_test(NAME edds_mip_modes COMMAND edds-mip-modes-test)
  add_test(NAME edds_mip_goldens COMMAND "${CMAKE_COMMAND}"
    "-DCLI=$<TARGET_FILE:enfusion>"
    "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
    "-DREFERENCE_READER=$<TARGET_FILE:edds-reference-reader>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edds/mip_modes_test.cmake")
  add_test(NAME edds_swizzling COMMAND "${CMAKE_COMMAND}"
    "-DCLI=$<TARGET_FILE:enfusion>"
    "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
    "-DREFERENCE_READER=$<TARGET_FILE:edds-reference-reader>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edds/swizzling_test.cmake")

  add_executable(edds-memory-test tests/edds/memory_test.c)
  target_link_libraries(edds-memory-test PRIVATE edds_core edds_test_fixture)
  enfusion_configure_target(edds-memory-test)
  add_test(NAME edds_memory COMMAND edds-memory-test)
  add_test(NAME edds_memory_cli
    COMMAND "${CMAKE_COMMAND}"
      "-DCLI=$<TARGET_FILE:enfusion>"
      "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edds/memory_protocol_test.cmake")
  add_test(NAME edds_cli_black_box
    COMMAND "${CMAKE_COMMAND}"
      "-DCLI=$<TARGET_FILE:enfusion>"
      "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
      "-DREFERENCE_READER=$<TARGET_FILE:edds-reference-reader>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edds/protocol_test.cmake")

  # Fonts: synthetic TrueType fonts and the outlines they were written from; no font_core inside.
  add_library(font_test_fixture STATIC tests/font/font_fixture.c)
  target_link_libraries(font_test_fixture PUBLIC edds_test_fixture)
  enfusion_configure_target(font_test_fixture)

  add_executable(enfusion-font-fixture tests/font/font_fixture_generator.c)
  target_link_libraries(enfusion-font-fixture PRIVATE font_test_fixture)
  enfusion_configure_target(enfusion-font-fixture)

  # Like the EDDS reader, it links no production code: the generator is not its own oracle.
  add_executable(enfusion-font-reference tests/font/font_reference.c)
  target_link_libraries(enfusion-font-reference PRIVATE font_test_fixture)
  enfusion_configure_target(enfusion-font-reference)

  add_executable(font-core-test tests/font/font_core_test.c)
  target_link_libraries(font-core-test PRIVATE font_core font_test_fixture)
  enfusion_configure_target(font-core-test)
  add_test(NAME font_core COMMAND font-core-test)
  add_test(NAME font_cli_black_box
    COMMAND "${CMAKE_COMMAND}"
      "-DCLI=$<TARGET_FILE:enfusion>"
      "-DFONT_FIXTURE=$<TARGET_FILE:enfusion-font-fixture>"
      "-DFONT_REFERENCE=$<TARGET_FILE:enfusion-font-reference>"
      "-DKERNING_GOLDEN=${CMAKE_CURRENT_SOURCE_DIR}/tests/workbench/font-kerning-golden.json"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/font/font_test.cmake")
endif()

if(ENFUSION_BUILD_FUZZER)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "ENFUSION_BUILD_FUZZER requires Clang/libFuzzer")
  endif()
  add_executable(edds-fuzz fuzz/fuzz_edds.c)
  target_link_libraries(edds-fuzz PRIVATE edds_core)
  enfusion_configure_target(edds-fuzz)
  target_compile_options(edds-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(edds-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  add_executable(font-fuzz fuzz/fuzz_font.c)
  target_link_libraries(font-fuzz PRIVATE font_core)
  enfusion_configure_target(font-fuzz)
  target_compile_options(font-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
  target_link_options(font-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
