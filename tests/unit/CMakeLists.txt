set(FIXTURES_DIR "${CMAKE_SOURCE_DIR}/tests/fixtures")

function(csvdb_add_unit_test name)
    add_executable(${name} ${name}.c)
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/src/lib)
    target_link_libraries(${name} PRIVATE csvdb_core)
    target_compile_definitions(${name} PRIVATE FIXTURES_DIR="${FIXTURES_DIR}")
    target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

csvdb_add_unit_test(test_value)
csvdb_add_unit_test(test_schema)
csvdb_add_unit_test(test_storage)
