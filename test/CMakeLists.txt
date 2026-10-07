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
  test_metrics.cpp
  test_saturation.cpp
  test_tdg2romeo.cpp
  test_tdg2pn_lowering.cpp
  test_pipeline_golden.cpp
)

target_link_libraries(ptpn_test
  PRIVATE
  PTPN_core
  gtest_main
)

target_include_directories(ptpn_test PRIVATE ${CMAKE_SOURCE_DIR}/src)

include(GoogleTest)
gtest_discover_tests(ptpn_test PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})