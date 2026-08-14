set(ROCKSGRAPH_AST_GENERATED_SOURCES
    cypher/CypherLexer.cc cypher/CypherParser.cc cypher/CypherVisitor.cc)
if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.27")
  set_source_files_properties(${ROCKSGRAPH_AST_GENERATED_SOURCES}
                              PROPERTIES SKIP_LINTING ON)
endif()

add_library(
  rocksgraph_ast STATIC
  ast_builder.cc
  ast_clone.cc
  ast_const_walker.cc
  ast_equal.cc
  ast_printer.cc
  ast_rewriter.cc
  ast_to_cypher.cc
  ast_walker.cc
  builtin_function.cc
  builtin_procedure.cc
  expression_to_string.cc
  semantic_table.cc
  semantic_validator.cc
  rewriters/anonymous_pattern_name_rewriter.cc
  rewriters/comparison_chain_rewriter.cc
  rewriters/count_star_rewriter.cc
  rewriters/existential_subquery_rewriter.cc
  rewriters/order_by_alias_rewriter.cc
  rewriters/parenthesized_expression_rewriter.cc
  rewriters/pattern_predicate_normalization_rewriter.cc
  rewriters/pattern_predicate_rewriter.cc
  rewriters/projection_alias_rewriter.cc
  rewriters/return_star_rewriter.cc
  rewriters/rewriter_pipeline.cc
  rewriters/rewriter_registry.cc
  rewriters/uniqueness_predicates_rewriter.cc
  ${ROCKSGRAPH_AST_GENERATED_SOURCES})

target_include_directories(rocksgraph_ast PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_ast PUBLIC ANTLR4Runtime::antlr4_runtime)
set_target_properties(rocksgraph_ast PROPERTIES BUILD_RPATH "/usr/local/lib")
