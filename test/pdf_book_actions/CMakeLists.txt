cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_book_actions_tests LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
endif()

# Copying changes only quoted-include lookup: the production translation unit
# is compiled byte-for-byte, while its device dependencies come from stubs.
configure_file(
  "${REPO_ROOT}/src/activities/home/BookActions.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/BookActionsUnderTest.cpp"
  COPYONLY
)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs/BookActions.h"
  "${CMAKE_CURRENT_BINARY_DIR}/BookActions.h"
  COPYONLY
)

add_executable(PdfBookActionsTest
  "${CMAKE_CURRENT_BINARY_DIR}/BookActionsUnderTest.cpp"
  PdfBookActionsTest.cpp
  stubs/TestState.cpp
)

target_include_directories(PdfBookActionsTest BEFORE PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)

target_compile_options(PdfBookActionsTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)

set(PRODUCTION_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/production_stubs")
file(REMOVE_RECURSE "${PRODUCTION_STUB_DIR}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/stubs/" DESTINATION "${PRODUCTION_STUB_DIR}")
file(REMOVE
  "${PRODUCTION_STUB_DIR}/HalStorage.h"
  "${PRODUCTION_STUB_DIR}/activities/reader/BookReadingStats.h"
)
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/production/HalStorage.h"
  "${PRODUCTION_STUB_DIR}/HalStorage.h"
  COPYONLY
)

add_executable(PdfBookActionsDurabilityTest
  "${CMAKE_CURRENT_BINARY_DIR}/BookActionsUnderTest.cpp"
  "${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp"
  production/PdfBookActionsDurabilityTest.cpp
  production/ProductionReadingStatsSupport.cpp
  production/ProductionTestStorage.cpp
  stubs/TestState.cpp
)

target_include_directories(PdfBookActionsDurabilityTest BEFORE PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${PRODUCTION_STUB_DIR}"
  "${REPO_ROOT}/src"
)

target_compile_definitions(PdfBookActionsDurabilityTest PRIVATE
  PDF_BOOK_ACTIONS_PRODUCTION_STATS=1
)

target_compile_options(PdfBookActionsDurabilityTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)

add_test(NAME PdfBookActionsTest COMMAND PdfBookActionsTest)
add_test(NAME PdfBookActionsDurabilityTest COMMAND PdfBookActionsDurabilityTest)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME PdfBookActionsLegacySourceGuard
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/legacy_source_guard.py"
)
add_test(
  NAME PdfBookActionsBehaviorMutations
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/behavior_negative_control_test.py"
)
add_test(
  NAME PdfBookActionsDurabilityMutations
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/durability_negative_control_test.py"
)
