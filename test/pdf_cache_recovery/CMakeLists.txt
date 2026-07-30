add_executable(PdfCacheRecoveryTest
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheManifest.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfBuildCheckpoint.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheStore.cpp
  PdfTestCacheIo.cpp
  PdfCacheAllocationTest.cpp
  PdfCacheRecoveryTest.cpp
  PdfCacheFaultInjectionTest.cpp
)

target_include_directories(PdfCacheRecoveryTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
)

target_link_libraries(PdfCacheRecoveryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PdfCacheRecoveryTest TEST_PREFIX "PdfCache")
