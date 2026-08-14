add_library(rocksgraph_planner STATIC component_planner.cc cost_model.cc idp.cc
                                      logical_plan_builder.cc plan_clone.cc)

target_include_directories(rocksgraph_planner PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_planner PUBLIC rocksgraph_ir)
set_target_properties(rocksgraph_planner PROPERTIES BUILD_RPATH
                                                    "/usr/local/lib")
