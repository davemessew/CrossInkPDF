add_executable(PdfNavigationTest
  ${REPO_ROOT}/lib/PixelCache/PixelCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSecurity.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectResolver.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfStreamDecoder.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfXref.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheManifest.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfBuildCheckpoint.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMetadataStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfEncoding.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfOutline.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfProgressStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemWordMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemsStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfReflowDocument.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSemanticWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfUnicode.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfWordCounter.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  PdfNavigationTest.cpp
)

target_include_directories(PdfNavigationTest BEFORE PRIVATE
  ${REPO_ROOT}/test/pdf_product_integration/stubs
)

target_include_directories(PdfNavigationTest PRIVATE
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PixelCache
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)

target_link_libraries(PdfNavigationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PdfNavigationTest TEST_PREFIX "PdfNavigation")
