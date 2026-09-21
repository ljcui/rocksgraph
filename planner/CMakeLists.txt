add_library(
  rocksgraph_planner
  component_planner.cc
  cost_model.cc
  idp.cc
  logical_plan_builder.cc
  logical_plan_rewriter.cc
  order_property.cc
  plan_clone.cc
  planned_query.cc)
add_library(RocksGraph::planner ALIAS rocksgraph_planner)

target_include_directories(rocksgraph_planner PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_planner PUBLIC RocksGraph::ir)
set_target_properties(rocksgraph_planner PROPERTIES BUILD_RPATH
                                                    "/usr/local/lib")
