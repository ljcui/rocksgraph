add_library(
  rocksgraph_ir STATIC
  logical_plan.cc
  logical_plan_printer.cc
  query_ir.cc
  query_ir_arguments.cc
  query_ir_builder.cc
  query_ir_graph_builder.cc
  query_ir_model.cc
  query_ir_printer.cc
  query_ir_util.cc)

target_include_directories(rocksgraph_ir PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_ir PUBLIC rocksgraph_ast)
set_target_properties(rocksgraph_ir PROPERTIES BUILD_RPATH "/usr/local/lib")
