cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(epub_legacy_href_tests LANGUAGES CXX)

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

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
  include(GoogleTest)
endif()

add_executable(epub_legacy_href_tests
  "${REPO_ROOT}/lib/Epub/Epub.cpp"
  "${REPO_ROOT}/lib/Epub/Epub/Section.cpp"
  "${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp"
  "${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp"
  "${REPO_ROOT}/test/reflow_document/EpubProductionTestStubs.cpp"
  EpubLegacyHrefTest.cpp
)

target_compile_definitions(epub_legacy_href_tests PRIVATE
  CROSSINK_PRODUCTION_TEST_SEAMS=1
)

target_include_directories(epub_legacy_href_tests BEFORE PRIVATE
  "${REPO_ROOT}/test/reflow_document/production_stubs"
  "${REPO_ROOT}/lib/Memory"
  "${REPO_ROOT}/lib/PdfReflow"
  "${REPO_ROOT}/lib/Reflow"
  "${REPO_ROOT}/lib/Epub"
  "${REPO_ROOT}/lib"
)

if(MSVC)
  target_compile_options(epub_legacy_href_tests PRIVATE /W4 /Gy)
  target_link_options(epub_legacy_href_tests PRIVATE /OPT:REF)
  target_link_libraries(epub_legacy_href_tests PRIVATE GTest::gtest_main)
else()
  target_compile_options(epub_legacy_href_tests PRIVATE
    -Wall
    -Wextra
    -pedantic
    -fno-exceptions
    -ffunction-sections
    -fdata-sections
    -fstack-usage
    -Os
  )
  target_link_options(epub_legacy_href_tests PRIVATE -Wl,--gc-sections)
  target_link_libraries(epub_legacy_href_tests PRIVATE GTest::gtest_main)
endif()

gtest_discover_tests(epub_legacy_href_tests)
