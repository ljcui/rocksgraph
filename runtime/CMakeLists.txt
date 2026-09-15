add_library(
  rocksgraph_runtime STATIC
  builtin_function_evaluator.cc
  expression_evaluator.cc
  graphdb_access.cc
  graphdb_planner_catalog.cc
  physical_plan.cc
  physical_plan_printer.cc
  query_executor.cc
  slotted_executor.cc
  slotted_executor_factory.cc
  slotted_executor_join.cc
  slotted_executor_scan.cc
  slotted_executor_support.cc
  slotted_executor_unary.cc
  slotted_row.cc)

target_include_directories(rocksgraph_runtime PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_runtime PUBLIC rocksgraph_planner transaction)
