cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_book_deletion_core_tests LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
endif()

add_executable(PdfBookDeletionCoreTest
  "${REPO_ROOT}/lib/BookStateMigration/PdfDeleteJournal.cpp"
  "${REPO_ROOT}/lib/BookStateMigration/PdfDeleteCoordinator.cpp"
  PdfDeleteCoordinatorTest.cpp
)

target_include_directories(PdfBookDeletionCoreTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
)

target_compile_options(PdfBookDeletionCoreTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)

add_test(NAME PdfBookDeletionCoreTest COMMAND PdfBookDeletionCoreTest)
