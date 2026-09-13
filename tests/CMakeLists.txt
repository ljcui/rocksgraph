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
add_rocksgraph_test(logical_plan_rewriter_test
                    planner/logical_plan_rewriter_test.cc rocksgraph_planner)
add_rocksgraph_test(planned_query_test planner/planned_query_test.cc
                    rocksgraph_planner)

add_rocksgraph_test(
  builtin_function_evaluator_test runtime/builtin_function_evaluator_test.cc
  rocksgraph_runtime)
add_rocksgraph_test(expression_evaluator_test
                    runtime/expression_evaluator_test.cc rocksgraph_runtime)
add_rocksgraph_test(physical_plan_test runtime/physical_plan_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(query_executor_test runtime/query_executor_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(slotted_row_test runtime/slotted_row_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(slotted_runtime_test runtime/slotted_runtime_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(open_cypher_optimizer_test runtime/open_cypher_optimizer_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(index_access_test runtime/index_access_test.cc
                    rocksgraph_runtime)
add_rocksgraph_test(graphdb_query_executor_test
                    runtime/graphdb_query_executor_test.cc rocksgraph_runtime)
target_link_libraries(graphdb_query_executor_test PRIVATE graphdb)

add_rocksgraph_test(value_test value/value_test.cc rocksgraph_value)

add_test(
  NAME cypher_run_persistence_test
  COMMAND
    ${CMAKE_COMMAND} -DCYPHER_RUN=$<TARGET_FILE:cypher_run>
    -DTEST_DB=${CMAKE_CURRENT_BINARY_DIR}/cypher_run_persistence_testdb -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/cypher_run_persistence_test.cmake)

find_package(OpenMP REQUIRED)
add_executable(
  graphdb_test
  graphdb/main.cpp
  graphdb/test_benchmark.cpp
  graphdb/test_concurrency.cpp
  graphdb/test_ftindex.cpp
  graphdb/test_graph_db.cpp
  graphdb/test_index.cpp
  graphdb/test_kv_engine.cpp
  graphdb/test_transaction.cpp
  graphdb/test_value.cpp
  graphdb/test_vector_index.cpp)
target_link_libraries(graphdb_test PRIVATE GTest::gtest graphdb
                                         OpenMP::OpenMP_CXX)
set_target_properties(graphdb_test PROPERTIES BUILD_RPATH "/usr/local/lib")
add_test(NAME graphdb_test COMMAND graphdb_test)

add_rocksgraph_test(tck_scenario_test tck/tck_scenario_test.cc
                    rocksgraph_runtime)
target_link_libraries(tck_scenario_test PRIVATE graphdb)
set_property(
  SOURCE tck/tck_scenario_test.cc APPEND PROPERTY OBJECT_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tck/tck_scenario_manifest.tsv")
target_compile_definitions(
  tck_scenario_test
  PRIVATE
    ROCKSGRAPH_TCK_DIR="${ROCKSGRAPH_TCK_DIR}"
    ROCKSGRAPH_TCK_MANIFEST="${CMAKE_CURRENT_SOURCE_DIR}/tck/tck_scenario_manifest.tsv")
