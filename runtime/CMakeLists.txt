add_library(
  rocksgraph_runtime
  builtin_function_evaluator.cc
  composite_value_key.cc
  expression_evaluator.cc
  graphdb_access.cc
  graphdb_planner_catalog.cc
  plan_cache.cc
  physical_plan.cc
  physical_plan_printer.cc
  query_executor.cc
  physical_executor.cc
  physical_executor_factory.cc
  physical_executor_join.cc
  physical_executor_scan.cc
  physical_executor_support.cc
  physical_executor_unary.cc
  execution_row.cc)
add_library(RocksGraph::runtime ALIAS rocksgraph_runtime)

target_include_directories(rocksgraph_runtime PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_runtime PUBLIC RocksGraph::graphdb
                                                RocksGraph::planner)
