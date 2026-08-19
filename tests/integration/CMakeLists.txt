set(FIXTURES_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures")

# csvdb_add_unit_test() is defined (as a CMake function, which has global
# scope once processed) by tests/unit/CMakeLists.txt; reused here since
# integration tests link against the same csvdb_core target.
csvdb_add_unit_test(test_executor)
