cmake_minimum_required(VERSION 3.15)
project(rocksgraph)

set(ROCKSGRAPH_TCK_DIR
    "${CMAKE_SOURCE_DIR}/../openCypher/tck"
    CACHE PATH "Path to an openCypher TCK checkout")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE
      "Debug"
      CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
if(TARGET gflags::gflags)
  set(GFLAGS_TARGET gflags::gflags)
else()
  set(GFLAGS_TARGET gflags)
endif()
if(TARGET spdlog::spdlog)
  set(SPDLOG_TARGET spdlog::spdlog)
else()
  set(SPDLOG_TARGET spdlog)
endif()

add_subdirectory(ast)
add_subdirectory(ir)
add_subdirectory(planner)
add_subdirectory(value)
add_subdirectory(storage)
add_subdirectory(runtime)
add_subdirectory(tools)

enable_testing()
find_package(GTest REQUIRED)
add_subdirectory(tests)
