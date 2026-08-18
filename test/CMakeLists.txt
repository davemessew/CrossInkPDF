cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

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

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(fs_helpers)
add_subdirectory(reflow_document)
add_subdirectory(epub_legacy_href)
add_subdirectory(pdf_reflow_core)
add_subdirectory(pdf_cache_recovery)
add_subdirectory(pdf_extraction)
add_subdirectory(pdf_document_contract)
add_subdirectory(pdf_product_integration)
add_subdirectory(pdf_section_contract)
add_subdirectory(pdf_navigation)
add_subdirectory(pdf_word_progress)
add_subdirectory(pdf_saved_items)
add_subdirectory(pdf_pixel_cache)
add_subdirectory(pdf_format_routing)
add_subdirectory(pdf_open_book_path)
add_subdirectory(pdf_cached_product_state)
add_subdirectory(pdf_cache_clear)
add_subdirectory(book_move_integration)
add_subdirectory(book_state_migration)
add_subdirectory(pdf_book_deletion_core)
add_subdirectory(pdf_delete_app)
add_subdirectory(pdf_book_actions)
add_subdirectory(pdf_home_sleep)
add_subdirectory(pdf_simulator_acceptance)
add_subdirectory(qemu_storage_capacity_cache)
add_subdirectory(web_upload)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(preview_block_locator)
add_subdirectory(text_pool)
add_subdirectory(clip_word_store)
add_subdirectory(dict_layout)
add_subdirectory(reader_progress_save_debouncer)
