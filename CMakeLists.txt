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
add_executable(
  rg-server
  main.cc
  ast/ast_builder.cc
  ast/ast_clone.cc
  ast/ast_const_walker.cc
  ast/ast_equal.cc
  ast/ast_printer.cc
  ast/ast_to_cypher.cc
  ast/expression_to_string.cc
  ast/ast_rewriter.cc
  ast/ast_walker.cc
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
  value/value.cc
  ${GENERATED_SOURCES})
target_link_libraries(rg-server ANTLR4Runtime::antlr4_runtime)
target_include_directories(rg-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(rg-server PROPERTIES BUILD_RPATH "/usr/local/lib")

add_executable(
  cypher_dump
  tools/cypher_dump.cc
  ast/ast_builder.cc
  ast/ast_clone.cc
  ast/ast_const_walker.cc
  ast/ast_equal.cc
  ast/ast_printer.cc
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
  ir/planner_query.cc
  ir/planner_query_arguments.cc
  ir/planner_query_builder.cc
  ir/planner_query_graph_builder.cc
  ir/planner_query_model.cc
  ir/planner_query_util.cc
  ir/planner_query_printer.cc
  ${GENERATED_SOURCES})
target_include_directories(cypher_dump PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cypher_dump ANTLR4Runtime::antlr4_runtime
                      ${GFLAGS_TARGET} ${SPDLOG_TARGET})
set_target_properties(cypher_dump PROPERTIES BUILD_RPATH "/usr/local/lib")

enable_testing()
find_package(GTest REQUIRED)

add_executable(
  ast_rewriter_test
  tests/ast_rewriter_test.cc
  ast/ast_builder.cc
  ast/ast_clone.cc
  ast/ast_const_walker.cc
  ast/ast_equal.cc
  ast/ast_printer.cc
  ast/ast_to_cypher.cc
  ast/expression_to_string.cc
  ast/ast_rewriter.cc
  ast/ast_walker.cc
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
target_include_directories(ast_rewriter_test
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ast_rewriter_test GTest::gtest_main
                      ANTLR4Runtime::antlr4_runtime)
set_target_properties(ast_rewriter_test PROPERTIES BUILD_RPATH "/usr/local/lib")
add_test(NAME ast_rewriter_test COMMAND ast_rewriter_test)

add_executable(
  ast_to_cypher_test
  tests/ast_to_cypher_test.cc
  ast/ast_builder.cc
  ast/ast_clone.cc
  ast/ast_const_walker.cc
  ast/ast_equal.cc
  ast/ast_printer.cc
  ast/ast_to_cypher.cc
  ast/expression_to_string.cc
  ast/ast_rewriter.cc
  ast/ast_walker.cc
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
target_include_directories(ast_to_cypher_test
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ast_to_cypher_test GTest::gtest_main
                      ANTLR4Runtime::antlr4_runtime)
set_target_properties(ast_to_cypher_test PROPERTIES BUILD_RPATH
                                                    "/usr/local/lib")
add_test(NAME ast_to_cypher_test COMMAND ast_to_cypher_test)

add_executable(value_test tests/value_test.cc value/value.cc)
target_include_directories(value_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(value_test GTest::gtest_main)
add_test(NAME value_test COMMAND value_test)

add_executable(logical_plan_test tests/logical_plan_test.cc ir/logical_plan.cc)
target_include_directories(logical_plan_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(logical_plan_test GTest::gtest_main)
add_test(NAME logical_plan_test COMMAND logical_plan_test)

add_executable(ast_node_type_test tests/ast_node_type_test.cc)
target_include_directories(ast_node_type_test
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ast_node_type_test GTest::gtest_main)
add_test(NAME ast_node_type_test COMMAND ast_node_type_test)

add_executable(
  semantic_validator_test
  tests/semantic_validator_test.cc
  ast/ast_builder.cc
  ast/ast_clone.cc
  ast/ast_const_walker.cc
  ast/ast_equal.cc
  ast/ast_printer.cc
  ast/ast_to_cypher.cc
  ast/expression_to_string.cc
  ast/ast_rewriter.cc
  ast/ast_walker.cc
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
target_include_directories(semantic_validator_test
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(semantic_validator_test GTest::gtest_main
                      ANTLR4Runtime::antlr4_runtime)
set_target_properties(semantic_validator_test PROPERTIES BUILD_RPATH
                                                         "/usr/local/lib")
add_test(NAME semantic_validator_test COMMAND semantic_validator_test)

add_executable(
  semantic_table_test
  tests/semantic_table_test.cc
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
target_include_directories(semantic_table_test
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(semantic_table_test GTest::gtest_main
                      ANTLR4Runtime::antlr4_runtime)
set_target_properties(semantic_table_test PROPERTIES BUILD_RPATH
                                                     "/usr/local/lib")
add_test(NAME semantic_table_test COMMAND semantic_table_test)

add_executable(
  planner_query_test
  tests/planner_query_test.cc
  ir/planner_query.cc
  ir/planner_query_arguments.cc
  ir/planner_query_builder.cc
  ir/planner_query_graph_builder.cc
  ir/planner_query_model.cc
  ir/planner_query_util.cc
  ir/planner_query_printer.cc
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
target_include_directories(planner_query_test
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(planner_query_test GTest::gtest_main
                      ANTLR4Runtime::antlr4_runtime)
set_target_properties(planner_query_test PROPERTIES BUILD_RPATH
                                                    "/usr/local/lib")
add_test(NAME planner_query_test COMMAND planner_query_test)
