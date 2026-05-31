cmake_minimum_required(VERSION 3.15)
project(rocksgraph)

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
    add_custom_command(
      TARGET format
      COMMAND ${CMAKE_FORMAT_BIN} -i ${CMAKE_SOURCE_DIR}/CMakeLists.txt
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
      COMMENT "Format CMakeLists.txt with cmake-format."
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

set(GENERATED_SOURCES ast/cypher/CypherLexer.cc ast/cypher/CypherParser.cc
                      ast/cypher/CypherVisitor.cc)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.27")
  set_source_files_properties(${GENERATED_SOURCES} PROPERTIES SKIP_LINTING ON)
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

set(ROCKSGRAPH_AST_SOURCES
  ast/ast_builder.cc
  ast/ast_clone.cc
  ast/ast_const_walker.cc
  ast/ast_equal.cc
  ast/ast_printer.cc
  ast/ast_to_cypher.cc
  ast/expression_to_string.cc
  ast/ast_rewriter.cc
  ast/ast_walker.cc
  ast/semantic_table.cc
  ast/semantic_validator.cc
  ast/rewriters/anonymous_pattern_name_rewriter.cc
  ast/rewriters/uniqueness_predicates_rewriter.cc
  ast/rewriters/comparison_chain_rewriter.cc
  ast/rewriters/count_star_rewriter.cc
  ast/rewriters/existential_subquery_rewriter.cc
  ast/rewriters/order_by_alias_rewriter.cc
  ast/rewriters/parenthesized_expression_rewriter.cc
  ast/rewriters/pattern_predicate_rewriter.cc
  ast/rewriters/pattern_predicate_normalization_rewriter.cc
  ast/rewriters/projection_alias_rewriter.cc
  ast/rewriters/return_star_rewriter.cc
  ast/rewriters/rewriter_registry.cc
  ast/rewriters/rewriter_pipeline.cc
  ${GENERATED_SOURCES})

set(ROCKSGRAPH_IR_SOURCES
  ir/logical_plan.cc
  ir/logical_plan_printer.cc
  ir/planner_query.cc
  ir/planner_query_arguments.cc
  ir/planner_query_builder.cc
  ir/planner_query_graph_builder.cc
  ir/planner_query_model.cc
  ir/planner_query_printer.cc
  ir/planner_query_util.cc
  ir/planner/component_planner.cc
  ir/planner/cost_model.cc
  ir/planner/idp.cc
  ir/planner/logical_plan_builder.cc
  ir/planner/plan_clone.cc)

set(ROCKSGRAPH_STORAGE_SOURCES storage/in_memory_graph.cc)

set(ROCKSGRAPH_RUNTIME_SOURCES runtime/query_executor.cc)

add_library(rocksgraph_ast STATIC ${ROCKSGRAPH_AST_SOURCES})
target_include_directories(rocksgraph_ast PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rocksgraph_ast PUBLIC ANTLR4Runtime::antlr4_runtime)
set_target_properties(rocksgraph_ast PROPERTIES BUILD_RPATH "/usr/local/lib")

add_library(rocksgraph_ir STATIC ${ROCKSGRAPH_IR_SOURCES})
target_include_directories(rocksgraph_ir PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rocksgraph_ir PUBLIC rocksgraph_ast)
set_target_properties(rocksgraph_ir PROPERTIES BUILD_RPATH "/usr/local/lib")

add_library(rocksgraph_value STATIC value/value.cc)
target_include_directories(rocksgraph_value PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_library(rocksgraph_storage STATIC ${ROCKSGRAPH_STORAGE_SOURCES})
target_include_directories(rocksgraph_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rocksgraph_storage PUBLIC rocksgraph_ir
                                                rocksgraph_value)

add_library(rocksgraph_runtime STATIC ${ROCKSGRAPH_RUNTIME_SOURCES})
target_include_directories(rocksgraph_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rocksgraph_runtime PUBLIC rocksgraph_storage
                                                rocksgraph_ir)

add_executable(rg-server main.cc)
target_link_libraries(rg-server PRIVATE rocksgraph_runtime ${GFLAGS_TARGET}
                                        ${SPDLOG_TARGET})
set_target_properties(rg-server PROPERTIES BUILD_RPATH "/usr/local/lib")

add_executable(cypher_dump tools/cypher_dump.cc)
target_link_libraries(cypher_dump PRIVATE rocksgraph_ir ${GFLAGS_TARGET}
                                          ${SPDLOG_TARGET})
set_target_properties(cypher_dump PROPERTIES BUILD_RPATH "/usr/local/lib")

enable_testing()
find_package(GTest REQUIRED)

add_executable(ast_rewriter_test tests/ast_rewriter_test.cc)
target_link_libraries(ast_rewriter_test PRIVATE GTest::gtest_main
                                                rocksgraph_ast)
set_target_properties(ast_rewriter_test PROPERTIES BUILD_RPATH "/usr/local/lib")
add_test(NAME ast_rewriter_test COMMAND ast_rewriter_test)

add_executable(ast_to_cypher_test tests/ast_to_cypher_test.cc)
target_link_libraries(ast_to_cypher_test PRIVATE GTest::gtest_main
                                                 rocksgraph_ast)
set_target_properties(ast_to_cypher_test PROPERTIES BUILD_RPATH
                                                    "/usr/local/lib")
add_test(NAME ast_to_cypher_test COMMAND ast_to_cypher_test)

add_executable(value_test tests/value_test.cc)
target_link_libraries(value_test PRIVATE GTest::gtest_main rocksgraph_value)
add_test(NAME value_test COMMAND value_test)

add_executable(logical_plan_test tests/logical_plan_test.cc)
target_link_libraries(logical_plan_test PRIVATE GTest::gtest_main
                                                rocksgraph_ir)
add_test(NAME logical_plan_test COMMAND logical_plan_test)

add_executable(cost_model_test tests/cost_model_test.cc)
target_link_libraries(cost_model_test PRIVATE GTest::gtest_main rocksgraph_ir)
add_test(NAME cost_model_test COMMAND cost_model_test)

add_executable(idp_test tests/idp_test.cc)
target_link_libraries(idp_test PRIVATE GTest::gtest_main rocksgraph_ir)
add_test(NAME idp_test COMMAND idp_test)

add_executable(logical_plan_builder_test tests/logical_plan_builder_test.cc)
target_link_libraries(logical_plan_builder_test PRIVATE GTest::gtest_main
                                                        rocksgraph_ir)
set_target_properties(logical_plan_builder_test PROPERTIES BUILD_RPATH
                                                           "/usr/local/lib")
add_test(NAME logical_plan_builder_test COMMAND logical_plan_builder_test)

add_executable(ast_node_type_test tests/ast_node_type_test.cc)
target_link_libraries(ast_node_type_test PRIVATE GTest::gtest_main
                                                 rocksgraph_ast)
add_test(NAME ast_node_type_test COMMAND ast_node_type_test)

add_executable(semantic_validator_test tests/semantic_validator_test.cc)
target_link_libraries(semantic_validator_test PRIVATE GTest::gtest_main
                                                      rocksgraph_ast)
set_target_properties(semantic_validator_test PROPERTIES BUILD_RPATH
                                                         "/usr/local/lib")
add_test(NAME semantic_validator_test COMMAND semantic_validator_test)

add_executable(semantic_table_test tests/semantic_table_test.cc)
target_link_libraries(semantic_table_test PRIVATE GTest::gtest_main
                                                 rocksgraph_ast)
set_target_properties(semantic_table_test PROPERTIES BUILD_RPATH
                                                     "/usr/local/lib")
add_test(NAME semantic_table_test COMMAND semantic_table_test)

add_executable(planner_query_test tests/planner_query_test.cc)
target_link_libraries(planner_query_test PRIVATE GTest::gtest_main
                                                 rocksgraph_ir)
set_target_properties(planner_query_test PROPERTIES BUILD_RPATH
                                                    "/usr/local/lib")
add_test(NAME planner_query_test COMMAND planner_query_test)

add_executable(query_executor_test tests/query_executor_test.cc)
target_link_libraries(query_executor_test PRIVATE GTest::gtest_main
                                                  rocksgraph_runtime)
set_target_properties(query_executor_test PROPERTIES BUILD_RPATH
                                                     "/usr/local/lib")
add_test(NAME query_executor_test COMMAND query_executor_test)
