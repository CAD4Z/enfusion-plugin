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

add_library(edds_core STATIC
  src/edds.c
  src/image.c
  src/memory.c
  src/sources.c
  src/conversions.c
  src/swizzling.c
  src/gpu.c
  src/bc7.c
  src/decode_png.c
  src/decode_tga.c
  src/decode_jpeg.c
  src/decode_tiff.c
  src/decode_dds.c
  src/convert.c
  src/metadata.c
  src/batch.c
  src/pool.c)
target_include_directories(edds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(edds_core PUBLIC Threads::Threads)
enfusion_configure_target(edds_core)

# One executable, one first-level subcommand per area: `enfusion edds ...`.
add_executable(enfusion src/main.c src/cli.c)
target_link_libraries(enfusion PRIVATE edds_core)
enfusion_configure_target(enfusion)

install(TARGETS enfusion RUNTIME DESTINATION .)
install(FILES NOTICE.txt THIRD-PARTY.md DESTINATION .)

include(CTest)
if(BUILD_TESTING AND ENFUSION_BUILD_TESTS)
  add_library(edds_test_fixture STATIC tests/fixture.c)
  target_include_directories(edds_test_fixture PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  enfusion_configure_target(edds_test_fixture)

  add_executable(edds-core-test tests/core_test.c)
  target_link_libraries(edds-core-test PRIVATE edds_core edds_test_fixture)
  enfusion_configure_target(edds-core-test)

  add_executable(edds-fixture tests/fixture_generator.c)
  target_link_libraries(edds-fixture PRIVATE edds_test_fixture)
  enfusion_configure_target(edds-fixture)

  # Deliberately does not link edds_core: production code must not be its own test oracle.
  add_executable(edds-reference-reader tests/reference_reader.c)
  enfusion_configure_target(edds-reference-reader)

  add_test(NAME edds_core COMMAND edds-core-test)
  add_executable(edds-mip-modes-test tests/mip_modes_test.c)
  target_link_libraries(edds-mip-modes-test PRIVATE edds_core edds_test_fixture)
  enfusion_configure_target(edds-mip-modes-test)
  add_test(NAME edds_mip_modes COMMAND edds-mip-modes-test)
  add_test(NAME edds_mip_goldens COMMAND "${CMAKE_COMMAND}"
    "-DCLI=$<TARGET_FILE:enfusion>"
    "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
    "-DREFERENCE_READER=$<TARGET_FILE:edds-reference-reader>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/mip_modes_test.cmake")
  add_test(NAME edds_swizzling COMMAND "${CMAKE_COMMAND}"
    "-DCLI=$<TARGET_FILE:enfusion>"
    "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
    "-DREFERENCE_READER=$<TARGET_FILE:edds-reference-reader>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/swizzling_test.cmake")
  add_executable(edds-memory-test tests/memory_test.c)
  target_link_libraries(edds-memory-test PRIVATE edds_core edds_test_fixture)
  enfusion_configure_target(edds-memory-test)
  add_test(NAME edds_memory COMMAND edds-memory-test)
  add_test(NAME edds_memory_cli
    COMMAND "${CMAKE_COMMAND}"
      "-DCLI=$<TARGET_FILE:enfusion>"
      "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/memory_protocol_test.cmake")
  add_test(
    NAME edds_cli_black_box
    COMMAND "${CMAKE_COMMAND}"
      "-DCLI=$<TARGET_FILE:enfusion>"
      "-DFIXTURE=$<TARGET_FILE:edds-fixture>"
      "-DREFERENCE_READER=$<TARGET_FILE:edds-reference-reader>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/protocol_test.cmake"
  )
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
endif()
