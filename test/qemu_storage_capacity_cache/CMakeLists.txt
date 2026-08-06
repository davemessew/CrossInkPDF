add_executable(QemuStorageCapacityCacheTest
  QemuStorageCapacityCacheTest.cpp
)

target_include_directories(QemuStorageCapacityCacheTest PRIVATE
  ${REPO_ROOT}/test/qemu/hal/src
)

target_link_libraries(QemuStorageCapacityCacheTest PRIVATE
  GTest::gtest_main
)

if(MSVC)
  target_compile_options(QemuStorageCapacityCacheTest PRIVATE /W4)
else()
  target_compile_options(QemuStorageCapacityCacheTest PRIVATE -Wall -Wextra -pedantic)
endif()

gtest_discover_tests(QemuStorageCapacityCacheTest TEST_PREFIX "QemuStorageCapacityCache")
