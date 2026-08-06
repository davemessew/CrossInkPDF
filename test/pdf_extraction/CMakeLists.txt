add_executable(PdfExtractionTest
  ${REPO_ROOT}/lib/PixelCache/PixelCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageBuildSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageExtractor.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageObject.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImagePreparation.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfJpegPreview.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMaskSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPixelCacheWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfContentInterpreter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfEncoding.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfFontMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfHiddenText.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectResolver.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPageModel.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPreparedContent.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPageTree.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMetadataStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfOutline.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSemanticWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfStreamDecoder.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfUnicode.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfWordCounter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfXref.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheManifest.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfBuildCheckpoint.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfFixedRecordSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfResourceTracker.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPreparation.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  PdfAdversarialTest.cpp
  PdfPreparationContentStreamsTest.cpp
  PdfPreparationProductGapRedTest.cpp
  PdfPreparedContentTest.cpp
  PdfPreparationTest.cpp
  PdfResourceTrackerTest.cpp
)

target_include_directories(PdfExtractionTest PRIVATE
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PixelCache
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)

target_link_libraries(PdfExtractionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PdfExtractionTest TEST_PREFIX "PdfExtraction")

add_test(
  NAME PdfCacheSetupDispatchContract
  COMMAND
    ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cache_setup_dispatch_contract_test.py
)
