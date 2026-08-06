cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pdf_home_sleep_tests C CXX)
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

add_executable(PdfHomeSleepTest
  ${REPO_ROOT}/lib/PdfReflow/PdfIo.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLexer.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfObjectParser.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSourceIdentity.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCacheManifest.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfMetadataStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfOutline.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfProgressStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSemanticWriter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfUnicode.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfWordCounter.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfLayoutWordIndex.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfCachedProductState.cpp
  ${REPO_ROOT}/src/activities/home/RecentBookProgress.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/PdfSleepPageCache.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepCoverAssets.cpp
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  ${REPO_ROOT}/test/pdf_reflow_core/PdfTestIo.cpp
  PdfHomeSleepProductTest.cpp
)

target_include_directories(PdfHomeSleepTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/home
  ${REPO_ROOT}/src/activities/boot_sleep
  ${REPO_ROOT}/test/pdf_cache_recovery
  ${REPO_ROOT}/test/pdf_reflow_core
)
target_compile_definitions(PdfHomeSleepTest PRIVATE CROSSINK_ENABLE_PDF=1)

if(MSVC)
  target_compile_options(PdfHomeSleepTest PRIVATE /W4)
else()
  target_compile_options(PdfHomeSleepTest PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

target_link_libraries(PdfHomeSleepTest PRIVATE GTest::gtest_main)

add_library(PdfSleepFeatureOffObject OBJECT
  ${REPO_ROOT}/src/activities/boot_sleep/PdfSleepPageCache.cpp
)
target_include_directories(PdfSleepFeatureOffObject PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Reflow
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/boot_sleep
)
target_compile_definitions(PdfSleepFeatureOffObject PRIVATE CROSSINK_ENABLE_PDF=0)

add_library(PdfHomeProgressFeatureOffObject OBJECT
  ${REPO_ROOT}/src/activities/home/RecentBookProgress.cpp
)
target_include_directories(PdfHomeProgressFeatureOffObject PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Xtc
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/home
)
target_compile_definitions(PdfHomeProgressFeatureOffObject PRIVATE CROSSINK_ENABLE_PDF=0)

add_executable(PdfSleepFeatureOffLink
  PdfSleepFeatureDisabledProbe.cpp
  $<TARGET_OBJECTS:PdfSleepFeatureOffObject>
  $<TARGET_OBJECTS:PdfHomeProgressFeatureOffObject>
)
target_include_directories(PdfSleepFeatureOffLink PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/boot_sleep
  ${REPO_ROOT}/src/activities/home
)
target_compile_definitions(PdfSleepFeatureOffLink PRIVATE CROSSINK_ENABLE_PDF=0)
if(NOT MSVC)
  target_compile_options(PdfSleepFeatureOffObject PRIVATE -ffunction-sections -fdata-sections)
  target_compile_options(PdfHomeProgressFeatureOffObject PRIVATE -ffunction-sections -fdata-sections)
  target_link_options(PdfSleepFeatureOffLink PRIVATE -Wl,--gc-sections)
endif()

gtest_discover_tests(PdfHomeSleepTest TEST_PREFIX "PdfHomeSleep.")
add_test(NAME PdfHomeSleep.PdfFeatureOffLink COMMAND PdfSleepFeatureOffLink)
if(CMAKE_NM)
  add_test(
    NAME PdfHomeSleep.PdfFeatureOffSymbols
    COMMAND ${CMAKE_COMMAND}
            -DNM=${CMAKE_NM}
            -DSLEEP_OBJECT=$<TARGET_OBJECTS:PdfSleepFeatureOffObject>
            -DHOME_OBJECT=$<TARGET_OBJECTS:PdfHomeProgressFeatureOffObject>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check_pdf_home_disabled.cmake
  )
endif()

find_program(NODE_EXECUTABLE node)
if(NODE_EXECUTABLE)
  add_test(NAME PdfHomeSleep.SourceContract
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/source_contract_test.js)
  add_test(NAME PdfHomeSleep.SourceContractNegativeControls
           COMMAND ${NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/source_contract_negative_control_test.js)
endif()
