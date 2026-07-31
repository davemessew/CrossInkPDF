cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_delete_app_tests LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
endif()

configure_file(
  "${REPO_ROOT}/src/util/PdfDeleteUtils.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDeleteUtilsUnderTest.cpp"
  COPYONLY
)
configure_file(
  "${REPO_ROOT}/src/util/PdfDeleteUtils.h"
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDeleteUtils.h"
  COPYONLY
)

add_executable(PdfDeleteAppTest
  "${REPO_ROOT}/lib/BookStateMigration/PdfDeleteCoordinator.cpp"
  "${REPO_ROOT}/lib/BookStateMigration/PdfDeleteJournal.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDeleteUtilsUnderTest.cpp"
  PdfDeleteAppTest.cpp
)

target_include_directories(PdfDeleteAppTest BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  "${REPO_ROOT}/test/book_move_integration/stubs"
)
target_include_directories(PdfDeleteAppTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
  "${REPO_ROOT}/src/util"
)
target_compile_options(PdfDeleteAppTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
target_compile_definitions(PdfDeleteAppTest PRIVATE PDF_DELETE_TESTING=1)

add_test(NAME PdfDeleteAppTest COMMAND PdfDeleteAppTest)

configure_file(
  "${REPO_ROOT}/src/util/PdfDirectoryDeleteScan.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDirectoryDeleteScanUnderTest.cpp"
  COPYONLY
)
configure_file(
  "${REPO_ROOT}/src/util/PdfDirectoryDeleteScan.h"
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDirectoryDeleteScan.h"
  COPYONLY
)

add_executable(PdfDirectoryDeleteScanTest
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDirectoryDeleteScanUnderTest.cpp"
  PdfDirectoryDeleteScanTest.cpp
)
target_include_directories(PdfDirectoryDeleteScanTest BEFORE PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${REPO_ROOT}/test/book_move_integration/stubs"
)
target_include_directories(PdfDirectoryDeleteScanTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
  "${REPO_ROOT}/lib/FsHelpers"
)
target_compile_options(PdfDirectoryDeleteScanTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
target_compile_definitions(PdfDirectoryDeleteScanTest PRIVATE CROSSINK_ENABLE_PDF=1)
add_test(NAME PdfDirectoryDeleteScanTest COMMAND PdfDirectoryDeleteScanTest)

add_executable(PdfDirectoryDeleteLegacyPathTest
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDirectoryDeleteScanUnderTest.cpp"
  PdfDirectoryDeleteLegacyPathTest.cpp
)
target_include_directories(PdfDirectoryDeleteLegacyPathTest BEFORE PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${REPO_ROOT}/test/book_move_integration/stubs"
)
target_include_directories(PdfDirectoryDeleteLegacyPathTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
  "${REPO_ROOT}/lib/FsHelpers"
)
target_compile_options(PdfDirectoryDeleteLegacyPathTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
target_compile_definitions(PdfDirectoryDeleteLegacyPathTest PRIVATE
  CROSSINK_ENABLE_PDF=1
)
add_test(NAME PdfDirectoryDeleteLegacyPathTest COMMAND PdfDirectoryDeleteLegacyPathTest)

add_executable(PdfDirectoryDeleteSpoolTest
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDirectoryDeleteScanUnderTest.cpp"
  PdfDirectoryDeleteSpoolTest.cpp
)
target_include_directories(PdfDirectoryDeleteSpoolTest BEFORE PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${REPO_ROOT}/test/book_move_integration/stubs"
)
target_include_directories(PdfDirectoryDeleteSpoolTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
  "${REPO_ROOT}/lib/FsHelpers"
)
target_compile_options(PdfDirectoryDeleteSpoolTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
target_compile_definitions(PdfDirectoryDeleteSpoolTest PRIVATE
  CROSSINK_ENABLE_PDF=1
)
add_test(NAME PdfDirectoryDeleteSpoolTest COMMAND PdfDirectoryDeleteSpoolTest)

add_executable(PdfDirectoryDeleteScanSimulatorTest
  "${CMAKE_CURRENT_BINARY_DIR}/PdfDirectoryDeleteScanUnderTest.cpp"
  PdfDirectoryDeleteScanSimulatorTest.cpp
)
target_include_directories(PdfDirectoryDeleteScanSimulatorTest BEFORE PRIVATE
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${REPO_ROOT}/test/book_move_integration/stubs"
)
target_include_directories(PdfDirectoryDeleteScanSimulatorTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
  "${REPO_ROOT}/lib/FsHelpers"
)
target_compile_definitions(PdfDirectoryDeleteScanSimulatorTest PRIVATE
  CROSSINK_ENABLE_PDF=1
  SIMULATOR=1
)
target_compile_options(PdfDirectoryDeleteScanSimulatorTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
add_test(NAME PdfDirectoryDeleteScanSimulatorTest COMMAND PdfDirectoryDeleteScanSimulatorTest)

find_path(
  ARDUINOJSON_INCLUDE_DIR
  ArduinoJson.h
  PATHS
    "${REPO_ROOT}/.pio/libdeps/simulator/ArduinoJson/src"
    "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src"
  NO_DEFAULT_PATH
  REQUIRED
)

add_executable(RecentBooksDurableDeleteTest
  "${REPO_ROOT}/src/RecentBooksStore.cpp"
  "${REPO_ROOT}/src/util/BookMoveDurableFile.cpp"
  recents_stubs/BookMoveUtils.cpp
  RecentBooksDurableDeleteTest.cpp
)
target_include_directories(RecentBooksDurableDeleteTest BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/recents_stubs"
  "${CMAKE_CURRENT_SOURCE_DIR}/../book_move_integration/stubs"
)
target_include_directories(RecentBooksDurableDeleteTest PRIVATE
  "${REPO_ROOT}/src"
  "${REPO_ROOT}/src/util"
  "${ARDUINOJSON_INCLUDE_DIR}"
)
target_compile_definitions(RecentBooksDurableDeleteTest PRIVATE
  SIMULATOR=1
  PDF_RECENT_DELETE_TESTING=1
  ARDUINOJSON_ENABLE_ARDUINO_STRING=1
)
target_compile_options(RecentBooksDurableDeleteTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
add_test(NAME RecentBooksDurableDeleteTest COMMAND RecentBooksDurableDeleteTest)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(
  NAME PdfDeleteAppSourceContract
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source_contract_test.py"
)
add_test(
  NAME PdfDeleteAppSourceNegativeControls
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/source_contract_negative_control_test.py"
)
