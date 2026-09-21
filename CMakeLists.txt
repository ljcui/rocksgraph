cmake_minimum_required(VERSION 3.15)
project(rocksgraph)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      "Debug"
      CACHE STRING "Choose the type of build." FORCE)
endif()

# Default to shared libraries for debug builds (faster incremental linking) and
# static libraries otherwise. Override with -DBUILD_SHARED_LIBS=ON/OFF.
if(NOT DEFINED BUILD_SHARED_LIBS)
  string(TOLOWER "${CMAKE_BUILD_TYPE}" ROCKSGRAPH_BUILD_TYPE_LOWER)
  if(ROCKSGRAPH_BUILD_TYPE_LOWER STREQUAL "debug")
    set(BUILD_SHARED_LIBS ON)
  else()
    set(BUILD_SHARED_LIBS OFF)
  endif()
  unset(ROCKSGRAPH_BUILD_TYPE_LOWER)
endif()

# Prefer static third-party libraries when the project itself is static, so
# release binaries carry no runtime .so dependencies.
if(NOT BUILD_SHARED_LIBS)
  set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
  set(ROCKSGRAPH_LIBRARY_KIND "static")
else()
  set(ROCKSGRAPH_LIBRARY_KIND "shared")
endif()

# find_library caches absolute paths, so switching the library kind in an
# existing build directory would silently mix static and shared lookups.
if(DEFINED ROCKSGRAPH_LIBRARY_KIND_CACHE
   AND NOT ROCKSGRAPH_LIBRARY_KIND_CACHE STREQUAL "${ROCKSGRAPH_LIBRARY_KIND}")
  message(
    FATAL_ERROR
      "Library kind changed from ${ROCKSGRAPH_LIBRARY_KIND_CACHE} to"
      " ${ROCKSGRAPH_LIBRARY_KIND}; reconfigure from a clean build directory"
      " (delete CMakeCache.txt and CMakeFiles/) or pass"
      " -DBUILD_SHARED_LIBS explicitly.")
endif()
set(ROCKSGRAPH_LIBRARY_KIND_CACHE
    "${ROCKSGRAPH_LIBRARY_KIND}"
    CACHE INTERNAL "Library kind of the previous configure.")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

execute_process(
  COMMAND git rev-parse --abbrev-ref HEAD
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE GIT_BRANCH
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(
  COMMAND git log -1 --format=%h
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  OUTPUT_VARIABLE GIT_COMMIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set(CXX_COMPILER_ID "${CMAKE_CXX_COMPILER_ID}")
set(CXX_COMPILER_VERSION "${CMAKE_CXX_COMPILER_VERSION}")
set(BUILD_TYPE "${CMAKE_BUILD_TYPE}")
configure_file(common/version.h.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/common/version.h" @ONLY)

set(ROCKSGRAPH_CLANG_TIDY_FILE_REGEX "^(?!.*ast/cypher/).*$")
find_program(RUN_CLANG_TIDY_BIN NAMES run-clang-tidy run-clang-tidy.py)
if(RUN_CLANG_TIDY_BIN)
  add_custom_target(
    clang_tidy
    COMMAND ${RUN_CLANG_TIDY_BIN} -p ${CMAKE_BINARY_DIR} -j 8
            ${ROCKSGRAPH_CLANG_TIDY_FILE_REGEX}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run clang-tidy checks (excluding ast/cypher)."
    VERBATIM)
  add_custom_target(
    clang_tidy_fix
    COMMAND ${RUN_CLANG_TIDY_BIN} -p ${CMAKE_BINARY_DIR} -j 8 -fix -format
            ${ROCKSGRAPH_CLANG_TIDY_FILE_REGEX}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Run clang-tidy and apply automatic fixes (excluding ast/cypher)."
    VERBATIM)
else()
  message(
    STATUS
      "run-clang-tidy not found, clang_tidy and clang_tidy_fix targets are disabled."
  )
endif()

find_program(CLANG_FORMAT_BIN NAMES clang-format)
find_program(CMAKE_FORMAT_BIN NAMES cmake-format)
if(CLANG_FORMAT_BIN OR CMAKE_FORMAT_BIN)
  add_custom_target(format COMMENT "Format project sources.")

  if(CLANG_FORMAT_BIN)
    file(GLOB_RECURSE ROCKSGRAPH_FORMAT_FILES CONFIGURE_DEPENDS
         ${CMAKE_SOURCE_DIR}/*.h ${CMAKE_SOURCE_DIR}/*.cc)
    list(FILTER ROCKSGRAPH_FORMAT_FILES EXCLUDE REGEX
         "^${CMAKE_SOURCE_DIR}/(build|cmake-build-debug)/")
    list(FILTER ROCKSGRAPH_FORMAT_FILES EXCLUDE REGEX
         "^${CMAKE_SOURCE_DIR}/ast/cypher/Cypher.*\\.(cc|h)$")
    list(FILTER ROCKSGRAPH_FORMAT_FILES EXCLUDE REGEX
         "^${CMAKE_SOURCE_DIR}/(proto|etcd_raft/raftpb)/.*\\.pb\\.(cc|h)$")
    list(FILTER ROCKSGRAPH_FORMAT_FILES EXCLUDE REGEX
         "^${CMAKE_SOURCE_DIR}/graphdb/ftindex/include/lib\\.rs\\.h$")
    list(FILTER ROCKSGRAPH_FORMAT_FILES EXCLUDE REGEX
         "^${CMAKE_SOURCE_DIR}/tools/linenoise/")
    add_custom_command(
      TARGET format
      COMMAND ${CLANG_FORMAT_BIN} -i -style=file ${ROCKSGRAPH_FORMAT_FILES}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Format C++ sources with clang-format."
      VERBATIM)
  else()
    message(STATUS "clang-format not found, C/C++ formatting is skipped.")
  endif()

  if(CMAKE_FORMAT_BIN)
    file(GLOB_RECURSE ROCKSGRAPH_CMAKE_FILES CONFIGURE_DEPENDS
         ${CMAKE_SOURCE_DIR}/CMakeLists.txt)
    list(FILTER ROCKSGRAPH_CMAKE_FILES EXCLUDE REGEX
         "^${CMAKE_SOURCE_DIR}/(build|cmake-build-debug)/")
    add_custom_command(
      TARGET format
      COMMAND ${CMAKE_FORMAT_BIN} -i ${ROCKSGRAPH_CMAKE_FILES}
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Format CMakeLists.txt files with cmake-format."
      VERBATIM)
  else()
    message(
      STATUS "cmake-format not found, CMakeLists.txt formatting is skipped.")
  endif()
else()
  message(
    STATUS "clang-format and cmake-format not found, format target is disabled."
  )
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(ANTLR4Runtime REQUIRED)
find_package(gflags REQUIRED)
find_package(spdlog REQUIRED)
find_package(RocksDB REQUIRED)
find_package(OpenMP REQUIRED)

if(BUILD_SHARED_LIBS)
  if(TARGET RocksDB::rocksdb-shared)
    set(ROCKSDB_TARGET RocksDB::rocksdb-shared)
  else()
    set(ROCKSDB_TARGET RocksDB::rocksdb)
  endif()
  set(ROCKSGRAPH_OPENMP_CXX OpenMP::OpenMP_CXX)
else()
  # The static RocksDB target hard-codes gflags::gflags_shared in its interface;
  # mirror the target with static gflags so the final binaries do not pull in
  # the shared gflags library.
  find_library(ROCKSDB_STATIC_LIBRARY NAMES rocksdb REQUIRED)
  get_target_property(ROCKSDB_STATIC_INCLUDE_DIR RocksDB::rocksdb
                      INTERFACE_INCLUDE_DIRECTORIES)
  add_library(rocksgraph_rocksdb_static STATIC IMPORTED)
  set_target_properties(
    rocksgraph_rocksdb_static
    PROPERTIES
      IMPORTED_LOCATION "${ROCKSDB_STATIC_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${ROCKSDB_STATIC_INCLUDE_DIR}"
      INTERFACE_LINK_LIBRARIES
      "Threads::Threads;\$<LINK_ONLY:JeMalloc::JeMalloc>;\$<LINK_ONLY:gflags::gflags_static>;\$<LINK_ONLY:lz4::lz4>;\$<LINK_ONLY:zstd::zstd>"
  )
  set(ROCKSDB_TARGET rocksgraph_rocksdb_static)

  # OpenMP::OpenMP_CXX links the shared libgomp; use GCC's static libgomp.a
  # instead (libfaiss.a and benchmark code reference OpenMP symbols).
  execute_process(
    COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libgomp.a
    OUTPUT_VARIABLE ROCKSGRAPH_GOMP_STATIC_LIBRARY
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  add_library(rocksgraph_openmp_static INTERFACE)
  target_compile_options(rocksgraph_openmp_static INTERFACE -fopenmp)
  target_link_libraries(rocksgraph_openmp_static
                        INTERFACE "${ROCKSGRAPH_GOMP_STATIC_LIBRARY}")
  set(ROCKSGRAPH_OPENMP_CXX rocksgraph_openmp_static)
endif()

if(TARGET gflags::gflags_static AND NOT BUILD_SHARED_LIBS)
  set(GFLAGS_TARGET gflags::gflags_static)
elseif(TARGET gflags::gflags)
  set(GFLAGS_TARGET gflags::gflags)
else()
  set(GFLAGS_TARGET gflags)
endif()

if(BUILD_SHARED_LIBS AND TARGET spdlog::spdlog)
  set(SPDLOG_TARGET spdlog::spdlog)
elseif(BUILD_SHARED_LIBS)
  set(SPDLOG_TARGET spdlog)
else()
  # No libspdlog.a is installed; fall back to header-only spdlog with a
  # header-only external fmt (FMT_HEADER_ONLY) to avoid libspdlog/libfmt.
  add_library(rocksgraph_spdlog_static INTERFACE)
  target_compile_definitions(rocksgraph_spdlog_static
                             INTERFACE SPDLOG_FMT_EXTERNAL FMT_HEADER_ONLY)
  target_link_libraries(rocksgraph_spdlog_static INTERFACE Threads::Threads)
  set(SPDLOG_TARGET rocksgraph_spdlog_static)
endif()

if(NOT BUILD_SHARED_LIBS)
  # Static C++ runtime for fully static release binaries; glibc stays dynamic.
  add_link_options(-static-libstdc++ -static-libgcc)
endif()

add_subdirectory(common)
add_subdirectory(value)
add_subdirectory(bolt)
add_subdirectory(etcd_raft)
add_subdirectory(proto)
add_subdirectory(raft_driver)
add_subdirectory(graphdb)

add_subdirectory(ast)
add_subdirectory(ir)
add_subdirectory(planner)
add_subdirectory(runtime)
add_subdirectory(server)
add_subdirectory(tools)

enable_testing()
find_package(GTest REQUIRED)
add_subdirectory(tests)
