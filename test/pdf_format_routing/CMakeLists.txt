cmake_minimum_required(VERSION 3.16)
project(pdf_format_routing_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)

add_executable(PdfFormatRoutingTest
  "${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp"
  "${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp"
  "${REPO_ROOT}/src/util/NextBookFinder.cpp"
  NextBookFinderTest.cpp
)

target_include_directories(PdfFormatRoutingTest BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)

target_include_directories(PdfFormatRoutingTest PRIVATE
  "${REPO_ROOT}/lib/FsHelpers"
  "${REPO_ROOT}/lib/Memory"
  "${REPO_ROOT}/src/util"
)

target_compile_options(PdfFormatRoutingTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)

enable_testing()
add_test(NAME PdfFormatRoutingTest COMMAND PdfFormatRoutingTest)
