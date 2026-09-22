function(rocksgraph_add_test target source library)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE GTest::gtest_main ${library})
  set_target_properties(${target} PROPERTIES BUILD_RPATH "/usr/local/lib")
  add_test(NAME ${target} COMMAND ${target})
endfunction()

rocksgraph_add_test(exception_test common/exception_test.cc RocksGraph::common)
rocksgraph_add_test(exception_boundary_test bolt/exception_boundary_test.cc
                    RocksGraph::bolt)
rocksgraph_add_test(bolt_summary_test bolt/summary_test.cc RocksGraph::bolt)
rocksgraph_add_test(bolt_hydrator_test bolt/hydrator_test.cc RocksGraph::bolt)

rocksgraph_add_test(ast_node_type_test ast/ast_node_type_test.cc
                    RocksGraph::ast)
rocksgraph_add_test(ast_rewriter_test ast/ast_rewriter_test.cc RocksGraph::ast)
rocksgraph_add_test(ast_to_cypher_test ast/ast_to_cypher_test.cc
                    RocksGraph::ast)
rocksgraph_add_test(semantic_table_test ast/semantic_table_test.cc
                    RocksGraph::ast)
rocksgraph_add_test(semantic_validator_test ast/semantic_validator_test.cc
                    RocksGraph::ast)

rocksgraph_add_test(logical_plan_test ir/logical_plan_test.cc RocksGraph::ir)
rocksgraph_add_test(query_ir_test ir/query_ir_test.cc RocksGraph::ir)

rocksgraph_add_test(cost_model_test planner/cost_model_test.cc
                    RocksGraph::planner)
rocksgraph_add_test(idp_test planner/idp_test.cc RocksGraph::planner)
rocksgraph_add_test(logical_plan_builder_test
                    planner/logical_plan_builder_test.cc RocksGraph::planner)
rocksgraph_add_test(logical_plan_rewriter_test
                    planner/logical_plan_rewriter_test.cc RocksGraph::planner)
rocksgraph_add_test(planned_query_test planner/planned_query_test.cc
                    RocksGraph::planner)

rocksgraph_add_test(
  builtin_function_evaluator_test runtime/builtin_function_evaluator_test.cc
  RocksGraph::runtime)
rocksgraph_add_test(expression_evaluator_test
                    runtime/expression_evaluator_test.cc RocksGraph::runtime)
rocksgraph_add_test(physical_plan_test runtime/physical_plan_test.cc
                    RocksGraph::runtime)
target_link_libraries(physical_plan_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(plan_cache_test runtime/plan_cache_test.cc
                    RocksGraph::runtime)
target_link_libraries(plan_cache_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(query_executor_test runtime/query_executor_test.cc
                    RocksGraph::runtime)
target_link_libraries(query_executor_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(slotted_row_test runtime/slotted_row_test.cc
                    RocksGraph::runtime)
target_link_libraries(slotted_row_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(slotted_runtime_test runtime/slotted_runtime_test.cc
                    RocksGraph::runtime)
target_link_libraries(slotted_runtime_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(open_cypher_optimizer_test
                    runtime/open_cypher_optimizer_test.cc RocksGraph::runtime)
target_link_libraries(open_cypher_optimizer_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(index_access_test runtime/index_access_test.cc
                    RocksGraph::runtime)
target_link_libraries(index_access_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(graphdb_query_executor_test
                    runtime/graphdb_query_executor_test.cc RocksGraph::runtime)
target_link_libraries(graphdb_query_executor_test PRIVATE RocksGraph::graphdb)

rocksgraph_add_test(value_test value/value_test.cc RocksGraph::value)

add_test(
  NAME cypher_run_persistence_test
  COMMAND
    ${CMAKE_COMMAND} -DCYPHER_RUN=$<TARGET_FILE:cypher_run>
    -DTEST_DB=${CMAKE_CURRENT_BINARY_DIR}/cypher_run_persistence_testdb -P
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/cypher_run_persistence_test.cmake)

add_test(NAME rg_cli_test
         COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tools/rg_cli_test.sh
                 $<TARGET_FILE:rg-server> $<TARGET_FILE:rg-cli>)
set_tests_properties(rg_cli_test PROPERTIES TIMEOUT 60)

find_package(OpenMP REQUIRED)
add_executable(
  graphdb_test
  graphdb/graphdb_test_main.cc
  graphdb/benchmark_test.cc
  graphdb/concurrency_test.cc
  graphdb/ftindex_test.cc
  graphdb/graph_db_test.cc
  graphdb/id_codec_test.cc
  graphdb/index_test.cc
  graphdb/kv_engine_test.cc
  graphdb/transaction_test.cc
  graphdb/value_test.cc
  graphdb/vector_index_test.cc)
target_link_libraries(graphdb_test PRIVATE GTest::gtest RocksGraph::graphdb
                                           ${ROCKSGRAPH_OPENMP_CXX})
set_target_properties(graphdb_test PROPERTIES BUILD_RPATH "/usr/local/lib")
add_test(NAME graphdb_test COMMAND graphdb_test)

rocksgraph_add_test(raft_cluster_test graphdb/raft_cluster_test.cc
                    RocksGraph::server)

add_custom_target(
  check-tck
  COMMAND
    ${CMAKE_COMMAND} -E env "ROCKSGRAPH_TCK_SERVER_BIN=$<TARGET_FILE:rg-server>"
    "${CMAKE_CURRENT_SOURCE_DIR}/tck/run_tck.sh" --no-color --no-junit
    --no-capture --format progress
  DEPENDS rg-server
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "Run the full openCypher TCK with Behave."
  USES_TERMINAL VERBATIM)
