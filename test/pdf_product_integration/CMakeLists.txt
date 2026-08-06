add_executable(PdfProductIntegrationTest
  ${REPO_ROOT}/lib/PixelCache/PixelCache.cpp
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
  PdfCachedImageRenderingTest.cpp
  PdfReflowDocumentTest.cpp
  PdfRoutingTest.cpp
)

target_include_directories(PdfProductIntegrationTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/allocation_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_include_directories(PdfProductIntegrationTest PRIVATE
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/PixelCache
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

target_compile_definitions(PdfProductIntegrationTest PRIVATE
  CROSSINK_PDF_REFLOW_DOCUMENT_TEST=1
)

gtest_discover_tests(PdfProductIntegrationTest TEST_PREFIX "PdfProduct")

add_executable(PdfCachedImageBlockTest
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  PdfCachedImageBlockTest.cpp
)

target_include_directories(PdfCachedImageBlockTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/image_stubs
)

target_include_directories(PdfCachedImageBlockTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/blocks
  ${REPO_ROOT}/lib/Epub/Epub/converters
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
)

target_compile_definitions(PdfCachedImageBlockTest PRIVATE CROSSINK_ENABLE_PDF=1)

target_link_libraries(PdfCachedImageBlockTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_compile_options(PdfCachedImageBlockTest PRIVATE -ffunction-sections -fdata-sections)
target_link_options(PdfCachedImageBlockTest PRIVATE -Wl,--gc-sections)

if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_link_options(PdfCachedImageBlockTest PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=free
  )
endif()

gtest_discover_tests(PdfCachedImageBlockTest TEST_PREFIX "PdfCachedImageBlock")

add_executable(PdfPreparedCacheRenderingTest
  ${REPO_ROOT}/lib/PixelCache/PixelCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfBuildCheckpoint.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheManifest.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfContentInterpreter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfEncoding.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfFixedRecordSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfFontMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageBuildSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageExtractor.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageObject.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImagePreparation.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfJpegPreview.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMaskSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMetadataStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectResolver.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfOutline.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPageModel.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPageTree.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPixelCacheWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPreparedContent.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPreparation.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfProgressStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfReflowDocument.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfResourceTracker.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemWordMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemsStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSemanticWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfStreamDecoder.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfUnicode.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfWordCounter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfXref.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  PdfPreparedCacheRenderingTest.cpp
)

target_include_directories(PdfPreparedCacheRenderingTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/image_stubs
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_include_directories(PdfPreparedCacheRenderingTest PRIVATE
  ${REPO_ROOT}/lib/PixelCache
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/blocks
  ${REPO_ROOT}/lib/Epub/Epub/converters
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)

target_link_libraries(PdfPreparedCacheRenderingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_compile_definitions(PdfPreparedCacheRenderingTest PRIVATE CROSSINK_ENABLE_PDF=1)

target_compile_options(PdfPreparedCacheRenderingTest PRIVATE
  -Wall
  -Wextra
  -pedantic
)

gtest_discover_tests(PdfPreparedCacheRenderingTest TEST_PREFIX "PdfPreparedCacheRendering")
