cmake_minimum_required(VERSION 3.16)
project(crossink_pdf_document_contract_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

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

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(PdfDocumentContractTest
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
  ${REPO_ROOT}/lib/PdfReflow/PdfFixedRecordSpool.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectResolver.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPageModel.cpp
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
  ${REPO_ROOT}/lib/PdfReflow/PdfResourceTracker.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPreparedContent.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPreparation.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  stubs/ContractCacheIo.cpp
  PdfDocumentContractTest.cpp
  PdfLookupContractTest.cpp
)

target_include_directories(PdfDocumentContractTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PixelCache
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/pdf_reflow_core
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

if(MSVC)
  target_compile_options(PdfDocumentContractTest PRIVATE /W4 $<$<COMPILE_LANGUAGE:CXX>:/FIstring_view>)
else()
  target_compile_options(PdfDocumentContractTest PRIVATE -Wall -Wextra -pedantic
    $<$<COMPILE_LANGUAGE:CXX>:-include> $<$<COMPILE_LANGUAGE:CXX>:string_view>)
endif()

target_link_libraries(PdfDocumentContractTest PRIVATE GTest::gtest_main)

enable_testing()
include(GoogleTest)
gtest_discover_tests(PdfDocumentContractTest TEST_PREFIX "PdfDocumentContract.")
