# ============================================================================
# LiteQuery — top-level build
#
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
#
# Targets:
#   litequery      static library (link this + include/litequery/litequery.h)
#   lq_tests       C++ unit/integration test suite   (ctest: unit)
#   lq_capi_test   pure-C test using only the public header (ctest: capi)
#   lq_demo        small CLI demo
# ============================================================================

cmake_minimum_required(VERSION 3.16)
project(LiteQuery VERSION 0.2.0 LANGUAGES C CXX)

# ---- Standards -------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- Options ---------------------------------------------------------------
option(LITEQUERY_BUILD_TESTS "Build the test suites"          ON)
option(LITEQUERY_BUILD_DEMO  "Build the CLI demo"            ON)
option(LITEQUERY_BUILD_SHELL "Build the lq interactive shell" ON)
option(LITEQUERY_BUILD_BENCH "Build the benchmark harness"    ON)
option(LITEQUERY_BUILD_SQLITE_BENCH "Build the LiteQuery-vs-SQLite benchmark (compiles vendored SQLite)" OFF)
option(LITEQUERY_ASAN        "Build with AddressSanitizer/UBSan" OFF)

# ---- Warnings / sanitizers -------------------------------------------------
add_library(lq_flags INTERFACE)
if(MSVC)
  target_compile_options(lq_flags INTERFACE /W4 /permissive-)
else()
  target_compile_options(lq_flags INTERFACE -Wall -Wextra)
  if(LITEQUERY_ASAN)
    target_compile_options(lq_flags INTERFACE -fsanitize=address,undefined -g)
    target_link_options(lq_flags INTERFACE -fsanitize=address,undefined)
  endif()
endif()

# ---- The library -----------------------------------------------------------
set(LITEQUERY_SOURCES
  src/parser/lexer.cpp
  src/parser/parser.cpp
  src/planner/logical_plan.cpp
  src/planner/optimizer.cpp
  src/execution/eval.cpp
  src/execution/physical_plan.cpp
  src/execution/fast_aggregate.cpp
  src/storage/csv_reader.cpp
  src/storage/persistence.cpp
  src/api/connection.cpp
  src/api/c_api.cpp
)

add_library(litequery STATIC ${LITEQUERY_SOURCES})

# Public include dir (the C header) is exposed to consumers; internal headers
# are visible only to the library's own translation units.
target_include_directories(litequery
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/litequery
    ${CMAKE_CURRENT_SOURCE_DIR}/src/parser
    ${CMAKE_CURRENT_SOURCE_DIR}/src/planner
    ${CMAKE_CURRENT_SOURCE_DIR}/src/catalog
    ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
    ${CMAKE_CURRENT_SOURCE_DIR}/src/execution
    ${CMAKE_CURRENT_SOURCE_DIR}/src/api
)
# Warning/sanitizer flags are build-only: wrap them in BUILD_INTERFACE so they
# never leak into the exported/installed target's usage requirements.
target_link_libraries(litequery PRIVATE $<BUILD_INTERFACE:lq_flags>)

# The internal C++ headers are needed by tests/demo that use the C++ API.
set(LITEQUERY_INTERNAL_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/include/litequery
  ${CMAKE_CURRENT_SOURCE_DIR}/src/parser
  ${CMAKE_CURRENT_SOURCE_DIR}/src/planner
  ${CMAKE_CURRENT_SOURCE_DIR}/src/catalog
  ${CMAKE_CURRENT_SOURCE_DIR}/src/storage
  ${CMAKE_CURRENT_SOURCE_DIR}/src/execution
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api
)

# ---- Shared library (for the Python/ctypes bindings and other FFI users) ----
# Exposes the pure-C API (litequery.h) from a single .dll/.so/.dylib that any
# language with a C FFI can dlopen — independent of how the host runtime was
# compiled (important on Windows, where the Python is MSVC-built but this may be
# compiled with mingw GCC).
option(LITEQUERY_BUILD_SHARED "Build the shared library (C API)" ON)
if(LITEQUERY_BUILD_SHARED)
  add_library(litequery_shared SHARED ${LITEQUERY_SOURCES})
  target_include_directories(litequery_shared
    PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${LITEQUERY_INTERNAL_INCLUDES})
  target_link_libraries(litequery_shared PRIVATE $<BUILD_INTERFACE:lq_flags>)
  # A stable output name: liblitequery.so / liblitequery.dll / liblitequery.dylib
  set_target_properties(litequery_shared PROPERTIES
    OUTPUT_NAME litequery
    WINDOWS_EXPORT_ALL_SYMBOLS ON)   # export the extern "C" API for ctypes
  # Statically link the compiler runtime so the shared library is self-contained
  # — it can then be loaded by any host (e.g. an MSVC-built Python via ctypes)
  # without the toolchain's runtime DLLs needing to be on the search path.
  if(MINGW OR (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND WIN32))
    target_link_options(litequery_shared PRIVATE
      -static -static-libgcc -static-libstdc++)
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(litequery_shared PRIVATE -static-libgcc -static-libstdc++)
  endif()
  install(TARGETS litequery_shared
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

# ---- Tests -----------------------------------------------------------------
if(LITEQUERY_BUILD_TESTS)
  enable_testing()

  add_executable(lq_tests tests/test_litequery.cpp)
  target_include_directories(lq_tests PRIVATE
    ${LITEQUERY_INTERNAL_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_link_libraries(lq_tests PRIVATE litequery lq_flags)
  add_test(NAME unit COMMAND lq_tests)

  # Pure-C test: compiled as C, links the C++ library.
  add_executable(lq_capi_test tests/test_capi.c)
  target_link_libraries(lq_capi_test PRIVATE litequery)
  # Ensure the C++ runtime is linked even though the driver TU is C.
  set_target_properties(lq_capi_test PROPERTIES LINKER_LANGUAGE CXX)
  add_test(NAME capi COMMAND lq_capi_test)
endif()

# ---- Demo ------------------------------------------------------------------
if(LITEQUERY_BUILD_DEMO)
  add_executable(lq_demo examples/demo.cpp)
  target_include_directories(lq_demo PRIVATE ${LITEQUERY_INTERNAL_INCLUDES})
  target_link_libraries(lq_demo PRIVATE litequery lq_flags)
endif()

# ---- Interactive shell (the `lq` binary) -----------------------------------
if(LITEQUERY_BUILD_SHELL)
  add_executable(lq tools/lq_shell.cpp)
  target_include_directories(lq PRIVATE ${LITEQUERY_INTERNAL_INCLUDES})
  target_link_libraries(lq PRIVATE litequery lq_flags)
  install(TARGETS lq RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---- Benchmark harness -----------------------------------------------------
if(LITEQUERY_BUILD_BENCH)
  add_executable(lq_bench bench/bench.cpp)
  target_include_directories(lq_bench PRIVATE ${LITEQUERY_INTERNAL_INCLUDES})
  target_link_libraries(lq_bench PRIVATE litequery lq_flags)
endif()

# ---- LiteQuery vs SQLite comparison (opt-in; compiles vendored SQLite) ------
if(LITEQUERY_BUILD_SQLITE_BENCH)
  # SQLite amalgamation as its own static lib (its warnings are not ours).
  add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
  target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite)
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=0 SQLITE_DEFAULT_MEMSTATUS=0)
  if(NOT MSVC)
    target_compile_options(sqlite3 PRIVATE -w -O2)
  endif()
  # Some SQLite features need dl/pthread on Unix.
  find_package(Threads QUIET)
  if(Threads_FOUND)
    target_link_libraries(sqlite3 PUBLIC Threads::Threads)
  endif()

  add_executable(lq_vs_sqlite bench/bench_sqlite.cpp)
  target_include_directories(lq_vs_sqlite PRIVATE ${LITEQUERY_INTERNAL_INCLUDES})
  target_link_libraries(lq_vs_sqlite PRIVATE litequery sqlite3 lq_flags)
endif()

# ---- Install & package export ----------------------------------------------
# Lets downstream projects do:
#     find_package(LiteQuery REQUIRED)
#     target_link_libraries(app PRIVATE LiteQuery::litequery)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Public consumers get the include/ directory; drop the INTERFACE flags target
# from the exported interface (it is a build-tree private detail).
install(TARGETS litequery
  EXPORT LiteQueryTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES include/litequery/litequery.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/litequery)

# Export the target under the LiteQuery:: namespace.
install(EXPORT LiteQueryTargets
  FILE LiteQueryTargets.cmake
  NAMESPACE LiteQuery::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiteQuery)

# Generate LiteQueryConfig.cmake and a version file.
configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LiteQueryConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/LiteQueryConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiteQuery)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/LiteQueryConfigVersion.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion)
install(FILES
  ${CMAKE_CURRENT_BINARY_DIR}/LiteQueryConfig.cmake
  ${CMAKE_CURRENT_BINARY_DIR}/LiteQueryConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LiteQuery)
