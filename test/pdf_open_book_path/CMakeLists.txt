cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_open_book_path_tests CXX)

  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  enable_testing()

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()

add_executable(OpenBookPathAccessorTest
  OpenBookPathAccessorTest.cpp
)

target_include_directories(OpenBookPathAccessorTest PRIVATE
  ${REPO_ROOT}/src
)

target_compile_options(OpenBookPathAccessorTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
)

add_test(NAME OpenBookPathAccessorTest COMMAND OpenBookPathAccessorTest)
