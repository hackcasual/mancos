include(FetchContent)
FetchContent_Declare(
  doctest
  URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
  URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(doctest)

add_executable(yafc_tests
  test_main.cc
  cost_analysis_test.cc
  data_stage_test.cc
  lead_golden_test.cc
  database_test.cc
  milestones_test.cc
  production_table_test.cc
  tech_science_test.cc
  serialization_test.cc
  zip_mod_test.cc
  graph_test.cc
  production_table_solver_test.cc
  solver_test.cc
  lua_test.cc
)
target_link_libraries(yafc_tests PRIVATE yafc_core doctest::doctest miniz)

if(EMSCRIPTEN)
  # NODERAWFS: tests run under node and read the real filesystem (game data
  # for the data-stage tracer). Browser builds use OPFS/worker FS instead.
  target_link_options(yafc_tests PRIVATE
    -sEXIT_RUNTIME=1 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576 -sNODERAWFS=1)
endif()

# Under emscripten, CMAKE_CROSSCOMPILING_EMULATOR (node) runs the .js output.
add_test(NAME yafc_tests COMMAND yafc_tests)
