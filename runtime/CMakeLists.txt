add_library(
  rocksgraph_runtime STATIC
  aggregation_evaluator.cc
  apply_executor.cc
  builtin_function_evaluator.cc
  expression_evaluator.cc
  graph_access_executor.cc
  join_executor.cc
  procedure_executor.cc
  query_executor.cc
  query_row_util.cc
  result_set_executor.cc
  row_operator_executor.cc
  write_executor.cc)

target_include_directories(rocksgraph_runtime PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_runtime PUBLIC rocksgraph_planner
                                                rocksgraph_storage)
