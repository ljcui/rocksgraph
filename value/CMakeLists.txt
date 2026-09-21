add_library(rocksgraph_value temporal.cc value.cc)
add_library(RocksGraph::value ALIAS rocksgraph_value)

target_include_directories(rocksgraph_value PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_value PUBLIC RocksGraph::common)
