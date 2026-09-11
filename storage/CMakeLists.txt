add_library(rocksgraph_storage STATIC in_memory_graph.cc)

target_include_directories(rocksgraph_storage PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_storage PUBLIC rocksgraph_planner
                                                rocksgraph_value)
