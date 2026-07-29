add_executable(reflow_document_tests
  ReflowDocumentContractTest.cpp
)

target_link_libraries(reflow_document_tests PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(reflow_document_tests)
