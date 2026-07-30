add_executable(PdfProductIntegrationTest
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheManifest.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfBuildCheckpoint.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMetadataStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfOutline.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfProgressStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemWordMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemsStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfReflowDocument.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSemanticWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfUnicode.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfWordCounter.cpp
  ${REPO_ROOT}/src/activities/reader/ReaderRoute.cpp
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  PdfReflowDocumentTest.cpp
  PdfRoutingTest.cpp
)

target_include_directories(PdfProductIntegrationTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_include_directories(PdfProductIntegrationTest PRIVATE
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)

target_link_libraries(PdfProductIntegrationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PdfProductIntegrationTest TEST_PREFIX "PdfProduct")
