include(GoogleTest)

add_library(rocksgraph_test_main STATIC test_main.cc)
target_link_libraries(rocksgraph_test_main PUBLIC GTest::gtest
                                                  RocksGraph::common)

function(rocksgraph_discover_tests target)
  gtest_discover_tests(
    ${target}
    TEST_PREFIX "${target}." DISCOVERY_TIMEOUT 30
    PROPERTIES
    TIMEOUT 120 LABELS "${target}")
endfunction()

function(rocksgraph_add_test target source library)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE rocksgraph_test_main ${library})
  set_target_properties(${target} PROPERTIES BUILD_RPATH "/usr/local/lib")
  rocksgraph_discover_tests(${target})
endfunction()

rocksgraph_add_test(exception_test common/exception_test.cc RocksGraph::common)
rocksgraph_add_test(table_printer_test common/table_printer_test.cc
                    RocksGraph::common)
rocksgraph_add_test(http_test common/http_test.cc RocksGraph::common)
rocksgraph_add_test(exception_boundary_test bolt/exception_boundary_test.cc
                    RocksGraph::bolt)
rocksgraph_add_test(bolt_server_test bolt/bolt_server_test.cc RocksGraph::bolt)
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
rocksgraph_add_test(execution_row_test runtime/execution_row_test.cc
                    RocksGraph::runtime)
target_link_libraries(execution_row_test PRIVATE RocksGraph::graphdb)
rocksgraph_add_test(physical_executor_test runtime/physical_executor_test.cc
                    RocksGraph::runtime)
target_link_libraries(physical_executor_test PRIVATE RocksGraph::graphdb)
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
set_tests_properties(rg_cli_test PROPERTIES TIMEOUT 60 LABELS "rg_cli_test")
set_tests_properties(
  cypher_run_persistence_test PROPERTIES TIMEOUT 120 LABELS
                                         "cypher_run_persistence_test")

find_package(OpenMP REQUIRED)
add_executable(
  graphdb_test
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
target_link_libraries(
  graphdb_test PRIVATE rocksgraph_test_main RocksGraph::graphdb
                       ${ROCKSGRAPH_OPENMP_CXX})
set_target_properties(graphdb_test PROPERTIES BUILD_RPATH "/usr/local/lib")
rocksgraph_discover_tests(graphdb_test)

rocksgraph_add_test(raft_cluster_test graphdb/raft_cluster_test.cc
                    RocksGraph::server)
rocksgraph_add_test(http_snapshot_test server/http_snapshot_test.cc
                    RocksGraph::server)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import neo4j"
    RESULT_VARIABLE ROCKSGRAPH_NEO4J_IMPORT_RESULT
    OUTPUT_QUIET ERROR_QUIET)
  if(ROCKSGRAPH_NEO4J_IMPORT_RESULT EQUAL 0)
    add_test(
      NAME neo4j_routing_test
      COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/server/neo4j_routing_test.py --server
        $<TARGET_FILE:rg-server>)
    set_tests_properties(neo4j_routing_test PROPERTIES TIMEOUT 120 LABELS
                                                       "neo4j_routing_test")
  else()
    message(STATUS "Neo4j Python driver not found; neo4j_routing_test disabled")
  endif()
endif()

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
