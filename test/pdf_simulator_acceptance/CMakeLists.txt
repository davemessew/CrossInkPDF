cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_simulator_acceptance_tests LANGUAGES CXX)
  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  enable_testing()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(
  PdfAcceptanceBehaviorTest
  PdfAcceptanceBehaviorTest.cpp
)
target_compile_features(PdfAcceptanceBehaviorTest PRIVATE cxx_std_20)
target_compile_definitions(PdfAcceptanceBehaviorTest PRIVATE SIMULATOR=1)
target_include_directories(
  PdfAcceptanceBehaviorTest
  PRIVATE
    "${REPO_ROOT}/src"
    "${REPO_ROOT}/lib/I18n"
    "${REPO_ROOT}/lib/PdfReflow"
)

if(MSVC)
  target_compile_options(PdfAcceptanceBehaviorTest PRIVATE /W4 /WX)
else()
  target_compile_options(
    PdfAcceptanceBehaviorTest
    PRIVATE -Wall -Wextra -Werror -pedantic -fno-exceptions -fno-rtti
  )
endif()

add_test(
  NAME PdfSimulatorAcceptance.BehaviorControls
  COMMAND PdfAcceptanceBehaviorTest
)

add_test(
  NAME PdfSimulatorAcceptance.SourceAndMutationContracts
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_pdf_simulator_acceptance.py" -v
)
set_tests_properties(
  PdfSimulatorAcceptance.SourceAndMutationContracts
  PROPERTIES WORKING_DIRECTORY "${REPO_ROOT}"
)

add_test(
  NAME PdfSimulatorAcceptance.RealActivityUiContracts
  COMMAND
    "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/test_pdf_ui_acceptance.py" -v
)
set_tests_properties(
  PdfSimulatorAcceptance.RealActivityUiContracts
  PROPERTIES WORKING_DIRECTORY "${REPO_ROOT}"
)
