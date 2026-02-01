cmake_minimum_required(VERSION 3.15)
project(rocksgraph)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose the type of build." FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(GENERATED_SOURCES
        ast/CypherLexer.cc
        ast/CypherParser.cc
        ast/CypherVisitor.cc
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(ANTLR4Runtime REQUIRED)
add_executable(rg-server
    main.cc
        ast/ast_builder.cc
        ast/ast_clone.cc
        ast/ast_equal.cc
        ast/ast_printer.cc
        ast/ast_to_cypher.cc
        ast/ast_rewriter.cc
        ast/ast_walker.cc
        ast/semantic_validator.cc
        ast/rewriters/anonymous_pattern_name_rewriter.cc
        ast/rewriters/comparison_chain_rewriter.cc
        ast/rewriters/count_star_rewriter.cc
        ast/rewriters/existential_subquery_rewriter.cc
        ast/rewriters/order_by_alias_rewriter.cc
        ast/rewriters/parenthesized_expression_rewriter.cc
        ast/rewriters/pattern_predicate_rewriter.cc
        ast/rewriters/projection_alias_rewriter.cc
        ast/rewriters/return_star_rewriter.cc
        ast/rewriters/rewriter_registry.cc
        ast/rewriters/rewriter_pipeline.cc
        value/value.cc
    ${GENERATED_SOURCES}
)
target_link_libraries(rg-server ANTLR4Runtime::antlr4_runtime)
target_include_directories(rg-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(rg-server PROPERTIES BUILD_RPATH "/usr/local/lib")

enable_testing()
find_package(GTest REQUIRED)

add_executable(ast_rewriter_test
    tests/ast_rewriter_test.cc
    ast/ast_builder.cc
    ast/ast_clone.cc
    ast/ast_equal.cc
    ast/ast_printer.cc
    ast/ast_to_cypher.cc
    ast/ast_rewriter.cc
    ast/ast_walker.cc
    ast/semantic_validator.cc
    ast/rewriters/anonymous_pattern_name_rewriter.cc
    ast/rewriters/comparison_chain_rewriter.cc
    ast/rewriters/count_star_rewriter.cc
    ast/rewriters/existential_subquery_rewriter.cc
    ast/rewriters/order_by_alias_rewriter.cc
    ast/rewriters/parenthesized_expression_rewriter.cc
    ast/rewriters/pattern_predicate_rewriter.cc
    ast/rewriters/projection_alias_rewriter.cc
    ast/rewriters/return_star_rewriter.cc
    ast/rewriters/rewriter_registry.cc
    ast/rewriters/rewriter_pipeline.cc
    ${GENERATED_SOURCES}
)
target_include_directories(ast_rewriter_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ast_rewriter_test GTest::gtest_main ANTLR4Runtime::antlr4_runtime)
set_target_properties(ast_rewriter_test PROPERTIES BUILD_RPATH "/usr/local/lib")
add_test(NAME ast_rewriter_test COMMAND ast_rewriter_test)

add_executable(ast_to_cypher_test
    tests/ast_to_cypher_test.cc
    ast/ast_builder.cc
    ast/ast_clone.cc
    ast/ast_equal.cc
    ast/ast_printer.cc
    ast/ast_to_cypher.cc
    ast/ast_rewriter.cc
    ast/ast_walker.cc
    ast/semantic_validator.cc
    ast/rewriters/anonymous_pattern_name_rewriter.cc
    ast/rewriters/comparison_chain_rewriter.cc
    ast/rewriters/count_star_rewriter.cc
    ast/rewriters/existential_subquery_rewriter.cc
    ast/rewriters/order_by_alias_rewriter.cc
    ast/rewriters/parenthesized_expression_rewriter.cc
    ast/rewriters/pattern_predicate_rewriter.cc
    ast/rewriters/projection_alias_rewriter.cc
    ast/rewriters/return_star_rewriter.cc
    ast/rewriters/rewriter_registry.cc
    ast/rewriters/rewriter_pipeline.cc
    ${GENERATED_SOURCES}
)
target_include_directories(ast_to_cypher_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ast_to_cypher_test GTest::gtest_main ANTLR4Runtime::antlr4_runtime)
set_target_properties(ast_to_cypher_test PROPERTIES BUILD_RPATH "/usr/local/lib")
add_test(NAME ast_to_cypher_test COMMAND ast_to_cypher_test)

add_executable(value_test
    tests/value_test.cc
    value/value.cc
)
target_include_directories(value_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(value_test GTest::gtest_main)
add_test(NAME value_test COMMAND value_test)

add_executable(semantic_validator_test
    tests/semantic_validator_test.cc
    ast/ast_builder.cc
    ast/ast_clone.cc
    ast/ast_equal.cc
    ast/ast_printer.cc
    ast/ast_to_cypher.cc
    ast/ast_rewriter.cc
    ast/ast_walker.cc
    ast/semantic_validator.cc
    ast/rewriters/anonymous_pattern_name_rewriter.cc
    ast/rewriters/comparison_chain_rewriter.cc
    ast/rewriters/count_star_rewriter.cc
    ast/rewriters/existential_subquery_rewriter.cc
    ast/rewriters/order_by_alias_rewriter.cc
    ast/rewriters/parenthesized_expression_rewriter.cc
    ast/rewriters/pattern_predicate_rewriter.cc
    ast/rewriters/projection_alias_rewriter.cc
    ast/rewriters/return_star_rewriter.cc
    ast/rewriters/rewriter_registry.cc
    ast/rewriters/rewriter_pipeline.cc
    ${GENERATED_SOURCES}
)
target_include_directories(semantic_validator_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(semantic_validator_test GTest::gtest_main ANTLR4Runtime::antlr4_runtime)
set_target_properties(semantic_validator_test PROPERTIES BUILD_RPATH "/usr/local/lib")
add_test(NAME semantic_validator_test COMMAND semantic_validator_test)
