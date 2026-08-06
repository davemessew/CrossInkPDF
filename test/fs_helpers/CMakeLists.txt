add_executable(fs_helpers_tests
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
  FsHelpersTest.cpp
)

target_include_directories(fs_helpers_tests PRIVATE
  ${REPO_ROOT}/test/pdf_format_routing/stubs
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(fs_helpers_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(fs_helpers_tests)
