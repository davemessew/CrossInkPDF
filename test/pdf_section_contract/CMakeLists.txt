cmake_minimum_required(VERSION 3.16)
project(pdf_section_contract_tests LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(PdfSectionContractTest
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
  ${REPO_ROOT}/lib/PdfReflow/PdfFixedRecordSpool.cpp
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
  ${REPO_ROOT}/lib/PdfReflow/PdfResourceTracker.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfPreparation.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  PdfSectionContractTest.cpp
)

target_include_directories(PdfSectionContractTest PRIVATE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PixelCache
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)

target_compile_options(PdfSectionContractTest PRIVATE
  -Wall
  -Wextra
  -pedantic
  -Os
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
  $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
)

target_compile_definitions(PdfSectionContractTest PRIVATE
  PDF_SECTION_CONTRACT_REPO_ROOT="${REPO_ROOT}"
)

enable_testing()
add_test(NAME PdfSectionContractCommitControl
         COMMAND PdfSectionContractTest --commit-control)
add_test(NAME PdfSectionContractOracleControls
         COMMAND PdfSectionContractTest --oracle-controls)
add_test(NAME PdfSectionContractNoOutlineContinuous
         COMMAND PdfSectionContractTest --no-outline)
add_test(NAME PdfSectionContractOutlineBoundary
         COMMAND PdfSectionContractTest --outline-boundary)
add_test(NAME PdfSectionContractNumericRectLink
         COMMAND PdfSectionContractTest --numeric-rect-link)
add_test(NAME PdfSectionContractOutsideRectUnlinked
         COMMAND PdfSectionContractTest --outside-rect-unlinked)
add_test(NAME PdfSectionContractNonOutlineTarget
         COMMAND PdfSectionContractTest --non-outline-target)
add_test(NAME PdfSectionContractRectEdgeInclusive
         COMMAND PdfSectionContractTest --rect-edge)
add_test(NAME PdfSectionContractRotatedCropRect
         COMMAND PdfSectionContractTest --rotated-crop)
add_test(NAME PdfSectionContractFixedLayouts
         COMMAND PdfSectionContractTest --layout-contract)
add_test(NAME PdfSectionContractOutlineStoredCapacity
         COMMAND PdfSectionContractTest --outline-capacity)
add_test(NAME PdfSectionContractPrefixCrashBeforeJournal
         COMMAND PdfSectionContractTest --prefix-crash-before-journal)
add_test(NAME PdfSectionContractPrefixCrashAfterJournal
         COMMAND PdfSectionContractTest --prefix-crash-after-journal)
add_test(NAME PdfSectionContractPrefixCrashAfterCheckpoint
         COMMAND PdfSectionContractTest --prefix-crash-after-checkpoint)
