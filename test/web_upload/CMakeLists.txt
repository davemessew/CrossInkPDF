cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(web_upload_tests CXX)

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

add_executable(AtomicBookUploadTest
  ${REPO_ROOT}/lib/BookUpload/AtomicBookUpload.cpp
  AtomicBookUploadTest.cpp
  TestSha256.cpp
)

target_include_directories(AtomicBookUploadTest PRIVATE
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/BookUpload
)

target_link_libraries(AtomicBookUploadTest PRIVATE
  GTest::gtest_main
)

target_compile_options(AtomicBookUploadTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
)

gtest_discover_tests(AtomicBookUploadTest TEST_PREFIX "WebUpload")

find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
  add_test(
    NAME WebUploadSourceContract
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/source_contract_test.js
  )
  add_test(
    NAME WebUploadLegacyBehavior
    COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/legacy_transport_trace_test.js
  )
endif()
