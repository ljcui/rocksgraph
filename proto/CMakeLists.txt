find_package(Protobuf REQUIRED)

add_library(rocksgraph_graph_storage_proto SHARED graph_storage.pb.cc)
add_library(RocksGraph::graph_storage_proto ALIAS
            rocksgraph_graph_storage_proto)

target_compile_features(rocksgraph_graph_storage_proto PUBLIC cxx_std_20)
target_include_directories(rocksgraph_graph_storage_proto
                           PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(rocksgraph_graph_storage_proto
                      PUBLIC protobuf::libprotobuf)

add_library(rocksgraph_graph_replication_proto SHARED graph_replication.pb.cc)
add_library(RocksGraph::graph_replication_proto ALIAS
            rocksgraph_graph_replication_proto)

target_compile_features(rocksgraph_graph_replication_proto PUBLIC cxx_std_20)
target_include_directories(rocksgraph_graph_replication_proto
                           PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(
  rocksgraph_graph_replication_proto
  PUBLIC protobuf::libprotobuf RocksGraph::etcd_raft)
