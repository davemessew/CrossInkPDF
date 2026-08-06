cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(book_move_integration_tests LANGUAGES CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
endif()

configure_file(
  "${REPO_ROOT}/src/util/BookMoveUtils.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/BookMoveUtilsUnderTest.cpp"
  COPYONLY
)

add_executable(BookMoveIntegrationTest
  "${REPO_ROOT}/lib/BookStateMigration/BookMoveCoordinator.cpp"
  "${REPO_ROOT}/lib/BookStateMigration/BookStateMigrationJournal.cpp"
  BookMoveCoordinatorTest.cpp
)

target_include_directories(BookMoveIntegrationTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
)

target_compile_options(BookMoveIntegrationTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)

add_test(NAME BookMoveIntegrationTest COMMAND BookMoveIntegrationTest)

add_executable(BookMoveProductTest
  "${REPO_ROOT}/lib/BookStateMigration/BookMoveCoordinator.cpp"
  "${REPO_ROOT}/lib/BookStateMigration/BookStateMigrationJournal.cpp"
  "${CMAKE_CURRENT_BINARY_DIR}/BookMoveUtilsUnderTest.cpp"
  BookMoveProductTest.cpp
)

target_include_directories(BookMoveProductTest BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
target_include_directories(BookMoveProductTest PRIVATE
  "${REPO_ROOT}/lib/BookStateMigration"
  "${REPO_ROOT}/src/util"
)
target_compile_options(BookMoveProductTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
add_test(NAME BookMoveProductTest COMMAND BookMoveProductTest)

add_executable(BookMoveDurableFileTest
  "${REPO_ROOT}/src/util/BookMoveDurableFile.cpp"
  BookMoveDurableFileTest.cpp
)
target_include_directories(BookMoveDurableFileTest BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
)
target_include_directories(BookMoveDurableFileTest PRIVATE
  "${REPO_ROOT}/src/util"
)
target_compile_options(BookMoveDurableFileTest PRIVATE
  -Wall
  -Wextra
  -Werror
  -pedantic
  -fno-exceptions
  -fno-rtti
  -fstack-usage
  -Os
)
add_test(NAME BookMoveDurableFileTest COMMAND BookMoveDurableFileTest)

find_program(NODE_EXECUTABLE NAMES node nodejs)
if(NODE_EXECUTABLE)
  add_test(NAME BookMoveSourceContract
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/source_contract_test.js)
  add_test(NAME BookMovePathFenceNegativeControls
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/path_fence_negative_control_test.js)
endif()
