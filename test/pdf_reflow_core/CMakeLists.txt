find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(PdfReflowCoreTest
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  PdfTestIo.cpp
  PdfByteSourceTest.cpp
  PdfAllocationTest.cpp
)

target_include_directories(PdfReflowCoreTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/uzlib/src
)

target_link_libraries(PdfReflowCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PdfReflowCoreTest TEST_PREFIX "Pdf")

add_test(
  NAME PdfFixtureDeterminism
  COMMAND
    ${Python3_EXECUTABLE}
    ${REPO_ROOT}/scripts/generate_pdf_reflow_fixtures.py
    --check
)
