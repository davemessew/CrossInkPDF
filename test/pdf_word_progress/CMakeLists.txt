add_executable(PdfWordProgressTest
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfProgressStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  PdfWordProgressTest.cpp
)

target_include_directories(PdfWordProgressTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)

target_link_libraries(PdfWordProgressTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PdfWordProgressTest TEST_PREFIX "PdfWordProgress")
