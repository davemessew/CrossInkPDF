cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_cache_clear_tests CXX)

  set(CMAKE_CXX_STANDARD 17)
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

add_executable(PdfCacheClearTest
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  PdfCacheClearTest.cpp
  stubs/TestStorage.cpp
)

add_library(PdfCacheClearSimulatorCompile OBJECT
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
)

target_include_directories(PdfCacheClearTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_include_directories(PdfCacheClearSimulatorCompile BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)

target_include_directories(PdfCacheClearTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/src/util
)

target_include_directories(PdfCacheClearSimulatorCompile PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/src/util
)

target_link_libraries(PdfCacheClearTest PRIVATE
  GTest::gtest_main
)

target_compile_definitions(PdfCacheClearSimulatorCompile PRIVATE
  SIMULATOR
)

target_compile_options(PdfCacheClearTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
)

target_compile_options(PdfCacheClearSimulatorCompile PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
)

gtest_discover_tests(PdfCacheClearTest TEST_PREFIX "PdfCacheClear")
