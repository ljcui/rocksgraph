function(add_rocksgraph_test target source library)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE GTest::gtest_main ${library})
  set_target_properties(${target} PROPERTIES BUILD_RPATH "/usr/local/lib")
  add_test(NAME ${target} COMMAND ${target})
endfunction()

add_rocksgraph_test(ast_node_type_test ast/ast_node_type_test.cc rocksgraph_ast)
add_rocksgraph_test(ast_rewriter_test ast/ast_rewriter_test.cc rocksgraph_ast)
add_rocksgraph_test(ast_to_cypher_test ast/ast_to_cypher_test.cc rocksgraph_ast)
add_rocksgraph_test(semantic_table_test ast/semantic_table_test.cc
                    rocksgraph_ast)
add_rocksgraph_test(semantic_validator_test ast/semantic_validator_test.cc
                    rocksgraph_ast)

add_rocksgraph_test(logical_plan_test ir/logical_plan_test.cc rocksgraph_ir)
add_rocksgraph_test(query_ir_test ir/query_ir_test.cc rocksgraph_ir)

add_rocksgraph_test(cost_model_test planner/cost_model_test.cc
                    rocksgraph_planner)
add_rocksgraph_test(idp_test planner/idp_test.cc rocksgraph_planner)
add_rocksgraph_test(logical_plan_builder_test
                    planner/logical_plan_builder_test.cc rocksgraph_planner)

add_rocksgraph_test(aggregation_evaluator_test
                    runtime/aggregation_evaluator_test.cc rocksgraph_runtime)
add_rocksgraph_test(apply_executor_test runtime/apply_executor_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(
  builtin_function_evaluator_test runtime/builtin_function_evaluator_test.cc
  rocksgraph_runtime)
add_rocksgraph_test(expression_evaluator_test
                    runtime/expression_evaluator_test.cc rocksgraph_runtime)
add_rocksgraph_test(graph_access_executor_test
                    runtime/graph_access_executor_test.cc rocksgraph_runtime)
add_rocksgraph_test(join_executor_test runtime/join_executor_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(procedure_executor_test runtime/procedure_executor_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(query_executor_test runtime/query_executor_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(result_set_executor_test
                    runtime/result_set_executor_test.cc rocksgraph_runtime)
add_rocksgraph_test(row_operator_executor_test
                    runtime/row_operator_executor_test.cc rocksgraph_runtime)
add_rocksgraph_test(write_executor_test runtime/write_executor_test.cc
                    rocksgraph_runtime)

add_rocksgraph_test(value_test value/value_test.cc rocksgraph_value)

add_rocksgraph_test(tck_scenario_test tck/tck_scenario_test.cc
                    rocksgraph_runtime)
set_property(
  SOURCE tck/tck_scenario_test.cc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tck/tck_scenario_manifest.tsv")
target_compile_definitions(
  tck_scenario_test
  PRIVATE
    ROCKSGRAPH_TCK_DIR="${ROCKSGRAPH_TCK_DIR}"
    ROCKSGRAPH_TCK_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/tck/tck_scenario_manifest.tsv")
