add_library(rocksgraph_server STATIC bolt_handler.cpp graph_manager.cpp
                                     graph_server.cpp raft_server.cpp)
add_library(RocksGraph::server ALIAS rocksgraph_server)

target_compile_features(rocksgraph_server PUBLIC cxx_std_20)
target_include_directories(rocksgraph_server PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_server PUBLIC RocksGraph::bolt
                                               RocksGraph::runtime)

add_executable(rg-server server_main.cpp)
target_include_directories(rg-server PRIVATE "${CMAKE_BINARY_DIR}/generated"
                                             ${PROJECT_SOURCE_DIR})
target_link_libraries(rg-server PRIVATE RocksGraph::server RocksGraph::common)
set_target_properties(rg-server PROPERTIES BUILD_RPATH "/usr/local/lib")
