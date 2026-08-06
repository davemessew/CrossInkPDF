add_executable(reflow_document_tests
  ReflowCapabilityTest.cpp
  ReflowDocumentContractTest.cpp
  ReflowReaderProgressTest.cpp
  ReflowSectionSourceTest.cpp
)

target_include_directories(reflow_document_tests BEFORE PRIVATE
  ${REPO_ROOT}/lib/Reflow
)

if(MSVC)
  target_include_directories(reflow_document_tests PRIVATE
    ${REPO_ROOT}
    ${REPO_ROOT}/lib
  )
  target_compile_options(reflow_document_tests PRIVATE /W4)
  target_link_libraries(reflow_document_tests PRIVATE GTest::gtest_main)
else()
  target_link_libraries(reflow_document_tests PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )
endif()

gtest_discover_tests(reflow_document_tests)

add_executable(epub_production_path_tests
  ${REPO_ROOT}/lib/Epub/Epub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp
  EpubProductionPathTest.cpp
  EpubProductionTestStubs.cpp
)

target_include_directories(epub_production_path_tests BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/production_stubs
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib
)

if(MSVC)
  target_compile_options(epub_production_path_tests PRIVATE /W4 /Gy)
  target_link_options(epub_production_path_tests PRIVATE /OPT:REF)
  target_link_libraries(epub_production_path_tests PRIVATE GTest::gtest_main)
else()
  target_compile_options(epub_production_path_tests PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(epub_production_path_tests PRIVATE -Wl,--gc-sections)
  target_link_libraries(epub_production_path_tests PRIVATE crosspoint_test_common GTest::gtest_main)
endif()

gtest_discover_tests(epub_production_path_tests)
