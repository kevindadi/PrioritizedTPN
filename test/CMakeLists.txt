include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

enable_testing()

add_executable(ptpn_test
  test_json_parser.cpp
  test_ptpn_parser.cpp
  test_analysis_semantics.cpp
  test_extrapolation.cpp
  test_zone_merging.cpp
  test_metrics.cpp
  test_reference_analysis.cpp
  test_symmetry.cpp
  test_saturation.cpp
  test_tdg2pn_lowering.cpp
  test_cone_projection.cpp
  test_pipeline_golden.cpp
  test_invariants.cpp
  test_parser_robustness.cpp
  test_time_horizon.cpp
  test_scheduling_policy.cpp
  test_edf.cpp
  test_edf_multicore.cpp
  test_fifo.cpp
  test_fifo_multicore.cpp
  test_llf.cpp
  test_llf_multicore.cpp
  test_allocation.cpp
  test_diagnostics.cpp
  test_describe.cpp
  test_rtsl_lexer.cpp
  test_rtsl_parser.cpp
  test_rtsl_validation.cpp
  test_rtsl_lowering.cpp
  test_rtsl_pipeline.cpp
  test_rtsl_check_cli.cpp
  test_app_rtsl_input.cpp
  test_output_failures.cpp
  test_unsupported_class.cpp
)

target_link_libraries(ptpn_test
  PRIVATE
  PTPN_core
  gtest_main
)

# Integration tests exercise the CLI through its diagnostics envelope.
target_compile_definitions(ptpn_test PRIVATE PTPN_CLI_PATH="$<TARGET_FILE:ptpn>")
add_dependencies(ptpn_test ptpn)

target_include_directories(ptpn_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(ptpn_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
