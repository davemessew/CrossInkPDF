cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_pixel_cache_tests C CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

add_executable(PdfPixelCacheTest
  ${REPO_ROOT}/lib/PixelCache/PixelCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPixelCacheWriter.cpp
  PdfPixelCacheTest.cpp
)

target_include_directories(PdfPixelCacheTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/PixelCache
)

target_link_libraries(PdfPixelCacheTest PRIVATE
  GTest::gtest_main
)

target_compile_options(PdfPixelCacheTest PRIVATE
  -Wall
  -Wextra
  -pedantic
)

if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_definitions(PdfPixelCacheTest PRIVATE
    PDF_PIXEL_CACHE_WRAP_MALLOC=1
  )
  target_link_options(PdfPixelCacheTest PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=free
  )
endif()

gtest_discover_tests(PdfPixelCacheTest TEST_PREFIX "PdfPixelCache")

add_executable(PdfImageExtractorTest
  ${REPO_ROOT}/lib/PixelCache/PixelCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageExtractor.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPixelCacheWriter.cpp
  PdfImageExtractorTest.cpp
)

target_include_directories(PdfImageExtractorTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/PixelCache
)

target_link_libraries(PdfImageExtractorTest PRIVATE
  GTest::gtest_main
)

target_compile_options(PdfImageExtractorTest PRIVATE
  -Wall
  -Wextra
  -pedantic
)

if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_definitions(PdfImageExtractorTest PRIVATE
    PDF_IMAGE_EXTRACTOR_WRAP_MALLOC=1
  )
  target_link_options(PdfImageExtractorTest PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=free
  )
endif()

gtest_discover_tests(PdfImageExtractorTest TEST_PREFIX "PdfImageExtractor")

add_executable(PdfImageObjectTest
  ${REPO_ROOT}/lib/PdfReflow/PdfImageObject.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  PdfImageObjectTest.cpp
)

target_include_directories(PdfImageObjectTest PRIVATE
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/PixelCache
  ${REPO_ROOT}/lib/uzlib/src
)

target_link_libraries(PdfImageObjectTest PRIVATE
  GTest::gtest_main
)

target_compile_options(PdfImageObjectTest PRIVATE
  -Wall
  -Wextra
  -pedantic
)

if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_definitions(PdfImageObjectTest PRIVATE
    PDF_IMAGE_OBJECT_WRAP_MALLOC=1
  )
  target_link_options(PdfImageObjectTest PRIVATE
    -Wl,--wrap=malloc
    -Wl,--wrap=free
  )
endif()

gtest_discover_tests(PdfImageObjectTest TEST_PREFIX "PdfImageObject")

add_executable(PdfImagePlacementTest
  ${REPO_ROOT}/lib/PdfReflow/PdfCMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfContentInterpreter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfEncoding.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfFontMap.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPageModel.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfUnicode.cpp
  PdfImagePlacementTest.cpp
)

target_include_directories(PdfImagePlacementTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
)

target_link_libraries(PdfImagePlacementTest PRIVATE
  GTest::gtest_main
)

target_compile_options(PdfImagePlacementTest PRIVATE
  -Wall
  -Wextra
  -pedantic
)

gtest_discover_tests(PdfImagePlacementTest TEST_PREFIX "PdfImagePlacement")

add_executable(PdfImagePreparationTest
  ${REPO_ROOT}/lib/PixelCache/PixelCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfBuildCheckpoint.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheManifest.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfFixedRecordSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageBuildSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageCache.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageExtractor.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImageObject.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfImagePreparation.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfJpegPreview.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMaskSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPixelCacheWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMetadataStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectResolver.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfOutline.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPageTree.cpp
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
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  PdfImagePreparationTest.cpp
)

target_include_directories(PdfImagePreparationTest BEFORE PRIVATE
  ${REPO_ROOT}/test/pdf_product_integration/stubs
)

target_include_directories(PdfImagePreparationTest PRIVATE
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PixelCache
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)

target_link_libraries(PdfImagePreparationTest PRIVATE
  GTest::gtest_main
)

target_compile_options(PdfImagePreparationTest PRIVATE
  -Wall
  -Wextra
  -pedantic
)

gtest_discover_tests(PdfImagePreparationTest TEST_PREFIX "PdfImagePreparation")
