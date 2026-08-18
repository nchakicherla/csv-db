cmake_minimum_required(VERSION 3.15)
project(csvdb C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# --- cJSON: FetchContent from upstream (it ships its own working CMakeLists.txt) ---
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)
# cJSON v1.7.19's own cmake_minimum_required predates policies CMake 4+ requires opting into.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_Declare(
    cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.19
)
FetchContent_MakeAvailable(cjson)

# --- vendored third-party libraries ---
# Kept as separate static-lib targets rather than compiled directly into
# csvdb_core: libcsv is LGPL-2.1, and this keeps it a distinct, swappable
# unit for relinking purposes (see third_party/VERSIONS.md).
add_library(libcsv_vendor STATIC third_party/libcsv/csv.c)
target_include_directories(libcsv_vendor PUBLIC third_party/libcsv)

add_library(linenoise_vendor STATIC third_party/linenoise/linenoise.c)
target_include_directories(linenoise_vendor PUBLIC third_party/linenoise)

# --- libcsvdb core library ---
add_library(csvdb_core STATIC
    src/lib/csvdb.c
    src/lib/value.c
    src/lib/schema.c
    src/lib/storage.c
    src/lib/lock.c
    src/lib/catalog.c
)
target_include_directories(csvdb_core PUBLIC include)
# cJSON's own CMakeLists doesn't export its include dir via the target (it
# relies on `install()` for that), so add it directly for FetchContent's
# build-tree usage.
target_include_directories(csvdb_core PRIVATE ${cjson_SOURCE_DIR})
target_link_libraries(csvdb_core PUBLIC libcsv_vendor cjson)
target_compile_options(csvdb_core PRIVATE -Wall -Wextra -Werror)

# --- csvdb executable (CLI + REPL entry point) ---
add_executable(csvdb
    src/cli/main.c
)
target_link_libraries(csvdb PRIVATE csvdb_core linenoise_vendor)
target_compile_options(csvdb PRIVATE -Wall -Wextra -Werror)

enable_testing()
add_subdirectory(tests)
